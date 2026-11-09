// Included in graphics_system.cpp. 60: gta4_native_stream_record — the direct emitter.
//
// With gta4_native_direct_record the submitting thread (RenderGuest) assembles every command
// right after capturing it. Stream record also prepares and records it there: the frame's
// command buffers are begun at its first command, PrepareFrameTexturesTask and
// RecordNativeFrameTask take each command as the worker retains it, and suspend between
// commands. Present only records the tail (the last stream_.lookahead commands, which the
// merges look ahead over) and the presentation, then submits:
//   - preparation command buffer: texture uploads / clears of images created for this frame,
//     null image initialization. The GPU runs it before the frame's command buffer, as it ran
//     PrepareFrameTextures before RecordNativeFrame: only images created by the preparation
//     itself are written there, so no image is touched out of order;
//   - frame command buffer: everything RecordNativeFrame records.
// What needs the whole frame is replaced by the previous frame's values: the descriptor and
// constant budgets, has-water, the presenter size, the present source and the frame number.
// The optimizations that look further than the window are off in a streamed frame:
// present-from-surface (A11), planned resolve swaps (A2), the lossless HDR present mirror.
// Every frame that cannot be streamed is recorded the classic way at Present.

template <typename RefreshContext>
bool Gta4NativeGraphicsSystem::StreamSubmit(RefreshContext* context) {
  auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const ui::vulkan::VulkanDevice* vulkan_device =
      vulkan_provider ? vulkan_provider->vulkan_device() : nullptr;
  bool committed = false;
  const auto finish = MakeScopeExit([&] {
    // The tasks have finished; destroying them releases their frames.
    stream_.prep.reset();
    stream_.record.reset();
    stream_.open = stream_.closing = stream_.no_present = false;
    stream_.presenter_image = VK_NULL_HANDLE;
    stream_.presenter_view = VK_NULL_HANDLE;
    recording_frame_ = &current_frame_;
    if (!committed) {
      // As ClearGuestOutput's failure cleanup: the recording never reached the queue.
      semantic_light_setup_lineage_.OnBoundary(NativeLightingBatchBoundary::kDiscardedRecording);
      RecoverFailedNativeFrameRecording();
      DiscardTemporalFrame();
      stream_disabled_ = true;
      stream_last_reason_ = "submit-failed";
      REXLOG_ERROR("gta4-native-stream: frame {} was not submitted; stream record is off",
                   stream_.frame);
    }
  });
  if (!vulkan_device || !submission_tracker_) return false;
  const auto& dfn = vulkan_device->functions();
  const uint32_t submitted_frame = stream_.frame;

  if (context) {
    AcquireNativePresenterImage(command_buffer_);
    VkImageMemoryBarrier release_barrier{};
    release_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    release_barrier.srcAccessMask =
        stream_.presenter_written
            ? (stream_.presenter_transfer_written ? VK_ACCESS_TRANSFER_WRITE_BIT
                                                  : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            : 0;
    release_barrier.dstAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
    release_barrier.oldLayout = stream_.presenter_transfer_written
                                    ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                    : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    release_barrier.newLayout = ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
    release_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    release_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    release_barrier.image = context->image();
    release_barrier.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    profile::CpuCall(profile::CpuOp::kDriverBarrier, [&] {
      return (++g_barrier_sites[__LINE__], dfn.vkCmdPipelineBarrier)(
          command_buffer_,
          stream_.presenter_transfer_written ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                             : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0, nullptr,
          1, &release_barrier);
    });
  }
  {
    VkMemoryBarrier prep_barrier{};
    prep_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    prep_barrier.srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    prep_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    profile::CpuCall(profile::CpuOp::kDriverBarrier, [&] {
      return (++g_barrier_sites[__LINE__], dfn.vkCmdPipelineBarrier)(
          stream_.prep_command_buffer,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          0, 1, &prep_barrier, 0, nullptr, 0, nullptr);
    });
  }
  if (profile::CpuCall(profile::CpuOp::kDriverEnd, [&] {
        return dfn.vkEndCommandBuffer(stream_.prep_command_buffer);
      }) != VK_SUCCESS ||
      profile::CpuCall(profile::CpuOp::kDriverEnd, [&] {
        return dfn.vkEndCommandBuffer(command_buffer_);
      }) != VK_SUCCESS) {
    return false;
  }

  // The preparation buffer first: the frame samples what it uploads.
  const std::array<VkCommandBuffer, 2> buffers = {stream_.prep_command_buffer, command_buffer_};
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = uint32_t(buffers.size());
  submit_info.pCommandBuffers = buffers.data();
  ui::vulkan::ImageAccessSubmit image_access_submit;
  if (context && !image_access_submit.Attach(submit_info, context->image_access())) return false;
  const uint64_t submission = submission_tracker_->GetCurrentSubmission();
  ui::vulkan::VulkanSubmissionTracker::FenceAcquisition fence_acquisition(
      submission_tracker_->AcquireFenceToAdvanceSubmission());
  if (!fence_acquisition.fence()) {
    fence_acquisition.SubmissionFailedOrDropped();
    return false;
  }
  const auto ring_submission =
      frame_context_ring_.PrepareSubmission(frame_context_tokens_[active_frame_slot_].value());
  if (!ring_submission) {
    REXLOG_ERROR("gta4-native-stream: failed to prepare slot {} for GPU submission",
                 active_frame_slot_);
    fence_acquisition.SubmissionFailedOrDropped();
    return false;
  }
  frame_context_serials_[active_frame_slot_] = *ring_submission;
  StageNativeFlightResource(NativeFlightResourceKind::kCommandPool,
                            NativeVulkanHandleIdentity(command_pool_));
  StageNativeFlightResource(NativeFlightResourceKind::kCommandBuffer,
                            NativeVulkanHandleIdentity(command_buffer_));
  StageNativeFlightResource(NativeFlightResourceKind::kCommandBuffer,
                            NativeVulkanHandleIdentity(stream_.prep_command_buffer));
  StageNativeFlightResource(NativeFlightResourceKind::kUploadBuffer,
                            NativeVulkanHandleIdentity(upload_buffer_.buffer));
  for (const NativeUploadBuffer& overflow : overflow_upload_buffers_) {
    StageNativeFlightResource(NativeFlightResourceKind::kUploadBuffer,
                              NativeVulkanHandleIdentity(overflow.buffer));
  }
  if (active_frame_slot_ < frame_constant_arenas_.size()) {
    StageNativeFlightResource(
        NativeFlightResourceKind::kUploadBuffer,
        NativeVulkanHandleIdentity(frame_constant_arenas_[active_frame_slot_].storage.buffer));
  }
  StageNativeFlightResource(NativeFlightResourceKind::kDescriptorPool,
                            NativeVulkanHandleIdentity(frame_descriptor_pool_));
  VkResult submit_result;
  {
    auto queue = profile::CpuCall(profile::CpuOp::kQueueLock, [&] {
      return vulkan_device->AcquireQueue(vulkan_device->queue_family_native_offscreen(), 0);
    });
    submit_result = profile::CpuCall(profile::CpuOp::kDriverSubmit, [&] {
      return dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, fence_acquisition.fence());
    });
    if (submit_result == VK_SUCCESS) {
      MarkFrameTimeline(submitted_frame, kTimelineQueueSubmit);
      if (context) context->MarkImageAccessSubmitted();
    }
  }
  if (submit_result != VK_SUCCESS) {
    gpu_flight::Fail("native.stream-queue-submit", int32_t(submit_result),
                     NativeVulkanHandleIdentity(command_buffer_), submission, submitted_frame);
    fence_acquisition.SubmissionFailedOrDropped();
    return false;
  }
  // The constant arena budget of the next streamed frames (it cannot grow while bound).
  if (active_frame_slot_ < frame_constant_arenas_.size()) {
    const VkDeviceSize used =
        VkDeviceSize(frame_constant_arenas_[active_frame_slot_].index.bytes_used());
    constexpr VkDeviceSize kStreamConstantArenaFloor = VkDeviceSize(32) << 20;
    stream_constant_reserve_ =
        std::max({used, stream_constant_reserve_ - stream_constant_reserve_ / 16, kStreamConstantArenaFloor});
  }
  CommitNativeFlightSubmission(active_frame_slot_, submission, submitted_frame);
  command_buffer_submission_ = submission;
  if (native_descriptor_backend_ == NativeDescriptorBackend::kIndexed && native_descriptor_paging_ &&
      !native_descriptor_working_sets_[active_frame_slot_].MarkSubmitted(submission)) {
    REXLOG_ERROR("gta4-native-descriptors: frame ownership commit failed slot={} submission={}",
                 active_frame_slot_, submission);
  }
  if (native_cached_descriptor_state_) {
    native_cached_descriptor_state_->slots[active_frame_slot_].descriptors.MarkSubmitted(submission);
  }
  if (!frame_constant_arenas_[active_frame_slot_].index.MarkSubmitted(submission)) {
    REXLOG_ERROR("gta4-native-constants: failed to mark slot {} arena submitted at {}",
                 active_frame_slot_, submission);
  }
  const bool ring_committed = frame_context_ring_.CommitSubmission(
      frame_context_tokens_[active_frame_slot_].value(), *ring_submission);
  committed = true;
  if (!ring_committed) {
    REXLOG_ERROR(
        "gta4-native-frames: queue accepted submission {} but slot {} ownership commit failed",
        submission, active_frame_slot_);
  }
  if (context) {
    context->SetCompletion(std::make_shared<NativeSubmissionCompletion>(submission_tracker_, submission));
    context->SetIs8bpc(false);
  }
  UpdateNativeMemoryProfile(stream_.resource_frame);
  ++stream_frames_;
  return context && stream_.presenter_written;
}

bool Gta4NativeGraphicsSystem::StreamRecordAllowed() {
  if (!stream_record_ || stream_disabled_ || !direct_record_ || !pipelined_recording_ ||
      !inline_recording_ || native_renderer_recovery_failed_ || !presenter_ || !provider_) {
    return false;
  }
  // Until a presenter callback has run (start-up, a refresh that skipped it).
  // Startup frames are recorded via the classic monolithic path without disabling stream.
  if (!stream_presenter_seen_) {
    return false;
  }
  // Ensure Vulkan objects (descriptor sets, layouts, pipelines) are initialized before checking backend.
  if (!pipeline_layout_ && !InitializeNativeRendererObjects()) {
    return false;
  }
  // Draw bindings are assigned per command only in the paged indexed backend; the others
  // allocate the frame's descriptor sets after its last command.
  if (native_descriptor_backend_ != NativeDescriptorBackend::kIndexed || !native_descriptor_paging_) {
    stream_disabled_ = true;
    stream_last_reason_ = "descriptor-backend";
    REXLOG_WARN("gta4-native-stream: needs the paged indexed descriptor backend; stream record is off");
    return false;
  }
  // Frames that need the whole frame before recording, or a profiler, go the classic way:
  // temporal upscaling (the previous frame had it), the native GPU profiler, the traces.
  if (frame_has_temporal_updates_ || temporal_fields_set_ || temporal_active_ ||
      temporal_display_ready_ ||
      CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeProfiler) ||
      PhoneTraceConfig().enabled || FireTraceConfig().enabled || TvTraceConfig().enabled ||
      EmissionTraceConfig().enabled) {
    return false;
  }
  return true;
}

bool Gta4NativeGraphicsSystem::StreamOpenFrame() {
  auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const ui::vulkan::VulkanDevice* vulkan_device =
      vulkan_provider ? vulkan_provider->vulkan_device() : nullptr;
  if (!vulkan_device || !InitializeNativeRendererObjects()) return false;
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!submission_tracker_) {
    submission_tracker_ = std::make_shared<ui::vulkan::VulkanSubmissionTracker>(vulkan_device);
  }
  const uint32_t frame = stream_next_frame_;
  // The slots alternate: the one taken now was submitted two frames ago (ClearGuestOutput takes
  // it by frame parity; at Present, not at the frame's first command).
  const uint32_t slot = (active_frame_slot_ + 1) % NativeFrameContextRing::kSlotCount;
  if (!ActivateNativeFrameSlot(slot) || !CompleteActiveNativeFrameSlot(nullptr, nullptr)) {
    return false;
  }
  DiscardStagedNativeFlightResources();
  const auto frame_token = frame_context_ring_.BeginFrame(active_frame_slot_);
  if (!frame_token) {
    REXLOG_ERROR("gta4-native-stream: failed to begin slot {} for frame {}", active_frame_slot_,
                 frame);
    return false;
  }
  frame_context_tokens_[active_frame_slot_] = *frame_token;
  frame_context_serials_[active_frame_slot_] = 0;
  bool recording_started = false;
  bool opened = false;
  const auto failure_cleanup = MakeScopeExit([&] {
    if (opened) return;
    stream_.prep.reset();
    stream_.record.reset();
    stream_.open = stream_.closing = false;
    recording_frame_ = &current_frame_;
    semantic_light_setup_lineage_.OnBoundary(NativeLightingBatchBoundary::kDiscardedRecording);
    if (recording_started) {
      RecoverFailedNativeFrameRecording();
    } else {
      RollbackActiveNativeFrameSlot();
    }
    DiscardTemporalFrame();
  });

  // ClearGuestOutput's housekeeping, at the frame's start instead of its Present.
  const uint32_t resource_frame = NativeResourceFrameForBatch(active_texture_frame_, frame, true);
  active_texture_frame_ = resource_frame;
  ReleasePendingSurfaceImages();
  ReleaseUnusedTextureImages(resource_frame, PredictStreamPresentSource());
  if (!ReleaseRetiredTextureImages()) return false;
  ReleaseUnusedBufferResources(resource_frame);
  ReleaseUnusedPersistentBuffers(completed_command_buffer_submission_);
  if (!PrepareTemporalFrame()) return false;
  // No upload capacity plan: it walks a frame that is not assembled yet and would shrink the
  // arena; AllocateUpload grows it on demand (overflow buffers), and it stays grown.
  stream_capacity_planning_ = true;
  const bool arena_ready = EnsureFrameConstantArenaCapacity();
  stream_capacity_planning_ = false;
  if (!arena_ready) return false;

  if (!command_pool_) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vulkan_device->queue_family_native_offscreen();
    if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
      command_pool_ = VK_NULL_HANDLE;
      return false;
    }
    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(device, &allocate_info, &command_buffer_) != VK_SUCCESS) {
      dfn.vkDestroyCommandPool(device, command_pool_, nullptr);
      command_pool_ = VK_NULL_HANDLE;
      return false;
    }
  } else if (dfn.vkResetCommandPool(device, command_pool_, 0) != VK_SUCCESS) {
    return false;
  } else {
    ++command_pool_reset_count_;
  }
  // The preparation buffer comes from the same pool (reset together with it).
  stream_.prep_command_buffer = VK_NULL_HANDLE;
  for (const auto& [pool, buffer] : stream_prep_buffers_) {
    if (pool == command_pool_) stream_.prep_command_buffer = buffer;
  }
  if (!stream_.prep_command_buffer) {
    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(device, &allocate_info, &stream_.prep_command_buffer) !=
        VK_SUCCESS) {
      stream_.prep_command_buffer = VK_NULL_HANDLE;
      return false;
    }
    // Two pools alternate with the slots; they live until shutdown.
    auto* entry = &stream_prep_buffers_[0];
    for (auto& candidate : stream_prep_buffers_) {
      if (!candidate.first) {
        entry = &candidate;
        break;
      }
    }
    *entry = {command_pool_, stream_.prep_command_buffer};
  }
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(stream_.prep_command_buffer, &begin_info) != VK_SUCCESS) return false;
  if (dfn.vkBeginCommandBuffer(command_buffer_, &begin_info) != VK_SUCCESS) return false;
  recording_started = true;
  {
    VkMemoryBarrier acquire_barrier{};
    acquire_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    acquire_barrier.srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    acquire_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    profile::CpuCall(profile::CpuOp::kDriverBarrier, [&] {
      return (++g_barrier_sites[__LINE__], dfn.vkCmdPipelineBarrier)(
          command_buffer_,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          0, 1, &acquire_barrier, 0, nullptr, 0, nullptr);
    });
  }
  native_draw_state_cache_.Reset();
  ubo_bound_layout_ = VK_NULL_HANDLE;
  if (resolve_recording_epoch_ != UINT64_MAX) ++resolve_recording_epoch_;
  g_resolve_reuse_candidates += resolve_reuse_candidates_;
  g_resolve_reuse_hits += resolve_reuse_hits_;
  resolve_reuse_candidates_ = resolve_reuse_hits_ = resolve_reuse_pixels_ = 0;
  resolve_shared_hdr_mirrors_ = 0;
  // The guest output image is known only in the presenter callback at Present.
  temporal_presenter_acquired_ = false;
  if (!RecordNullImageInitialization(stream_.prep_command_buffer)) return false;
  if (!pending_detile_validations_.empty()) CheckTextureDetileValidations(upload_buffer_.buffer);
  CheckBreadcrumbs();
  std::memset(upload_buffer_.mapping, 0, size_t(kDefaultVertexDataSize));
  upload_buffer_.write_offset = kDefaultVertexDataSize;
  for (NativeUploadBuffer& overflow : overflow_upload_buffers_) {
    std::memset(overflow.mapping, 0, size_t(kDefaultVertexDataSize));
    overflow.write_offset = kDefaultVertexDataSize;
  }
  FinalizePlannedResolveSwaps();
  // RunRecorderJob begins it before recording; a streamed frame records from here.
  BeginModernShaderFrame();

  stream_.frame = frame;
  stream_.resource_frame = resource_frame;
  stream_.closing = stream_.no_present = false;
  stream_.prep_source = 0;
  stream_.lookahead = REXCVAR_GET(gta4_native_stream_lookahead);
  stream_.width = stream_last_width_ ? stream_last_width_ : kDefaultOutputWidth;
  stream_.height = stream_last_height_ ? stream_last_height_ : kDefaultOutputHeight;
  stream_.presenter_image = VK_NULL_HANDLE;
  stream_.presenter_view = VK_NULL_HANDLE;
  stream_.present_source = PredictStreamPresentSource();  // 61
  stream_.environmental_data.reset();
  stream_.hdr_output = stream_last_hdr_output_;
  stream_.hdr_headroom = stream_last_hdr_headroom_;
  stream_.presenter_transfer_written = stream_.presenter_written = false;
  stream_.trace_stages = false;
  stream_.trace_sequence = 0;
  stream_.force_content_probe = false;
  stream_.open = true;
  recording_frame_ = &assembly_frame_;
  recording_ring_serial_ = worker_ring_serial_;
  if (assembly_frame_.capacity() < 8192) {
    assembly_frame_.reserve(8192);
  }
  // Its start resets the frame's descriptor pool and begins the descriptor working set; it then
  // prepares the frame's first command and suspends.
  stream_.prep = PrepareFrameTexturesTask(stream_.prep_command_buffer, true, frame, false, true);
  if (stream_.prep.done()) {
    REXLOG_ERROR("gta4-native-stream: texture preparation of frame {} failed at its start", frame);
    return false;
  }
  // As ClearGuestOutput after PrepareFrameTextures (the conversions take sets from the pool just
  // reset): depth aliases from their sources as they are at the frame's start. With
  // stream_.open the filter also takes the aliases the previous frame used.
  if (!RefreshPackedDepthAliases(command_buffer_, 0)) return false;
  stream_.record = RecordNativeFrameTask(
      command_buffer_, stream_.width, stream_.height, stream_.presenter_image,
      stream_.presenter_view, frame, stream_.present_source, stream_.environmental_data,
      stream_.hdr_output, stream_.hdr_headroom, stream_.presenter_transfer_written,
      stream_.presenter_written, stream_.trace_stages, stream_.trace_sequence,
      stream_.force_content_probe, true);
  if (stream_.record.done()) {
    REXLOG_ERROR("gta4-native-stream: recording of frame {} failed at its start", frame);
    return false;
  }
  opened = true;
  return true;
}

void Gta4NativeGraphicsSystem::StreamPump() {
  // Lazy resolves keep a destination that was read within 120 frames. RecordNativeFrame marks
  // the whole frame's inputs before its first command; a streamed frame marks each command
  // here, before the window behind it is recorded.
  const auto mark_inputs = [this](size_t first, uint32_t frame) {
    if (!REXCVAR_GET(gta4_native_lazy_resolves)) return;
    const auto mark = [&](const std::shared_ptr<const NativeTextureResource>& texture) {
      if (!texture) return;
      texture_input_frames_[texture->generation] = frame;
      if (texture->packed_depth_source)
        texture_input_frames_[texture->packed_depth_source->generation] = frame;
    };
    for (size_t index = first; index < assembly_frame_.size(); ++index) {
      const NativeCommand& command = assembly_frame_[index];
      for (const auto& texture : command.textures) mark(texture);
      mark(command.postfx_half_scene);
      mark(command.depth_handoff_source);
    }
  };
  if (!stream_.open) {
    // A frame is streamed from its first command, or recorded the classic way at Present.
    if (assembly_frame_.size() != 1 || !StreamRecordAllowed()) return;
    mark_inputs(0, stream_next_frame_);
    if (!StreamOpenFrame()) {  // prepares the first command; records it if lookahead is 0
      stream_disabled_ = true;
      stream_last_reason_ = "open-failed";
      REXLOG_ERROR("gta4-native-stream: could not open a streamed frame; stream record is off");
    }
    ++stream_pumps_;
    return;
  }
  if (recording_frame_ != &assembly_frame_) return;  // a recorder-sync command holds the frame
  // Nothing new since the last pump (a state command): neither task can advance.
  if (stream_.prep_source == assembly_frame_.size()) return;
  ++stream_pumps_;
  mark_inputs(stream_.prep_source, stream_.frame);
  stream_.prep.resume();
  if (stream_.prep.done()) {
    // Only a failure ends it before the frame is closed (a descriptor capacity fallback): the
    // frame records what it has; later frames are recorded the classic way.
    stream_disabled_ = true;
    stream_last_reason_ = "preparation-failed";
  }
  stream_.record.resume();
}

bool Gta4NativeGraphicsSystem::StreamFinish(bool present) {
  stream_.closing = true;
  stream_.no_present = !present;
  stream_.prep.resume();  // the remaining commands; publishes the frame's descriptor pages
  const bool prepared = stream_.prep.done() && stream_.prep.result();
  stream_.record.resume();  // the tail, then the presentation
  const bool recorded = stream_.record.done() && stream_.record.result();
  FinalizePlannedResolveSwaps();
  if (!stream_.prep.done() || !stream_.record.done()) {
    REXLOG_ERROR("gta4-native-stream: frame {} did not finish its tasks", stream_.frame);
  }
  return prepared && recorded;
}

void Gta4NativeGraphicsSystem::StreamFlush(const char* reason) {
  if (!stream_.open) return;
  ++stream_flushes_;
  stream_last_reason_ = reason;
  StreamFinish(false);
  StreamSubmit<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext>(nullptr);
}

bool Gta4NativeGraphicsSystem::ClearGuestOutputStreamed(
    uint32_t width, uint32_t height, uint32_t display_width, uint32_t display_height,
    const PresentCommand& present,
    const std::shared_ptr<const NativeTextureResource>& present_source,
    const std::shared_ptr<const EnvironmentalDataV2>& environmental_data) {
  const profile::CpuScope profile_scope(profile::CpuOp::kRefreshOutput);
  if (!presenter_ || !provider_) {
    StreamFlush("no-presenter");
    return false;
  }
  // The values the frame was recorded with were predictions; these are the real ones.
  const uint32_t submitted_frame = present.submitted_frame;
  if (present.device) {
    // Swapped in at this Present: the frame's own assembled bound.
    stream_has_water_ = frame_constant_bound_.valid && frame_constant_bound_.has_water;
    if (submitted_frame != stream_.frame) ++stream_frame_mismatches_;
    stream_next_frame_ = submitted_frame + 1;
    // 61: by generation (the object is the same while its generation is), and the prediction
    // is two presents back when the title alternates two frontbuffers.
    const uint64_t predicted = stream_.present_source ? stream_.present_source->generation : 0;
    if ((present_source ? present_source->generation : 0) != predicted) {
      ++stream_source_mismatches_;
    }
    CountStreamPresentHandle(present_source);  // 62
    RememberStreamPresentSource(present_source);
    stream_last_width_ = width;
    stream_last_height_ = height;
  }
  stream_.width = width;
  stream_.height = height;
  stream_.present_source = present_source;
  stream_.environmental_data = environmental_data;
  stream_.force_content_probe = present.diagnostic_force_content_probe != 0;
  bool callback_ran = false;
  const bool refresh_result = presenter_->RefreshGuestOutput(
      width, height, display_width, display_height,
      [this, present_source, &callback_ran](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        callback_ran = true;
        auto& vulkan_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        // ClearGuestOutput's acquire barrier, recorded when the tail first targets the image.
        VkImageMemoryBarrier acquire_barrier{};
        acquire_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire_barrier.srcAccessMask = vulkan_context.image_ever_written_previously()
                                            ? ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                                            : 0;
        acquire_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        acquire_barrier.oldLayout = vulkan_context.image_ever_written_previously()
                                        ? ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
        acquire_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        acquire_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.image = vulkan_context.image();
        acquire_barrier.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
        temporal_presenter_acquire_ = acquire_barrier;
        temporal_presenter_acquire_stages_ =
            vulkan_context.image_ever_written_previously()
                ? ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        temporal_presenter_acquired_ = false;
        stream_.presenter_image = vulkan_context.image();
        stream_.presenter_view = vulkan_context.image_view();
        stream_.hdr_output = stream_last_hdr_output_ = vulkan_context.hdr_output();
        stream_.hdr_headroom = stream_last_hdr_headroom_ = vulkan_context.hdr_headroom();
        // The frontbuffer, as ClearGuestOutput prepares it before PrepareFrameTextures: into
        // the preparation buffer (it creates the image if the frame never resolved into it).
        if (present_source &&
            !GetOrCreateTextureImage(stream_.prep_command_buffer, present_source)) {
          REXLOG_ERROR("gta4-native-stream: failed to prepare frontbuffer texture generation {}",
                       present_source->generation);
        }
        if (!StreamFinish(true)) {
          ++stream_failed_frames_;
        }
        return StreamSubmit(&vulkan_context);
      },
      ui::GuestOutputProvenance{
          .frame_rate_limit = rex::cvar::Query<uint32_t>("gta4_frame_limit"),
          .producer_backpressure = present.device != 0,
          .tv_session_id = present.diagnostic_tv_session_id,
          .title_present_id = present.diagnostic_present_id,
          .selected_generation = present_source ? present_source->generation : 0,
          .tv_final_sequence = present.diagnostic_tv_final_sequence,
          .tv_movie_sequence = present.diagnostic_tv_movie_sequence,
          .tv_rect_sequence = present.diagnostic_tv_rect_sequence,
          .tv_bink_sequence = present.diagnostic_tv_bink_sequence,
          .submitted_frame = present.submitted_frame,
          .present_origin = present.diagnostic_origin,
          .guest_caller = present.diagnostic_guest_caller,
          .selected_texture = present_source ? present_source->handle : 0,
          .native_command_count = uint32_t(recording_frame_->size()),
          .tv_bink_result = present.diagnostic_tv_bink_result,
          .diagnostic_trace = present.diagnostic_trace != 0,
      });
  stream_presenter_seen_ = callback_ran;
  if (!callback_ran && stream_.open) {
    // The presenter did not take this frame: submit what was recorded, without presentation.
    ++stream_presenter_fallbacks_;
    StreamFlush("presenter-skipped-callback");
  }
  ReportStreamRecord();
  return refresh_result;
}

void Gta4NativeGraphicsSystem::ReportStreamRecord() {
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t frequency = rex::chrono::Clock::QueryHostTickFrequency();
  if (!stream_report_tick_) stream_report_tick_ = now;
  if (!frequency || now - stream_report_tick_ < 10 * frequency) return;
  REXLOG_INFO(
      "gta4-native-stream: {:.1f} s: streamed={} classic={} flushes={} failed={} "
      "presenter-fallbacks={} frame-mismatches={} source-mismatches={} pumps/frame={:.0f} "
      "lookahead={} constant-reserve={}KB draw-reserve={} resolve-reserve={} task-frame={}KB "
      "disabled={} last={}",
      double(now - stream_report_tick_) / double(frequency), stream_frames_,
      stream_classic_frames_, stream_flushes_, stream_failed_frames_, stream_presenter_fallbacks_,
      stream_frame_mismatches_, stream_source_mismatches_,
      stream_frames_ ? double(stream_pumps_) / double(stream_frames_) : 0.0, stream_.lookahead,
      uint64_t(stream_constant_reserve_ >> 10), stream_draw_reserve_, stream_resolve_reserve_,
      NativeFrameTaskMemory::Get().largest >> 10, stream_disabled_, stream_last_reason_);
#if defined(__ANDROID__)
  // 61: 64-bit counters with %llu (they were %u); deferred-fb: skipped frontbuffer resolves of
  // the previous frame recorded at a streamed frame's start (alternating frontbuffers).
  using ull = unsigned long long;
  __android_log_print(
      ANDROID_LOG_WARN, "LibertyRecomp",
      "gta4-native-stream: %.1f s: streamed=%llu classic=%llu flushes=%llu failed=%llu "
      "presenter-fallbacks=%llu frame-mismatches=%llu source-mismatches=%llu deferred-fb=%llu "
      "fb-handle-same=%llu fb-handle-other=%llu "
      "fb-new-gen[none/cpu/lifetime/dirty/fetch/alias/pre-resolve/snapshot]="
      "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu tex-images-created=%llu "
      "pumps/frame=%.0f lookahead=%u constant-reserve=%lluKB draw-reserve=%u resolve-reserve=%u "
      "task-frame=%lluKB disabled=%d last=%s",
      double(now - stream_report_tick_) / double(frequency), ull(stream_frames_),
      ull(stream_classic_frames_), ull(stream_flushes_), ull(stream_failed_frames_),
      ull(stream_presenter_fallbacks_), ull(stream_frame_mismatches_),
      ull(stream_source_mismatches_), ull(stream_deferred_frontbuffer_resolves_),
      ull(stream_handle_same_), ull(stream_handle_other_),
      ull(frontbuffer_new_generation_[0].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[1].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[2].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[3].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[4].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[5].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[6].load(std::memory_order_relaxed)),
      ull(frontbuffer_new_generation_[7].load(std::memory_order_relaxed)),
      ull(texture_images_created_.load(std::memory_order_relaxed)),
      stream_frames_ ? double(stream_pumps_) / double(stream_frames_) : 0.0,
      unsigned(stream_.lookahead), ull(stream_constant_reserve_ >> 10),
      unsigned(stream_draw_reserve_), unsigned(stream_resolve_reserve_),
      ull(NativeFrameTaskMemory::Get().largest >> 10), stream_disabled_ ? 1 : 0,
      stream_last_reason_ ? stream_last_reason_ : "none");
#endif
  stream_report_tick_ = now;
  stream_frames_ = stream_classic_frames_ = stream_flushes_ = stream_failed_frames_ = 0;
  stream_presenter_fallbacks_ = stream_frame_mismatches_ = stream_source_mismatches_ = 0;
  stream_pumps_ = 0;
  stream_deferred_frontbuffer_resolves_ = 0;
  stream_handle_same_ = stream_handle_other_ = 0;
  for (auto& count : frontbuffer_new_generation_) count.store(0, std::memory_order_relaxed);
  texture_images_created_.store(0, std::memory_order_relaxed);
}

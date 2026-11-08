// Package 56: deferred capture, the GtaCapture thread (gta4_native_capture_thread).
// Design and reasons: native_capture_ring.h. Included by graphics_system.cpp inside
// namespace rex::graphics::gta4_native, after its file-local helpers.
//
// Who does what:
//   - SubmitTitleCommand, on any submitting thread (in practice RenderGuest):
//     DeferTitleCommand records the command into the ring. Not recordable (trace envelopes,
//     the transport profiler, malformed or oversized commands): everything recorded before is
//     captured first (DrainCaptureThread), then the command is captured on the caller as
//     before;
//   - GtaCapture (CaptureThreadMain): replays every record through CaptureTitleCommand, the
//     capture code of package 54 unchanged, with the device read from its own image;
//   - ExecuteTitleCommand (texture lock): drains first, so that the queue order stays
//     identical;
//   - the title's Lock and Release hooks: ExecuteTitleCommand(kCaptureBarrierAbi, resource)
//     waits until the last record that referenced the resource has been captured.
//
// The order of commands is the order of the ring, which is the order in which the producers
// took producer_mutex (before 56: command_capture_mutex_).

// Must match gta4_native_hooks.cpp. ExecuteTitleCommand(kTitleId, kCaptureBarrierAbi,
// &resource, 4, &state, 4); older renderers return false and leave state 0.
inline constexpr uint32_t kCaptureBarrierAbi = 0x43425231;  // 'CBR1'

namespace {

// Every N draws (gta4_native_capture_verify): what GtaCapture checks before it captures the
// draw.
struct CaptureVerifyObject {
  uint32_t handle = 0;
  uint32_t kind = 0;  // 1 vertex buffer of stream 0, 2 index buffer, 3 render target 0, 4 depth
  uint8_t words[48]{};  // guest object bytes 0..47 at submit
  uint64_t data_hash = 0;  // buffers up to 256 KB: XXH3 of the data at submit
  uint32_t data_size = 0;
  uint32_t reserved = 0;
};
struct CaptureVerifyBlock {
  uint8_t read_set[capture::kDeviceReadLines * capture::kLineBytes];
  CaptureVerifyObject objects[4];
};
constexpr uint32_t kCaptureVerifyMaxData = 256 * 1024;
constexpr const char* kCaptureVerifyKind[] = {"?", "vertex-buffer-0", "index-buffer",
                                              "render-target-0", "depth"};

uint32_t LoadCaptureWord(const uint8_t* bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return __builtin_bswap32(value);
}

// Words of the object the capture reads: buffers flags / address / size (CaptureBufferResource),
// surfaces flags / base / address / dimensions / format (CaptureSurfaceDescriptor).
bool CaptureVerifyWordRead(uint32_t kind, uint32_t word) {
  if (kind == 1 || kind == 2) return word == 0 || word == 6 || word == 7;
  return word == 0 || word == 6 || word == 7 || word == 9 || word == 10;
}

}  // namespace

struct Gta4NativeGraphicsSystem::CaptureThreadState {
  explicit CaptureThreadState(size_t ring_bytes)
      : ring(ring_bytes),
        producer_images(kGuestDeviceSize),
        consumer_images(kGuestDeviceSize) {}

  capture::Ring ring;
  capture::PendingRefs refs;

  // Producers (any submitting thread).
  std::mutex producer_mutex;
  capture::DeviceImages producer_images;  // producer_mutex: the device state last recorded
  uint64_t next_sequence = 0;             // producer_mutex
  uint32_t verify_interval = 0;
  uint32_t verify_counter = 0;            // producer_mutex
  std::atomic<uint64_t> last_sequence{0};
  // The last Present recorded: a submitter records the next one only after GtaCapture
  // captured it — at most one frame ahead of GtaCapture (GtaCapture itself at most
  // gta4_native_max_queued_frames ahead of recording).
  uint64_t last_present_sequence = 0;     // producer_mutex
  // 57: surface objects last recorded per slot (RT0..3, depth).
  capture::SurfaceWords producer_surfaces[capture::kSurfaceSlots]{};  // producer_mutex

  // GtaCapture.
  capture::DeviceImages consumer_images;
  capture::SurfaceWords consumer_surfaces[capture::kSurfaceSlots]{};
  uint32_t replay_clock = 0;
  std::thread thread;

  struct Counters {
    std::atomic<uint64_t> records{0}, bytes{0}, device_records{0}, device_lines{0};
    std::atomic<uint64_t> up_bytes{0}, frames{0};
    std::atomic<uint64_t> fallbacks{0}, drains{0}, drain_ns{0};
    std::atomic<uint64_t> present_waits{0}, present_wait_ns{0};
    // [0] Lock, [1] Release (57: only a Release that destroys the object asks)
    std::atomic<uint64_t> barrier_calls[2]{}, barrier_waits[2]{}, barrier_wait_ns[2]{};
    std::atomic<uint64_t> barrier_wait_max_ns{0};
    std::atomic<uint64_t> replay_ns{0}, replay_samples{0}, max_used{0}, surface_entries{0};
    std::atomic<uint64_t> verified{0}, verify_lines{0}, verify_objects{0}, verify_data{0};
    std::atomic<uint64_t> verify_surfaces{0}, verify_surface_live{0};
  } counters;
  struct Snapshot {
    uint64_t records = 0, bytes = 0, device_records = 0, device_lines = 0, up_bytes = 0;
    uint64_t frames = 0, fallbacks = 0, drains = 0, drain_ns = 0, present_waits = 0;
    uint64_t present_wait_ns = 0, barrier_calls[2]{}, barrier_waits[2]{}, barrier_wait_ns[2]{};
    uint64_t replay_ns = 0, replay_samples = 0, space_waits = 0, space_wait_ns = 0, sleeps = 0;
    uint64_t wakes = 0, notifies = 0, surface_entries = 0;
  } reported;
  std::chrono::steady_clock::time_point report_time = std::chrono::steady_clock::now();
  uint32_t verify_logged = 0;
};

void Gta4NativeGraphicsSystem::CaptureThreadStateDeleter::operator()(
    CaptureThreadState* state) const {
  delete state;
}

namespace {
void CaptureMax(std::atomic<uint64_t>& max, uint64_t value) {
  uint64_t current = max.load(std::memory_order_relaxed);
  while (value > current &&
         !max.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}
}  // namespace

const uint8_t* Gta4NativeGraphicsSystem::CaptureDeviceMemory(uint32_t device,
                                                             const CaptureReplay* replay) const {
  if (replay && replay->device_image && replay->device == device) return replay->device_image;
  return memory_->TranslateVirtual<const uint8_t*>(device);
}

void Gta4NativeGraphicsSystem::StartCaptureThread() {
  capture_thread_configured_ = REXCVAR_GET(gta4_native_capture_thread);
  if (!capture_thread_configured_ || capture_thread_) return;
  if (direct_record_) {  // 58: one thread does everything
    capture_thread_configured_ = false;
    REXLOG_WARN("gta4-native-capture: GtaCapture off: gta4_native_direct_record");
    return;
  }
  // These read the whole device at capture time (or compare it with guest memory); GtaCapture
  // has only the lines of kDeviceReadSet.
  const char* reason = nullptr;
  const std::string transport = REXCVAR_GET(gta4_native_state_transport);
  if (transport == "snapshot" || transport == "compare") {
    reason = "gta4_native_state_transport";
  } else if (REXCVAR_GET(gta4_validate_native_dirty_snapshots)) {
    reason = "gta4_validate_native_dirty_snapshots";
  } else if (CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTrace) ||
             CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeProbes) ||
             CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTranslucency) ||
             CachedDiagnosticsEnabled(rex::diagnostics::Category::kTransition)) {
    reason = "native trace / probes / translucency / transition diagnostics";
  }
  if (reason) {
    capture_thread_configured_ = false;
    REXLOG_WARN("gta4-native-capture: GtaCapture off: {} needs the live device", reason);
    return;
  }
  auto* state = new CaptureThreadState(size_t(REXCVAR_GET(gta4_native_capture_ring_kb)) * 1024);
  capture_thread_.reset(state);
  state->ring.set_wake_records(REXCVAR_GET(gta4_native_capture_wake_records));
  const uint64_t spin_ns = uint64_t(REXCVAR_GET(gta4_native_capture_spin_us)) * 1000;
  state->ring.set_spin_ns(spin_ns, spin_ns);
  state->verify_interval = REXCVAR_GET(gta4_native_capture_verify);
  state->thread = std::thread([this]() { CaptureThreadMain(); });
  capture_thread_active_.store(true, std::memory_order_release);
  REXLOG_INFO(
      "gta4-native-capture: GtaCapture on: ring {} KB (records up to {} KB), wake after {} "
      "records, spin {} us, verify every {} draws, zero-allocate {}",
      state->ring.capacity() / 1024, state->ring.max_record() / 1024,
      REXCVAR_GET(gta4_native_capture_wake_records), spin_ns / 1000, state->verify_interval,
      g_native_zero_allocate ? "on" : "off");
}

void Gta4NativeGraphicsSystem::StopCaptureThread() {
  if (!capture_thread_) return;
  CaptureThreadState& state = *capture_thread_;
  {
    // No record is being written while the ring stops; later submitters capture themselves.
    std::lock_guard lock(state.producer_mutex);
    capture_thread_active_.store(false, std::memory_order_release);
    state.ring.Stop();
  }
  if (state.thread.joinable()) state.thread.join();
  ReportCaptureThread(state);
}

void Gta4NativeGraphicsSystem::DrainCaptureThread() {
  if (!capture_thread_) return;
  CaptureThreadState& state = *capture_thread_;
  const uint64_t target = state.last_sequence.load(std::memory_order_acquire);
  if (state.ring.done_sequence() >= target) return;
  const uint64_t ns = state.ring.WaitDone(target);
  state.counters.drains.fetch_add(1, std::memory_order_relaxed);
  state.counters.drain_ns.fetch_add(ns, std::memory_order_relaxed);
}

int Gta4NativeGraphicsSystem::CaptureSynchronousFallback() {
  DrainCaptureThread();
  CaptureThreadState& state = *capture_thread_;
  {
    // 57: the hook consumed the draw's dirty words; the device state it captures here never
    // reaches the producer images, so the next record sends the whole read set again.
    std::lock_guard lock(state.producer_mutex);
    state.producer_images.InvalidateAll();
  }
  state.counters.fallbacks.fetch_add(1, std::memory_order_relaxed);
  return -1;
}

void Gta4NativeGraphicsSystem::CaptureBarrier(uint32_t resource, uint32_t kind) {
  CaptureThreadState& state = *capture_thread_;
  const uint32_t k = kind == 2 ? 1 : 0;
  state.counters.barrier_calls[k].fetch_add(1, std::memory_order_relaxed);
  const uint32_t referenced = state.refs.Get(resource);
  if (!referenced) return;
  const uint64_t done = state.ring.done_sequence();
  const int32_t ahead = int32_t(referenced - uint32_t(done));
  if (ahead <= 0) return;
  const uint64_t ns = state.ring.WaitDone(done + uint32_t(ahead));
  state.counters.barrier_waits[k].fetch_add(1, std::memory_order_relaxed);
  state.counters.barrier_wait_ns[k].fetch_add(ns, std::memory_order_relaxed);
  CaptureMax(state.counters.barrier_wait_max_ns, ns);
}

int Gta4NativeGraphicsSystem::DeferTitleCommand(uint32_t abi_version, const void* command,
                                                size_t command_size) {
  CaptureThreadState& state = *capture_thread_;
  const void* title_command = command;
  size_t title_size = command_size;
  uint32_t title_abi = abi_version;
  GpuPassOrigin origin{};
  bool has_origin = false;
  if (abi_version == kGpuPassEnvelopeAbi) {
    if (!UnpackGpuPassEnvelope(title_command, title_size, title_abi, origin)) return 0;
    has_origin = true;
  }
  // Trace envelopes and the transport profiler (it times the capture on the submitter).
  if (title_abi != kTitleCommandAbi || title_size < sizeof(CommandHeader) ||
      (CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeProfiler) &&
       (g_native_profile_transport_active.load(std::memory_order_acquire) ||
        g_native_profile_capture_requested.load(std::memory_order_acquire))))
    return CaptureSynchronousFallback();
  CommandHeader header{};
  std::memcpy(&header, title_command, sizeof(header));
  // A command the capture would reject: rejected there, as before.
  if (CommandSize(header.type) != title_size || header.size != title_size)
    return CaptureSynchronousFallback();
  const CommandType type = header.type;
  const bool draw = type == CommandType::kDrawPrimitive ||
                    type == CommandType::kDrawPrimitiveUp ||
                    type == CommandType::kDrawIndexedPrimitive;
  const bool device_command = draw || type == CommandType::kClear;
  uint32_t device = 0;
  const uint8_t* device_memory = nullptr;
  if (device_command) {
    device = CommandDevice(type, title_command);
    device_memory = device ? memory_->TranslateVirtual<const uint8_t*>(device) : nullptr;
    if (!device_memory) return CaptureSynchronousFallback();
  }
  const uint8_t* up_payload = nullptr;
  uint32_t up_size = 0;
  if (type == CommandType::kDrawPrimitiveUp) {
    DrawPrimitiveUpCommand draw_up;
    std::memcpy(&draw_up, title_command, sizeof(draw_up));
    if (draw_up.vertex_data_size) {
      up_payload = draw_up.vertex_data && draw_up.vertex_data_size <= kMaximumUpPayloadSize
                       ? memory_->TranslateVirtual<const uint8_t*>(draw_up.vertex_data)
                       : nullptr;
      if (!up_payload) return CaptureSynchronousFallback();
      up_size = draw_up.vertex_data_size;
    }
  }
  const bool present = type == CommandType::kPresent;
  if (present) {
    uint64_t previous;
    {
      std::lock_guard lock(state.producer_mutex);
      previous = state.last_present_sequence;
    }
    if (previous && state.ring.done_sequence() < previous) {
      const uint64_t ns = state.ring.WaitDone(previous);
      state.counters.present_waits.fetch_add(1, std::memory_order_relaxed);
      state.counters.present_wait_ns.fetch_add(ns, std::memory_order_relaxed);
    }
  }

  std::unique_lock lock(state.producer_mutex);
  const bool verify = draw && state.verify_interval &&
                      ++state.verify_counter >= state.verify_interval;
  if (verify) state.verify_counter = 0;
  const size_t verify_bytes = verify ? sizeof(CaptureVerifyBlock) : 0;
  const capture::RecordLayout reserve = capture::LayoutRecord(
      has_origin, title_size, up_size, device_command ? capture::kDeviceReadLines : 0,
      verify_bytes, device_command ? capture::kSurfaceSlots : 0);
  uint8_t* record = state.ring.Reserve(reserve.end);
  if (!record) {  // stopped, or larger than a quarter of the ring
    lock.unlock();
    return CaptureSynchronousFallback();
  }
  // The ring's old contents are dead: take the lines without reading them from memory (54).
  // Up to the first four device lines; more than that is rare.
  if (g_native_zero_allocate)
    ZeroAllocateLines(record, std::min(reserve.end, reserve.lines + 4 * capture::kLineBytes),
                      64);
  const uint64_t sequence = ++state.next_sequence;
  uint16_t line_index[capture::kDeviceReadLines];
  capture::SurfaceWords surfaces[capture::kSurfaceSlots];
  uint32_t lines = 0, surface_count = 0;
  if (device_command) {
    bool created = false;
    uint8_t* shadow = state.producer_images.Get(device, created);
    // 57: float constants only where the title's dirty words say (the hook consumed them for
    // this command); the rest of the read set always.
    const NativeDirtyState* dirty = CommandDirtyState(type, title_command);
    lines = capture::DiffDeviceReadSetDirty(device_memory, shadow, created,
                                            dirty ? dirty->words.data() : nullptr,
                                            record + reserve.lines, line_index);
    // 57: the surface objects the capture reads for this command, where they changed.
    for (uint32_t slot = 0; slot < capture::kSurfaceSlots; ++slot) {
      const uint32_t handle = LoadCaptureWord(
          device_memory, slot < 4 ? kRenderTargetOffset + slot * sizeof(uint32_t)
                                  : kDepthStencilOffset);
      capture::SurfaceWords now{};
      now.slot = slot;
      now.handle = handle;
      const uint8_t* object =
          handle ? memory_->TranslateVirtual<const uint8_t*>(handle) : nullptr;
      if (object) std::memcpy(now.words, object, sizeof(now.words));
      capture::SurfaceWords& last = state.producer_surfaces[slot];
      if (created || last.handle != now.handle ||
          std::memcmp(last.words, now.words, sizeof(now.words)) != 0) {
        last = now;
        surfaces[surface_count++] = now;
      }
    }
    if (draw) {
      // What the capture of this draw may dereference later: a Lock / Release of any of these
      // waits for this record (CaptureBarrier). 57: not the render targets and the depth
      // surface (words 1..5) — their objects travel in the record now.
      const uint8_t* words = device_memory + capture::kDeviceHandleWordsOffset;
      for (uint32_t i = 0; i < capture::kDeviceHandleWords; ++i) {
        if (i >= 1 && i <= 5) continue;
        uint32_t handle;
        std::memcpy(&handle, words + i * sizeof(uint32_t), sizeof(handle));
        if (handle) state.refs.Mark(__builtin_bswap32(handle), uint32_t(sequence));
      }
    }
  } else if (type == CommandType::kDeviceCreated || type == CommandType::kDeviceDestroyed) {
    state.producer_images.Invalidate(CommandDevice(type, title_command));
  }
  const capture::RecordLayout layout = capture::LayoutRecord(
      has_origin, title_size, up_size, lines, verify_bytes, surface_count);
  if (has_origin) std::memcpy(record + layout.origin, &origin, sizeof(origin));
  std::memcpy(record + layout.command, title_command, title_size);
  if (up_size) std::memcpy(record + layout.up, up_payload, up_size);
  if (lines) std::memcpy(record + layout.index, line_index, lines * sizeof(uint16_t));
  if (surface_count)
    std::memcpy(record + layout.surfaces, surfaces, surface_count * sizeof(surfaces[0]));
  if (verify) {
    auto& block = *reinterpret_cast<CaptureVerifyBlock*>(record + layout.verify);
    size_t at = 0;
    for (const auto& range : capture::kDeviceReadSet)
      for (uint32_t line = range.first; line < uint32_t(range.first + range.count); ++line)
        std::memcpy(block.read_set + (at++) * capture::kLineBytes,
                    device_memory + line * capture::kLineBytes, capture::kLineBytes);
    const uint32_t handles[4] = {LoadCaptureWord(device_memory, kStreamBufferBase),
                                 LoadCaptureWord(device_memory, kIndexBufferOffset),
                                 LoadCaptureWord(device_memory, kRenderTargetOffset),
                                 LoadCaptureWord(device_memory, kDepthStencilOffset)};
    for (uint32_t i = 0; i < 4; ++i) {
      CaptureVerifyObject& object = block.objects[i];
      object = {};
      const uint8_t* bytes =
          handles[i] ? memory_->TranslateVirtual<const uint8_t*>(handles[i]) : nullptr;
      if (!bytes) continue;
      object.handle = handles[i];
      object.kind = i + 1;
      std::memcpy(object.words, bytes, sizeof(object.words));
      if (i < 2) {
        const auto metadata = DecodeNativeBufferMetadata(
            LoadCaptureWord(bytes, 0), LoadCaptureWord(bytes, kResourceDataOffset),
            LoadCaptureWord(bytes, kResourceSizeOffset));
        const uint8_t* data =
            metadata && metadata->guest_size && metadata->guest_size <= kCaptureVerifyMaxData
                ? memory_->TranslateVirtual<const uint8_t*>(metadata->guest_address)
                : nullptr;
        if (data) {
          object.data_size = metadata->guest_size;
          object.data_hash = XXH3_64bits(data, object.data_size);
        }
      }
    }
  }
  auto& out = *reinterpret_cast<capture::RecordHeader*>(record);
  out.bytes = uint32_t(layout.end);
  out.kind = capture::RecordKind::kTitle;
  out.lines = uint16_t(lines);
  out.command_size = uint32_t(title_size);
  out.device = device_command ? device : 0;
  out.up_size = up_size;
  out.flags = (has_origin ? capture::kRecordOrigin : 0u) | (present ? capture::kRecordPresent : 0u) |
              (verify ? capture::kRecordVerify : 0u) |
              (surface_count << capture::kRecordSurfaceShift);
  out.sequence = sequence;
  if (present) state.last_present_sequence = sequence;
  const bool urgent = present || type == CommandType::kDeviceCreated ||
                      type == CommandType::kDeviceDestroyed ||
                      state.ring.used_bytes() > state.ring.capacity() / 2;
  state.ring.Commit(layout.end, urgent);
  state.last_sequence.store(sequence, std::memory_order_release);
  const size_t used = state.ring.used_bytes();
  lock.unlock();

  auto& c = state.counters;
  c.records.fetch_add(1, std::memory_order_relaxed);
  c.bytes.fetch_add(layout.end, std::memory_order_relaxed);
  if (device_command) {
    c.device_records.fetch_add(1, std::memory_order_relaxed);
    c.device_lines.fetch_add(lines, std::memory_order_relaxed);
  }
  if (up_size) c.up_bytes.fetch_add(up_size, std::memory_order_relaxed);
  if (surface_count) c.surface_entries.fetch_add(surface_count, std::memory_order_relaxed);
  CaptureMax(c.max_used, used);
  return 1;
}

void Gta4NativeGraphicsSystem::ReplayCaptureRecord(CaptureThreadState& state,
                                                   const capture::RecordHeader& header) {
  const auto* base = reinterpret_cast<const uint8_t*>(&header);
  const bool verify = header.flags & capture::kRecordVerify;
  const uint32_t surface_count = capture::RecordSurfaces(header.flags);
  const capture::RecordLayout layout = capture::LayoutRecord(
      header.flags & capture::kRecordOrigin, header.command_size, header.up_size, header.lines,
      verify ? sizeof(CaptureVerifyBlock) : 0, surface_count);
  CaptureReplay replay;
  if (header.flags & capture::kRecordOrigin)
    replay.origin = reinterpret_cast<const GpuPassOrigin*>(base + layout.origin);
  if (header.device) {
    bool created = false;
    uint8_t* image = state.consumer_images.Get(header.device, created);
    capture::ApplyDeviceLines(image, base + layout.lines,
                              reinterpret_cast<const uint16_t*>(base + layout.index),
                              header.lines);
    replay.device = header.device;
    replay.device_image = image;
    // 57: the surface objects as they were at submit (only changed slots are recorded).
    const auto* entries = reinterpret_cast<const capture::SurfaceWords*>(base + layout.surfaces);
    for (uint32_t i = 0; i < surface_count; ++i)
      if (entries[i].slot < capture::kSurfaceSlots) state.consumer_surfaces[entries[i].slot] = entries[i];
    replay.surfaces = state.consumer_surfaces;
  }
  if (header.up_size) {
    replay.up_payload = base + layout.up;
    replay.up_size = header.up_size;
  }
  if (verify && replay.device_image) {
    const auto& block = *reinterpret_cast<const CaptureVerifyBlock*>(base + layout.verify);
    auto& c = state.counters;
    c.verified.fetch_add(1, std::memory_order_relaxed);
    auto log = [&](const char* what, std::string details) {
      if (state.verify_logged >= 32) return;
      ++state.verify_logged;
      REXLOG_WARN("gta4-native-capture-verify: {} sequence={} pending-records-ahead={} {}",
                  what, header.sequence,
                  state.last_sequence.load(std::memory_order_relaxed) - header.sequence,
                  details);
    };
    size_t at = 0;
    for (const auto& range : capture::kDeviceReadSet) {
      for (uint32_t line = range.first; line < uint32_t(range.first + range.count); ++line) {
        if (std::memcmp(replay.device_image + line * capture::kLineBytes,
                        block.read_set + (at++) * capture::kLineBytes, capture::kLineBytes)) {
          c.verify_lines.fetch_add(1, std::memory_order_relaxed);
          log("device-image-differs", fmt::format("line={} offset={}", line,
                                                  line * capture::kLineBytes));
        }
      }
    }
    for (const CaptureVerifyObject& object : block.objects) {
      if (!object.handle) continue;
      const uint8_t* now = memory_->TranslateVirtual<const uint8_t*>(object.handle);
      if (!now) continue;
      // 57: surfaces are captured from the recorded words (CaptureSurfaceDescriptor with the
      // replay). Their check: recorded == submit (must hold); live changes only counted.
      const capture::SurfaceWords* recorded = nullptr;
      if (object.kind >= 3)
        for (uint32_t slot = 0; slot < capture::kSurfaceSlots; ++slot)
          if (state.consumer_surfaces[slot].handle == object.handle)
            recorded = &state.consumer_surfaces[slot];
      for (uint32_t word = 0; word < 12; ++word) {
        if (!CaptureVerifyWordRead(object.kind, word)) continue;
        const uint32_t then = LoadCaptureWord(object.words, word * 4);
        const uint32_t current = LoadCaptureWord(now, word * 4);
        if (object.kind >= 3) {
          if (word >= 11) continue;
          if (then != current) c.verify_surface_live.fetch_add(1, std::memory_order_relaxed);
          const uint32_t replayed =
              recorded ? __builtin_bswap32(recorded->words[word]) : ~then;
          if (replayed != then) {
            c.verify_surfaces.fetch_add(1, std::memory_order_relaxed);
            log("surface-snapshot-differs",
                fmt::format("kind={} handle={:08X} word={} submit={:08X} replayed={:08X}",
                            kCaptureVerifyKind[object.kind], object.handle, word, then,
                            replayed));
          }
          continue;
        }
        if (then != current) {
          c.verify_objects.fetch_add(1, std::memory_order_relaxed);
          log("object-changed-after-submit",
              fmt::format("kind={} handle={:08X} word={} submit={:08X} now={:08X}",
                          kCaptureVerifyKind[object.kind], object.handle, word, then, current));
        }
      }
      if (object.data_size) {
        const auto metadata = DecodeNativeBufferMetadata(
            LoadCaptureWord(object.words, 0), LoadCaptureWord(object.words, kResourceDataOffset),
            LoadCaptureWord(object.words, kResourceSizeOffset));
        const uint8_t* data =
            metadata ? memory_->TranslateVirtual<const uint8_t*>(metadata->guest_address)
                     : nullptr;
        if (data && XXH3_64bits(data, object.data_size) != object.data_hash) {
          c.verify_data.fetch_add(1, std::memory_order_relaxed);
          log("buffer-data-changed-after-submit",
              fmt::format("kind={} handle={:08X} bytes={}", kCaptureVerifyKind[object.kind],
                          object.handle, object.data_size));
        }
      }
    }
  }
  // 57: every 32nd record timed (batch 56: two clock reads per record were 3.5 ms per frame of
  // GtaCapture); the report scales the sample.
  if ((++state.replay_clock & 31) == 0) {
    const auto begin = std::chrono::steady_clock::now();
    CaptureTitleCommand(kTitleCommandAbi, base + layout.command, header.command_size, &replay);
    state.counters.replay_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - begin)
                     .count()),
        std::memory_order_relaxed);
    state.counters.replay_samples.fetch_add(1, std::memory_order_relaxed);
  } else {
    CaptureTitleCommand(kTitleCommandAbi, base + layout.command, header.command_size, &replay);
  }
  if (header.flags & capture::kRecordPresent) {
    // Pop (after this) wakes a submitter waiting for the Present slot (WaitDone).
    state.counters.frames.fetch_add(1, std::memory_order_relaxed);
    if (std::chrono::steady_clock::now() - state.report_time >= std::chrono::seconds(10))
      ReportCaptureThread(state);
  }
}

void Gta4NativeGraphicsSystem::CaptureThreadMain() {
#if REX_PLATFORM_ANDROID
  pthread_setname_np(pthread_self(), "GtaCapture");
#endif
  CaptureThreadState& state = *capture_thread_;
  for (;;) {
    const capture::RecordHeader* header = state.ring.Front();
    if (!header) {
      if (state.ring.stopped()) break;  // nothing is recorded after the stop
      state.ring.WaitForData(std::chrono::microseconds(1000));
      continue;
    }
    ReplayCaptureRecord(state, *header);
    state.ring.Pop(header);
  }
}

void Gta4NativeGraphicsSystem::ReportCaptureThread(CaptureThreadState& state) {
  const auto now = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(now - state.report_time).count();
  state.report_time = now;
  auto& c = state.counters;
  auto& r = state.reported;
  auto& ring = state.ring.stats();
  auto delta = [](uint64_t current, uint64_t& previous) {
    const uint64_t d = current - previous;
    previous = current;
    return d;
  };
  const uint64_t frames = std::max<uint64_t>(1, delta(c.frames.load(), r.frames));
  const uint64_t records = delta(c.records.load(), r.records);
  const uint64_t bytes = delta(c.bytes.load(), r.bytes);
  const uint64_t device_records = delta(c.device_records.load(), r.device_records);
  const uint64_t device_lines = delta(c.device_lines.load(), r.device_lines);
  const uint64_t surface_entries = delta(c.surface_entries.load(), r.surface_entries);
  const uint64_t up_bytes = delta(c.up_bytes.load(), r.up_bytes);
  const uint64_t fallbacks = delta(c.fallbacks.load(), r.fallbacks);
  const uint64_t drains = delta(c.drains.load(), r.drains);
  const uint64_t drain_ns = delta(c.drain_ns.load(), r.drain_ns);
  const uint64_t present_waits = delta(c.present_waits.load(), r.present_waits);
  const uint64_t present_wait_ns = delta(c.present_wait_ns.load(), r.present_wait_ns);
  uint64_t barrier_calls[2], barrier_waits[2], barrier_wait_ns[2];
  for (int k = 0; k < 2; ++k) {
    barrier_calls[k] = delta(c.barrier_calls[k].load(), r.barrier_calls[k]);
    barrier_waits[k] = delta(c.barrier_waits[k].load(), r.barrier_waits[k]);
    barrier_wait_ns[k] = delta(c.barrier_wait_ns[k].load(), r.barrier_wait_ns[k]);
  }
  const uint64_t replay_ns = delta(c.replay_ns.load(), r.replay_ns);
  const uint64_t replay_samples = delta(c.replay_samples.load(), r.replay_samples);
  const uint64_t space_waits = delta(ring.space_waits.load(), r.space_waits);
  const uint64_t space_wait_ns = delta(ring.space_wait_ns.load(), r.space_wait_ns);
  const uint64_t sleeps = delta(ring.consumer_sleeps.load(), r.sleeps);
  const uint64_t wakes = delta(ring.consumer_wakes.load(), r.wakes);
  const uint64_t notifies = delta(ring.notifies.load(), r.notifies);
  const double f = double(frames);
  // Capture time: the sampled records (every 32nd) scaled to all records.
  const double capture_ms =
      replay_samples ? double(replay_ns) / double(replay_samples) * double(records) / f / 1e6
                     : 0.0;
  REXLOG_INFO(
      "gta4-native-capture: {:.1f} s {} frames | per frame: records {:.0f} ({:.0f} B each), "
      "device lines {:.1f} and surfaces {:.2f} per draw/clear, UP {:.1f} KB, capture ~{:.2f} ms "
      "| submitter waits per frame: full ring {:.2f} ({:.3f} ms), Present slot {:.2f} ({:.3f} "
      "ms), drains {:.2f} ({:.3f} ms), captured on submitter {:.2f} | barriers per frame: Lock "
      "{:.1f} waited {:.2f} ({:.3f} ms), final Release {:.1f} waited {:.2f} ({:.3f} ms), max "
      "{:.0f} us | GtaCapture sleeps {:.1f} wakes {:.1f}, signals {:.1f} per frame | ring max "
      "{} KB of {} | verify {} draws: image {} buffer-object {} buffer-data {} surface-snapshot "
      "{} (surface changed live {})",
      seconds, frames, records / f, records ? double(bytes) / records : 0.0,
      device_records ? double(device_lines) / device_records : 0.0,
      device_records ? double(surface_entries) / device_records : 0.0, up_bytes / f / 1024.0,
      capture_ms, space_waits / f, space_wait_ns / f / 1e6, present_waits / f,
      present_wait_ns / f / 1e6, drains / f, drain_ns / f / 1e6, fallbacks / f,
      barrier_calls[0] / f, barrier_waits[0] / f, barrier_wait_ns[0] / f / 1e6,
      barrier_calls[1] / f, barrier_waits[1] / f, barrier_wait_ns[1] / f / 1e6,
      c.barrier_wait_max_ns.exchange(0) / 1e3, sleeps / f, wakes / f, notifies / f,
      c.max_used.exchange(0) / 1024, state.ring.capacity() / 1024, c.verified.load(),
      c.verify_lines.load(), c.verify_objects.load(), c.verify_data.load(),
      c.verify_surfaces.load(), c.verify_surface_live.load());
}

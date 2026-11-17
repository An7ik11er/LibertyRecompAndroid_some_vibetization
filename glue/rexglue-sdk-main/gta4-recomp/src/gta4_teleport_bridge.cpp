#include "gta4_teleport_bridge.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include "gta4_init.h"

namespace gta4::teleport {
namespace {

constexpr uint32_t kPrimaryPlayer = 0x82A98778;
constexpr uint32_t kPlayerInfoTable = 0x82C01C70;
constexpr uint32_t kPedOffset = 1400;
constexpr uint32_t kCamPtrAddr = 0x82BEF9B8u;

uint32_t s_frame_counter = 0;

std::atomic<bool> s_snap_active{false};
std::atomic<bool> s_lock_angle{false};
std::atomic<float> s_target_heading{kDefaultCamHeading};
std::atomic<float> s_target_pitch{kDefaultPitch};
std::atomic<bool> s_camera_settled{true};

enum class TeleportPhase {
  kIdle = 0,
  kWarpedWaiting2Sec,
  kSnappingCamera
};

TeleportPhase s_phase = TeleportPhase::kIdle;
int32_t s_phase_countdown = 0;

bool s_pending_ok = false;
float s_pending_x = kDefaultTestX;
float s_pending_y = kDefaultTestY;
float s_pending_z = kDefaultTestZ;
float s_pending_heading = kDefaultHeading;
float s_pending_cam_heading = kDefaultCamHeading;
float s_pending_pitch = kDefaultPitch;

inline bool GuestSpan(uint8_t* base, uint32_t address, uint32_t size) {
  return base && address >= 0x10000u && (uint64_t(address) + size) <= 0x100000000ull;
}

inline void StoreGuestF32(uint8_t* base, uint32_t address, float value) {
  uint32_t u;
  std::memcpy(&u, &value, sizeof(u));
  REX_STORE_U32(address, u);
}

}  // namespace

void RequestCameraHeading(float heading_degrees, float pitch_degrees, bool persistent_lock) {
  s_target_heading.store(heading_degrees, std::memory_order_relaxed);
  s_target_pitch.store(pitch_degrees, std::memory_order_relaxed);
  s_lock_angle.store(persistent_lock, std::memory_order_release);
  s_camera_settled.store(true, std::memory_order_release);
  s_snap_active.store(true, std::memory_order_release);
}

void UnlockCamera() {
  s_lock_angle.store(false, std::memory_order_release);
  s_snap_active.store(false, std::memory_order_release);
  REXLOG_INFO("gta4-teleport: camera angle unlocked");
}

bool IsCameraSettled() {
  return s_camera_settled.load(std::memory_order_acquire);
}

bool IsSnapActive() {
  return s_snap_active.load(std::memory_order_acquire) || s_lock_angle.load(std::memory_order_acquire);
}

void OnCameraUpdate(uint8_t* base, uint32_t camera, uint32_t pitch, uint32_t yaw,
                    uint32_t prev_pitch, uint32_t prev_yaw) {
  if (!base || !camera || !GuestSpan(base, camera, std::max(yaw, pitch) + 16)) return;

  if (s_snap_active.load(std::memory_order_relaxed) || s_lock_angle.load(std::memory_order_relaxed)) {
    const float heading_rad = s_target_heading.load(std::memory_order_relaxed) * (3.14159265358979323846f / 180.0f);
    const float pitch_rad = s_target_pitch.load(std::memory_order_relaxed) * (3.14159265358979323846f / 180.0f);

    StoreGuestF32(base, camera + yaw, heading_rad);
    StoreGuestF32(base, camera + prev_yaw, heading_rad);
    StoreGuestF32(base, camera + pitch, pitch_rad);
    StoreGuestF32(base, camera + prev_pitch, pitch_rad);

    // Zero out angular velocities (at yaw + 4 and pitch + 4)
    StoreGuestF32(base, camera + yaw + 4, 0.0f);
    StoreGuestF32(base, camera + pitch + 4, 0.0f);
  }
}

bool TeleportPlayer(PPCContext& ctx, uint8_t* base, float x, float y, float z,
                    float heading, float cam_heading, float pitch, bool snap_camera) {
  if (!base) return false;

  const uint32_t index = REX_LOAD_U32(kPrimaryPlayer);
  if (index >= 4) {
    REXLOG_WARN("gta4-teleport: invalid primary player index {}", index);
    return false;
  }

  const uint32_t info = REX_LOAD_U32(kPlayerInfoTable + index * 4);
  if (!info || !GuestSpan(base, info, 1404)) {
    REXLOG_WARN("gta4-teleport: null player info pointer");
    return false;
  }

  const uint32_t ped = REX_LOAD_U32(info + kPedOffset);
  if (!ped || !GuestSpan(base, ped, 2692)) {
    REXLOG_WARN("gta4-teleport: null player ped pointer");
    return false;
  }

  const bool in_vehicle = (REX_LOAD_U32(ped + 572) & 0x20000000u) != 0;
  REXLOG_INFO("gta4-teleport: requesting teleport to ({:.3f}, {:.3f}, {:.3f}) ped_heading={:.2f} cam_heading={:.2f} pitch={:.2f} [ped={:08X}, in_veh={}]",
              x, y, z, heading, cam_heading, pitch, ped, in_vehicle);

  // 1. Request collision streaming at target location so physics does not fall through ground
  {
    PPCContext call = ctx;
    call.fpscr.disableFlushMode();
    call.f1.f64 = double(x);
    call.f2.f64 = double(y);
    call.f3.f64 = double(z);
    sub_825865E0(call, base);
  }

  // 2. Teleport ped (if in vehicle, sub_822028C0 automatically teleports the vehicle via sub_823AA240)
  {
    PPCContext call = ctx;
    call.fpscr.disableFlushMode();
    call.r3.u64 = ped;
    call.f1.f64 = double(x);
    call.f2.f64 = double(y);
    call.f3.f64 = double(z);
    call.r7.u64 = 1;  // warp physics instance & update bounds
    call.r8.u64 = 1;  // reset linear & angular velocity
    call.r9.u64 = 0;
    sub_822028C0(call, base);
  }

  // 3. Set orientation matrix directly on ped (and vehicle if applicable)
  const uint32_t matrix = REX_LOAD_U32(ped + 32);
  if (matrix && GuestSpan(base, matrix, 64)) {
    const float rad = heading * (3.14159265358979323846f / 180.0f);
    const float cos_h = std::cos(rad);
    const float sin_h = std::sin(rad);

    // Row 0 (Right vector)
    StoreGuestF32(base, matrix + 0, cos_h);
    StoreGuestF32(base, matrix + 4, sin_h);
    StoreGuestF32(base, matrix + 8, 0.0f);
    StoreGuestF32(base, matrix + 12, 0.0f);

    // Row 1 (Forward vector)
    StoreGuestF32(base, matrix + 16, -sin_h);
    StoreGuestF32(base, matrix + 20, cos_h);
    StoreGuestF32(base, matrix + 24, 0.0f);
    StoreGuestF32(base, matrix + 28, 0.0f);

    // Row 2 (Up vector)
    StoreGuestF32(base, matrix + 32, 0.0f);
    StoreGuestF32(base, matrix + 36, 0.0f);
    StoreGuestF32(base, matrix + 40, 1.0f);
    StoreGuestF32(base, matrix + 44, 0.0f);

    // Row 3 (Position vector & heading scalar)
    StoreGuestF32(base, matrix + 48, x);
    StoreGuestF32(base, matrix + 52, y);
    StoreGuestF32(base, matrix + 56, z);
    StoreGuestF32(base, matrix + 60, -rad);
  }

  // 4. Initial camera hint
  if (snap_camera) {
    const uint32_t cam = REX_LOAD_U32(kCamPtrAddr);
    if (cam && GuestSpan(base, cam, 300)) {
      // Camera Position (Offset 80 / 0x50)
      StoreGuestF32(base, cam + 80, kDefaultCamX);
      StoreGuestF32(base, cam + 84, kDefaultCamY);
      StoreGuestF32(base, cam + 88, kDefaultCamZ);

      // Camera Right (Offset 240 / 0xF0)
      StoreGuestF32(base, cam + 240, kDefaultCamRightX);
      StoreGuestF32(base, cam + 244, kDefaultCamRightY);
      StoreGuestF32(base, cam + 248, 0.0f);

      // Camera Up (Offset 256 / 0x100)
      StoreGuestF32(base, cam + 256, kDefaultCamUpX);
      StoreGuestF32(base, cam + 260, kDefaultCamUpY);
      StoreGuestF32(base, cam + 264, kDefaultCamUpZ);

      // Camera Look / Forward (Offset 272 / 0x110)
      StoreGuestF32(base, cam + 272, kDefaultCamLookX);
      StoreGuestF32(base, cam + 276, kDefaultCamLookY);
      StoreGuestF32(base, cam + 280, kDefaultCamLookZ);
    }
  }

  REXLOG_INFO("gta4-teleport: player successfully warped to ({:.3f}, {:.3f}, {:.3f}) heading={:.2f}",
              x, y, z, heading);
  return true;
}

void PollTeleportBridge(PPCContext& ctx, uint8_t* base) {
  const std::filesystem::path folder = rex::filesystem::GetUserFolder();

  // Handle 2-second settle state machine:
  if (s_phase == TeleportPhase::kWarpedWaiting2Sec) {
    if (--s_phase_countdown <= 0) {
      REXLOG_INFO("gta4-teleport: 2.0s elapsed after teleport, now executing secondary camera rotation...");
      RequestCameraHeading(s_pending_cam_heading, s_pending_pitch);
      s_phase = TeleportPhase::kSnappingCamera;
      s_phase_countdown = 60;  // max timeout for camera settle
    }
    return;
  } else if (s_phase == TeleportPhase::kSnappingCamera) {
    if (IsCameraSettled() || --s_phase_countdown <= 0) {
      s_phase = TeleportPhase::kIdle;
      const std::filesystem::path done_file = folder / "teleport.done";
      std::ofstream out(done_file, std::ios::trunc);
      if (out.is_open()) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        out << (s_pending_ok ? "OK" : "ERROR") << " " << ms << " " << s_pending_x << " " << s_pending_y
            << " " << s_pending_z << " " << s_pending_heading << "\n";
      }
      REXLOG_INFO("gta4-teleport: completed 2-second settle handshake written to teleport.done");
    }
    return;
  }

  // Throttle filesystem check to once every 5 frames (~80ms @ 60 FPS)
  if ((++s_frame_counter % 5) != 0) {
    return;
  }

  std::error_code ec;
  const std::filesystem::path cmd_file = folder / "teleport.cmd";

  if (!std::filesystem::exists(cmd_file, ec) || ec) {
    return;
  }

  std::ifstream f(cmd_file);
  if (!f.is_open()) {
    return;
  }

  std::string line;
  std::getline(f, line);
  f.close();

  // Clean trailing whitespace / newlines
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
    line.pop_back();
  }

  float target_x = kDefaultTestX;
  float target_y = kDefaultTestY;
  float target_z = kDefaultTestZ;
  float target_heading = kDefaultHeading;
  float target_cam_heading = kDefaultCamHeading;
  float target_pitch = kDefaultPitch;
  bool snap_camera = true;

  if (line == "unlock") {
    UnlockCamera();
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "lock") {
    RequestCameraHeading(s_target_heading.load(), s_target_pitch.load(), true);
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "higher" || line == "up") {
    RequestCameraHeading(s_target_heading.load(), s_target_pitch.load() + 1.0f);
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "lower" || line == "down") {
    RequestCameraHeading(s_target_heading.load(), s_target_pitch.load() - 1.0f);
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "left") {
    RequestCameraHeading(s_target_heading.load() - 2.0f, s_target_pitch.load());
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "right") {
    RequestCameraHeading(s_target_heading.load() + 2.0f, s_target_pitch.load());
    std::filesystem::remove(cmd_file, ec);
    return;
  }

  if (line != "test_spot" && line != "test" && line != "default" && !line.empty()) {
    // Replace commas with spaces for easy parsing
    for (char& c : line) {
      if (c == ',') c = ' ';
    }
    std::stringstream ss(line);
    float px = 0.0f, py = 0.0f, pz = 0.0f, ph = 0.0f, pch = 0.0f, pp = 0.0f;
    if (ss >> px >> py >> pz) {
      target_x = px;
      target_y = py;
      target_z = pz;
      if (ss >> ph) {
        target_heading = ph;
      }
      if (ss >> pch) {
        target_cam_heading = pch;
      }
      if (ss >> pp) {
        target_pitch = pp;
      }
    }
  }

  const bool ok = TeleportPlayer(ctx, base, target_x, target_y, target_z, target_heading, target_cam_heading, target_pitch, snap_camera);

  // Remove command file
  std::filesystem::remove(cmd_file, ec);

  // Arm 2.0-second (~120 frames) settle countdown before secondary camera snap
  s_phase = TeleportPhase::kWarpedWaiting2Sec;
  s_phase_countdown = 120;
  s_pending_ok = ok;
  s_pending_x = target_x;
  s_pending_y = target_y;
  s_pending_z = target_z;
  s_pending_heading = target_heading;
  s_pending_cam_heading = target_cam_heading;
  s_pending_pitch = target_pitch;
}

}  // namespace gta4::teleport

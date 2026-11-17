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
constexpr uint32_t kCamCountAddr = 0x82BEF9B0u;
constexpr uint32_t kCamArrayAddr = 0x82BEF9B8u;

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

uint32_t FindActive3DCamera(uint8_t* base) {
  if (!base) return 0;
  const uint32_t count = REX_LOAD_U32(kCamCountAddr);
  uint32_t best_cam = 0;
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    const uint32_t c = REX_LOAD_U32(kCamArrayAddr + i * 4);
    if (c && GuestSpan(base, c, 300)) {
      float pz = 0.0f, lz = 0.0f;
      const uint32_t u_pz = REX_LOAD_U32(c + 88);
      const uint32_t u_lz = REX_LOAD_U32(c + 280);
      std::memcpy(&pz, &u_pz, 4);
      std::memcpy(&lz, &u_lz, 4);
      if (pz > 0.0f && std::abs(lz) < 0.99f) {
        best_cam = c;
      }
    }
  }
  return best_cam;
}

void ApplyCameraVectors(uint8_t* base, uint32_t cam, float yaw_deg, float pitch_deg) {
  if (!base || !cam || !GuestSpan(base, cam, 300)) return;

  const float rad_y = yaw_deg * (3.14159265358979323846f / 180.0f);
  const float rad_p = pitch_deg * (3.14159265358979323846f / 180.0f);
  const float cos_y = std::cos(rad_y);
  const float sin_y = std::sin(rad_y);
  const float cos_p = std::cos(rad_p);
  const float sin_p = std::sin(rad_p);

  // Look vector (+272): Lx = -sin(yaw)*cos(pitch), Ly = cos(yaw)*cos(pitch), Lz = sin(pitch)
  StoreGuestF32(base, cam + 272, -sin_y * cos_p);
  StoreGuestF32(base, cam + 276,  cos_y * cos_p);
  StoreGuestF32(base, cam + 280,  sin_p);

  // Right vector (+240): Rx = cos(yaw), Ry = sin(yaw), Rz = 0
  StoreGuestF32(base, cam + 240,  cos_y);
  StoreGuestF32(base, cam + 244,  sin_y);
  StoreGuestF32(base, cam + 248,  0.0f);

  // Up vector (+256): Right x Look
  StoreGuestF32(base, cam + 256,  sin_y * sin_p);
  StoreGuestF32(base, cam + 260, -cos_y * sin_p);
  StoreGuestF32(base, cam + 264,  cos_p);
}

void ReadCameraAngles(uint8_t* base, uint32_t cam, float& out_yaw_deg, float& out_pitch_deg) {
  if (!base || !cam || !GuestSpan(base, cam, 300)) return;
  float lx = 0.0f, ly = 0.0f, lz = 0.0f;
  const uint32_t u_lx = REX_LOAD_U32(cam + 272);
  const uint32_t u_ly = REX_LOAD_U32(cam + 276);
  const uint32_t u_lz = REX_LOAD_U32(cam + 280);
  std::memcpy(&lx, &u_lx, 4);
  std::memcpy(&ly, &u_ly, 4);
  std::memcpy(&lz, &u_lz, 4);

  if (lx != 0.0f || ly != 0.0f) {
    float yaw = std::atan2(-lx, ly) * (180.0f / 3.14159265358979323846f);
    while (yaw < 0.0f) yaw += 360.0f;
    while (yaw >= 360.0f) yaw -= 360.0f;
    out_yaw_deg = yaw;

    const float clamped_lz = std::max(-1.0f, std::min(1.0f, lz));
    out_pitch_deg = std::asin(clamped_lz) * (180.0f / 3.14159265358979323846f);
  }
}

}  // namespace

void RequestCameraHeading(float heading_degrees, float pitch_degrees, bool persistent_lock) {
  while (heading_degrees < 0.0f) heading_degrees += 360.0f;
  while (heading_degrees >= 360.0f) heading_degrees -= 360.0f;
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
    float h_deg = s_target_heading.load(std::memory_order_relaxed);
    while (h_deg > 180.0f) h_deg -= 360.0f;
    while (h_deg < -180.0f) h_deg += 360.0f;
    const float heading_rad = h_deg * (3.14159265358979323846f / 180.0f);

    float p_deg = s_target_pitch.load(std::memory_order_relaxed);
    while (p_deg > 180.0f) p_deg -= 360.0f;
    while (p_deg < -180.0f) p_deg += 360.0f;
    const float pitch_rad = p_deg * (3.14159265358979323846f / 180.0f);

    StoreGuestF32(base, camera + yaw, heading_rad);
    StoreGuestF32(base, camera + prev_yaw, heading_rad);
    StoreGuestF32(base, camera + pitch, pitch_rad);
    StoreGuestF32(base, camera + prev_pitch, pitch_rad);

    // Zero out velocity components without corrupting yaw (pitch velocity at pitch + 12, yaw at yaw + 4)
    StoreGuestF32(base, camera + yaw + 4, 0.0f);
    StoreGuestF32(base, camera + pitch + 12, 0.0f);
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

  // 4. Initial camera update directly on active 3D camera
  if (snap_camera) {
    RequestCameraHeading(cam_heading, pitch, true);
    const uint32_t active_cam = FindActive3DCamera(base);
    if (active_cam) {
      ApplyCameraVectors(base, active_cam, cam_heading, pitch);
    }
  }

  REXLOG_INFO("gta4-teleport: player successfully warped to ({:.3f}, {:.3f}, {:.3f}) heading={:.2f}",
              x, y, z, heading);
  return true;
}

void PollTeleportBridge(PPCContext& ctx, uint8_t* base) {
  // Apply locked camera vectors directly every frame to active 3D camera
  const uint32_t active_cam = FindActive3DCamera(base);
  if (active_cam) {
    if (s_lock_angle.load(std::memory_order_relaxed) || s_snap_active.load(std::memory_order_relaxed)) {
      ApplyCameraVectors(base, active_cam, s_target_heading.load(std::memory_order_relaxed), s_target_pitch.load(std::memory_order_relaxed));
    } else {
      float live_yaw = 0.0f, live_pitch = 0.0f;
      ReadCameraAngles(base, active_cam, live_yaw, live_pitch);
      if (live_yaw != 0.0f || live_pitch != 0.0f) {
        s_target_heading.store(live_yaw, std::memory_order_relaxed);
        s_target_pitch.store(live_pitch, std::memory_order_relaxed);
      }
    }
  }

  const std::filesystem::path folder = rex::filesystem::GetUserFolder();

  // Handle settle state machine:
  if (s_phase == TeleportPhase::kWarpedWaiting2Sec) {
    if (--s_phase_countdown <= 0) {
      REXLOG_INFO("gta4-teleport: elapsed after teleport, snapping camera vectors...");
      RequestCameraHeading(s_pending_cam_heading, s_pending_pitch, true);
      if (active_cam) {
        ApplyCameraVectors(base, active_cam, s_pending_cam_heading, s_pending_pitch);
      }
      s_phase = TeleportPhase::kSnappingCamera;
      s_phase_countdown = 20;
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
      REXLOG_INFO("gta4-teleport: completed handshake written to teleport.done");
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
    if (active_cam) {
      ApplyCameraVectors(base, active_cam, s_target_heading.load(), s_target_pitch.load());
    }
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "higher" || line == "up") {
    RequestCameraHeading(s_target_heading.load(), s_target_pitch.load() + 1.0f, true);
    if (active_cam) {
      ApplyCameraVectors(base, active_cam, s_target_heading.load(), s_target_pitch.load());
    }
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "lower" || line == "down") {
    RequestCameraHeading(s_target_heading.load(), s_target_pitch.load() - 1.0f, true);
    if (active_cam) {
      ApplyCameraVectors(base, active_cam, s_target_heading.load(), s_target_pitch.load());
    }
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "left") {
    RequestCameraHeading(s_target_heading.load() - 2.0f, s_target_pitch.load(), true);
    if (active_cam) {
      ApplyCameraVectors(base, active_cam, s_target_heading.load(), s_target_pitch.load());
    }
    std::filesystem::remove(cmd_file, ec);
    return;
  } else if (line == "right") {
    RequestCameraHeading(s_target_heading.load() + 2.0f, s_target_pitch.load(), true);
    if (active_cam) {
      ApplyCameraVectors(base, active_cam, s_target_heading.load(), s_target_pitch.load());
    }
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

  // Arm brief settle countdown before confirming
  s_phase = TeleportPhase::kWarpedWaiting2Sec;
  s_phase_countdown = 30;
  s_pending_ok = ok;
  s_pending_x = target_x;
  s_pending_y = target_y;
  s_pending_z = target_z;
  s_pending_heading = target_heading;
  s_pending_cam_heading = target_cam_heading;
  s_pending_pitch = target_pitch;
}

}  // namespace gta4::teleport

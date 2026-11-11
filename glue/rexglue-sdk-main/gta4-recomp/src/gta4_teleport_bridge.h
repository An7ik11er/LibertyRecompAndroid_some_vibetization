#pragma once

#include <cstdint>

struct PPCContext;

namespace gta4::teleport {

// Default target coordinates & orientation captured live from Hove Beach benchmark spot:
// Default target coordinates & orientation captured live from Hove Beach benchmark spot:
constexpr float kDefaultTestX = 903.8455f;
constexpr float kDefaultTestY = -499.9254f;
constexpr float kDefaultTestZ = 15.0528f;
constexpr float kDefaultHeading = 187.95f;
constexpr float kDefaultCamHeading = 11.58f;
constexpr float kDefaultPitch = -6.85f;

// Camera position & orientation looking at the benchmark scene:
constexpr float kDefaultCamX = 904.7194f;
constexpr float kDefaultCamY = -504.1902f;
constexpr float kDefaultCamZ = 15.5758f;
constexpr float kDefaultCamLookX = 0.4251f;
constexpr float kDefaultCamLookY = -0.3096f;
constexpr float kDefaultCamLookZ = 0.8506f;
constexpr float kDefaultCamRightX = 0.5884f;
constexpr float kDefaultCamRightY = 0.8086f;
constexpr float kDefaultCamUpX = -0.6876f;
constexpr float kDefaultCamUpY = 0.5005f;
constexpr float kDefaultCamUpZ = 0.5260f;

// Call once per frame on Main XThread (e.g. from sub_828CCD60 in gta4_input_hooks.cpp)
void PollTeleportBridge(PPCContext& ctx, uint8_t* base);

// Direct C++ invocation
bool TeleportPlayer(PPCContext& ctx, uint8_t* base, float x, float y, float z,
                    float heading = kDefaultHeading, float cam_heading = kDefaultCamHeading,
                    float pitch = kDefaultPitch, bool snap_camera = true);

// Camera heading & pitch control
void RequestCameraHeading(float heading_degrees, float pitch_degrees = kDefaultPitch, bool persistent_lock = true);
void UnlockCamera();
void OnCameraUpdate(uint8_t* base, uint32_t camera, uint32_t pitch, uint32_t yaw,
                    uint32_t prev_pitch, uint32_t prev_yaw);
bool IsCameraSettled();
bool IsSnapActive();

}  // namespace gta4::teleport

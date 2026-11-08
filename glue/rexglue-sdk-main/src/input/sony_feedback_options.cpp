#include <rex/input/sony_feedback.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gta4_sony_enabled, true, "GTA IV/Sony Controller", "Enable supported Sony controller features");
REXCVAR_DEFINE_BOOL(gta4_sony_gestures, true, "GTA IV/Sony Controller", "Enable controller touchpad gestures");
REXCVAR_DEFINE_BOOL(gta4_sony_lighting, true, "GTA IV/Sony Controller", "Show episode and wanted state on the controller light");
REXCVAR_DEFINE_BOOL(gta4_sony_rumble, true, "GTA IV/Sony Controller", "Enhance confirmed shot, damage and explosion rumble");
REXCVAR_DEFINE_BOOL(gta4_sony_triggers, true, "GTA IV/Sony Controller", "Enable supported DualSense adaptive triggers");
REXCVAR_DEFINE_DOUBLE(gta4_sony_intensity, 0.5, "GTA IV/Sony Controller", "Strength of added controller feedback").range(0.0, 1.0);

namespace rex::input::sony {
bool SetFeaturesEnabled(bool enabled) {
  // Hold the master off while changing the child switches. These are registered
  // in this translation unit, and a failed write leaves the bundle disabled.
  if (!rex::cvar::SetFlagByName("gta4_sony_enabled", "false")) return false;
  const char* value = enabled ? "true" : "false";
  for (const char* flag : {"gta4_sony_gestures", "gta4_sony_lighting",
                           "gta4_sony_rumble", "gta4_sony_triggers"}) {
    if (!rex::cvar::SetFlagByName(flag, value)) return false;
  }
  return rex::cvar::SetFlagByName("gta4_sony_enabled", value);
}

Options GetOptions() {
  // Direct REXCVAR_GET avoids Query / GetFlagByName string allocations and mutex locks.
  return {REXCVAR_GET(gta4_sony_enabled),
          REXCVAR_GET(gta4_sony_gestures),
          REXCVAR_GET(gta4_sony_lighting),
          REXCVAR_GET(gta4_sony_rumble),
          REXCVAR_GET(gta4_sony_triggers),
          static_cast<float>(REXCVAR_GET(gta4_sony_intensity))};
}
}  // namespace rex::input::sony

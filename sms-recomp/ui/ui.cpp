#include "ui.h"

#include "settings_menu.h"

#include <RmlUi/Core.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/rmlui.hpp>
#include <lucent/log.h>

#include <cstdlib>

namespace sb::ui {
namespace {

bool env_enabled(const char *name) noexcept {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool quit_requested() noexcept {
  const AuroraEvent *event = aurora_update();
  while (event != nullptr && event->type != AURORA_NONE) {
    if (event->type == AURORA_EXIT)
      return true;
    ++event;
  }
  return false;
}

bool prepare_rmlui() {
  if (!aurora::rmlui::is_initialized()) {
    lucent::error(
        "ui",
        "RmlUi did not initialize; the settings screen cannot be displayed");
    return false;
  }
  if (!Rml::LoadFontFace("res/FiraSans-Regular.ttf", true)) {
    lucent::error("ui", "failed to load res/FiraSans-Regular.ttf");
    return false;
  }
  return true;
}

} // namespace

bool run_prelaunch() {
  if (env_enabled("SB_HEADLESS") || env_enabled("SBR_SKIP_PRELAUNCH"))
    return true;
  if (!prepare_rmlui())
    return false;

  SettingsMenu menu;
  if (!menu.valid()) {
    lucent::error("ui", "failed to create the Sunbright settings document");
    return false;
  }
  menu.show();

  bool frameActive = aurora_begin_frame();
  while (!menu.launch_requested()) {
    if (frameActive)
      aurora_end_frame();
    if (quit_requested())
      return false;
    frameActive = aurora_begin_frame();
  }
  if (frameActive)
    aurora_end_frame();
  return true;
}

bool render_settings_control(unsigned frames) {
  if (frames == 0 || !prepare_rmlui())
    return false;
  SettingsMenu menu;
  if (!menu.valid()) {
    lucent::error("ui",
                  "failed to create the Sunbright settings control document");
    return false;
  }
  menu.show();
  for (unsigned frame = 0; frame < frames; ++frame) {
    if (!aurora_begin_frame()) {
      lucent::error("ui", "Aurora refused settings control frame {} of {}",
                    frame + 1, frames);
      return false;
    }
    aurora_end_frame();
    if (frame == 0 && !menu.layout_valid())
      return false;
    if (quit_requested())
      return false;
  }
  lucent::info("ui", "settings control rendered {} frame(s)", frames);
  return true;
}

} // namespace sb::ui

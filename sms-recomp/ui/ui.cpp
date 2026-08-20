#include "ui.h"

#include "runtime.h"
#include "settings_menu.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
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

bool push_escape() {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.type = SDL_EVENT_KEY_DOWN;
  event.key.scancode = SDL_SCANCODE_ESCAPE;
  event.key.key = SDLK_ESCAPE;
  event.key.down = true;
  if (SDL_PushEvent(&event))
    return true;
  lucent::error("ui", "SDL_PushEvent(Escape) failed: {}", SDL_GetError());
  return false;
}

bool never_quit() { return false; }

} // namespace

bool run_prelaunch() {
  if (env_enabled("SB_HEADLESS") || env_enabled("SBR_SKIP_PRELAUNCH"))
    return runtime().initialize();
  if (!runtime().initialize())
    return false;

  SettingsMenu menu(true);
  if (!menu.valid()) {
    lucent::error("ui", "failed to create the prelaunch settings window");
    return false;
  }
  menu.show();

  bool frameActive = aurora_begin_frame();
  while (!menu.launch_requested()) {
    if (frameActive)
      aurora_end_frame();
    else
      aurora_discard_frame();
    if (quit_requested())
      return false;
    frameActive = aurora_begin_frame();
  }
  if (frameActive)
    aurora_end_frame();
  else
    aurora_discard_frame();
  return true;
}

bool run_escape_control(unsigned frames) {
  if (frames == 0 || !runtime().initialize() || !push_escape())
    return false;
  if (runtime().handle_events(aurora_update()) || !runtime().visible()) {
    lucent::error("ui", "Escape did not open the settings window");
    return false;
  }
  for (unsigned frame = 0; frame < frames; ++frame) {
    if (!aurora_begin_frame()) {
      lucent::error("ui", "Aurora refused UI control frame {} of {}", frame + 1,
                    frames);
      return false;
    }
    aurora_end_frame();
    if (frame == 0 && !runtime().layout_valid())
      return false;
    if (runtime().handle_events(aurora_update()))
      return false;
  }
  if (!push_escape())
    return false;
  bool frameActive = aurora_begin_frame();
  if (!runtime().pause_while_open(frameActive, never_quit, nullptr) ||
      runtime().visible()) {
    lucent::error("ui", "second Escape did not close the settings window");
    return false;
  }
  if (frameActive)
    aurora_end_frame();
  else
    aurora_discard_frame();
  lucent::info(
      "ui", "Escape opened, rendered, and closed settings across {} frame(s)",
      frames);
  return true;
}

} // namespace sb::ui

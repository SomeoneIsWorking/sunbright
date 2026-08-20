#include "runtime.h"

#include "settings_menu.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/rmlui.hpp>
#include <lucent/log.h>

namespace sb::ui {
namespace {

bool load_font(const char *path, bool fallback = false) {
  if (Rml::LoadFontFace(path, fallback))
    return true;
  lucent::error("ui", "failed to load {}", path);
  return false;
}

bool is_escape_down(const AuroraEvent &event) noexcept {
  return event.type == AURORA_SDL_EVENT &&
         event.sdl.type == SDL_EVENT_KEY_DOWN && !event.sdl.key.repeat &&
         event.sdl.key.scancode == SDL_SCANCODE_ESCAPE;
}

} // namespace

Runtime::~Runtime() = default;

bool Runtime::initialize() {
  if (m_menu)
    return true;
  if (!aurora::rmlui::is_initialized()) {
    lucent::error("ui", "RmlUi did not initialize");
    return false;
  }
  if (!load_font("res/FiraSans-Regular.ttf", true) ||
      !load_font("res/FiraSans-Bold.ttf") ||
      !load_font("res/FiraSansCondensed-Regular.ttf") ||
      !load_font("res/FiraSansCondensed-Bold.ttf"))
    return false;
  m_menu = std::make_unique<SettingsMenu>();
  if (!m_menu->valid()) {
    lucent::error("ui", "failed to create the settings window");
    m_menu.reset();
    return false;
  }
  return true;
}

void Runtime::shutdown() { m_menu.reset(); }

bool Runtime::handle_events(const AuroraEvent *events) {
  bool exitRequested = false;
  for (const AuroraEvent *event = events;
       event != nullptr && event->type != AURORA_NONE; ++event) {
    exitRequested |= event->type == AURORA_EXIT;
    if (is_escape_down(*event))
      toggle();
  }
  return exitRequested;
}

bool Runtime::pause_while_open(bool &frameActive,
                               RuntimePredicate quitRequested,
                               RuntimeCallback beforePresent) {
  while (visible()) {
    if (beforePresent != nullptr)
      beforePresent();
    if (frameActive)
      aurora_end_frame();
    else
      aurora_discard_frame();
    const bool exitRequested = handle_events(aurora_update());
    if (exitRequested || (quitRequested != nullptr && quitRequested()))
      return false;
    frameActive = aurora_begin_frame();
  }
  return true;
}

void Runtime::toggle() {
  if (auto *settings = menu(); settings != nullptr) {
    if (settings->visible())
      settings->hide();
    else
      settings->show();
  }
}

bool Runtime::visible() const noexcept { return m_menu && m_menu->visible(); }

bool Runtime::layout_valid() const { return m_menu && m_menu->layout_valid(); }

SettingsMenu *Runtime::menu() noexcept { return m_menu.get(); }

Runtime &runtime() {
  static Runtime instance;
  return instance;
}

} // namespace sb::ui

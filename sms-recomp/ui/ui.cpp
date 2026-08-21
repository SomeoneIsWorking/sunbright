#include "ui.h"

#include "runtime.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <lucent/log.h>

namespace sb::ui {
bool inject_escape() {
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

namespace {

bool never_quit() {
    return false;
}

} // namespace

bool run_escape_control(unsigned frames) {
    if (frames == 0 || !runtime().initialize() || !inject_escape())
        return false;
    if (runtime().handle_events(aurora_update()) || !runtime().visible()) {
        lucent::error("ui", "Escape did not open the settings window");
        return false;
    }
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (!aurora_begin_frame()) {
            lucent::error("ui", "Aurora refused UI control frame {} of {}", frame + 1, frames);
            return false;
        }
        aurora_end_frame();
        if (frame == 0 && !runtime().layout_valid())
            return false;
        if (runtime().handle_events(aurora_update()))
            return false;
    }
    if (!inject_escape())
        return false;
    bool frameActive = aurora_begin_frame();
    if (!runtime().pause_while_open(frameActive, never_quit, nullptr) || runtime().visible()) {
        lucent::error("ui", "second Escape did not close the settings window");
        return false;
    }
    if (frameActive)
        aurora_end_frame();
    else
        aurora_discard_frame();
    lucent::info("ui", "Escape opened, rendered, and closed settings across {} frame(s)", frames);
    return true;
}

} // namespace sb::ui

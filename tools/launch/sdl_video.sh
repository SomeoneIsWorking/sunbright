#!/usr/bin/env bash

# Configure SDL's video transport without deciding whether the caller shows its window. Aurora's
# SB_HEADLESS path still needs an SDL window object for framebuffer sizing, but it does not need a
# display server or WSI surface.
configure_sunbright_sdl_video() {
    if [[ "$(uname)" == "Darwin" ]]; then
        return
    fi
    if [ "${SB_HEADLESS:-0}" != "0" ]; then
        export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
    else
        # Native Wayland cannot create the Vulkan surface on this machine; XWayland works.
        export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
        export DISPLAY="${DISPLAY:-:0}"
    fi
}

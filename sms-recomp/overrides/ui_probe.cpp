// ui_probe — open the settings menu on a LIVE run, through the same SDL Escape event a keyboard
// produces.
//
// WHY IT EXISTS. The RmlUi settings document can only be reached by pressing Escape at the window,
// so every defect that needs the menu OPEN is invisible to an automated run: the crash this file
// was written for (the overlay's geometry recorded into an interpolated replay emission, aborting
// the frame) reproduced in one keystroke by hand and in no headless run at all. Driving it through
// the shipping event route rather than by showing the document directly is the point — a probe that
// called SettingsMenu::show() would prove nothing about the path the player takes.
//
//   SBR_PROBE=1 ./run-safe.sh ...
//   curl -s '127.0.0.1:17654/ui?want=open'
//
// ONE-WAY, and that is a property of the pause loop rather than an omission here: while the menu is
// open the game sits in Runtime::pause_while_open and never reaches the frame seam, which is where
// probe handlers run. So this can OPEN the menu on a running game; closing it means stopping the
// run (kill by PID).

#include "../runtime/probe_server.h"
#include "ui/runtime.h"
#include "ui/ui.h"

#include <cstdio>
#include <string>

namespace {

// Registered at static-init, like the other probe endpoints. The toggle is not immediate — the
// pushed event is consumed by the next aurora_update() — so the reply reports what was ASKED for
// and the visibility as of this frame, never a state it has not observed.
const bool g_ui_probe = [] {
    sb_probe_register(
        "/ui", "settings menu: press Escape (want=open|close to make it conditional)",
        [](const ProbeArgs& args) {
            const bool visible = sb::ui::runtime().visible();
            const std::string want = args.str("want");
            char buf[192];
            if ((want == "open" && visible) || (want == "close" && !visible)) {
                std::snprintf(buf, sizeof buf, "settings menu already %s; no Escape pushed\n",
                              visible ? "open" : "closed");
                return std::string(buf);
            }
            const bool pushed = sb::ui::inject_escape();
            std::snprintf(buf, sizeof buf,
                          "Escape %s; menu was %s, so it should be %s from the next event pump\n",
                          pushed ? "pushed" : "FAILED TO PUSH (see the ui error line)",
                          visible ? "open" : "closed",
                          !pushed   ? "unchanged"
                          : visible ? "closed"
                                    : "open");
            return std::string(buf);
        });
    return true;
}();

} // namespace

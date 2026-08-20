#pragma once

namespace sb::ui {

// Runs the Dusklight-style prelaunch settings document. Returns false if the
// window was closed. Headless and explicitly skipped runs return immediately
// without creating a document.
bool run_prelaunch();

// Renders a bounded settings-screen control without entering the game. Used by
// the verification harness so RmlUi resource loading and layout can be checked
// headlessly without a ROM.
bool render_settings_control(unsigned frames);

} // namespace sb::ui

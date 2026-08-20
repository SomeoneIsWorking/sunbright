#pragma once

namespace sb::ui {

// Runs the Dusklight-style prelaunch settings document. Returns false if the
// window was closed. Headless and explicitly skipped runs return immediately
// without creating a document.
bool run_prelaunch();

// Pushes Escape through SDL, verifies that the shipping event route opens the
// settings window, renders it for a bounded number of frames, then pushes
// Escape again and verifies that it closes.
bool run_escape_control(unsigned frames);

} // namespace sb::ui

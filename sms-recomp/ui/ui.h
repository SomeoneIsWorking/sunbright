#pragma once

namespace sb::ui {

// Pushes Escape through SDL, verifies that the shipping event route opens the
// settings window, renders it for a bounded number of frames, then pushes
// Escape again and verifies that it closes.
bool run_escape_control(unsigned frames);

} // namespace sb::ui

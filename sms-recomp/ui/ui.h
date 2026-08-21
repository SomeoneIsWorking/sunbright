#pragma once

namespace sb::ui {

// Pushes Escape through SDL, verifies that the shipping event route opens the
// settings window, renders it for a bounded number of frames, then pushes
// Escape again and verifies that it closes.
bool run_escape_control(unsigned frames);

// Push one Escape key-down through SDL, exactly as a player's keyboard does. Shared with the probe
// server's /ui endpoint so an automated run can open the settings window over a live game through
// the SHIPPING event route — a test that opened the document directly would prove nothing about the
// path the player takes.
bool inject_escape();

} // namespace sb::ui

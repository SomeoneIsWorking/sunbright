---
id: I026
kind: instrument
status: trusted
created: 2026-08-20
---

## Instrument

SBR_UI_SELFTEST headless Escape, modal-loop, and RmlUi layout control

## Validated by

Reported INVALID when the settings panel extended beyond the 1280x960 viewport. After the RCSS fix, `SBR_UI_SELFTEST=2` pushed Escape through SDL into the shipping Aurora event route, observed the window open, reported a 1088x768 window with the then-current 7/7 controls visible, exercised `Runtime::pause_while_open`, pushed Escape again, observed it close, and exited 0 under SDL's windowless offscreen driver on 2026-08-20. Re-run after the haze control was added on 2026-08-22: 8/8 controls visible, open/close path clean, zero GPU faults.

## Known failure modes

Inside the filesystem/device sandbox, Vulkan may be unavailable; Aurora then falls back to its Null backend, whose staging-buffer limit aborts before the UI control can run. This is an explicit failed run, not evidence about layout or Escape. Run the windowless harness with GPU device access and require the Vulkan adapter line plus the open/render/close line.

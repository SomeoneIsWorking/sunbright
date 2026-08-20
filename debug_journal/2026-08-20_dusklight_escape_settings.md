# Dusklight-style Escape settings window

The earlier RmlUi work stopped at a prelaunch page. That was the root mismatch with Dusklight:
Dusklight's settings are a `Document → Window → panes/controls` subsystem, and its in-game input
route owns a modal window lifetime. Sunbright now has the same ownership split in `sms-recomp/ui/`.
Aurora sends each SDL event to RmlUi and then exposes the same event in its `AuroraEvent` array;
Sunbright consumes Escape only from that array, once. Handling Escape in both the Rml document and
the frame seam would close and immediately reopen the menu from one key event.

`ui::Runtime::pause_while_open` stays inside the frame-present seam until Escape or the close button
hides the window. Guest execution therefore cannot advance behind the settings menu. The loop still
obeys Aurora's begin/end/discard contract and uses the existing GPU-submission throttle callback.

Two defects were found by the windowless control:

- The runtime-owned document initially survived past `aurora_shutdown`; its process-static
  destructor then closed an Rml document after RmlUi was gone and segfaulted. `Runtime::shutdown`
  now destroys documents before Aurora.
- Interpolation policy was cached on the first `sbr_lerp_enabled()` call. An in-game selection could
  change typed settings while replay presentation stayed in its startup state. The cache is gone;
  Aurora's host override can be disabled, and enabling a new interpolation interval snaps the first
  tick exact so it cannot blend against old history.

Control: `./run-safe.sh SBR_UI_SELFTEST=2` under SDL offscreen/Vulkan pushed Escape through SDL,
reported a 1088x768 window and 7/7 controls, exercised the modal loop, closed through a second
Escape, exited 0, and produced zero kernel GPU faults. A separate 60-present windowless boot reached
Delfino gameplay and exited cleanly, showing the hidden runtime document does not disturb the normal
frame seam.

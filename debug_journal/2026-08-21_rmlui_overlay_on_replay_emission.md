# The settings menu aborted the run: RmlUi geometry in a replay emission (2026-08-21)

## Symptom

`./run.sh`, press Escape to open the settings menu with interpolated 60fps on:

```
[fatal] [aurora::gfx] Replay emission recorded geometry of its own:
        verts=47840 indices=16920 storage=0 bytes
[rt:error] SIGABRT ... aurora::gfx::end_frame <- sb::ui::Runtime::pause_while_open
```

The abort is the guard in `aurora::gfx::end_frame` doing its job — the bug is what tripped it.

## Root cause

A replay emission (the in-between frame the 60fps path presents) is a COPY of the previous
packet: its draw commands still name vertex/index/storage ranges in the GLOBAL buffers, which are
only correct because nothing wrote those buffers in between. RmlUi draws through
`gfx::push_verts` / `gfx::push_indices` — the same staging as the game — and it draws once per
PRESENTED FRAME, in-betweens included. In a replay packet those pushes start at offset 0, and the
staging copy lands them at offset 0 of the global buffers, on top of the tick's own geometry: one
tick's vertices with another's indices. The assert caught it at the first byte instead.

Nothing about the pause loop is special; it is simply where the menu is drawn. Any overlay drawn
on an in-between frame hits this.

## Fix

`install_replay_snapshot` now RESERVES the first emission's high-water mark in the replay packet's
verts/indices/storage (`ByteBuffer::append_uninitialized`, so no mapped memory is touched) and
seeds `frame.copied` to the same mark. An overlay therefore appends ABOVE the game's bytes, and
the reserved prefix — this packet's own staging buffer, holding some other frame's leftovers — is
never copied down over them. The two seeds must move together: seeding `copied` alone would make a
later push satisfy `highWater <= copied` and emit no copy at all; reserving the buffer alone would
copy the garbage prefix over the real data. The mark is rounded UP to 4 because
`copy_staging_buffer_range` aligns its start DOWN to 4.

`interpolate_recorded_frame`'s two reads of `frame.verts` are now gated on the range lying above
`replayPrefix`, not merely inside the buffer: on a replay packet the recorded ranges are below the
prefix, where this packet's staging holds another frame's bytes. (Behaviour is unchanged — before
the reservation `frame.verts` was empty and the same ranges failed the bounds test — but the
reservation would have made a garbage read look valid.)

The `end_frame` assert now checks the real invariant: nothing written below the reserved prefix,
and no staging copy scheduled below it.

## Instruments

* One-time line when the case actually occurs, so a run that exercises it and a run where the menu
  never drew do not both report silence:
  `[aurora::gfx] replay emission carried its own geometry above the reserved prefix: verts +N ...`
* `/ui` on the probe server (`sms-recomp/overrides/ui_probe.cpp`; `SBR_PROBE=1`, `curl 127.0.0.1:17654/ui?want=open`) pushes a real SDL
  Escape, so an automated run can open the menu over a live game through the shipping event route.
  Without it this crash needed a human at the window and was unreproducible headless.
  NOTE: while the menu is open the game is paused inside `pause_while_open`, which never reaches
  the frame seam — so the probe cannot be used to CLOSE it. Stop the run instead (kill by PID).

## Evidence (red/green, same repro)

`./run-safe.sh SBR_LERP60=1 SBR_STAGE=1 SBR_SCENARIO=0 SBR_PROBE=1 SBR_QUIT_AFTER=100000`, then
`curl 127.0.0.1:17654/ui?want=open`:

* aurora at HEAD~ (fix reverted, everything else identical): `Replay emission recorded geometry of
  its own: verts=38240 indices=11976` + SIGABRT, process gone.
* with the fix: `carried its own geometry above the reserved prefix: verts +38240 indices +11976`,
  run continues. A framebuffer dump taken while paused with the menu open
  (`SB_DUMP_FRAME_EVERY`) shows Delfino Plaza rendering correctly — no vertex corruption.
  (The dump samples the EFB present source, so the RmlUi overlay itself is not in it: the overlay
  is composited onto the swapchain in the present callback.)

# Game-first host startup

The host used to call `ui::run_prelaunch()` immediately after Aurora initialization. That function
showed a second `SettingsMenu` instance and blocked in a RmlUi render loop until its Play button was
clicked. The game therefore was not the product's startup surface even though the same settings
document already had an in-game Escape route.

The prelaunch mode was a second lifetime and policy path, not a requirement of RmlUi. The host now
initializes the single persistent `ui::Runtime` document while it is hidden and proceeds directly
to guest memory, disc, DOL, and game execution. Escape still toggles that document and its existing
modal frame-seam loop still pauses guest simulation. The prelaunch-only Play button, constructor
mode, loop, environment bypass, and styles were deleted so there is only one settings lifetime.

Verification uses two independent controls: `SBR_UI_SELFTEST=2` pushes Escape through the shipping
event route and proves the hidden document opens, lays out 7/7 controls, enters the modal loop, and
closes; a bounded normal windowless boot routed through the shipping `run.sh` launcher must log
`entering recompiled code` and advance game presents without any prelaunch action.

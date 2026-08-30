---
id: C087
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j2d,recomp,decomp
depends: sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_graph_context, decomp/sms/src/JSystem/J2D/J2DGrafContext.cpp#J2DGrafContext::J2DGrafContext
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:45:16
---

## Claim

A base J2DGrafContext does not initialize its type word in retail code, while J2DOrthoGraph installs vtable 0x803e14b0 and type 1; the recomp active-canvas classifier must require both, and the native decomp base constructor must initialize the discriminator to zero.

## Evidence

GMSE01 generated bodies at 0x802eb460 and 0x802eb51c omit the +0x04 write; J2DOrthoGraph constructors at 0x802ecfcc and 0x802ed0a8 install 0x803e14b0 and write 1; j2d_picture_adapter_test base-vtable/stale-type control refuses capture; native decomp initializes only under SMS_NATIVE_PLATFORM.

## What would falsify it

Falsified if the retail DOL writes a stable base-context discriminator, if another supported orthographic context uses a different vtable, or if a real orthographic setup is refused by the combined vtable/type classifier.

## Re-confirmed 2026-08-30

Reverified after moving native active-canvas ownership into native_j2d_context.cpp: the stale-type/base-vtable negative control still refuses, the production-linked decomp context test passes, both full Clang builds pass, and the guarded Delfino run reaches the active canvas without refusal.

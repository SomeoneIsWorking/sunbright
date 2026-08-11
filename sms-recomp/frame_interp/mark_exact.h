// mark_exact.h — declare the primitives a seam emits to be presented EXACTLY on an interpolated
// frame. See mark_exact.cpp for what that means and why it is a declaration rather than a test.
//
// Scoped, and it marks PRIMITIVES rather than the call: a function that emits a screen mask often
// emits ordinary interpolating geometry from the same call, and only the immediate primitives are
// marked. Nesting is safe — the previous GXBegin hook is restored, not cleared.

#pragma once

struct SbExactScope {
    SbExactScope();
    ~SbExactScope();

    bool on;
    void (*prevHook)();
};

void sbr_mark_exact_report();

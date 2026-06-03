// (Intentionally empty.)
//
// The earlier VIWaitForRetrace/GXDrawDone overrides replaced the vsync wait with a same-thread
// CoreTiming advance, which never yields — it starved every other guest thread and froze the game.
// Now that the GC scheduler / context-switch / interrupt primitives are recompiled (the recompiler
// models mtmsr/rfi/SRR), these run their normal recompiled path: OSSleepThread yields to other
// threads, and the VI retrace ISR wakes the waiter. No override needed.

// ===========================================================================
// port/pal/os/os_sync.cpp — TOMBSTONE.
//
// The GameCube OS mutex primitives (OSInitMutex / OSLockMutex / OSUnlockMutex /
// OSTryLockMutex) used to live here as single-threaded no-op placeholders.
//
// They now have REAL, blocking, recursive implementations in os_thread.cpp,
// alongside the native cooperative thread scheduler they depend on (a contended
// mutex must be able to BLOCK its waiter, which requires the scheduler). Defining
// them here too would be a multiple-definition error, so this file is now empty.
//
// Kept as a tombstone (rather than deleted) so the history of where the mutex PAL
// moved is discoverable, and because the PAL glob (pal/*.cpp) tolerates an empty
// translation unit.
// ===========================================================================

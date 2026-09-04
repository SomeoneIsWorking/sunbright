// watchdog.cpp — SIGALRM stall watchdog. The frame seam kicks it once per
// frame; if no frame completes within the timeout the handler dumps a
// backtrace of every thread (the game thread plus any library-internal
// threads: SDL audio, Dawn workers) and aborts. SB_WATCHDOG_SECS overrides
// the default (5 s).

#include "config.h"
#include <csignal>
#include <cstdio>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>

// SIGUSR1 handler: each thread that receives it dumps ITS OWN userspace bt.
// The watchdog broadcasts SIGUSR1 to every task tid so we get all threads.
static void sb_sigusr1_handler(int) {
    pid_t tid = (pid_t)syscall(SYS_gettid);
    dprintf(2, "\n--- Thread tid=%d ---\n", (int)tid);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
}

static void sb_watchdog_handler(int) {
    const char* msg = "\n=== WATCHDOG: no frame within timeout, dumping all threads ===\n";
    write(2, msg, std::strlen(msg));

    pid_t self_tid = (pid_t)syscall(SYS_gettid);
    dprintf(2, "\n--- Signal-recipient thread (tid=%d) ---\n", (int)self_tid);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);

    DIR* d = opendir("/proc/self/task");
    if (d) {
        struct dirent* de;
        pid_t pid = getpid();
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.')
                continue;
            pid_t tid = (pid_t)atoi(de->d_name);
            if (tid == self_tid)
                continue;
            syscall(SYS_tgkill, pid, tid, SIGUSR1);
        }
        closedir(d);
    }
    // Give the handlers time to run before we exit.
    struct timespec ts = {0, 200 * 1000 * 1000};
    nanosleep(&ts, nullptr);
    write(2, "\n=== end thread dump ===\n", 25);
    _exit(134);
}

extern "C" void sb_watchdog_kick(void) {
    alarm(sb::runtime_config().watchdogSeconds);
}

extern "C" void sb_watchdog_install(void) {
    struct sigaction sa{};
    sa.sa_handler = sb_watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);

    struct sigaction su{};
    su.sa_handler = sb_sigusr1_handler;
    sigemptyset(&su.sa_mask);
    su.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &su, nullptr);

    sb_watchdog_kick();
}

# `count_getenv.c` — which env vars are actually read, and how often

An `LD_PRELOAD` interposer that counts `getenv()` calls **by name**. Built for the question
"which of our ~120 diagnostic switches are on a hot path", which cannot be answered from the
source: the same line costs nothing inside an `if (!s_init)` block and millions of environ scans
inside `draw_prim`.

```sh
gcc -shared -fPIC -O2 -o count_getenv.so tools/perf/count_getenv.c -ldl
LD_PRELOAD=$PWD/count_getenv.so GETENV_REPORT=$PWD/scratch/getenv/report.txt \
  ./run.sh --diagnostic --stage 1 --run-secs 30
```

Two things it deliberately does not do:

* It never reports an empty table. If it intercepted nothing it says `INTERPOSER SAW NOTHING`
  and states that this is not evidence the program made no calls — a failed `LD_PRELOAD` and a
  clean program are otherwise identical output.
* It does not rely on its destructor. `sms-boot` exits through a path that does not run one, so
  the report is also flushed every 200k calls.

Validate it against a known positive before trusting a run (a two-line program calling `getenv`
a known number of times); the first result in `debug_journal/2026-08-05_getenv_sprawl_measured_and_converged.md`
was taken that way.

**Send SIGTERM, not SIGKILL** — `timeout -s KILL` prevents the flush at exit.

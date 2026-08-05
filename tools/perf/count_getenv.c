/* count_getenv.c — LD_PRELOAD interposer that counts getenv() calls by NAME.
 *
 * WHY: aurora has ~120 SB_* env gates. Converting all of them to lucent channels is a large
 * refactor; converting only the HOT ones is the win. Which are hot cannot be read off the source
 * (a call inside a `if (!s_init)` block runs once; the same line inside draw_prim runs 46k times a
 * frame), so it is counted instead.
 *
 * NEGATIVE CASE: if the report is empty, that means the interposer never intercepted anything --
 * it prints "INTERPOSER SAW NOTHING" rather than an empty table, because an empty table and a
 * broken LD_PRELOAD look identical otherwise.
 *
 * Build:  gcc -shared -fPIC -O2 -o count_getenv.so count_getenv.c -ldl
 * Use:    LD_PRELOAD=$PWD/count_getenv.so GETENV_REPORT=/path/report.txt <program>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 512
static char  g_name[MAXN][64];
static long  g_cnt[MAXN];
static int   g_n = 0;
static long  g_total = 0;

static char *(*real_getenv)(const char *) = NULL;
static void report(void);

char *getenv(const char *name) {
  if (!real_getenv) real_getenv = dlsym(RTLD_NEXT, "getenv");
  if (name) {
    g_total++;
    int i;
    for (i = 0; i < g_n; i++)
      if (strcmp(g_name[i], name) == 0) { g_cnt[i]++; break; }
    if (i == g_n && g_n < MAXN) {
      snprintf(g_name[g_n], sizeof g_name[0], "%s", name);
      g_cnt[g_n] = 1;
      g_n++;
    }
  }
  /* Do not rely on the destructor: the host exits through a path that does not run it, and an
   * un-run destructor is indistinguishable from "no calls". Flush periodically instead. */
  if ((g_total % 200000) == 0) report();
  return real_getenv(name);
}

static void report(void) {
  const char *path = real_getenv ? real_getenv("GETENV_REPORT") : NULL;
  FILE *f = path ? fopen(path, "w") : stderr;
  if (!f) f = stderr;
  if (g_total == 0) {
    fprintf(f, "INTERPOSER SAW NOTHING: getenv() was never intercepted. Either LD_PRELOAD did not\n"
               "take effect, or the binary resolved getenv internally. This is NOT evidence that\n"
               "the program makes no getenv calls.\n");
  } else {
    fprintf(f, "getenv calls intercepted: %ld total across %d distinct names\n", g_total, g_n);
    /* selection sort, descending; g_n is tiny */
    for (int a = 0; a < g_n; a++) {
      int best = a;
      for (int b = a + 1; b < g_n; b++) if (g_cnt[b] > g_cnt[best]) best = b;
      long tc = g_cnt[a]; g_cnt[a] = g_cnt[best]; g_cnt[best] = tc;
      char tn[64]; memcpy(tn, g_name[a], 64); memcpy(g_name[a], g_name[best], 64); memcpy(g_name[best], tn, 64);
      fprintf(f, "%10ld  %s\n", g_cnt[a], g_name[a]);
    }
  }
  if (f != stderr) fclose(f);
}

__attribute__((destructor)) static void report_at_exit(void) { report(); }

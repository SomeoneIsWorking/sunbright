/* Cost of one getenv() on THIS machine, under THIS process's environment size.
 * Measured in CPU time (CLOCK_PROCESS_CPUTIME_ID) so a loaded machine does not inflate it.
 * Control: a name that is PRESENT and one that is ABSENT — an absent name is the worst case
 * (full scan) and is what a switched-off diagnostic actually pays. If the two come out equal,
 * the benchmark is not measuring the scan and must not be trusted. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
  int envn=0; extern char **environ; for(char**e=environ;*e;e++)envn++;
  const long N=2000000; volatile const char* s;
  for(long i=0;i<N/10;i++) s=getenv("PATH");             /* warm */
  double t0=now(); for(long i=0;i<N;i++) s=getenv("PATH");          double tp=now()-t0;
  t0=now();        for(long i=0;i<N;i++) s=getenv("SB_NOT_SET_XYZ"); double ta=now()-t0;
  printf("environ entries: %d\n", envn);
  printf("present name : %.1f ns/call\n", tp/N*1e9);
  printf("absent  name : %.1f ns/call   <- what a switched-off diagnostic pays\n", ta/N*1e9);
  if (ta < tp*1.05) printf("WARNING: absent is not slower than present; this benchmark is NOT "
                           "measuring the environ scan and its numbers must not be used.\n");
  (void)s; return 0;
}

/* dol_extract.c — extract main.dol from a GC disc image (ISO/RVZ/WIA/...) via libnod.
 *
 * The RE tooling (tools/re/ppcdis.py, tools/re/disasm_range.py, tools/dol_sda.py)
 * expects the retail DOL at scratch/bin/sms.dol; this regenerates it from the ROM
 * after a scratch/ wipe.
 *
 * Build (uses the nod prebuilt fetched by the aurora build):
 *   cc -O2 -o scratch/bin/dol_extract tools/re/dol_extract.c \
 *      -Ibuild/_deps/nod_prebuilt-src/include \
 *      build/_deps/nod_prebuilt-src/lib/libnod.a -lm -lpthread -lstdc++
 *
 * Usage: dol_extract <disc-image> <out.dol>
 */
#include <nod.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <disc-image> <out.dol>\n", argv[0]);
    return 2;
  }
  struct NodHandle* disc = NULL;
  enum NodResult r = nod_disc_open(argv[1], NULL, &disc);
  if (r != NOD_RESULT_OK || !disc) {
    fprintf(stderr, "FATAL: nod_disc_open(%s) failed: %d\n", argv[1], (int)r);
    return 1;
  }
  struct NodHandle* part = NULL;
  r = nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, NULL, &part);
  if (r != NOD_RESULT_OK || !part) {
    fprintf(stderr, "FATAL: nod_disc_open_partition_kind failed: %d\n", (int)r);
    return 1;
  }
  struct NodPartitionMeta meta;
  r = nod_partition_meta(part, &meta);
  if (r != NOD_RESULT_OK) {
    fprintf(stderr, "FATAL: nod_partition_meta failed: %d\n", (int)r);
    return 1;
  }
  if (!meta.raw_dol.data || meta.raw_dol.size < 0x100) {
    fprintf(stderr, "FATAL: raw_dol blob absent/degenerate (size=%zu)\n", meta.raw_dol.size);
    return 1;
  }
  FILE* f = fopen(argv[2], "wb");
  if (!f || fwrite(meta.raw_dol.data, 1, meta.raw_dol.size, f) != meta.raw_dol.size) {
    fprintf(stderr, "FATAL: writing %s failed\n", argv[2]);
    return 1;
  }
  fclose(f);
  fprintf(stderr, "wrote %s (%zu bytes)\n", argv[2], meta.raw_dol.size);
  nod_free(part);
  nod_free(disc);
  return 0;
}

#!/usr/bin/env bash
# fileselect_overbright_drill.sh — AUTOMATICALLY attribute the SB_OWN_GXLIST file-select overbright
# to a render layer. For each candidate ablation it boots sms-boot to a SETTLED file-select, dumps
# one frame, measures the per-channel mean-RGB delta vs the Dolphin-GX oracle (fileselect_overbright.py),
# and ranks the configs by how much they REDUCE the overbright (mean |delta|). The ablation that drops
# the delta the most owns the overbright — no eyeballing batch dumps.
#
# Prereq: scratch/oracle/fileselect_gx_oracle.png (tools/render/fileselect_oracle.sh).
# Each run polls for the settled dump and kills as soon as it lands (settle ~present frame 564).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"
source .env 2>/dev/null
ORACLE=scratch/oracle/fileselect_gx_oracle.png
[ -f "$ORACLE" ] || { echo "missing $ORACLE (run fileselect_oracle.sh)"; exit 1; }
mkdir -p scratch/passes/drill
PAD=""; for f in $(seq 200 12 560); do PAD="$PAD ${f}:START $((f+6)):-"; done

# config label -> extra env (all run with SB_OWN_GXLIST=1 + settle dump)
declare -a LABELS=( "baseline" "no_additive_4_1" "no_premul_1_5" "no_screen_1_3" "no_onealpha_1_1" "no_all_blend" "no_imm_overlay" )
declare -a ENVS=(   ""         "SB_ABLATE_BM=4/1" "SB_ABLATE_BM=1/5" "SB_ABLATE_BM=1/3" "SB_ABLATE_BM=1/1" "SB_OPAQUE_ONLY=1" "SB_SKIP_IMM=1" )

run_one() {
  local label="$1" extra="$2"
  pkill -9 -x sms-boot 2>/dev/null; sleep 1
  rm -f scratch/frames/boot_*.ppm
  # shellcheck disable=SC2086
  timeout -s KILL 280 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
    SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_OWN_GXLIST=1 \
    SB_FRAME_DUMP=1 SB_FRAME_DUMP_SETTLE=1 SB_FRAME_DUMP_MAX=3 $extra \
    SB_PAD_SCRIPT="$PAD" ./build-native/sms-boot > "scratch/passes/drill/${label}.log" 2>&1 &
  local pid=$!
  # Poll up to ~270s for the settled dump, then kill immediately.
  local got=""
  for _ in $(seq 1 135); do
    sleep 2
    f=$(ls -t scratch/frames/boot_*.ppm 2>/dev/null | head -1)
    if [ -n "$f" ]; then sleep 2; got="$f"; break; fi
    kill -0 $pid 2>/dev/null || break
  done
  pkill -9 -x sms-boot 2>/dev/null; wait $pid 2>/dev/null
  if [ -z "$got" ]; then echo "  $label: NO settled frame"; return; fi
  cp "$got" "scratch/passes/drill/${label}.ppm"
  python3 tools/render/fileselect_overbright.py "scratch/passes/drill/${label}.ppm" "$ORACLE" \
    > "scratch/passes/drill/${label}.diff" 2>&1
  local d; d=$(grep 'mean |delta|' "scratch/passes/drill/${label}.diff" | grep -o 'channels: [0-9.]*' | grep -o '[0-9.]*')
  echo "$d $label $(basename "$got")"
}

echo "[drill] measuring overbright (mean |delta| vs oracle) per ablation…"
: > scratch/passes/drill/_results.txt
for i in "${!LABELS[@]}"; do
  echo "[drill] ${LABELS[$i]}  (${ENVS[$i]:-none})"
  run_one "${LABELS[$i]}" "${ENVS[$i]}" | tee -a scratch/passes/drill/_results.txt
done
echo; echo "=== RANKED (lowest mean|delta| = most overbright removed) ==="
sort -n scratch/passes/drill/_results.txt

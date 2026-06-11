#!/bin/bash
# Headless run + audio profile. Usage: run_check.sh [secs] [extra env as K=V...]
# Always headless, dumps audio, then prints per-second WAV profile + halt/exit status.
cd "$(dirname "$0")/../.."
set -a; . ./.env 2>/dev/null; set +a
secs=${1:-5}; shift 2>/dev/null
log=scratch/logs/run_check.log
env "$@" SUNBRIGHT_HEADLESS=1 SUNBRIGHT_DUMP_AUDIO=1 timeout -s KILL "$secs" ./build/sunbright >"$log" 2>&1
echo "exit=$? halt=$(grep -c Halting "$log") wd=$(grep -c 'watchdog' "$log") log=$log"
python3 - <<'EOF'
import struct,math
d=open('scratch/wav/native_dsp.wav','rb').read()[44:]
s=struct.unpack(f'<{len(d)//2}h',d); L=s[0::2]; sr=32028
out=[]
for sec in range(len(L)//sr):
    x=L[sec*sr:(sec+1)*sr]; e=math.sqrt(sum(v*v for v in x)/sr)
    out.append('s' if e<10 else ('DC' if max(x)==min(x) else f'{e:.0f}'))
print('dsp:', ' '.join(out) if out else '<empty>')
EOF

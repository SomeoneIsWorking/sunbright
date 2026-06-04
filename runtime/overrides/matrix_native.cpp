// Native PC port of the GameCube paired-single matrix/quaternion library (PSMTX*, 0x80349990+).
//
// WHY: the recompiler miscompiles these — they're hand-written paired-single (Gekko ps_*) routines
// the emitter doesn't fully model, so character skinning came out distorted (e.g. Mario lying
// contorted; pure-JIT was fine, recomp was not — bisected to this PSMTX cluster). Porting them to
// straightforward native float math fixes that exactly, and gives correct, reusable matrix/quat
// primitives we'll want for the transform-interpolation work (slerp/lerp of joint matrices).
//
// Standard GC SDK semantics. Mtx = f32[3][4] row-major (m[r][c] at byte r*16 + c*4). Vec = f32[3].
// Quaternion = f32[4] {x,y,z,w}. Float args come in f1.. (cpu.fpr[i].ps0), pointers in r3..
//
// A leaf function's blr is a plain C return, so each override just reads args, computes, writes the
// result (and a return value into r3 where applicable) and returns — no call_ppc.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>

static inline f32  mr(u32 m, int r, int c)        { return mem_rf32(m + (u32)(r * 16 + c * 4)); }
static inline void mw(u32 m, int r, int c, f32 v) { mem_wf32(m + (u32)(r * 16 + c * 4), v); }
static inline f32  fa(CPUState& cpu, int i)       { return (f32)cpu.fpr[i].ps0; }   // float arg f<i>

SUNBRIGHT_OVERRIDE(ov_PSMTXIdentity, 0x80349990u) {              // PSMTXIdentity(Mtx)
    const u32 m = cpu.gpr[3];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) mw(m, r, c, r == c ? 1.0f : 0.0f);
}

SUNBRIGHT_OVERRIDE(ov_PSMTXCopy, 0x803499bcu) {                  // PSMTXCopy(src, dst)
    const u32 s = cpu.gpr[3], d = cpu.gpr[4];
    for (int i = 0; i < 12; i++) mem_wf32(d + i * 4, mem_rf32(s + i * 4));
}

SUNBRIGHT_OVERRIDE(ov_PSMTXConcat, 0x803499f0u) {                // PSMTXConcat(a, b, ab):  ab = a*b
    const u32 a = cpu.gpr[3], b = cpu.gpr[4], ab = cpu.gpr[5];
    f32 t[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
        t[r][c] = mr(a, r, 0) * mr(b, 0, c) + mr(a, r, 1) * mr(b, 1, c) + mr(a, r, 2) * mr(b, 2, c)
                + (c == 3 ? mr(a, r, 3) : 0.0f);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) mw(ab, r, c, t[r][c]);
}

SUNBRIGHT_OVERRIDE(ov_PSMTXInverse, 0x80349abcu) {              // PSMTXInverse(src, inv) -> 0 if singular
    const u32 s = cpu.gpr[3], v = cpu.gpr[4];
    const f32 m00 = mr(s,0,0), m01 = mr(s,0,1), m02 = mr(s,0,2), m03 = mr(s,0,3);
    const f32 m10 = mr(s,1,0), m11 = mr(s,1,1), m12 = mr(s,1,2), m13 = mr(s,1,3);
    const f32 m20 = mr(s,2,0), m21 = mr(s,2,1), m22 = mr(s,2,2), m23 = mr(s,2,3);
    const f32 det = m00*(m11*m22 - m12*m21) - m01*(m10*m22 - m12*m20) + m02*(m10*m21 - m11*m20);
    if (det == 0.0f) { cpu.gpr[3] = 0; return; }
    const f32 id = 1.0f / det;
    const f32 i00=(m11*m22-m12*m21)*id, i01=(m02*m21-m01*m22)*id, i02=(m01*m12-m02*m11)*id;
    const f32 i10=(m12*m20-m10*m22)*id, i11=(m00*m22-m02*m20)*id, i12=(m02*m10-m00*m12)*id;
    const f32 i20=(m10*m21-m11*m20)*id, i21=(m01*m20-m00*m21)*id, i22=(m00*m11-m01*m10)*id;
    mw(v,0,0,i00); mw(v,0,1,i01); mw(v,0,2,i02); mw(v,0,3, -(i00*m03 + i01*m13 + i02*m23));
    mw(v,1,0,i10); mw(v,1,1,i11); mw(v,1,2,i12); mw(v,1,3, -(i10*m03 + i11*m13 + i12*m23));
    mw(v,2,0,i20); mw(v,2,1,i21); mw(v,2,2,i22); mw(v,2,3, -(i20*m03 + i21*m13 + i22*m23));
    cpu.gpr[3] = 1;
}

static void rot_trig(u32 m, char axis, f32 s, f32 c) {
    for (int r = 0; r < 3; r++) for (int cc = 0; cc < 4; cc++) mw(m, r, cc, r == cc ? 1.0f : 0.0f);
    switch (axis) {
        case 'x': case 'X': mw(m,1,1,c); mw(m,1,2,-s); mw(m,2,1,s); mw(m,2,2,c); break;
        case 'y': case 'Y': mw(m,0,0,c); mw(m,0,2,s); mw(m,2,0,-s); mw(m,2,2,c); break;
        case 'z': case 'Z': mw(m,0,0,c); mw(m,0,1,-s); mw(m,1,0,s); mw(m,1,1,c); break;
    }
}
SUNBRIGHT_OVERRIDE(ov_PSMTXRotRad, 0x80349bb4u) {                // PSMTXRotRad(Mtx, char axis, rad)
    rot_trig(cpu.gpr[3], (char)cpu.gpr[4], std::sin(fa(cpu,1)), std::cos(fa(cpu,1)));
}
SUNBRIGHT_OVERRIDE(ov_PSMTXRotTrig, 0x80349c24u) {               // PSMTXRotTrig(Mtx, char axis, sin, cos)
    rot_trig(cpu.gpr[3], (char)cpu.gpr[4], fa(cpu,1), fa(cpu,2));
}

SUNBRIGHT_OVERRIDE(ov_PSMTXTrans, 0x80349dd0u) {                 // PSMTXTrans(Mtx, x, y, z)
    const u32 m = cpu.gpr[3];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) mw(m, r, c, r == c ? 1.0f : 0.0f);
    mw(m,0,3,fa(cpu,1)); mw(m,1,3,fa(cpu,2)); mw(m,2,3,fa(cpu,3));
}
SUNBRIGHT_OVERRIDE(ov_PSMTXTransApply, 0x80349e04u) {            // PSMTXTransApply(src, dst, x, y, z)
    const u32 s = cpu.gpr[3], d = cpu.gpr[4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) mw(d, r, c, mr(s, r, c));
    mw(d,0,3, mr(s,0,3)+fa(cpu,1)); mw(d,1,3, mr(s,1,3)+fa(cpu,2)); mw(d,2,3, mr(s,2,3)+fa(cpu,3));
}

SUNBRIGHT_OVERRIDE(ov_PSMTXScale, 0x80349e44u) {                 // PSMTXScale(Mtx, x, y, z)
    const u32 m = cpu.gpr[3];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) mw(m, r, c, 0.0f);
    mw(m,0,0,fa(cpu,1)); mw(m,1,1,fa(cpu,2)); mw(m,2,2,fa(cpu,3));
}
SUNBRIGHT_OVERRIDE(ov_PSMTXScaleApply, 0x80349e6cu) {            // PSMTXScaleApply(src, dst, x, y, z)
    const u32 s = cpu.gpr[3], d = cpu.gpr[4];
    const f32 sx = fa(cpu,1), sy = fa(cpu,2), sz = fa(cpu,3);
    for (int c = 0; c < 4; c++) { mw(d,0,c, mr(s,0,c)*sx); mw(d,1,c, mr(s,1,c)*sy); mw(d,2,c, mr(s,2,c)*sz); }
}

SUNBRIGHT_OVERRIDE(ov_PSMTXQuat, 0x80349eb8u) {                  // PSMTXQuat(Mtx, const Quaternion*)
    const u32 m = cpu.gpr[3], q = cpu.gpr[4];
    const f32 x = mem_rf32(q+0), y = mem_rf32(q+4), z = mem_rf32(q+8), w = mem_rf32(q+12);
    const f32 norm = x*x + y*y + z*z + w*w;
    const f32 s = (norm > 0.0f) ? 2.0f / norm : 0.0f;
    const f32 xs=x*s, ys=y*s, zs=z*s, wx=w*xs, wy=w*ys, wz=w*zs,
              xx=x*xs, xy=x*ys, xz=x*zs, yy=y*ys, yz=y*zs, zz=z*zs;
    mw(m,0,0,1.0f-(yy+zz)); mw(m,0,1,xy-wz);        mw(m,0,2,xz+wy);        mw(m,0,3,0.0f);
    mw(m,1,0,xy+wz);        mw(m,1,1,1.0f-(xx+zz)); mw(m,1,2,yz-wx);        mw(m,1,3,0.0f);
    mw(m,2,0,xz-wy);        mw(m,2,1,yz+wx);        mw(m,2,2,1.0f-(xx+yy)); mw(m,2,3,0.0f);
}

SUNBRIGHT_OVERRIDE(ov_PSMTXMultVec, 0x8034a2d0u) {               // PSMTXMultVec(m, src, dst): dst=m*(src,1)
    const u32 m = cpu.gpr[3], s = cpu.gpr[4], d = cpu.gpr[5];
    const f32 x = mem_rf32(s+0), y = mem_rf32(s+4), z = mem_rf32(s+8);
    const f32 ox = mr(m,0,0)*x + mr(m,0,1)*y + mr(m,0,2)*z + mr(m,0,3);
    const f32 oy = mr(m,1,0)*x + mr(m,1,1)*y + mr(m,1,2)*z + mr(m,1,3);
    const f32 oz = mr(m,2,0)*x + mr(m,2,1)*y + mr(m,2,2)*z + mr(m,2,3);
    mem_wf32(d+0,ox); mem_wf32(d+4,oy); mem_wf32(d+8,oz);
}
SUNBRIGHT_OVERRIDE(ov_PSMTXMultVecArray, 0x8034a324u) {          // PSMTXMultVecArray(m, src, dst, n)
    const u32 m = cpu.gpr[3]; u32 s = cpu.gpr[4], d = cpu.gpr[5], n = cpu.gpr[6];
    const f32 a00=mr(m,0,0),a01=mr(m,0,1),a02=mr(m,0,2),a03=mr(m,0,3),
              a10=mr(m,1,0),a11=mr(m,1,1),a12=mr(m,1,2),a13=mr(m,1,3),
              a20=mr(m,2,0),a21=mr(m,2,1),a22=mr(m,2,2),a23=mr(m,2,3);
    for (u32 i = 0; i < n; i++, s += 12, d += 12) {
        const f32 x = mem_rf32(s+0), y = mem_rf32(s+4), z = mem_rf32(s+8);
        mem_wf32(d+0, a00*x + a01*y + a02*z + a03);
        mem_wf32(d+4, a10*x + a11*y + a12*z + a13);
        mem_wf32(d+8, a20*x + a21*y + a22*z + a23);
    }
}
SUNBRIGHT_OVERRIDE(ov_PSMTXMultVecSR, 0x8034a3b0u) {             // PSMTXMultVecSR(m, src, dst): dst=m3x3*src
    const u32 m = cpu.gpr[3], s = cpu.gpr[4], d = cpu.gpr[5];
    const f32 x = mem_rf32(s+0), y = mem_rf32(s+4), z = mem_rf32(s+8);
    const f32 ox = mr(m,0,0)*x + mr(m,0,1)*y + mr(m,0,2)*z;
    const f32 oy = mr(m,1,0)*x + mr(m,1,1)*y + mr(m,1,2)*z;
    const f32 oz = mr(m,2,0)*x + mr(m,2,1)*y + mr(m,2,2)*z;
    mem_wf32(d+0,ox); mem_wf32(d+4,oy); mem_wf32(d+8,oz);
}

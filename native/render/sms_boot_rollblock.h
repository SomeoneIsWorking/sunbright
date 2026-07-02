// sms_boot_rollblock.h — pure spec for the TRollBlock::calcRootMatrix port
// (@0x801efdc4). TRollBlock is 回転板 ("rotating plate"): a rail-borne rotating board
// whose root matrix is (TRS from TMapObjBase) * (Z-axis rotation by unk138 degrees).
//
// Everything in calcRootMatrix except the pure Z-axis rotation-matrix build is a call
// out to engine primitives (MsMtxSetXYZRPH, J3DModel::setBaseScale, PSMTXConcat, and
// the JMASSin/JMASCos table lookup), which already have coverage elsewhere. What is
// worth pinning down as a hand-written spec is exactly which 12 slots of the 3x4 base
// matrix the roll build populates — the disasm writes cos/-sin/sin/cos into the [0][0]
// [0][1] [1][0] [1][1] slots and 1.0 into [2][2], with 0 everywhere else. Get the sign
// or slot wrong and the roll axis silently flips or migrates to X/Y — the block would
// wobble instead of spin.
//
// Spec (from scratch/disasm.py 0x801efdc4, the 12 back-to-back stfs's at 0x2c..0x58):
//    [0][0]=cos  [0][1]=-sin [0][2]=0  [0][3]=0
//    [1][0]=sin  [1][1]= cos [1][2]=0  [1][3]=0
//    [2][0]=0    [2][1]=0    [2][2]=1  [2][3]=0
// (rotate-around-Z, faithful to the RE's slot order.)

#pragma once

namespace sb {

// Pure builder: given a precomputed (cos, sin) for the roll angle, fill a 3x4 matrix
// (row-major, `float m[3][4]`) with the Z-axis rotation. Callers hand in cos/sin from
// whatever lookup (JMASSin/JMASCos in the port, or hand-picked spot values in the test).
// This is what makes the port's slot layout falsifiable without dragging in the sincos
// global table.
inline void rollblock_build_z_rot_mtx(float cos_val, float sin_val, float m[3][4])
{
    m[0][0] = cos_val;    m[0][1] = -sin_val;   m[0][2] = 0.0f;   m[0][3] = 0.0f;
    m[1][0] = sin_val;    m[1][1] =  cos_val;   m[1][2] = 0.0f;   m[1][3] = 0.0f;
    m[2][0] = 0.0f;       m[2][1] =  0.0f;      m[2][2] = 1.0f;   m[2][3] = 0.0f;
}

}  // namespace sb

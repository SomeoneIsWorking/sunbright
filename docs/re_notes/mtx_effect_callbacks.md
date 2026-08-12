# TMtxSwingRZCallBack / TMtxTimeLagCallBack — addresses resolved, port BLOCKED on the calc bodies

2026-08-12. Both are reached stubs on every Delfino run (`sms-boot/boot_stubs/unresolved_stubs.cpp`).

## Why they could not just be written

The callbacks themselves are thin — `TMtxTimeLagCallBack` is 14 instructions — but each one's whole
body is a call to a `calc` method that **does not exist anywhere in the tree**:

    TMtxTimeLag::calc(MtxPtr)                  declared MarioUtil/MtxUtil.hpp, no definition
    TMtxSwingRZ::calc(MtxPtr)                  same
    TMtxSwingRZ::calcLocalXY(MtxPtr,Vec*,Vec*) same

No definition and no stub, so writing the callback would not link. The port is therefore the three
`calc` bodies FIRST, then the three callbacks on top — not the other way round.

## US addresses, resolved (they are absent from `reference/sms_gmse01_funcs.txt`)

Recovered by JP→US delta from `decomp/sms/config/GMSJ01/symbols.txt`, then **verified by content**,
because the delta was not unanimous — two candidate deltas appeared in the neighbourhood
(0x164bd0 with 8-12 votes, 0x164c44 with 4). Votes alone would have been a coin flip on two of the
three, so each candidate was disassembled and checked to be a real function entry whose extent
matches the JP symbol's recorded size.

| function | JP | size | US | check |
|---|---|---|---|---|
| `TMtxSwingRZReverseXZCallBack` | 0x800c79a8 | 0xAC | 0x8022c578 | entry |
| `TMtxSwingRZCallBack` | 0x800c7a54 | 0x94 | 0x8022c624 | `mflr`/`stwu` entry, branch target inside the 0x94 extent |
| `TMtxTimeLagCallBack` | 0x800c7d48 | 0x38 | 0x8022c918 | entry, `blr` at +0x34 = exactly 0x38 bytes |

The winning delta is 0x164bd0 for all three.

## TMtxTimeLagCallBack, fully decoded (the easy one, kept for whoever unblocks it)

```
int TMtxTimeLagCallBack(J3DNode* node, int param)
{
    if (param == 0)
        ((TMtxTimeLag*)node->mCallBackUserData)->calc(J3DSys::mCurrentMtx);
    return 1;          // 1 on BOTH paths (li r3,1 sits after the join)
}
```

`node->mCallBackUserData` is the `lwz r3, 4(r3)`; J3DNode has it at +0x04.

**The global at US 0x80404788 is `J3DSys::mCurrentMtx`**, and that is not inferred from the name.
46 sites in the image form that address, and they are overwhelmingly matrix code
(`TBossGessoMtxCalc::calc`, `TBossWanwanMtxCalc::calc`, `TBGKMtxCalc::calc`, `GessoBodyCallback`).
One of them is already decompiled: `src/Enemy/gesso.cpp:235` reads
`MTXConcat(J3DSys::mCurrentMtx, local_74, J3DSys::mCurrentMtx)`, and the disassembly at 0x8004a00c
is `lis/addi` of 0x80404788 into r3 AND r5 around a `bl PSMTXConcat` — the same matrix in both the
source and destination slots. That is a match on structure, not on a plausible name.

# Runtime call census — what the game actually executes

Captured 2026-06-12 via SUNBRIGHT_CALL_CENSUS=1 (call_ppc dispatch counter, every emitted
bl/bctrl), 260 s headless run: boot → title → file-select → Delfino gameplay (move/jump/
spray). 377,023,840 dispatched calls, 3,232 unique functions. Dump: scratch/logs/
call_census.tsv (regenerate any time: env + curl /census). Aggregation drives the
port order in docs/port_roadmap.md.

| Calls | Layer | Port notes |
|---|---|---|
| 122.6M | game actors (misc) | stays recompiled (doctrine) |
| 71.6M | GX SDK writers | TIER 1: pure FIFO builders, documented API — native port = GPU command substrate |
| 38.5M | J3D | TIER 2: object-model port (render-port direction) |
| 36.3M | T* framework (TViewObj/TLiveActor perform walk) | with JDrama |
| 18.5M | JPA particles | after J3D |
| 17.2M | TMap (incl. checkDistance 37M total top fn) | checkDistance = TIER 1 pure math |
| 15.2M | JUT (font metrics 6M+) | with J2D ortho ownership |
| 13.3M | JDrama (testPerform 11M) | TIER 2 traversal |
| 7.6M | PSMTX matrix SDK | TIER 1 pure math |
| 5.2M | OS | already native-owned |
| 4.5M | TSun (getZBufValue 4.4M) | already overridden; consider caching |
| 4.0M | THP (THPPlayerQuit 4M = per-frame poll) | documented; M4 gates known |
| 3.4M | JAI/JAL/JAS audio | native-owned/partial (campaign ongoing) |
| 11.3M/13.3M/10M | sinf / fabsf / __cvt_fp2unsigned | TIER 1 — table-based sinf must be ported bit-faithfully, not host sinf |

Top single functions: checkDistance(JGeometry) 37.1M, fabsf 13.3M, __GXXfVtxSpecs 11.8M,
sinf 11.3M, JDrama::TViewObj::testPerform 11.0M, TMapObjBase::perform 7.7M,
J3DSkinDeform::initMtxIndexArray 6.4M, TLiveActor::perform 5.9M.

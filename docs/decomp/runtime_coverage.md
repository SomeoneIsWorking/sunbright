# Runtime call census — what the game actually executes

Captured 2026-06-12 by the then-current call dispatcher over a 260 s headless route: boot → title →
file-select → Delfino gameplay (move/jump/spray). It observed 377,023,840 calls across 3,232 unique
functions. This is retained as a reached-code population measurement; it neither prescribes an
execution engine nor replaces the native/dynarec migration order in `docs/port/migration.md`.

| Calls | Layer | Port notes |
|---|---|---|
| 122.6M | game actors (misc) | ordinary guest behavior unless deliberately owned by a native override |
| 71.6M | GX SDK writers | semantic renderer bypasses these only after the owning high-level draw family is verified |
| 38.5M | J3D | high-value game-semantic renderer boundary |
| 36.3M | T* framework (TViewObj/TLiveActor perform walk) | with JDrama |
| 18.5M | JPA particles | after J3D |
| 17.2M | TMap (incl. checkDistance 37M total top fn) | pure game math remains ordinary JIT work unless a measured native owner is justified |
| 15.2M | JUT (font metrics 6M+) | with J2D ortho ownership |
| 13.3M | JDrama (testPerform 11M) | traversal and camera/light semantic boundary |
| 7.6M | PSMTX matrix SDK | ordinary JIT work unless replaced by a verified native math owner |
| 5.2M | OS | already native-owned |
| 4.5M | TSun (getZBufValue 4.4M) | already overridden; consider caching |
| 4.0M | THP (THPPlayerQuit 4M = per-frame poll) | documented; M4 gates known |
| 3.4M | JAI/JAL/JAS audio | native-owned/partial (campaign ongoing) |
| 11.3M/13.3M/10M | sinf / fabsf / __cvt_fp2unsigned | preserve exact guest semantics; any native replacement needs binary-derived controls |

Top single functions: checkDistance(JGeometry) 37.1M, fabsf 13.3M, __GXXfVtxSpecs 11.8M,
sinf 11.3M, JDrama::TViewObj::testPerform 11.0M, TMapObjBase::perform 7.7M,
J3DSkinDeform::initMtxIndexArray 6.4M, TLiveActor::perform 5.9M.

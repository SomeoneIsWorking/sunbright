# Full-game native-port burn-down worklist

76 gap files remaining · ~580,464 bytes of JP code to RE (≈ burden estimate from JP symbol sizes).

Ordered by category, then RE burden (biggest first). `factory`: whether the actor type is registered / commented-out / not in getNameRef_Enemy.
Regenerate: `python3 tools/re/gap_worklist.py --md > docs/port/worklist.md`


## Animal

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/Animal/BeeHive.cpp | 12,592 | 34 | commented | TBeeHive, TNerveBeeHiveAttack, TNerveBeeHiveMarioWaterIn, TNerveBeeHiveBreak, TBeeHiveManager, TNerveBeeHiveFall |
| decomp/sms/src/Animal/boid.cpp | 4,892 | 8 | - | TBoidLeader, TBoid |
| decomp/sms/src/Animal/fishoid.cpp | 1,808 | 10 | commented | TFishoid, TFishoidManager |
| decomp/sms/src/Animal/Butterfly.cpp | 784 | 4 | commented | TButterfly |

## Enemy

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/Enemy/Koopa.cpp | 76,976 | 271 | - | TKoopa, TKoopaJrSubmarine, TTinKoopa, TLimitKoopa, TKoopaJr, TNerveKoopaFlame |
| decomp/sms/src/Enemy/bosseel.cpp | 31,752 | 89 | - | TBossEel, TBossEelTooth, TNerveBossEelEat, TNerveBossEelOutWait, TBossEelSaveParams, TNerveBossEelMouthOpenWait |
| decomp/sms/src/Enemy/bosstelesa.cpp | 30,752 | 61 | registered | TBossTelesa, TNerveBossTelesaDie, TNerveBossTelesaPrepareSlot, TNerveBossTelesaAppear, TBossTelesaSaveLoadParams, TNerveBossTelesaSlotStart |
| decomp/sms/src/Enemy/killer.cpp | 30,432 | 96 | commented | TBathtubKiller, TKiller, TCoasterKiller, TBathtubKillerParams, TNerveBathtubKillerChaseStraight, TNerveBathtubKillerWander |
| decomp/sms/src/Enemy/pakkun.cpp | 29,676 | 101 | commented | TBossPakkun, TPakkun, TNervePakkunStay, TStayPakkun, TPakkunSeed, TPakkunManager |
| decomp/sms/src/Enemy/koopajr.cpp | 24,384 | 93 | - | TKoopaJrSubmarine, TKoopaJr, TLimitKoopaJr, TKoopaJrSubmarineParams, TNerveLimitKoopaJrWait, TLimitKoopaJrParams |
| decomp/sms/src/Enemy/cannon.cpp | 23,280 | 66 | commented | TCannon, TNerveCannonSearch, TNerveCannonDamage, TDemoCannon, TNerveCannonShoot, TNerveCannonClose |
| decomp/sms/src/Enemy/enemyMario.cpp | 21,636 | 32 | - | TEnemyMario |
| decomp/sms/src/Enemy/chuuhana.cpp | 19,248 | 55 | commented | TChuuHana, TChuuHanaSaveLoadParams, TNerveChuuHanaAttack, TNerveChuuHanaKeepBalance, TNerveChuuHanaForceJumped, TChuuHanaManager |
| decomp/sms/src/Enemy/limitkoopa.cpp | 17,208 | 80 | - | TLimitKoopa, TLimitKoopaJr, TLimitKoopaManager, TNerveLimitKoopaJrWait, TLimitKoopaHead, TLimitKoopaJrParams |
| decomp/sms/src/Enemy/tinkoopa.cpp | 15,840 | 50 | - | TTinKoopa, TTinKoopaPartsBase, TTinKoopaFlame, TNerveTinKoopaBreak, TTinKoopaParams, TTinKoopaManager |
| decomp/sms/src/Enemy/tobiPuku.cpp | 15,568 | 82 | commented | TTobiPuku, TNerveTobiPukuLand, TTobiPukuLaunchPad, TTobiPukuLaunchPadManager, TNerveTobiPukuReturnLaunch, TNerveTobiPukuHitWater |
| decomp/sms/src/Enemy/popo.cpp | 15,468 | 49 | commented | TPopo, TPopoSaveLoadParams, TNervePopoFly, TPopoManager, TNervePopoExplosion, TNervePopoPossessedNozzle |
| decomp/sms/src/Enemy/bossManta.cpp | 15,232 | 35 | - | TBossManta, TBossMantaManager, TBossMantaAdditionalCollisionSet, TBossMantaParams, TBossMantaAdditionalCollision |
| decomp/sms/src/Enemy/bosspakkun.cpp | 12,708 | 25 | - | TBossPakkun, TBossPakkunMtxCalc, TBossPakkunParams, TBossPakkunManager |
| decomp/sms/src/Enemy/TabePuku.cpp | 11,284 | 40 | commented | TTabePuku, TNerveTabePukuDrag, TNerveTabePukuAttack, TTabePukuManager, TNerveTabePukuFound, TNerveTabePukuRecoverGraph |
| decomp/sms/src/Enemy/Kazekun.cpp | 10,580 | 38 | - | TKazekun, TNerveKazekunAttack, TKazekunParams, TNerveKazekunPreAttack, TNerveKazekunTurn, TNerveKazekunHitWater |
| decomp/sms/src/Enemy/amiNoko.cpp | 10,324 | 31 | commented | TAmiNoko, TNerveAmiNokoDie, TNerveAmiNokoTurn, TNerveAmiNokoWalkOnFence, TAmiNokoManager, TNerveAmiNokoFreeze |
| decomp/sms/src/Enemy/bombhei.cpp | 10,240 | 46 | commented | TBombHei, TBombHeiManager, TNerveBombHeiExplosion, TNerveBombHeiWaitExplosion, TNerveBombHeiGenerate, TNerveBombHeiThrown |
| decomp/sms/src/Enemy/bosswanwan.cpp | 9,744 | 20 | - | TBossWanwan, TBossWanwanManager, TBossWanwanMtxCalc |
| decomp/sms/src/Enemy/Kukku.cpp | 9,684 | 35 | - | TKukku, TKukkuBall, TNerveKukkuGraphWander, TKukkuManager, TNerveKukkuFall, TNerveKukkuPostFall |
| decomp/sms/src/Enemy/elecNokonoko.cpp | 9,516 | 42 | commented | TElecNokonoko, TElecNokonokoManager, TNerveElecNokonokoFreeze, TNerveElecNokonokoCollect, TNerveElecNokonokoTurn, TNerveElecNokonokoShoot |
| decomp/sms/src/Enemy/wireTrap.cpp | 9,136 | 31 | commented | TWireTrap, TNerveWireTrapOnewayMove, TNerveWireTrapSearch, TNerveWireTrapReturnMove, TWireTrapManager, TNerveWireTrapOnewayMoveEnd |
| decomp/sms/src/Enemy/hanasambo.cpp | 8,012 | 38 | commented | THanaSambo, THanaSamboManager, TNerveHanaSamboHide, TNerveHanaSamboAppear, TNerveHanaSamboDie, TNerveHanaSamboWait |
| decomp/sms/src/Enemy/gatekeeper.cpp | 7,804 | 24 | commented | TBiancoGateKeeper, TBiancoGateKeeperManager, TGateKeeperBase |
| decomp/sms/src/Enemy/igaiga.cpp | 7,756 | 40 | commented | TIgaiga, TIgaigaManager, TNerveIgaigaWaterHit, TNerveIgaigaShootFromCannon, TNerveIgaigaRollOnGraph, TIgaigaPolluteModelManager |
| decomp/sms/src/Enemy/BossHanachanParts.cpp | 7,020 | 25 | - | TBossHanachanPartsBase, TBossHanachanPartsBody, TBossHanachanPartsHead |
| decomp/sms/src/Enemy/rocket.cpp | 6,328 | 31 | commented | TRocket, TRocketManager, TNerveRocketPossessedNozzle, TNerveRocketFly, TNerveRocketWait |
| decomp/sms/src/Enemy/limitkoopajr.cpp | 5,752 | 26 | - | TLimitKoopaJr, TNerveLimitKoopaJrWait, TLimitKoopaJrParams, TNerveLimitKoopaJrRun, TNerveLimitKoopaJrLaunch, TNerveLimitKoopaJrYahoo |
| decomp/sms/src/Enemy/BathtubBinder.cpp | 1,672 | 5 | - | TBathtubBinder |
| decomp/sms/src/Enemy/BathtubPeach.cpp | 1,624 | 15 | - | TBathtubPeach, TBathtubPeachManager |
| decomp/sms/src/Enemy/SleepBossHanachan.cpp | 1,004 | 8 | - | TSleepBossHanachan, TSleepBossHanachanManager |
| decomp/sms/src/Enemy/BossHanachanSave.cpp | 204 | 1 | - | TDemoBossHanachanSaveParams |
| decomp/sms/src/Enemy/BossHanachanAnm.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/BossHanachanEffect.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/BossHanachanMain.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/BossHanachanNerve.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/BossHanachanSound.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/BossHanachanSub.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/DemoBossHanachanBase.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/feetinv.cpp | 0 | 0 | - | — |
| decomp/sms/src/Enemy/yunbo.cpp | 0 | 0 | - | — |

## GC2D

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/GC2D/Talk2D2.cpp | 17,696 | 22 | - | TTalk2D2 |
| decomp/sms/src/GC2D/Guide.cpp | 14,764 | 13 | - | TGuide |
| decomp/sms/src/GC2D/BlendPane.cpp | 512 | 3 | - | TBlendPane |
| decomp/sms/src/GC2D/ChangeValue.cpp | 0 | 0 | - | — |
| decomp/sms/src/GC2D/SelectShine2.cpp | 0 | 0 | - | — |

## JSystem

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/JSystem/J3D/J3DGraphAnimator/J3DMaterialAttach.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/J3D/J3DGraphLoader/J3DClusterLoader.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JAudio/JADebug/JADHioNode.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JAudio/JAInterface/JAIDebug.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JAudio/JASystem/JASInstEffect.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JAudio/JASystem/JASRate.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JDrama/JDRDStageGroup.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JKernel/JKRFileCache.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JParticle/JPADataBlock.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JStage/JSGAmbientLight.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JUtility/JUTDbPrint.cpp | 0 | 0 | - | — |
| decomp/sms/src/JSystem/JUtility/JUTVideo.cpp | 0 | 0 | - | — |

## Map

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/Map/StickyStainManager.cpp | 120 | 2 | - | TStickyStainManager |

## MoveBG

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/MoveBG/ModelGate.cpp | 6,200 | 8 | - | TModelGate |
| decomp/sms/src/MoveBG/MapObjFlag.cpp | 5,160 | 13 | - | TMapObjFlagManager, TMapObjFlag |
| decomp/sms/src/MoveBG/MapObjFence.cpp | 0 | 0 | - | — |
| decomp/sms/src/MoveBG/MapObjSample.cpp | 0 | 0 | - | — |
| decomp/sms/src/MoveBG/MapObjSirena.cpp | 0 | 0 | - | — |

## Player

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/Player/Atom.cpp | 0 | 0 | - | — |

## Strategic

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/Strategic/binder.cpp | 7,364 | 20 | - | TBWBinder, TWireBinder, TBathtubBinder, TBGBinder, TBinder |

## System

| file | burden | methods | factory | classes |
|---|--:|--:|---|---|
| decomp/sms/src/System/StageEventInfo.cpp | 444 | 4 | - | TStageEventInfo |
| decomp/sms/src/System/ScenarioArchiveName.cpp | 220 | 3 | - | TScenarioArchiveName |
| decomp/sms/src/System/ProcessMeter.cpp | 64 | 1 | - | TProcessMeter |
| decomp/sms/src/System/MarDirectorCreateObjects.cpp | 0 | 0 | - | — |
| decomp/sms/src/System/TexCache.cpp | 0 | 0 | - | — |

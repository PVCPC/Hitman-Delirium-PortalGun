# Hitman WoA bones: read / write / rig (ZHMModSDK 4.2.1, game 3.290.1)

Game checked: `F:\SteamLibrary\steamapps\common\HITMAN 3\Retail\HITMAN3.exe`, FileVersion **3.290.1.0**, written 2026-11-03.

Legend: **[V-exe]** checked statically in this exe (RTTI/vtable/byte scan, read-only). **[V-comm]** used by working, released community code on 3.290.1. **[V-3.280]** community-verified on 3.280.0 only. **[U]** unverified guess.

## Main source: hitman-coop (working code that already does this)

`github.com/JordanRodrigues021/hitman-coop` (v0.14.0, 2026-11-12, ZHMModSDK v4.2.1, tested on 3.290.1). It copies whole skeleton poses between characters every frame (reads 47's pose, writes it onto an NPC stand-in, and corrects NPC root transforms). The files worth reading:
- `plugin/Src/PoseHook.cpp/.h`: the read/write hook (copy it).
- `plugin/Src/RigScan.cpp`: finds the parent-index table.
- `plugin/Src/BoneProbe.cpp`: memory dumps of morpheme/bone data.
- `plugin/Src/SdkWorld.cpp` ~l.695-775: the wiring (how it gets animator pointers).

The class layouts match Hitman Absolution's (from `pavledev/HitmanAbsolutionSDK2`, `include/Animation/*.h`), which has the real names for everything below.

## 1. Object chain from a character

```
ZHM5BaseCharacter (ZActor / ZHitman5)
  +0x88 TEntityRef<IMorphemeEntity> m_pMorphemeEntity          (SDK header)
        .m_entityRef.QueryInterface<IBoneAnimator>()  -> IBoneAnimator*  [V-comm]
ZMorphemeEntity layout:
  +0x00 ZEntityImpl vtbl     RVA 0x1F35928                            [V-exe]
  +0x88 IMorphemeEntity vtbl RVA 0x1F35860                            [V-exe]
  +0x90 IBoneAnimator vtbl   RVA 0x1F35830  (QueryInterface result)    [V-exe]
  +0xE0 SBoneAnimatorInfo.m_pBoneData (= IBoneAnimator+0x50)           [V-exe: UpdateBones reads [rcx+0x50]]
```
Every ZMorphemeEntity (47 and all NPCs) shares one vtable, so one patch covers every character. [V-comm]

Other IBoneAnimator implementers in this exe [V-exe]:
| class | IBoneAnimator subobject | vtable RVA | UpdateBones (slot 1) |
|---|---|---|---|
| ZMorphemeEntity | +0x90 | 0x1F35830 | HITMAN3.exe+0xFDDF70 |
| ZPhysicsSystemEntity (ragdoll/physics) | +0x20 | 0x1F40478 | +0x10C7FB0 |
| ZHM5Animator (47's `ZHitman5::m_Animator` @0x738) | +0x18 | 0x1E430B0 | +0x722CF0 |

## 2. The per-frame pose hook (read and write)

IBoneAnimator vtable (Absolution names, slot order matches the WoA behaviour) [V-comm for slot 1]:
```
0 ~IBoneAnimator
1 void UpdateBones(const ZAnimationBoneData* boneData, ZAnimationBonePose* outPose, const SMatrix& world, const SMatrix* unk)
2 bool TryCalculateBounds(...)
3 void SetBoneAnimatorInfo(const SBoneAnimatorInfo&)
```
Hook slot 1. Either patch the shared vtable entry the way hitman-coop does (no pattern needed), or detour the function with the SDK:

Signatures (each unique in .text of 3.290.1) [V-exe]:
- ZMorphemeEntity::UpdateBones: `48 8B C4 48 89 58 18 48 89 70 20 55 57 41 54 48 8D 68 ? 48 81 EC ? ? ? ? 0F 29 78 ? 4D 8B E1 48 8B 41 50`
- ZPhysicsSystemEntity::UpdateBones: `48 8B C4 56 41 55 41 57 48 81 EC ? ? ? ? 48 8B F1 4D 8B E8 48 8B 49 48 4C 8B FA`
- ZHM5Animator::UpdateBones: `4D 85 C0 0F 84 ? ? ? ? 4C 8B DC 55 56 41 55 41 56 49 8D 6B ? 48 81 EC ? ? ? ? 48 8D 41 E8`

Call the original first, then read or overwrite `outPose`. `this` is the IBoneAnimator* (entity+0x90), so compare it against `actor->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>()` to find out which character it is.
**It runs on the game's worker threads, in parallel.** Lock your shared buffers.

### Pose buffer (ZAnimationBonePose) [V-comm]
```
+0x00 vtbl (IComponentInterface)
+0x08 SQV*   m_pData
+0x10 uint32 m_nNumBones        (sanity: 0 < n < 1024)
+0x14 uint32 m_nAllocationSize  [U]
+0x18 bool m_bHasBoneScales, m_bBoundsCalculated [U]
SQV (32 B): float rot[4] (quat x,y,z,w), float pos[4] (x,y,z,pad)
```
- **Parent-relative (local)**. Bone 0's transform is in the character's object space. [V-comm]
- World pose = FK: `M_i = local_i * M_parent`, with `M_root_parent = world` (the `world` arg). Row vectors (DirectX style, translation in row 3). [V-comm: coop's root fix-up uses `root' = root * wanted * actual^-1`]
- `world` arg layout: SDK `SMatrix` is 4x4 rows. hitman-coop still detects 4x4 versus 4x3 at runtime (checks it is rigid and near the actor) and doesn't say which one it found. Copy its `WorldRows()` check or confirm it once with a log line. [U which one]
- Units: metres, Z-up (Glacier). [V-comm: coop pins with world-metre positions]
- When you write rotations, keep the quaternion sign on the same hemisphere as the value already there (`dot<0 → negate`). The engine blends after you, and a flipped sign turned an NPC upside down. [V-comm, `WriteRotation()`]
- If you write positions for every bone, the limb lengths change. coop writes positions only for bones 0 and 1 (root, pelvis) when the source skeleton differs. For a ragdoll driving its own skeleton, writing all of them is fine.

### World pose of the character without the hook
Use the SDK's `ZSpatialEntity::GetWorldMatrix()` on the actor's spatial entity, then do FK on the captured local pose. Capture the pose in the hook (keep a copy per animator) and read it on the main thread.

## 3. Ragdoll caveat (important for Delirium)

- When the game ragdolls a character (death, KO, ActivateRagdoll), the morpheme pose keeps animating "standing" and **is not what gets drawn**. Physics takes over. [V-comm: "an NPC's animation pose stays standing under [a ragdoll]"]
- Setting a ragdolled body's transform from outside does nothing ("matrix set once or every frame, ragdoll off or on … it never left the spot", tested on 3.290.1, 2026-11-03). Only `ZRagdollHandler_ApplyImpulseOnRagdoll` moves it. [V-comm]
- Recommended plan: keep the character out of the game's ragdoll and overwrite the full local pose in ZMorphemeEntity::UpdateBones with Delirium's output (converted to parent-relative SQV). To stop the game ragdolling it, detour `ZHM5BaseCharacter_ActivateRagdoll` / `ActivatePoweredRagdoll`. Their patterns are in SDK `Src/Functions.cpp`; they are `PATTERN_FUNCTION`, so wrap them in your own MinHook detour. [U]
- Other route: hook ZPhysicsSystemEntity::UpdateBones (signature above) and overwrite its output while ragdolled. How that entity maps to a character (via `ZHM5BaseCharacter::m_pRagdollHandler` @0x68?) is not yet worked out. [U]
- For 47: if overrides on the morpheme pose get stomped (IK/aim layers), also try ZHM5Animator::UpdateBones (`ZHitman5::m_Animator` @0x738). [U] The SDK's `ZHitmanMorphemePostProcessor_UpdateWeaponRecoil` hook (pattern `48 8B C4 4C 89 48 20 4C 89 40 18 F3 0F 11 48 10`) is a post-process on 47 only and gets `charboneMap` + `hierarchy` (parent indices). Useful for 47's parents, not for NPCs.

## 4. Rig: parents, bind pose, names

### Parents, at runtime
`ZAnimationBoneData* bd = *(void**)(animator + 0x50)` (= ZMorphemeEntity+0xE0). Then `int32* parents = *(int32**)(bd + 0x70)`, with `parents[0] == -1` and `parents[i] < i`. [V-3.280; coop's fast path, with a validated pointer-search fallback (`RigScan::FindParents`) that it uses on 3.290.1]
This is the **runtime** hierarchy. The game re-parents twist bones (`ZAnimationRig::ReparentTwistBones`), so it can differ from the file's parents. Use this one for FK.

### Rest of ZAnimationBoneData (Absolution layout projected onto WoA; parents@0x70 fits it) [U]
```
+0x00 vtbl
+0x08 bool  m_bIsInitialized
+0x0C int   m_nNumberOfDeformBones
+0x10 int   m_nNumberOfAnimatedBones
+0x14 uint  m_nBoneConstraintsSize
+0x18 SBoneConstraintsHeader*
+0x20 SBoneDefinition* m_pBoneDefinitions   -> points into the BORG data: names + file parents
+0x28 ZAnimationFacePoseCollection
+0x68 ZAnimationRig m_rig { vtbl;
   +0x70 ZAnimationBoneHierarchy { int* m_pParentIndexes; TArrayRef<int>; int* m_pParentListIndexes; TArrayRef<int>; bool }
   ~+0xA8 ZAnimationBonePose m_bindPose { vtbl; SQV* data (~+0xB0); uint32 count (~+0xB8); ... }
   ~+0xC8 TArray<ZString> m_boneNames
   ... m_aMeshIDtoGlobalID, object-space<->bind-pose-space SMatrix43 / SQV arrays }
```
`SBoneDefinition` (0x40 B, packed): `float center[3]; int32 parent; float size[3]; char name[34]; uint8/int16 bodyPart`. Check the runtime layout before trusting it: names must be printable `"Ground"/"Pelvis"/...` and count == pose count.

### From game files (reliable, offline)
The bone rig is resource type **BORG** (installer `ZAnimationBoneDataInstaller`). Extract it with RPKG Tool / GlacierKit. Parser: `pawREP/GlacierFormats` `src/BORG.cpp`. The layout:
```
u32 headerOffset → Header { boneCount, animatedBoneCount, boneDefinitionsOff, bindPoseOff,
                            bindPoseInvGlobalMatsOff, boneConstraintsOff, poseBoneHeaderOff,
                            invertGlobalBonesOff, boneMapOff }
SBoneDefinition[boneCount]  (above: name, parent, center, size, bodyPart)
SQV bindPose[boneCount]     (local quat xyzw + vec4 pos; parent-relative)
3x4 inverse global bind matrices[boneCount]
```
Which BORG a character uses: the outfit's `m_pBonesAndCollisionResource` (ZLinkedEntity) or the PRIM's `lBoneRigResourceIndex`. [U]

### Bone ids
SDK `Glacier/Enums.h` `BoneId_Enum` (145 ids: GROUND=0, PELVIS=1, SPINE=2 ... HEAD=7, LEFT_THIGH=8 ... PISTOL_HOLSTER_ATTACHER=143) is the standard biped id list, and it is what `ApplyImpulseOnRagdoll(boneIndex)` uses. Whether these ids equal rig indices for every outfit is [U]. Match by name from the BORG or the runtime names to be safe.

## 5. Minimal plugin code (from hitman-coop PoseHook, cut down)

```cpp
struct SQV { float rot[4]; float pos[4]; };              // quat xyzw, pos xyz_
using UpdateFn = void(*)(void*, const void*, uint8_t*, const void*, void*);
static UpdateFn g_Orig; static void** g_Vt;

static void Hk(void* anim, const void* boneData, uint8_t* pose, const void* world, void* a5) {
    g_Orig(anim, boneData, pose, world, a5);
    SQV* b = *(SQV**)(pose + 0x08); uint32_t n = *(uint32_t*)(pose + 0x10);
    if (!b || !n || n >= 1024) return;
    // READ: copy b[0..n) (local) + *(SMatrix*)world under a lock, keyed by `anim`
    // WRITE: if anim is ragdoll-driven, overwrite b[i].rot / b[i].pos with Delirium's
    //        parent-relative output (keep the quat hemisphere: if dot(old,new)<0 negate new)
}

void Install(ZActor* a) {   // any character; the vtable is shared
    void* anim = a->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();
    g_Vt = *(void***)anim; g_Orig = (UpdateFn)g_Vt[1];
    DWORD o; VirtualProtect(&g_Vt[1], 8, PAGE_READWRITE, &o);
    InterlockedExchangePointer(&g_Vt[1], (void*)&Hk);
    VirtualProtect(&g_Vt[1], 8, o, &o);
}
// parents: int32* p = *(int32_t**)(*(uint8_t**)((uint8_t*)anim + 0x50) + 0x70);
```
Restore `g_Vt[1]` when unloading. hitman-coop passes 7 args through, which also works. Absolution's signature has 4 args plus `this`.

## Still to confirm in-game
1. `world` arg: 4x4 or 4x3 (log it once).
2. ZAnimationBoneData +0x20 / bind pose / names offsets on 3.290.1.
3. Whether skipping ActivateRagdoll keeps morpheme in control of a dead/KO'd NPC, or whether the ZPhysicsSystemEntity hook is needed.
4. Whether BoneId_Enum equals rig index.

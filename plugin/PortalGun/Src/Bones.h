// Hitman's skeletons, read and written: a patch on ZMorphemeEntity's IBoneAnimator::UpdateBones (one vtable shared by
// 47 and every NPC; hitman-coop's way, see docs/hitman-bones.md). Its pose is each bone's turn and place under its
// parent (bone 0 under the character's object), metres, Z up: NM's own space.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class ZHM5BaseCharacter;
class ZHitman5;

namespace Bones {
    struct SQV {
        float q[4]; // x, y, z, w
        float p[4]; // x, y, z, -
    };

    // The character's IBoneAnimator (its key here); patches the vtable the first time. Null if it has none.
    void* Animator(ZHM5BaseCharacter* p_Character);

    // 47's own animator (ZHM5Animator): his drawn pose is its output, not his morpheme entity's.
    void* HitmanAnimator(ZHitman5* p_Hitman);

    // Hooks and tracks any IBoneAnimator (null if four vtables are patched already).
    void* Track(void* p_Anim);

    // Its last animated pose (before any override), false before the game has posed it.
    bool Read(void* p_Animator, std::vector<SQV>& p_Local);

    // A new game frame (passes are counted per frame).
    void Tick();
    std::vector<std::string> NamesUncached(const uint8_t* p_Data, int p_Bones, std::string& p_Where);

    // Its bones' parents (-1 for the first), checked; empty if they can't be found.
    std::vector<int32_t> Parents(void* p_Animator, int p_Bones);

    // The engine's world matrix for it (bone 0's parent) as axes and place; p_Layout 44 or 43 (rows). False: none.
    bool World(void* p_Animator, float* p_X, float* p_Y, float* p_Z, float* p_T, int& p_Layout);

    // Its bones' names, found by a checked scan of its bone data (p_Where says where); empty if not found.
    std::vector<std::string> Names(void* p_Animator, int p_Bones, std::string& p_Where);

    // Its bind pose (under each parent), found the same way; empty if not found.
    std::vector<SQV> BindPose(void* p_Animator, int p_Bones, std::string& p_Where);

    // Poses it with this from now on (same bone count as Read's); empty: its animation again.
    void Override(void* p_Animator, std::vector<SQV> p_Local);

    // Puts the vtable back (unloading).
    void Uninstall();
}

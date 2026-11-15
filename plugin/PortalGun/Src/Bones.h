// Hitman's skeletons, read and written: a patch on ZMorphemeEntity's IBoneAnimator::UpdateBones (one vtable shared by
// 47 and every NPC; hitman-coop's way, see docs/hitman-bones.md). Its pose is each bone's turn and place under its
// parent (bone 0 under the character's object), metres, Z up: NM's own space.
#pragma once

#include <cstdint>
#include <vector>

class ZHM5BaseCharacter;

namespace Bones {
    struct SQV {
        float q[4]; // x, y, z, w
        float p[4]; // x, y, z, -
    };

    // The character's IBoneAnimator (its key here); patches the vtable the first time. Null if it has none.
    void* Animator(ZHM5BaseCharacter* p_Character);

    // Its last animated pose (before any override), false before the game has posed it.
    bool Read(void* p_Animator, std::vector<SQV>& p_Local);

    // Its bones' parents (-1 for the first), checked; empty if they can't be found.
    std::vector<int32_t> Parents(void* p_Animator, int p_Bones);

    // Poses it with this from now on (same bone count as Read's); empty: its animation again.
    void Override(void* p_Animator, std::vector<SQV> p_Local);

    // Puts the vtable back (unloading).
    void Uninstall();
}

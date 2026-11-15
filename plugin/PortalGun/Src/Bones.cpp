#include "Bones.h"

#include <Windows.h>

#include <Glacier/ZHM5BaseCharacter.h>
#include <Glacier/ZPhysics.h>

#include <cmath>
#include <mutex>
#include <unordered_map>

namespace {
    using UpdateFn = void(*)(void*, const void*, uint8_t*, const void*, void*, void*, void*);

    struct State {
        std::vector<Bones::SQV> m_Anim;
        std::vector<Bones::SQV> m_Override;
    };

    // ponytail: one lock over every character's pose (the hook runs on several workers); per-animator locks if it shows.
    std::mutex g_Mutex;
    std::unordered_map<void*, State> g_States;
    void** g_Vt = nullptr;
    UpdateFn g_Orig = nullptr;

    // Readable without faulting (the scans below poke at guessed layouts).
    bool Readable(const void* p, size_t n) {
        MEMORY_BASIC_INFORMATION m {};

        if (!p || !VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(m.Protect & (PAGE_READONLY | PAGE_READWRITE |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)))
            return false;

        return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(m.BaseAddress) + m.RegionSize;
    }

    void Hook(void* p_This, const void* p_Data, uint8_t* p_Pose, const void* p_World, void* a5, void* a6, void* a7) {
        g_Orig(p_This, p_Data, p_Pose, p_World, a5, a6, a7);

        if (!p_Pose)
            return;

        auto* s_Bones = *reinterpret_cast<Bones::SQV**>(p_Pose + 0x08);
        const uint32_t n = *reinterpret_cast<uint32_t*>(p_Pose + 0x10);

        if (!s_Bones || !n || n >= 1024)
            return;

        std::scoped_lock s_Lock(g_Mutex);
        const auto it = g_States.find(p_This);

        if (it == g_States.end())
            return;

        it->second.m_Anim.assign(s_Bones, s_Bones + n);

        if (it->second.m_Override.size() != n)
            return;

        for (uint32_t i = 0; i < n; ++i) {
            Bones::SQV s_New = it->second.m_Override[i];
            const float* o = s_Bones[i].q;

            // On the old turn's side (the engine blends after this; a flipped sign turns a body upside down).
            if (o[0] * s_New.q[0] + o[1] * s_New.q[1] + o[2] * s_New.q[2] + o[3] * s_New.q[3] < 0.f)
                for (float& c : s_New.q)
                    c = -c;

            s_New.p[3] = s_Bones[i].p[3];
            s_Bones[i] = s_New;
        }
    }

    uint8_t* BoneData(void* p_Animator) {
        auto* s_Data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(p_Animator) + 0x50);
        return Readable(s_Data, 0x200) ? s_Data : nullptr;
    }
}

void* Bones::Animator(ZHM5BaseCharacter* p_Character) {
    if (!p_Character || !p_Character->m_pMorphemeEntity.m_entityRef)
        return nullptr;

    void* s_Anim = p_Character->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();

    if (!s_Anim)
        return nullptr;

    std::scoped_lock s_Lock(g_Mutex);

    if (!g_Vt) {
        g_Vt = *static_cast<void***>(s_Anim);
        g_Orig = static_cast<UpdateFn>(g_Vt[1]);
        DWORD s_Old;
        VirtualProtect(&g_Vt[1], sizeof(void*), PAGE_READWRITE, &s_Old);
        InterlockedExchangePointer(&g_Vt[1], reinterpret_cast<void*>(&Hook));
        VirtualProtect(&g_Vt[1], sizeof(void*), s_Old, &s_Old);
    }

    // Another class's animator (not the shared vtable): not ours to read.
    if (*static_cast<void***>(s_Anim) != g_Vt)
        return nullptr;

    g_States.try_emplace(s_Anim);
    return s_Anim;
}

bool Bones::Read(void* p_Animator, std::vector<SQV>& p_Local) {
    std::scoped_lock s_Lock(g_Mutex);
    const auto it = g_States.find(p_Animator);

    if (it == g_States.end() || it->second.m_Anim.empty())
        return false;

    p_Local = it->second.m_Anim;
    return true;
}

std::vector<int32_t> Bones::Parents(void* p_Animator, int p_Bones) {
    const uint8_t* s_Data = BoneData(p_Animator);

    if (!s_Data || p_Bones <= 0)
        return {};

    const auto s_Valid = [&](const int32_t* p) {
        if (!Readable(p, p_Bones * sizeof(int32_t)) || p[0] != -1)
            return false;

        for (int i = 1; i < p_Bones; ++i)
            if (p[i] < 0 || p[i] >= i)
                return false;

        return true;
    };

    // 3.280's place first, then any pointer in the bone data that holds such a table (hitman-coop's fallback).
    const auto s_At = [&](size_t o) { return *reinterpret_cast<const int32_t* const*>(s_Data + o); };

    if (s_Valid(s_At(0x70)))
        return {s_At(0x70), s_At(0x70) + p_Bones};

    for (size_t o = 0; o + 8 <= 0x200; o += 8)
        if (s_Valid(s_At(o)))
            return {s_At(o), s_At(o) + p_Bones};

    return {};
}

void Bones::Override(void* p_Animator, std::vector<SQV> p_Local) {
    std::scoped_lock s_Lock(g_Mutex);

    if (const auto it = g_States.find(p_Animator); it != g_States.end())
        it->second.m_Override = std::move(p_Local);
}

void Bones::Uninstall() {
    std::scoped_lock s_Lock(g_Mutex);

    if (!g_Vt)
        return;

    DWORD s_Old;
    VirtualProtect(&g_Vt[1], sizeof(void*), PAGE_READWRITE, &s_Old);
    InterlockedExchangePointer(&g_Vt[1], reinterpret_cast<void*>(g_Orig));
    VirtualProtect(&g_Vt[1], sizeof(void*), s_Old, &s_Old);
    g_Vt = nullptr;
    g_States.clear();
}

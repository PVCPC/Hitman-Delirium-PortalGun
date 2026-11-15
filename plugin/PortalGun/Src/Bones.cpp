#include "Bones.h"

#include <Windows.h>

#include <Glacier/ZHM5BaseCharacter.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZPhysics.h>

#include <cmath>
#include <cstring>
#include <string>
#include <mutex>
#include <format>
#include <unordered_map>
#include <atomic>
#include <Logging.h>
#include "PortalLog.h"

#define Logger PortalLog

namespace {
    using UpdateFn = void(*)(void*, const void*, uint8_t*, const void*, void*, void*, void*);

    std::atomic<uint64_t> g_Frame {0};

    struct State {
        // UpdateBones runs more than once a frame for a character (the base, then upper-body / aim passes). Each
        // pass by its order in the frame; m_Anim is always the same pass (the last one), never whichever ran last.
        std::vector<std::vector<Bones::SQV>> m_Passes;
        uint64_t m_Frame = 0;
        int m_Count = 0;     // passes so far this frame
        int m_LastCount = 0; // passes last frame
        int m_Logged = -1;
        std::vector<Bones::SQV> m_Anim;
        std::vector<Bones::SQV> m_Override;
        float m_World[16] = {};
        bool m_HasWorld = false;
    };

    // The world argument's 16 floats (it may be a 4x3 of 12: the 4 after it are read and ignored then).
    bool CopyWorld(const void* p, float* p_Out) {
        __try {
            std::memcpy(p_Out, p, 16 * sizeof(float));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool Rigid(const float* x, const float* y, const float* z, const float* t) {
        const auto s_Dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

        for (const float* v : {x, y, z, t})
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(v[i]) || std::abs(v[i]) > 1e6f)
                    return false;

        return std::abs(s_Dot(x, x) - 1.f) < 0.05f && std::abs(s_Dot(y, y) - 1.f) < 0.05f &&
            std::abs(s_Dot(z, z) - 1.f) < 0.05f && std::abs(s_Dot(x, y)) < 0.05f && std::abs(s_Dot(y, z)) < 0.05f &&
            std::abs(s_Dot(x, z)) < 0.05f;
    }

    // ponytail: one lock over every character's pose (the hook runs on several workers); per-animator locks if it shows.
    std::mutex g_Mutex;
    std::unordered_map<void*, State> g_States;
    // The patched vtables (the NPCs' and 47's morpheme entity, 47's own animator) and their UpdateBones.
    struct Patched {
        void** m_Vt = nullptr;
        UpdateFn m_Orig = nullptr;
    };
    Patched g_Patched[4];

    UpdateFn OrigOf(void* p_This) {
        void** s_Vt = *static_cast<void***>(p_This);

        for (const Patched& p : g_Patched)
            if (p.m_Vt == s_Vt)
                return p.m_Orig;

        return nullptr;
    }

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
        OrigOf(p_This)(p_This, p_Data, p_Pose, p_World, a5, a6, a7);

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

        State& st = it->second;

        if (st.m_Frame != g_Frame) {
            st.m_Frame = g_Frame;

            if (st.m_Count > 0)
                st.m_LastCount = st.m_Count;

            st.m_Count = 0;
        }

        const int s_Pass = st.m_Count++;

        if (st.m_Passes.size() <= static_cast<size_t>(s_Pass))
            st.m_Passes.resize(s_Pass + 1);

        st.m_Passes[s_Pass].assign(s_Bones, s_Bones + n);

        // Which passes there are, once per change: how many bones each later pass turns off the first.
        if (st.m_LastCount != st.m_Logged && st.m_LastCount > 0 && s_Pass == st.m_LastCount - 1) {
            st.m_Logged = st.m_LastCount;
            std::string s_Diff;

            for (int k = 1; k < st.m_LastCount && k < static_cast<int>(st.m_Passes.size()); ++k) {
                int s_Off = 0;
                std::string s_Which;

                for (uint32_t i = 0; i < n && i < st.m_Passes[0].size() && i < st.m_Passes[k].size(); ++i) {
                    const float* a = st.m_Passes[0][i].q;
                    const float* b = st.m_Passes[k][i].q;

                    if (std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0.996f) { // ~10 degrees
                        if (++s_Off <= 12)
                            s_Which += std::format("{} ", i);
                    }
                }

                s_Diff += std::format("pass {}: {} bones off pass 0 ({}); ", k, s_Off, s_Which);
            }

            Logger::Info("[Bones] {}: {} UpdateBones passes a frame. {}", p_This, st.m_LastCount, s_Diff);
        }

        const int s_Use = st.m_LastCount > 0 ? st.m_LastCount - 1 : 0;

        if (s_Pass == s_Use || st.m_Anim.size() != n) {
            st.m_Anim.assign(s_Bones, s_Bones + n);

            if (p_World)
                st.m_HasWorld = CopyWorld(p_World, st.m_World);
        }

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

// Patches its vtable's UpdateBones (once per vtable) and tracks it.
void* Bones::Track(void* p_Anim) {
    if (!p_Anim)
        return nullptr;

    std::scoped_lock s_Lock(g_Mutex);
    void** s_Vt = *static_cast<void***>(p_Anim);
    bool s_Known = false;

    for (Patched& p : g_Patched) {
        if (p.m_Vt == s_Vt) {
            s_Known = true;
            break;
        }

        if (!p.m_Vt) {
            p.m_Orig = static_cast<UpdateFn>(s_Vt[1]);
            p.m_Vt = s_Vt;
            DWORD s_Old;
            VirtualProtect(&s_Vt[1], sizeof(void*), PAGE_READWRITE, &s_Old);
            InterlockedExchangePointer(&s_Vt[1], reinterpret_cast<void*>(&Hook));
            VirtualProtect(&s_Vt[1], sizeof(void*), s_Old, &s_Old);
            s_Known = true;
            break;
        }
    }

    if (!s_Known)
        return nullptr;

    g_States.try_emplace(p_Anim);
    return p_Anim;
}

void* Bones::Animator(ZHM5BaseCharacter* p_Character) {
    if (!p_Character || !p_Character->m_pMorphemeEntity.m_entityRef)
        return nullptr;

    return Track(p_Character->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>());
}

void* Bones::HitmanAnimator(ZHitman5* p_Hitman) {
    return p_Hitman && p_Hitman->m_Animator ? Track(p_Hitman->m_Animator.QueryInterface<IBoneAnimator>()) : nullptr;
}

void Bones::Tick() {
    ++g_Frame;
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

    for (Patched& p : g_Patched) {
        if (!p.m_Vt)
            continue;

        DWORD s_Old;
        VirtualProtect(&p.m_Vt[1], sizeof(void*), PAGE_READWRITE, &s_Old);
        InterlockedExchangePointer(&p.m_Vt[1], reinterpret_cast<void*>(p.m_Orig));
        VirtualProtect(&p.m_Vt[1], sizeof(void*), s_Old, &s_Old);
        p = {};
    }

    g_States.clear();
}

bool Bones::World(void* p_Animator, float* p_X, float* p_Y, float* p_Z, float* p_T, int& p_Layout) {
    float w[16];
    {
        std::scoped_lock s_Lock(g_Mutex);
        const auto it = g_States.find(p_Animator);

        if (it == g_States.end() || !it->second.m_HasWorld)
            return false;

        std::memcpy(w, it->second.m_World, sizeof(w));
    }

    // 4x4 rows (w column 0), else 4x3 rows (hitman-coop's check).
    if (std::abs(w[3]) < 0.01f && std::abs(w[7]) < 0.01f && std::abs(w[11]) < 0.01f && Rigid(w, w + 4, w + 8, w + 12)) {
        std::memcpy(p_X, w, 12), std::memcpy(p_Y, w + 4, 12), std::memcpy(p_Z, w + 8, 12), std::memcpy(p_T, w + 12, 12);
        p_Layout = 44;
        return true;
    }

    if (Rigid(w, w + 3, w + 6, w + 9)) {
        std::memcpy(p_X, w, 12), std::memcpy(p_Y, w + 3, 12), std::memcpy(p_Z, w + 6, 12), std::memcpy(p_T, w + 9, 12);
        p_Layout = 43;
        return true;
    }

    return false;
}

std::vector<std::string> Bones::Names(void* p_Animator, int p_Bones, std::string& p_Where) {
    const uint8_t* s_Data = BoneData(p_Animator);

    if (!s_Data || p_Bones <= 0)
        return {};

    // One rig, one list (every NPC in the same skeleton shares its bone data).
    static std::unordered_map<const void*, std::pair<std::vector<std::string>, std::string>> s_Cache;

    if (const auto it = s_Cache.find(s_Data); it != s_Cache.end() && static_cast<int>(it->second.first.size()) == p_Bones) {
        p_Where = it->second.second;
        return it->second.first;
    }

    std::vector<std::string> s_Found = NamesUncached(s_Data, p_Bones, p_Where);

    if (!s_Found.empty())
        s_Cache[s_Data] = {s_Found, p_Where};

    return s_Found;
}

std::vector<std::string> Bones::NamesUncached(const uint8_t* s_Data, int p_Bones, std::string& p_Where) {
    const auto s_Name = [](const char* c, size_t p_Max, size_t p_Len, bool p_Checked = false) -> std::string {
        if (!p_Checked && !Readable(c, p_Max))
            return {};

        size_t n = 0;

        while (n < p_Max && c[n] >= 0x20 && c[n] < 0x7f)
            ++n;

        if (n == 0 || n == p_Max || c[n] != 0 || (p_Len && n != p_Len))
            return {};

        return std::string(c, n);
    };

    // SBoneDefinition[] (0x40 each, the name at 0x1C), as Absolution's; or a TArray<ZString> (16 each: size, chars).
    for (size_t o = 0; o + 8 <= 0x200; o += 8) {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(s_Data + o);

        if (!Readable(p, 16))
            continue;

        for (const auto& [s_Stride, s_At] : {std::pair<size_t, size_t> {0x40, 0x1C}, {0x40, 0x20}, {0x44, 0x1C}}) {
            if (!Readable(p, s_Stride * p_Bones))
                continue;

            std::vector<std::string> s_Out;

            for (int i = 0; i < p_Bones; ++i) {
                std::string s = s_Name(reinterpret_cast<const char*>(p + i * s_Stride + s_At),
                    std::min<size_t>(34, s_Stride * p_Bones - i * s_Stride - s_At), 0, true);

                if (s.empty())
                    break;

                s_Out.push_back(std::move(s));
            }

            if (static_cast<int>(s_Out.size()) == p_Bones) {
                p_Where = std::format("bone data +0x{:X}: definitions, stride 0x{:X}, name at 0x{:X}", o, s_Stride, s_At);
                return s_Out;
            }
        }

        if (Readable(p, 16 * p_Bones)) {
            std::vector<std::string> s_Out;

            for (int i = 0; i < p_Bones; ++i) {
                const uint32_t s_Len = *reinterpret_cast<const uint32_t*>(p + i * 16) & 0x3FFFFFFF;
                const char* c = *reinterpret_cast<const char* const*>(p + i * 16 + 8);
                std::string s = s_Len && s_Len < 64 ? s_Name(c, s_Len + 1, s_Len) : std::string();

                if (s.empty())
                    break;

                s_Out.push_back(std::move(s));
            }

            if (static_cast<int>(s_Out.size()) == p_Bones) {
                p_Where = std::format("bone data +0x{:X}: ZString array", o);
                return s_Out;
            }
        }
    }

    return {};
}

std::vector<Bones::SQV> Bones::BindPose(void* p_Animator, int p_Bones, std::string& p_Where) {
    const uint8_t* s_Data = BoneData(p_Animator);

    if (!s_Data || p_Bones <= 0)
        return {};

    // A ZAnimationBonePose in the rig (Absolution: ~0xA8): its bones then its count, the count the pose's.
    for (size_t o = 0x28; o + 16 <= 0x200; o += 8) {
        const auto* b = *reinterpret_cast<const SQV* const*>(s_Data + o);
        const uint32_t n = *reinterpret_cast<const uint32_t*>(s_Data + o + 8);

        if (n != static_cast<uint32_t>(p_Bones) || !Readable(b, n * sizeof(SQV)))
            continue;

        bool s_Ok = true;

        for (int i = 0; i < p_Bones && s_Ok; ++i) {
            const float* q = b[i].q;
            const float s_Len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            s_Ok = std::abs(s_Len - 1.f) < 0.01f && std::abs(b[i].p[0]) < 5.f && std::abs(b[i].p[1]) < 5.f &&
                std::abs(b[i].p[2]) < 5.f;
        }

        if (s_Ok) {
            p_Where = std::format("bone data +0x{:X}", o);
            return {b, b + p_Bones};
        }
    }

    return {};
}

#include "Delirium.h"

#include <Windows.h>
#include "Hooks.h"
#include <Glacier/ZActor.h>
#include <Glacier/ZOutfit.h>
#include <Glacier/ZSpatialEntity.h>
#include <Glacier/ZResource.h>
#include <Glacier/ZEntity.h>
#include <Glacier/ZPhysics.h>
#include <Glacier/ZModule.h>
#include <Glacier/SExternalReferences.h>
#include <Glacier/EntityFactory.h>
#include <Globals.h>
#include <Functions.h>
#include <intrin.h>

#include "PortalLog.h"
#include <delirium_portal.h>
#define Logger PortalLog

#include "IRenderer.h"


#include <imgui.h>
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <unordered_set>

#include "gta4nm.h"

#include <chrono>

// Logs any of our steps that takes long enough to hitch a frame (what the hitches are).
struct HitchTimer {
    const char* m_What;
    std::chrono::steady_clock::time_point m_Start = std::chrono::steady_clock::now();
    ~HitchTimer() {
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_Start).count();

        if (ms > 4.f)
            Logger::Info("[Hitch] {} took {:.1f} ms.", m_What, ms);
    }
};

struct Laps {
    const std::string& m_Who;
    std::chrono::steady_clock::time_point m_Start = std::chrono::steady_clock::now(), m_Last = m_Start;
    std::string m_Out;
    void operator()(const char* p_What) {
        const auto s_Now = std::chrono::steady_clock::now();
        m_Out += std::format("{} {:.1f}, ", p_What, std::chrono::duration<float, std::milli>(s_Now - m_Last).count());
        m_Last = s_Now;
    }
    ~Laps() {
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_Start).count();

        if (ms > 4.f)
            Logger::Info("[Hitch] {}'s body: {:.1f} ms ({}rest {:.1f}).", m_Who, ms, m_Out,
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_Last).count());
    }
};

namespace {
    constexpr float c_Step = 1.f / 60.f;
    constexpr int c_Parts = 21;
    constexpr int c_MaxPeds = 8;
    constexpr int c_Chest = 10;
    constexpr int c_MaxNpcs = 6;
    constexpr int c_MaxMeshes = 32;
    constexpr float c_GetUp = 1.2f; // s from lying to the animation

    SVector3 V(const float* p) { return SVector3(p[0], p[1], p[2]); }

    // q (x, y, z, w) applied to v.
    SVector3 Rotate(const float* q, const SVector3& v) {
        const SVector3 u(q[0], q[1], q[2]);
        const float w = q[3];
        const SVector3 t = u.Cross(v) * 2.f;
        return v + t * w + u.Cross(t);
    }

    struct Q {
        float x, y, z, w;
    };

    Q Mul(const Q& a, const Q& b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    Q Conj(const Q& a) { return {-a.x, -a.y, -a.z, a.w}; }

    Q Norm(const Q& a) {
        const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
        return l > 1e-6f ? Q {a.x / l, a.y / l, a.z / l, a.w / l} : Q {0.f, 0.f, 0.f, 1.f};
    }

    SVector3 Rot(const Q& q, const SVector3& v) {
        const float f[4] = {q.x, q.y, q.z, q.w};
        return Rotate(f, v);
    }

    // The turn of a matrix whose rows are its X, Y and Z axes (a point goes to x X + y Y + z Z + T).
    Q FromAxes(const float4& p_X, const float4& p_Y, const float4& p_Z) {
        const SVector3 x = SVector3(p_X.x, p_X.y, p_X.z).Normalized();
        const SVector3 y = SVector3(p_Y.x, p_Y.y, p_Y.z).Normalized();
        const SVector3 z = SVector3(p_Z.x, p_Z.y, p_Z.z).Normalized();
        // m[r][c]: column c is axis c.
        const float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
        const float t = m00 + m11 + m22;
        Q q;

        if (t > 0.f) {
            const float s = std::sqrt(t + 1.f) * 2.f;
            q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
        }
        else if (m00 > m11 && m00 > m22) {
            const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
            q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
        }
        else if (m11 > m22) {
            const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
            q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
        }
        else {
            const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
            q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
        }

        return Norm(q);
    }

    float Facing(const SVector3& p_Left, const SVector3& p_Right) {
        const SVector3 r = p_Right - p_Left;
        return std::atan2(r.x, -r.y); // forward = up x right
    }

    // The characters Delirium drives: the game's own ragdoll (death, knockouts) is kept off them, NM's pose is shown.
    std::mutex g_OwnedMutex;
    std::unordered_set<void*> g_Owned;
    std::unordered_set<void*> g_OwnedNpcs;
    void (*g_Ragdoll)(ZHM5BaseCharacter*, bool) = nullptr;
    void (*g_Powered)(ZHM5BaseCharacter*, float, bool, bool, float, bool) = nullptr;
    void* g_RagdollAt = nullptr;
    void* g_PoweredAt = nullptr;
    void (*g_Deactivate)(ZHM5BaseCharacter*) = nullptr;
    void* g_DeactivateAt = nullptr;
    // NPCs whose NM body is down: the game may not take its ragdoll off them (Billy: its AI then got up on its own,
    // the green skeleton walked and talked on while Homer's body lay there). Only Delirium's own get-up does.
    std::unordered_set<void*> g_Down;
    thread_local bool t_OurDeactivate = false;

    bool Owned(void* p_Char) {
        std::scoped_lock s_Lock(g_OwnedMutex);
        return g_Owned.contains(p_Char);
    }

    void Own(void* p_Char, bool p_On, bool p_Npc) {
        std::scoped_lock s_Lock(g_OwnedMutex);

        if (p_On)
            g_Owned.insert(p_Char);
        else
            g_Owned.erase(p_Char);

        if (p_On && p_Npc)
            g_OwnedNpcs.insert(p_Char);
        else
            g_OwnedNpcs.erase(p_Char);
    }

    // ponytail: a killed NPC gets the game's death ragdoll (skipping it crashed on the shot, null +8 in HITMAN3);
    // the dead body's look is then the game's, not NM's.

    std::unordered_set<void*> g_GameTook; // NPCs the game ragdolled itself (Delirium lets them go)

    void ClearHeld();

    bool GameRagdolls(void* c) {
        std::scoped_lock s_Lock(g_OwnedMutex);

        if (!g_OwnedNpcs.contains(c))
            return !g_Owned.contains(c);

        g_GameTook.insert(c);
        return true;
    }

    bool GameTook(void* c) {
        std::scoped_lock s_Lock(g_OwnedMutex);
        return g_GameTook.erase(c) > 0;
    }

    int ReadRagdoll(void* p_Char, void** p_Set, void** p_Bodies, int* p_Bones);

    // Each owned NPC's ragdoll bodies as the game put them (on its bones, before a step moved them): Follow's offsets.
    std::unordered_map<void*, std::vector<SMatrix>> g_RagdollStart;

    void HkRagdoll(ZHM5BaseCharacter* c, bool p_InMotion) {
        if (!GameRagdolls(c))
            return;

        g_Ragdoll(c, p_InMotion);
        std::scoped_lock s_Lock(g_OwnedMutex);

        if (!g_GameTook.contains(c))
            return;

        void* s_Bodies[64];
        int s_Bones[64];
        void* s_Set = nullptr;
        const int n = ReadRagdoll(c, &s_Set, s_Bodies, s_Bones);
        std::vector<SMatrix>& s_Start = g_RagdollStart[c];
        s_Start.clear();

        for (int i = 0; i < n; ++i)
            s_Start.push_back(static_cast<IPhysicsObject*>(s_Bodies[i])->GetTransform());
    }

    // The game's own ragdoll is on (a kill, a shove): its handler's ragdoll has bodies.
    bool GameRagdollOn(void* c) {
        void* s_Bodies[64];
        int s_Bones[64];
        void* s_Set = nullptr;
        return ReadRagdoll(c, &s_Set, s_Bodies, s_Bones) > 0;
    }

    void HkDeactivate(ZHM5BaseCharacter* c) {
        {
            std::scoped_lock s_Lock(g_OwnedMutex);

            if (!t_OurDeactivate && g_Down.contains(c)) {
                static int s_Logs = 0;

                if (s_Logs++ < 5)
                    Logger::Info("[Delirium] The game tried to take its ragdoll off an NPC whose body is down; kept on.");

                return;
            }
        }

        g_Deactivate(c);
    }

    void Down(void* c, bool p_On) {
        std::scoped_lock s_Lock(g_OwnedMutex);
        p_On ? (void) g_Down.insert(c) : (void) g_Down.erase(c);
    }

    // Delirium's own calls to the game's deactivate (the get-up), past HkDeactivate.
    void OurDeactivate(ZHM5BaseCharacter* c) {
        if (!g_DeactivateAt)
            return;

        t_OurDeactivate = true;
        (g_Deactivate ? g_Deactivate : reinterpret_cast<void (*)(ZHM5BaseCharacter*)>(g_DeactivateAt))(c);
        t_OurDeactivate = false;
    }

    std::vector<SMatrix> TakeRagdollStart(void* c) {
        std::scoped_lock s_Lock(g_OwnedMutex);
        const auto it = g_RagdollStart.find(c);

        if (it == g_RagdollStart.end())
            return {};

        std::vector<SMatrix> s_Out = std::move(it->second);
        g_RagdollStart.erase(it);
        return s_Out;
    }

    void HkPowered(ZHM5BaseCharacter* c, float t, bool p_InMotion, bool p_Upper, float a5, bool a6) {
        if (GameRagdolls(c))
            g_Powered(c, t, p_InMotion, p_Upper, a5, a6);
    }

    // Glacier's stored bone turns may be the inverse of a standard (Hamilton, world = parent * local) quaternion: the
    // spine and standing legs look right either way, the arms and a run's legs don't. Found per game by a check at
    // the first body (feet on the floor, head at head height, the bind pose's hands level), logged.
    bool g_Conj = true;
    bool g_ConjKnown = true;

    // World frames (7 floats each) from parent-relative bones under a root.
    void Fk(const std::vector<Bones::SQV>& p_L, const std::vector<int32_t>& p_Parents, Q p_RootQ, SVector3 p_RootP, bool p_Conj,
        std::vector<float>& p_Out) {
        p_Out.resize(p_L.size() * 7);

        for (size_t i = 0; i < p_L.size(); ++i) {
            const int s_Parent = p_Parents[i];
            const float* w = s_Parent >= 0 ? &p_Out[s_Parent * 7] : nullptr;
            const Q s_Pq = w ? Q {w[3], w[4], w[5], w[6]} : p_RootQ;
            const SVector3 s_Pp = w ? V(w) : p_RootP;
            const Bones::SQV& l = p_L[i];
            const Q s_Lq = p_Conj ? Q {-l.q[0], -l.q[1], -l.q[2], l.q[3]} : Q {l.q[0], l.q[1], l.q[2], l.q[3]};
            const Q q = Norm(Mul(s_Pq, s_Lq));
            const SVector3 p = s_Pp + Rot(s_Pq, V(l.p));
            float* o = &p_Out[i * 7];
            o[0] = p.x, o[1] = p.y, o[2] = p.z, o[3] = q.x, o[4] = q.y, o[5] = q.z, o[6] = q.w;
        }
    }

    // Which of a bone's axes runs along the line to its child, and how far off (the rest and the live pose should agree:
    // a rest pose whose turns aren't the live pose's convention shows a different axis or angle).
    std::string AlongAxis(const float* p_Bone, const float* p_Child) {
        const SVector3 s_Line = (V(p_Child) - V(p_Bone)).Normalized();
        std::string s;

        for (int k = 0; k < 3; ++k) {
            const SVector3 s_A = Rotate(p_Bone + 3, SVector3(k == 0, k == 1, k == 2));
            s += std::format("{}{:+.2f} ", "XYZ"[k], s_A.Dot(s_Line));
        }

        return s;
    }

    // An EngineFunction's address (its only member).
    void* AddressOf(const void* p_Function) { return p_Function ? *static_cast<void* const*>(p_Function) : nullptr; }

    // A hook by a 14-byte absolute jump. MinHook needs free memory within 2 GB of the exe for its relay; in Billy's
    // run there was none (all three hooks failed, 9), so the game's ragdoll ran on NPCs Delirium drives. The first
    // `p_Len` bytes (whole instructions, none rip-relative: checked against `p_Bytes`) go to the trampoline.
    void* AbsHook(void* p_At, const uint8_t* p_Bytes, size_t p_Len, void* p_Detour) {
        auto* a = static_cast<uint8_t*>(p_At);

        if (!a || (reinterpret_cast<uintptr_t>(a) & 15) || p_Len < 14 || p_Len > 32 || std::memcmp(a, p_Bytes, p_Len) != 0)
            return nullptr;

        const auto s_Jmp = [](uint8_t* p, const void* p_To) {
            p[0] = 0xFF, p[1] = 0x25;
            std::memset(p + 2, 0, 4);
            std::memcpy(p + 6, &p_To, 8);
        };
        auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        DWORD s_Prot;

        if (!t || !VirtualProtect(a, 16, PAGE_EXECUTE_READWRITE, &s_Prot))
            return nullptr;

        std::memcpy(t, a, p_Len);
        s_Jmp(t + p_Len, a + p_Len);
        FlushInstructionCache(GetCurrentProcess(), t, 64);

        // One 16-byte write: no thread sees half a jump.
        alignas(16) long long s_Old[2], s_New[2];
        std::memcpy(s_Old, a, 16);
        std::memcpy(s_New, a, 16);
        s_Jmp(reinterpret_cast<uint8_t*>(s_New), p_Detour);
        const bool s_Ok = _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(a), s_New[1], s_New[0], s_Old);
        VirtualProtect(a, 16, s_Prot, &s_Prot);
        FlushInstructionCache(GetCurrentProcess(), a, 16);
        return s_Ok ? t : nullptr;
    }

    // Back as it was: its first 14 bytes are the trampoline's (the next two were never changed).
    void AbsUnhook(void* p_At, const void* p_Tramp) {
        DWORD s_Prot;

        if (!p_At || !p_Tramp || !VirtualProtect(p_At, 16, PAGE_EXECUTE_READWRITE, &s_Prot))
            return;

        alignas(16) long long s_Now[2], s_Was[2];
        std::memcpy(s_Now, p_At, 16);
        std::memcpy(s_Was, s_Now, 16);
        std::memcpy(s_Was, p_Tramp, 14);
        _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(p_At), s_Was[1], s_Was[0], s_Now);
        VirtualProtect(p_At, 16, s_Prot, &s_Prot);
        FlushInstructionCache(GetCurrentProcess(), p_At, 16);
    }

    std::string LastError() {
        char s_Buf[512] = {};
        nm_last_error(s_Buf, sizeof(s_Buf));
        return s_Buf;
    }

    bool KeyPressed(int p_Key, bool& p_Was) {
        DWORD s_Pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &s_Pid);
        const bool s_Down = s_Pid == GetCurrentProcessId() && (GetAsyncKeyState(p_Key) & 0x8000);
        const bool s_Pressed = s_Down && !p_Was;
        p_Was = s_Down;
        return s_Pressed;
    }

    // The trainer's behaviours: what they're called in the menu and what they send.
    constexpr const char* c_Actions[] = {
        "Shot (pistol)", "Shot (rifle)", "Shot (shotgun)", "Push", "Punch", "Explosion", "Taser", "Set on fire",
        "Near miss", "Flinch", "Balance", "Catch fall", "High fall", "Fall down a ledge", "Roll up", "Jump", "Dive",
        "Go limp", "Stop", "Stand up again",
    };
}

void Delirium::Configure(const std::filesystem::path& p_Dir, const std::string& p_Gta4) {
    m_Dir = p_Dir;
    m_Gta4 = p_Gta4;

    // ponytail: dead NPCs with a body then have no game ragdoll at all (dragging and dumping them may look off).
    if (!g_RagdollAt) {
        g_RagdollAt = AddressOf(Functions::ZHM5BaseCharacter_ActivateRagdoll);
        g_PoweredAt = AddressOf(Functions::ZHM5BaseCharacter_ActivatePoweredRagdoll);

        // Their first whole instructions (HITMAN3+fdbf0 / +fe640): mov [rsp+8],rcx; push rbx; push rbp; sub rsp,e8 and
        // push rbp; sub rsp,50; mov [rsp+60],rbx; mov rbp,rcx.
        static constexpr uint8_t c_Act[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x55, 0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00};
        static constexpr uint8_t c_Pow[] = {0x40, 0x55, 0x48, 0x83, 0xec, 0x50, 0x48, 0x89, 0x5c, 0x24, 0x60, 0x48, 0x8b, 0xe9};
        g_Ragdoll = reinterpret_cast<decltype(g_Ragdoll)>(AbsHook(g_RagdollAt, c_Act, sizeof(c_Act), reinterpret_cast<void*>(&HkRagdoll)));
        g_Powered = reinterpret_cast<decltype(g_Powered)>(AbsHook(g_PoweredAt, c_Pow, sizeof(c_Pow), reinterpret_cast<void*>(&HkPowered)));

        // Not the SDK's ZHM5BaseCharacter_DeactivateRagdoll: its pattern is ActivateRagdoll's (same bytes, same
        // address), so every "deactivate" so far switched the game's ragdoll ON (the get-up's end knocking NPCs back
        // down, the body off its character after). The real one (ragdoll +0x30 off, animator state 0), checked by
        // its first bytes.
        {
            static constexpr uint8_t c_Deact[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0x68, 0x48, 0x8b,
                0xd9, 0x48, 0x8b, 0x88, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x1f, 0x83, 0xb8, 0x40, 0x01, 0x00,
                0x00, 0x00, 0x75, 0x16};
            auto* s_At = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + 0xfd120;
            g_DeactivateAt = std::memcmp(s_At, c_Deact, sizeof(c_Deact)) == 0 ? s_At : nullptr;
            // 20 bytes: push rbx; sub rsp,20; mov rax,[rcx+68]; mov rbx,rcx; mov rcx,[rax+a8].
            g_Deactivate = reinterpret_cast<decltype(g_Deactivate)>(
                AbsHook(g_DeactivateAt, c_Deact, 20, reinterpret_cast<void*>(&HkDeactivate)));
        }

        Logger::Info("[Delirium] Game ragdoll hooks: {} (at {}), {} (at {}), {} (at {}).", g_Ragdoll ? "ragdoll" : "NO ragdoll",
            g_RagdollAt, g_Powered ? "powered" : "NO powered", g_PoweredAt, g_Deactivate ? "ragdoll-off" : "NO ragdoll-off",
            g_DeactivateAt);
    }
}

bool Delirium::Start() {
    if (m_Tried)
        return m_Ready;

    m_Tried = true;

    if (m_Gta4.empty()) {
        m_Status = "Needs your GTA IV (Your games, in this menu).";
        return false;
    }

    const auto s_Dll = m_Dir / "gta4nm.dll";

    if (!LoadLibraryW(s_Dll.c_str())) {
        m_Status = std::format("gta4nm.dll isn't in {}.", m_Dir.string());
        return false;
    }

    nm_crash_report(m_Dir.string().c_str());

    if (nm_unlock((std::filesystem::path(m_Gta4) / "GTAIV.exe").string().c_str()) < 0) {
        m_Status = "Couldn't read your GTA IV: " + LastError();
        return false;
    }

    // character.xrt from the export, else the game's own Fred.art.
    auto s_Xrt = m_Dir / "character.xrt";

    if (!std::filesystem::exists(s_Xrt))
        s_Xrt = std::filesystem::path(m_Gta4) / "pc/data/naturalmotion/Fred.art";

    if (nm_init(s_Xrt.string().c_str()) < 0) {
        m_Status = "Couldn't load NM's character: " + LastError();
        return false;
    }

    // Wilma too, for the women (her export in mods/Delirium/wilma).
    const auto s_Wilma = std::filesystem::path(m_Gta4) / "pc/data/naturalmotion/Wilma.art";
    const bool s_HaveWilma = std::filesystem::exists(m_Dir / "wilma/build.txt") && std::filesystem::exists(s_Wilma) &&
        nm_init_wilma(s_Wilma.string().c_str()) >= 0;

    if (std::filesystem::exists(m_Dir / "taskparams.txt"))
        nm_load_task_params((m_Dir / "taskparams.txt").string().c_str());

    // GTA IV's own weapons (push, damage, reaction, a shotgun's pellets) for the shots 47 lands.
    if (nm_weapons_load_info((std::filesystem::path(m_Gta4) / "common/data/weaponinfo.xml").string().c_str()) < 0)
        Logger::Warn("[Delirium] GTA IV's weaponinfo.xml didn't load ({}); the SDK's four default guns.", LastError());

    m_Build = nm_build_load((m_Dir / "build.txt").string().c_str());

    if (!m_Build) {
        m_Status = "No build.txt: export it once with Delirium (delirium-room --game <GTA IV> --export <mods/Delirium>).";
        return false;
    }

    if (s_HaveWilma)
        m_Wilma = nm_build_load((m_Dir / "wilma/build.txt").string().c_str());

    nm_set_step_limit(0);
    m_World = nm_world_create();
    nm_set_step_limit(0); // the Playroom's default: balance keeps stepping as long as it takes
    nm_set_carry_pushes(1); // we move bodies ourselves (follow, portals): off, a shot spins and flings them

    for (int i = 0; i < nm_build_part_count(m_Build); ++i) {
        float s_Mass, s_Radius, s_Half;
        int s_Parent;
        nm_build_part_info(m_Build, i, &s_Mass, &s_Radius, &s_Half, &s_Parent);
        m_Radius.push_back(s_Radius);
        m_Half.push_back(s_Half);
    }

    for (int m = 0; m < nm_message_count(); ++m) {
        char s_Name[128] = {};
        nm_message_name(m, s_Name, sizeof(s_Name));
        m_Messages.emplace_back(s_Name);
    }

    m_Ready = true;
    m_Status = std::format("Ready (gta4nm {}, {} behaviours, {}).", nm_version(), m_Messages.size(),
        m_Wilma ? "Fred and Wilma" : "Fred only");
    Logger::Info("[Delirium] {}", m_Status);
    return true;
}

// The game's ragdoll bodies are moved onto each downed NPC's NM body: a ground ray that hits them laid NM's ground on
// the body itself, which threw it up and dropped it again every scan ("flying up and down").
// The ray's hit (a ZPhysicsObjectRef's proxy) against a ragdoll body: the proxy itself, or a pointer in its first
// 0x80 bytes (its object). Raw reads under __try.
static bool ProxyIs(const void* p_Proxy, const void* p_Body) {
    if (p_Proxy == p_Body)
        return true;

    __try {
        const void* const* p = static_cast<const void* const*>(p_Proxy);

        for (int i = 0; i < 16; ++i)
            if (p[i] == p_Body)
                return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    return false;
}

bool Delirium::IsRagdollObject(const void* p_Proxy) const {
    for (const Body& b : m_Bodies) {
        if (!b.m_Char)
            continue;

        void* s_Rb[64];
        int s_Bn[64];
        void* s_Set = nullptr;
        const int n = ReadRagdoll(b.m_Char, &s_Set, s_Rb, s_Bn);

        for (int i = 0; i < n; ++i)
            if (ProxyIs(p_Proxy, s_Rb[i])) {
                static int s_Logs = 0;

                if (s_Logs++ < 3)
                    Logger::Info("[Delirium] Ground ray hit {}'s game ragdoll (by its physics object): skipped.", b.m_Name);
                return true;
            }
    }

    return false;
}

bool Delirium::BodyAt(const SVector3& p_At) const {
    for (const Body& b : m_Bodies) {
        if (!b.m_Agent || !b.m_Char)
            continue;

        // Billy: shot near walls they floated, sank into walls and contorted. The replays: 13 of 20 shot bodies had
        // their ground ~0.55 m over the pelvis: the ground ray hit the NPC's own game ragdoll (on as it was shot, not
        // yet followed, so the check below skipped it; its bodies still where the game made them). Its bodies are
        // never ground, followed or not.
        {
            void* s_Rb[64]; // c_MaxRagdollBodies (declared further down)
            int s_Bn[64];
            void* s_Set = nullptr;
            const int n = ReadRagdoll(b.m_Char, &s_Set, s_Rb, s_Bn);
            float s_Low = 1e9f;

            for (int i = 0; i < n; ++i)
                s_Low = std::min(s_Low, static_cast<IPhysicsObject*>(s_Rb[i])->GetTransform().Trans.z);

            for (int i = 0; i < n; ++i) {
                const float4 t = static_cast<IPhysicsObject*>(s_Rb[i])->GetTransform().Trans;

                // Near a body and over the ragdoll's lowest body: the body. The floor under and beside the feet is
                // under it (run 4: the floor by the feet skipped, fell through). Run 7: a ray started inside the
                // pelvis reported its underside (under the pelvis's centre, so "not on top"): floor at the pelvis
                // under 5 of 11 shots (Samir_Chandra-1794868319101 5.86 on a 4.95 street).
                if ((SVector3(t.x, t.y, t.z) - p_At).Length() < 0.35f && p_At.z > s_Low + 0.1f)
                    return true;
            }
        }

        if (!b.m_Follow)
            continue;

        for (int k = 0; k < c_Parts; ++k) {
            float p[3], q[4];
            nm_agent_get_part_transform(b.m_Agent, k, p, q);

            if ((V(p) - p_At).Length() < 0.4f)
                return true;
        }
    }

    return false;
}

void Delirium::Clear() {
    for (Ped& s_Ped : m_Peds)
        nm_agent_destroy(s_Ped.m_Agent);

    for (Body& s_Body : m_Bodies)
        Detach(s_Body);

    m_Bodies.clear();
    m_HitmanRagdoll = false;

    // Spawned NPCs are the scene's children: its teardown removes them. Deleting them here nulled their animated
    // actor (ZActor+0x10B8) and the teardown then read it (HITMAN3+14e222, the restart crash with spawned NPCs).
    m_SpawnedNpcs.clear();
    {
        std::scoped_lock s_Owned(g_OwnedMutex);
        g_RagdollStart.clear();
        g_GameTook.clear();
        g_Down.clear();
    }

    if (m_World)
        for (int i = 0; i < c_MaxMeshes; ++i)
            nm_world_set_mesh(m_World, i, nullptr, 0, nullptr, 0, nullptr);

    m_Peds.clear();
    m_Walls.clear();
    m_PatchAt.clear();
    m_HitmanTouches = false;
    ClearHeld();
    std::scoped_lock s_Lock(m_Mutex);
    m_Pose.clear();
    m_Names.clear();
    m_Target = 0;
}

void Delirium::Spawn(const Ray& p_Ray, const SVector3& p_Feet, const SVector3& p_Facing) {
    if (!Start() || m_Peds.size() >= c_MaxPeds)
        return;

    // In a row 2.5 m ahead of 47, 1.2 m apart (never on top of each other), on the ground, facing him.
    SVector3 s_Fwd = SVector3(p_Facing.x, p_Facing.y, 0.f);
    s_Fwd = s_Fwd.Length() > 0.01f ? s_Fwd.Normalized() : SVector3(0.f, 1.f, 0.f);
    const SVector3 s_Right(s_Fwd.y, -s_Fwd.x, 0.f);
    const int n = static_cast<int>(m_Peds.size());
    const float s_Side = (n % 2 ? 1.f : -1.f) * 1.2f * ((n + 1) / 2);
    SVector3 s_At = p_Feet + s_Fwd * 2.5f + s_Right * s_Side;

    SVector3 s_Hit, s_Normal;

    if (p_Ray(s_At + SVector3(0.f, 0.f, 1.5f), s_At - SVector3(0.f, 0.f, 3.f), s_Hit, s_Normal))
        s_At = s_Hit;

    const float s_Yaw = std::atan2(-s_Fwd.y, -s_Fwd.x);
    const float s_Pos[3] = {s_At.x, s_At.y, s_At.z};
    NmAgent* s_Agent = nm_agent_create(m_Build, s_Pos, s_Yaw, 0.1f * (n + 1));

    if (!s_Agent) {
        m_Status = "Couldn't make a ped: " + LastError();
        return;
    }

    m_Peds.push_back({s_Agent, std::format("Ped {}", n + 1)});
    UpdateGround(p_Ray);

    const float s_Look[3] = {m_Eye.x, m_Eye.y, m_Eye.z};
    nm_agent_task_balance(s_Agent, m_World, "*", s_Look);
    m_Target = static_cast<int>(m_Peds.size());
}

// The level under each ped, as a 3 m patch of ground sampled with Hitman's rays (nm_world_set_mesh per ped).
// ponytail: ground only, walls and ledges need side rays or the level's real collision mesh.
void Delirium::UpdateGround(const Ray& p_Ray, bool p_One) {
    HitchTimer s_T {p_One ? "Ground patch" : "Ground patches"};
    constexpr int c_N = 5;
    constexpr float c_Spacing = 0.75f;

    std::vector<NmAgent*> s_Agents;

    for (const Ped& s_Ped : m_Peds)
        s_Agents.push_back(s_Ped.m_Agent);

    for (const Body& s_Body : m_Bodies)
        s_Agents.push_back(s_Body.m_Agent);

    const size_t s_One = p_One && !s_Agents.empty() ? m_GroundNext++ % s_Agents.size() : SIZE_MAX;

    for (size_t p = 0; p < s_Agents.size() && p < c_MaxMeshes; ++p) {
        // Pelican: a body moving faster than 1 m/s (ragdolled, thrown) gets its patch every frame; still ones wait their turn.
        // Billy: a shot NPC sank into the floor. Its body was new (no patch yet) and stood still, so it waited its
        // turn: NM's ground under it was 1.4 m low (pelvis 2.22 m over it as it was shot) and it fell that far. No
        // patch, or one 0.3 m from where it is now: one this frame.
        if (s_One != SIZE_MAX && p != s_One) {
            float v[3], w[3], s_P[3], s_Q[4];
            nm_agent_get_part_velocity(s_Agents[p], 0, v, w);
            nm_agent_get_part_transform(s_Agents[p], 0, s_P, s_Q);
            const auto it = m_PatchAt.find(s_Agents[p]);
            const SVector3 d = it != m_PatchAt.end() ? V(s_P) - it->second.m_At : SVector3(1e9f, 0.f, 0.f);

            if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 1.f && SVector3(d.x, d.y, 0.f).Length() < 0.3f && std::abs(d.z) < 0.3f)
                continue;
        }

        if (std::ranges::any_of(m_Bodies, [&](const Body& b) { return b.m_Agent == s_Agents[p] && b.m_GettingUp; }))
            continue;

        float s_Pos[3], s_Rot[4];
        nm_agent_get_part_transform(s_Agents[p], 0, s_Pos, s_Rot);

        // Billy 11-16: he fell over across a floor portal and lay on its rim like a plank (the opening is 1.4 m wide,
        // he's 1.8 m), then slid off it under the map. Portal 2 has no rim to catch: a body whose middle is over the
        // opening gets no floor at all, so it drops straight in and its middle goes through inside it.
        bool s_Drop = false;

        if (m_PortalsOpen) {
            float s_Com[3], s_Cv[3];
            nm_agent_get_com(s_Agents[p], s_Com, s_Cv);

            for (const DpFrame& f : m_Portals) {
                const SVector3 n = Rotate(f.rot, SVector3(0.f, 0.f, 1.f));
                const float z = dp_local_z(&f, &s_Com);

                if (n.z < 0.7f || z < -0.2f || z > 1.6f)
                    continue;

                const float s_Below[3] = {f.at[0] + (s_Com[0] - n.x * (z + 0.05f) - f.at[0]) * 0.85f,
                    f.at[1] + (s_Com[1] - n.y * (z + 0.05f) - f.at[1]) * 0.85f,
                    f.at[2] + (s_Com[2] - n.z * (z + 0.05f) - f.at[2]) * 0.85f};
                s_Drop |= dp_crossed(&f, 1.f, &s_Below);
            }
        }

        {
            const auto it = m_PatchAt.find(s_Agents[p]);
            const SVector3 d = V(s_Pos) - (it != m_PatchAt.end() ? it->second.m_At : SVector3(1e9f, 0.f, 0.f));

            if (it != m_PatchAt.end() && SVector3(d.x, d.y, 0.f).Length() < 0.3f && std::abs(d.z) < 0.3f &&
                it->second.m_Open == m_PortalsOpen && it->second.m_Drop == s_Drop &&
                !std::memcmp(it->second.m_Portals, m_Portals, sizeof(it->second.m_Portals)))
                continue;

            PatchAt& s_At = m_PatchAt[s_Agents[p]];
            s_At.m_Drop = s_Drop;
            s_At.m_At = V(s_Pos);
            s_At.m_Open = m_PortalsOpen;
            std::memcpy(s_At.m_Portals, m_Portals, sizeof(s_At.m_Portals));
        }

        std::vector<float> s_Verts;
        std::vector<uint32_t> s_Tris;
        const float s_Half = c_Spacing * (c_N - 1) * 0.5f;

        const auto s_Last = m_LastGround.try_emplace(s_Agents[p], s_Pos[2] - 1.f).first;

        for (int y = 0; y < c_N; ++y) {
            for (int x = 0; x < c_N; ++x) {
                const SVector3 s_Top(s_Pos[0] - s_Half + x * c_Spacing, s_Pos[1] - s_Half + y * c_Spacing, s_Pos[2] + 1.f);
                SVector3 s_Hit, s_Normal;
                float s_Z = s_Last->second;

                // (A middle ray from the pelvis hit the inside of its game ragdoll: a spike of floor at the pelvis
                // under 8 of Billy's 20 run-5 shots. Rays from above; RayCastLevel skips characters and ragdoll tops.)
                if (p_Ray(s_Top, s_Top - SVector3(0.f, 0.f, 4.f), s_Hit, s_Normal)) {
                    s_Z = s_Hit.z;

                    if (x == c_N / 2 && y == c_N / 2)
                        s_Last->second = s_Z;
                }

                s_Verts.insert(s_Verts.end(), {s_Top.x, s_Top.y, s_Z});
            }
        }

        // Billy run 8: rays near the NPC hit his own body: floor at his hips under him (Herbert West: 0.82 m over his
        // feet). Within 1.1 m of him, a point over his lowest part + 0.35 m is his body: at his feet. Not the patch's
        // median: Emmanuel_Bruhn-1794868937162 stood on a porch 0.95 m over the street round it.
        // Under it by as much near him is a ray that went through his feet's floor (Tiffaney_Prout-1794868915662: 0.7 m
        // under her feet: floating 1.64 m).
        {
            float s_Low = 1e9f;

            for (int k = 0; k < c_Parts; ++k) {
                float s_P[3], s_R[4];
                nm_agent_get_part_transform(s_Agents[p], k, s_P, s_R);
                s_Low = std::min(s_Low, s_P[2]);
            }

            for (size_t i = 0; i < s_Verts.size(); i += 3)
                if (std::hypot(s_Verts[i] - s_Pos[0], s_Verts[i + 1] - s_Pos[1]) < 1.1f && std::abs(s_Verts[i + 2] - s_Low) > 0.35f)
                    s_Verts[i + 2] = s_Low;

            s_Last->second = s_Verts[(c_N / 2 * c_N + c_N / 2) * 3 + 2];
        }

        // A point over an open portal in the floor is in its hole: bodies fall in.
        const auto s_InHole = [&](const float (&s_C)[3]) {
            for (const DpFrame& f : m_Portals) {
                const float s_Z = dp_local_z(&f, &s_C);
                const SVector3 n = Rotate(f.rot, SVector3(0.f, 0.f, 1.f));

                if (n.z > 0.7f && std::abs(s_Z) < 0.4f) {
                    const float s_Below[3] = {s_C[0] - n.x * (s_Z + 0.05f), s_C[1] - n.y * (s_Z + 0.05f),
                        s_C[2] - n.z * (s_Z + 0.05f)};

                    if (dp_crossed(&f, 1.f, &s_Below))
                        return true;
                }
            }

            return false;
        };

        // Billy 11-16: 47 sat on a floor portal, or fell through the map. Whole 0.75 m cells were the hole, so it was
        // one cell (too small) or reached past the opening, where his middle went down outside it and never went
        // through. Near a floor portal each cell is cut into 8 x 8 and the hole follows the opening to 0.1 m.
        constexpr int c_Sub = 8;

        for (int y = 0; y + 1 < c_N; ++y) {
            for (int x = 0; x + 1 < c_N; ++x) {
                const uint32_t a = y * c_N + x, b = a + 1, c = a + c_N, d = c + 1;
                float A[3], B[3], C[3], D[3];

                for (int k = 0; k < 3; ++k)
                    A[k] = s_Verts[a * 3 + k], B[k] = s_Verts[b * 3 + k], C[k] = s_Verts[c * 3 + k], D[k] = s_Verts[d * 3 + k];

                bool s_Near = false;

                if (m_PortalsOpen)
                    for (const DpFrame& f : m_Portals)
                        s_Near |= Rotate(f.rot, SVector3(0.f, 0.f, 1.f)).z > 0.7f &&
                            std::hypot((A[0] + D[0]) * 0.5f - f.at[0], (A[1] + D[1]) * 0.5f - f.at[1]) < 2.5f &&
                            std::abs((A[2] + D[2]) * 0.5f - f.at[2]) < 1.f;

                if (s_Drop)
                    continue;

                if (!s_Near) {
                    s_Tris.insert(s_Tris.end(), {a, b, d, a, d, c});
                    continue;
                }

                const uint32_t s_Base = static_cast<uint32_t>(s_Verts.size() / 3);

                for (int j = 0; j <= c_Sub; ++j)
                    for (int i = 0; i <= c_Sub; ++i) {
                        const float u = float(i) / c_Sub, v = float(j) / c_Sub;

                        for (int k = 0; k < 3; ++k)
                            s_Verts.push_back((A[k] * (1 - u) + B[k] * u) * (1 - v) + (C[k] * (1 - u) + D[k] * u) * v);
                    }

                for (int j = 0; j < c_Sub; ++j)
                    for (int i = 0; i < c_Sub; ++i) {
                        const uint32_t sa = s_Base + j * (c_Sub + 1) + i, sb = sa + 1, sc = sa + c_Sub + 1, sd = sc + 1;
                        const float s_M[3] = {(s_Verts[sa * 3] + s_Verts[sd * 3]) * 0.5f,
                            (s_Verts[sa * 3 + 1] + s_Verts[sd * 3 + 1]) * 0.5f, (s_Verts[sa * 3 + 2] + s_Verts[sd * 3 + 2]) * 0.5f};

                        if (!s_InHole(s_M))
                            s_Tris.insert(s_Tris.end(), {sa, sb, sd, sa, sd, sc});
                    }
            }
        }

        // The walls round it: level rays out at knee and chest height, a wall panel stood where each hits (none in an
        // open portal: bodies go into it).
        const float s_Floor = s_Last->second;

        // 12 directions at three heights (the old 8 at two left gaps a limb or a flung body went through); panels widen
        // with distance so neighbours overlap. No skipping walls near people any more: rays only see the level now, and
        // that skip took the wall away wherever 47 stood next to it (Billy: pushed through walls).
        std::vector<Wall>& s_Walls = m_Walls[s_Agents[p]];
        const SVector3 s_Body(s_Pos[0], s_Pos[1], s_Floor);
        std::erase_if(s_Walls, [&](const Wall& w) {
            const SVector3 d(w.m_Hit.x - s_Body.x, w.m_Hit.y - s_Body.y, 0.f);
            return d.Length() > 3.5f || std::abs(w.m_Floor - s_Floor) > 1.5f;
        });

        for (int k = 0; k < 12; ++k) {
            const float s_Ang = k * 6.2831853f / 12.f;
            const SVector3 s_Dir(std::cos(s_Ang), std::sin(s_Ang), 0.f);

            for (const float s_H : {0.15f, 0.25f, 0.7f, 1.3f}) { // 0.15: under a fence's low rail (Pelican)
                const SVector3 s_From(s_Pos[0], s_Pos[1], s_Floor + s_H);
                SVector3 s_Hit, s_N;

                if (!p_Ray(s_From, s_From + s_Dir * 2.5f, s_Hit, s_N))
                    continue;

                // Billy run 8: these rays hit the NPC itself (Herbert_West-1794868913269: 14 wall boxes in a ring
                // 0.2-0.4 m round his own pelvis): caged in walls, he spun, contorted and was flung. Under 0.5 m from
                // his middle is his own body; a wall that close was seen (and kept, s_Walls) as he came to it.
                if ((SVector3(s_Hit.x, s_Hit.y, 0.f) - SVector3(s_From.x, s_From.y, 0.f)).Length() < 0.5f)
                    continue;

                SVector3 n(s_N.x, s_N.y, 0.f);

                if (n.Length() < 0.7f)
                    continue;

                n = n.Normalized();
                bool s_Hole = false;

                if (m_PortalsOpen)
                    for (const DpFrame& f : m_Portals) {
                        const float s_Behind[3] = {s_Hit.x - n.x * 0.05f, s_Hit.y - n.y * 0.05f, s_Hit.z};
                        s_Hole = s_Hole || dp_crossed(&f, 1.f, &s_Behind);
                    }

                if (s_Hole)
                    continue;

                const float s_Dist = (s_Hit - s_From).Length();
                bool s_Have = false;

                for (Wall& w : s_Walls)
                    if ((w.m_Hit - s_Hit).Length() < 0.25f && w.m_N.Dot(n) > 0.9f) {
                        w.m_Half = std::max(w.m_Half, std::max(0.6f, s_Dist * 0.3f));
                        s_Have = true;
                        break;
                    }

                if (!s_Have && s_Walls.size() < 64)
                    s_Walls.push_back({s_Hit, n, s_Floor, std::max(0.6f, s_Dist * 0.3f)});
            }
        }

        // Portals opened since: no wall in them.
        if (m_PortalsOpen)
            std::erase_if(s_Walls, [&](const Wall& w) {
                for (const DpFrame& f : m_Portals) {
                    const float s_Behind[3] = {w.m_Hit.x - w.m_N.x * 0.05f, w.m_Hit.y - w.m_N.y * 0.05f, w.m_Hit.z};

                    if (dp_crossed(&f, 1.f, &s_Behind))
                        return true;
                }

                return false;
            });

        if (s_Tris.empty())
            s_Tris = {0, 1, 2};

        nm_world_set_mesh(m_World, static_cast<int>(p), s_Verts.data(), static_cast<int>(s_Verts.size() / 3), s_Tris.data(),
            static_cast<int>(s_Tris.size() / 3), nullptr);
    }

    // Every body's wall panels as solid boxes (Pelican: nm_world_set_walls, delirium 10f5e0a87; a box holds a body at any
    // speed, triangle slabs let fast ones through). 0.8 m deep behind the hit face, 2.2 m up from the floor; one call for
    // the whole world (it replaces the last).
    std::vector<float> s_Boxes;

    for (const auto& [s_Agent, s_List] : m_Walls)
        for (const Wall& w : s_List) {
            if (std::ranges::find(s_Agents, s_Agent) == s_Agents.end()) // a body since gone: not its walls
                break;

            const float s_Yaw = std::atan2(-w.m_N.y, -w.m_N.x); // X onto -n
            const SVector3 c = w.m_Hit - w.m_N * 0.4f;
            s_Boxes.insert(s_Boxes.end(), {c.x, c.y, w.m_Floor + 1.1f, 0.f, 0.f, std::sin(s_Yaw * 0.5f), std::cos(s_Yaw * 0.5f),
                0.4f, w.m_Half, 1.1f});
        }

    nm_world_set_walls(m_World, s_Boxes.data(), static_cast<int>(s_Boxes.size() / 10));
}

NmAgent* Delirium::Target(int t, std::string*& p_Name, float*& p_Health, Body*& p_Body) {
    p_Body = nullptr;

    if (t == 0) {
        p_Body = Hitman();
    }
    else if (t - 1 < static_cast<int>(m_Peds.size())) {
        Ped& s_Ped = m_Peds[t - 1];
        p_Name = &s_Ped.m_Name;
        p_Health = &s_Ped.m_Health;
        return s_Ped.m_Agent;
    }
    else {
        int k = t - 1 - static_cast<int>(m_Peds.size());

        for (Body& s_Body : m_Bodies)
            if (s_Body.m_Npc && k-- == 0)
                p_Body = &s_Body;
    }

    if (!p_Body)
        return nullptr;

    p_Name = &p_Body->m_Name;
    p_Health = &p_Body->m_Health;
    return p_Body->m_Agent;
}

int Delirium::Aimed() {
    int s_Best = -1;
    float s_BestDist = 1e9f;
    const float s_From[3] = {m_Eye.x, m_Eye.y, m_Eye.z};
    const SVector3 s_End = m_Eye + m_Aim * 100.f;
    const float s_To[3] = {s_End.x, s_End.y, s_End.z};
    const int s_Count = 1 + static_cast<int>(m_Peds.size() + m_Bodies.size());

    // Not 47 (target 0) from his own camera.
    for (int t = 1; t < s_Count; ++t) {
        std::string* s_Name;
        float* s_Health;
        Body* s_Body;
        NmAgent* a = Target(t, s_Name, s_Health, s_Body);
        float s_Point[3], s_Normal[3];

        if (s_Body && !s_Body->m_Npc) // 47's own body (his shots started inside it and ragdolled him)
            continue;

        if (a && nm_agent_raycast(a, s_From, s_To, s_Point, s_Normal) >= 0) {
            const float s_D = (V(s_Point) - m_Eye).Length();

            if (s_D < s_BestDist) {
                s_BestDist = s_D;
                s_Best = t;
            }
        }
    }

    return s_Best;
}

void Delirium::Fire(NmAgent* a, const std::string& p_Name, float& p_Health, const std::string& p_What) {
    float s_Chest[3], s_Rot[4], s_Pelvis[3];
    nm_agent_get_part_transform(a, c_Chest, s_Chest, s_Rot);
    nm_agent_get_part_transform(a, 0, s_Pelvis, s_Rot);

    // From the camera: along the aim when it's on him, else straight at his chest.
    const float s_Eye[3] = {m_Eye.x, m_Eye.y, m_Eye.z};
    float s_At[3] = {s_Chest[0], s_Chest[1], s_Chest[2]};
    int s_Part = c_Chest;
    SVector3 s_Dir = (V(s_Chest) - m_Eye).Normalized();
    const SVector3 s_AimEnd = m_Eye + m_Aim * 100.f;
    const float s_AimTo[3] = {s_AimEnd.x, s_AimEnd.y, s_AimEnd.z};
    float s_Normal[3];

    if (const int s_Hit = nm_agent_raycast(a, s_Eye, s_AimTo, s_At, s_Normal); s_Hit >= 0) {
        s_Part = s_Hit;
        s_Dir = m_Aim;
    }
    else {
        std::copy_n(s_Chest, 3, s_At);
    }

    const float s_D[3] = {s_Dir.x, s_Dir.y, s_Dir.z};
    const float k = m_Strength;
    const auto s_Scaled = [&](float p_Ns, float* p_Out) {
        p_Out[0] = s_D[0] * p_Ns * k;
        p_Out[1] = s_D[1] * p_Ns * k;
        p_Out[2] = s_D[2] * p_Ns * k;
    };
    float s_Imp[3];
    int s_Result = 0;

    const auto s_Weapon = [&](const char* p_Name) {
        const int w = nm_weapon_find(p_Name);
        p_Health = w >= 0 ? nm_agent_shoot_weapon(a, m_World, w, s_Part, s_At, s_D, p_Health) : p_Health;
        s_Result = w;
    };

    if (p_What == "Shot (pistol)") s_Weapon("pistol");
    else if (p_What == "Shot (rifle)") s_Weapon("rifle");
    else if (p_What == "Shot (shotgun)") s_Weapon("shotgun");
    else if (p_What == "Push") { s_Scaled(60.f, s_Imp); s_Result = nm_agent_apply_push(a, s_At, s_Imp); }
    else if (p_What == "Punch") { s_Scaled(30.f, s_Imp); s_Result = nm_agent_task_melee_hit(a, m_World, s_At, s_Imp, s_Eye, 0); }
    else if (p_What == "Explosion") {
        const float s_Blast[3] = {s_Pelvis[0] - s_D[0] * 2.f, s_Pelvis[1] - s_D[1] * 2.f, s_Pelvis[2] - 0.8f};
        s_Result = nm_agent_task_explosion(a, m_World, s_Blast, 5.f, 15.f * k);
    }
    else if (p_What == "Taser") s_Result = nm_agent_tase(a, m_World, 3.f * k);
    else if (p_What == "Set on fire") s_Result = nm_agent_task_on_fire(a, m_World, p_Health);
    else if (p_What == "Near miss") {
        const float s_Miss[3] = {m_Eye.x, m_Eye.y, m_Eye.z + 0.0f};
        const SVector3 s_Past = (V(s_Chest) + SVector3(0.f, 0.f, 0.5f) - m_Eye).Normalized();
        const float s_PastD[3] = {s_Past.x, s_Past.y, s_Past.z};
        s_Result = nm_agent_near_miss(a, m_World, s_Miss, s_PastD);
    }
    else if (p_What == "Flinch") s_Result = nm_agent_task_flinch(a, m_World, "*", s_Eye);
    else if (p_What == "Balance") s_Result = nm_agent_task_balance(a, m_World, "*", s_Eye);
    else if (p_What == "Catch fall") s_Result = nm_agent_task_catch_fall(a, "*");
    else if (p_What == "High fall") s_Result = nm_agent_task_high_fall(a, m_World, "*", 0);
    else if (p_What == "Fall down a ledge") s_Result = nm_agent_task_fall_down(a, m_World, -1.f);
    else if (p_What == "Roll up") s_Result = nm_agent_task_roll_up_relax(a, m_World, -1.f, -1.f, 1500);
    else if (p_What == "Jump") s_Result = nm_agent_jump(a, 0, nullptr);
    else if (p_What == "Dive") s_Result = nm_agent_jump(a, 1, s_D);
    else if (p_What == "Go limp") s_Result = nm_agent_task_relax(a, -1.f, -1.f);
    else if (p_What == "Stop") {
        nm_agent_task_stop(a);
        s_Result = nm_msg_send_text(a, m_World, "stopAllBehaviours");
    }
    else if (p_What == "Stand up again") {
        // A fresh body where he lies, balancing.
        const float s_Pos[3] = {s_Pelvis[0], s_Pelvis[1], s_Pelvis[2] - 1.f};
        SVector3 s_Face = m_Eye - V(s_Pelvis);
        NmAgent* s_New = nm_agent_create(m_Build, s_Pos, std::atan2(s_Face.y, s_Face.x), 0.5f);

        if (s_New) {
            for (Ped& s_Ped : m_Peds)
                if (s_Ped.m_Agent == a)
                    s_Ped.m_Agent = s_New;

            nm_agent_destroy(a);
            p_Health = 200.f;
            s_Result = nm_agent_task_balance(s_New, m_World, "*", s_Eye);
        }
    }
    else if (p_What.starts_with("msg:")) s_Result = nm_msg_send_text(a, m_World, p_What.c_str() + 4);

    m_Line = s_Result < 0 ? std::format("{} on {}: {}", p_What, p_Name, LastError())
                          : std::format("{} on {} (health {:.0f}).", p_What, p_Name, p_Health);
    Logger::Info("[Delirium] {}", m_Line);
}

Delirium::Body* Delirium::Hitman() {
    for (Body& s_Body : m_Bodies)
        if (!s_Body.m_Npc)
            return &s_Body;

    return nullptr;
}

// Bone 0's parent: the world matrix the engine poses it with (else its spatial entity's).
void Delirium::Root(Body& p_Body, float* p_Turn, SVector3& p_P) {
    Q& p_Q = *reinterpret_cast<Q*>(p_Turn);
    float x[3], y[3], z[3], t[3];
    int s_Layout = 0;

    // Billy: a far NPC shot dead fell through the map. Its animator's world was stale, 8.5 m above its entity (Bobby
    // Caretaker: -93.49 vs -101.89), so Homer was made in the air and its ragdoll pulled up to him. Not that: the entity.
    const SMatrix s_E = p_Body.m_Spatial->GetObjectToWorldMatrix();

    if (Bones::World(p_Body.m_Anim, x, y, z, t, s_Layout) &&
        (SVector3(t[0], t[1], t[2]) - SVector3(s_E.Trans.x, s_E.Trans.y, s_E.Trans.z)).Length() < 1.5f) {
        p_Q = FromAxes(float4(x[0], x[1], x[2], 0.f), float4(y[0], y[1], y[2], 0.f), float4(z[0], z[1], z[2], 0.f));
        p_P = SVector3(t[0], t[1], t[2]);
    }
    else {
        const SMatrix s_M = p_Body.m_Spatial->GetObjectToWorldMatrix();
        p_Q = FromAxes(s_M.XAxis, s_M.YAxis, s_M.ZAxis);
        p_P = SVector3(s_M.Trans.x, s_M.Trans.y, s_M.Trans.z);
    }

    p_P = p_P + p_Body.m_RootFix;

    if (p_Body.m_Layout != s_Layout) {
        p_Body.m_Layout = s_Layout;
        const SMatrix s_M = p_Body.m_Spatial->GetObjectToWorldMatrix();
        Logger::Info("[Delirium] {}: world {} ({:.2f} {:.2f} {:.2f}; its entity at {:.2f} {:.2f} {:.2f}).", p_Body.m_Name,
            s_Layout == 44 ? "4x4 from the engine" : s_Layout == 43 ? "4x3 from the engine" : "from its entity", p_P.x, p_P.y,
            p_P.z, s_M.Trans.x, s_M.Trans.y, s_M.Trans.z);
    }
}

// Every bone's world frame from the animation's pose this frame (parents first, bone 0 under the character's object).
bool Delirium::Posed(Body& p_Body) {
    if (!Bones::Read(p_Body.m_Anim, p_Body.m_Local) || p_Body.m_Local.size() != p_Body.m_Parents.size())
        return false;

    Q s_ObjQ;
    SVector3 s_ObjP;
    Root(p_Body, &s_ObjQ.x, s_ObjP);
    Fk(p_Body.m_Local, p_Body.m_Parents, s_ObjQ, s_ObjP, g_Conj, p_Body.m_World);

    p_Body.m_Now.resize(p_Body.m_Bones.size() * 7);

    for (size_t r = 0; r < p_Body.m_Bones.size(); ++r)
        std::copy_n(&p_Body.m_World[p_Body.m_Bones[r] * 7], 7, &p_Body.m_Now[r * 7]);

    return true;
}

// NM's pose onto the skeleton: every rig bone's turn under its parent, the pelvis's world frame under its own parent.
void Delirium::WritePose(Body& p_Body) {
    const size_t n = p_Body.m_Bones.size();

    if (p_Body.m_Local.empty() || p_Body.m_World.empty())
        return;

    // The last two stepped poses, drawn between by how far into the next step this frame is (no 60 Hz judder).
    if (m_Stepped || p_Body.m_PoseB.size() != n * 4 + 3) {
        p_Body.m_PoseA = p_Body.m_PoseB;
        p_Body.m_PoseB.assign(n * 4 + 3, 0.f);
        p_Body.m_PoseHips = nm_rig_pose(p_Body.m_Rig, p_Body.m_Agent, p_Body.m_PoseB.data());
    }

    const int s_Hips = p_Body.m_PoseHips;

    if (s_Hips < 0 || s_Hips >= static_cast<int>(n))
        return;

    std::vector<float> s_Out = p_Body.m_PoseB;

    if (p_Body.m_PoseA.size() == s_Out.size()) {
        const float t = m_Alpha;
        const float* a = p_Body.m_PoseA.data();

        for (size_t r = 0; r < n; ++r) {
            float* q = &s_Out[r * 4];
            const float* p = &a[r * 4];
            const float s = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3] < 0.f ? -1.f : 1.f;
            const Q m = Norm(Q {p[0] + (q[0] * s - p[0]) * t, p[1] + (q[1] * s - p[1]) * t, p[2] + (q[2] * s - p[2]) * t,
                p[3] + (q[3] * s - p[3]) * t});
            q[0] = m.x, q[1] = m.y, q[2] = m.z, q[3] = m.w;
        }

        for (size_t c = n * 4; c < n * 4 + 3; ++c)
            s_Out[c] = a[c] + (s_Out[c] - a[c]) * t;
    }

    std::vector<Bones::SQV> s_Local = p_Body.m_Local;

    for (size_t r = 0; r < n; ++r) {
        float* q = s_Local[p_Body.m_Bones[r]].q;
        std::copy_n(&s_Out[r * 4], 4, q);

        if (g_Conj)
            q[0] = -q[0], q[1] = -q[1], q[2] = -q[2];
    }

    const int s_Bone = p_Body.m_Bones[s_Hips];
    const int s_Parent = p_Body.m_Parents[s_Bone];
    Q s_Pq;
    SVector3 s_Pp;

    if (s_Parent >= 0) {
        const float* w = &p_Body.m_World[s_Parent * 7];
        s_Pq = {w[3], w[4], w[5], w[6]};
        s_Pp = V(w);
    }
    else {
        Root(p_Body, &s_Pq.x, s_Pp);
    }

    const float* h = &s_Out[s_Hips * 4];
    const Q s_Lq = Norm(Mul(Conj(s_Pq), Q {h[0], h[1], h[2], h[3]}));
    const SVector3 s_Lp = Rot(Conj(s_Pq), V(&s_Out[n * 4]) - s_Pp);
    Bones::SQV& l = s_Local[s_Bone];
    l.q[0] = g_Conj ? -s_Lq.x : s_Lq.x, l.q[1] = g_Conj ? -s_Lq.y : s_Lq.y, l.q[2] = g_Conj ? -s_Lq.z : s_Lq.z, l.q[3] = s_Lq.w;
    l.p[0] = s_Lp.x, l.p[1] = s_Lp.y, l.p[2] = s_Lp.z;
    p_Body.m_Written = s_Local;
    Bones::Override(p_Body.m_Anim, std::move(s_Local));

    // The rig bones' world frames (Hamilton): the hips' as NM gave it, each other one under its parent.
    p_Body.m_RigWorld.assign(n * 7, 0.f);

    for (size_t r = 0; r < n; ++r) {
        float* o = &p_Body.m_RigWorld[r * 7];

        if (static_cast<int>(r) == s_Hips) {
            std::copy_n(&s_Out[n * 4], 3, o);
            std::copy_n(h, 4, o + 3);
            continue;
        }

        const int s_Pb = p_Body.m_Parents[p_Body.m_Bones[r]];
        const auto it = std::find(p_Body.m_Bones.begin(), p_Body.m_Bones.begin() + r, s_Pb);

        if (it == p_Body.m_Bones.begin() + r)
            continue;

        const float* w = &p_Body.m_RigWorld[(it - p_Body.m_Bones.begin()) * 7];
        const Q s_Wq {w[3], w[4], w[5], w[6]};
        const Q q = Norm(Mul(s_Wq, Q {s_Out[r * 4], s_Out[r * 4 + 1], s_Out[r * 4 + 2], s_Out[r * 4 + 3]}));
        const SVector3 p = V(w) + Rot(s_Wq, V(p_Body.m_Local[p_Body.m_Bones[r]].p));
        o[0] = p.x, o[1] = p.y, o[2] = p.z, o[3] = q.x, o[4] = q.y, o[5] = q.z, o[6] = q.w;
    }
}

namespace {
    // A character's death ragdoll (HITMAN3, from ZHM5BaseCharacter::ActivateRagdoll and its handler's activate):
    // character +0x68 the ragdoll handler, +0xa8 its ragdoll (+0x30 on), +0x68 the body set: +0x08 the bodies
    // (ZPhysicsObject*), +0x40 their bones (an int at the front of 0x50 each); both small vectors (bit 62 of the third
    // word: stored inline, its low byte the count).
    constexpr int c_MaxRagdollBodies = 64;

    uint8_t* At(const void* p, size_t o) { return *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(p) + o); }

    uint8_t* Small(uint8_t* p_Set, size_t o, size_t p_Stride, int& n) {
        const uint64_t f = *reinterpret_cast<uint64_t*>(p_Set + o + 0x10);

        if ((f >> 62) & 1) {
            n = static_cast<int>(f & 0xff);
            return p_Set + o;
        }

        uint8_t* b = At(p_Set, o);
        n = static_cast<int>((At(p_Set, o + 8) - b) / static_cast<ptrdiff_t>(p_Stride));
        return b;
    }

    int ReadRagdoll(void* p_Char, void** p_Set, void** p_Bodies, int* p_Bones) {
        __try {
            uint8_t* s_Handler = At(p_Char, 0x68);
            uint8_t* s_Ragdoll = s_Handler ? At(s_Handler, 0xa8) : nullptr;

            if (!s_Ragdoll || !s_Ragdoll[0x30])
                return 0;

            uint8_t* s_Set = At(s_Ragdoll, 0x68);

            if (!s_Set)
                return 0;

            int n = 0, m = 0;
            void** s_B = reinterpret_cast<void**>(Small(s_Set, 0x08, 8, n));
            uint8_t* s_Bn = Small(s_Set, 0x40, 0x50, m);
            n = n < m ? n : m;
            n = n < c_MaxRagdollBodies ? n : c_MaxRagdollBodies;

            for (int i = 0; i < n; ++i) {
                p_Bodies[i] = s_B[i];
                p_Bones[i] = *reinterpret_cast<int32_t*>(s_Bn + i * 0x50);

                if (!p_Bodies[i] || !*static_cast<void**>(p_Bodies[i]))
                    return -1;
            }

            *p_Set = s_Set;
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    // Pelican: the skin's pose isn't our bone buffer (nothing writes it after our Override; the ragdoll reads it).
    // Where is it: memory off the character, its animated actor and its animator (and one pointer further) holding
    // this NPC's hips position in world space. Raw reads under __try: no C++ objects in here.
    int FindNear(uintptr_t p_Root, size_t p_Len, const float* p_At, float p_Tol, int p_Depth, uintptr_t* p_Out,
        uintptr_t* p_Via, int p_Have, int p_Max) {
        MEMORY_BASIC_INFORMATION m;

        if (p_Have >= p_Max || p_Root < 0x10000 || p_Root > 0x7fffffffffffull || (p_Root & 7) ||
            !VirtualQuery(reinterpret_cast<void*>(p_Root), &m, sizeof(m)) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & (PAGE_READWRITE | PAGE_READONLY)))
            return p_Have;

        const size_t s_Len = std::min<size_t>(p_Len, reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize - p_Root);

        __try {
            const float* f = reinterpret_cast<const float*>(p_Root);

            for (size_t k = 0; k + 3 <= s_Len / 4 && p_Have < p_Max; ++k)
                if (std::abs(f[k] - p_At[0]) < p_Tol && std::abs(f[k + 1] - p_At[1]) < p_Tol && std::abs(f[k + 2] - p_At[2]) < p_Tol)
                    p_Via[p_Have] = p_Root, p_Out[p_Have++] = p_Root + k * 4;

            if (p_Depth > 0)
                for (size_t k = 0; k < s_Len / 8 && p_Have < p_Max; ++k)
                    p_Have = FindNear(reinterpret_cast<const uintptr_t*>(p_Root)[k], 0x2000, p_At, p_Tol, p_Depth - 1, p_Out,
                        p_Via, p_Have, p_Max);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        return p_Have;
    }

    // Hitman's own ragdoll recovery (HITMAN3+0x3b82f0, called with ZActor::m_pAnimatedActor by the unconscious and
    // knocked-down behaviours): powered ragdoll, picks Impacts|*_GetUp_Back/Stomake/LeftSide/RightSide from the
    // ragdoll's lie, plays it, ends at its eDE_RecoveryBlendEnd. Checked by its first bytes; false = not this build.
    bool StartRecovery(ZHM5BaseCharacter* c) {
        static constexpr uint8_t c_Head[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x79, 0x50};
        const auto s_Fn = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + 0x3b82f0;

        if (std::memcmp(s_Fn, c_Head, sizeof(c_Head)))
            return false;

        __try {
            void* s_Animated = static_cast<ZActor*>(c)->m_pAnimatedActor;

            if (!s_Animated || !*reinterpret_cast<void**>(static_cast<uint8_t*>(s_Animated) + 0x50))
                return false;

            reinterpret_cast<void (*)(void*)>(s_Fn)(s_Animated);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // An object's class (its RTTI name), for the log and the check.
    const char* ClassOf(void* p_Object) {
        __try {
            const auto* s_Col = (*static_cast<uint32_t***>(p_Object))[-1];
            const auto s_Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            return reinterpret_cast<const char*>(s_Base + s_Col[3] + 16);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return "?";
        }
    }

    // ZPhysicsObject's vtable: 65 IsKinematic, 66 SetKinematic(bool, bool) (HITMAN3).
    bool IsKinematic(void* p) { return (*reinterpret_cast<bool(***)(void*)>(p))[65](p); }

    // Bodies of downed NPCs that NM drives: the game turned them all dynamic again every so often, they fell a step on
    // their own and were teleported back (Pelican: the jitter, "fighting"). Its SetKinematic(false) on them is dropped.
    using SetKinematicFn = void (*)(void*, bool, bool);
    SetKinematicFn g_SetKinematic = nullptr;
    std::unordered_set<void*> g_Held;
    std::mutex g_HeldMutex;

    void HkSetKinematic(void* p, bool p_On, bool p_Wake) {
        if (!p_On) {
            std::scoped_lock s_Lock(g_HeldMutex);

            if (g_Held.contains(p))
                return;
        }

        g_SetKinematic(p, p_On, p_Wake);
    }

    // Billy 11-16: shot NPCs flicked every frame between two spots ~2 m apart. Follow put the bodies on Homer, then
    // the game's own ragdoll code moved them back (its pose/kinematic target; log: 19 of 19 bodies over 0.3 m off
    // every frame after the kill). On held bodies only Delirium moves them: the game's SetTransform (7),
    // MoveTransform (9) and slot 10 are dropped.
    using MoveFn = void (*)(void*, const SMatrix&);
    MoveFn g_Move[3] = {};
    thread_local bool g_OurMove = false;

    template <int I>
    void HkMove(void* p, const SMatrix& m) {
        if (!g_OurMove) {
            std::scoped_lock s_Lock(g_HeldMutex);

            if (g_Held.contains(p))
                return;
        }

        g_Move[I](p, m);
    }

    void HookMoves(void* p) {
        if (g_Move[0])
            return;

        void** s_V = *reinterpret_cast<void***>(p);
        const int s_Slots[3] = {7, 9, 10};
        void* s_Hk[3] = {reinterpret_cast<void*>(&HkMove<0>), reinterpret_cast<void*>(&HkMove<1>), reinterpret_cast<void*>(&HkMove<2>)};

        for (int i = 0; i < 3; ++i) {
            DWORD s_Old;

            if (VirtualProtect(&s_V[s_Slots[i]], sizeof(void*), PAGE_READWRITE, &s_Old)) {
                g_Move[i] = reinterpret_cast<MoveFn>(s_V[s_Slots[i]]);
                s_V[s_Slots[i]] = s_Hk[i];
                VirtualProtect(&s_V[s_Slots[i]], sizeof(void*), s_Old, &s_Old);
            }
        }

        Logger::Info("[Delirium] The game's moves of NM's ragdoll bodies are held.");
    }

    void HookSetKinematic(void* p) {
        HookMoves(p);

        if (g_SetKinematic)
            return;

        auto** s_Slot = &(*reinterpret_cast<void***>(p))[66];
        DWORD s_Old;

        if (VirtualProtect(s_Slot, sizeof(void*), PAGE_READWRITE, &s_Old)) {
            g_SetKinematic = reinterpret_cast<SetKinematicFn>(*s_Slot);
            *s_Slot = reinterpret_cast<void*>(&HkSetKinematic);
            VirtualProtect(s_Slot, sizeof(void*), s_Old, &s_Old);
            Logger::Info("[Delirium] The game's SetKinematic on NM's ragdoll bodies is held.");
        }
    }

    void ClearHeld() {
        std::scoped_lock s_Lock(g_HeldMutex);
        g_Held.clear();
    }

    void Hold(void* p, bool p_On) {
        std::scoped_lock s_Lock(g_HeldMutex);
        p_On ? (void)g_Held.insert(p) : (void)g_Held.erase(p);
    }

    void SetKinematic(void* p, bool p_On) {
        if (!p_On)
            Hold(p, false);

        (*reinterpret_cast<void(***)(void*, bool, bool)>(p))[66](p, p_On, true);
    }

    SMatrix ToMatrix(const Q& q, const SVector3& p) {
        SMatrix m;
        const SVector3 x = Rot(q, SVector3(1.f, 0.f, 0.f)), y = Rot(q, SVector3(0.f, 1.f, 0.f)), z = Rot(q, SVector3(0.f, 0.f, 1.f));
        m.XAxis = float4(x.x, x.y, x.z, 0.f);
        m.YAxis = float4(y.x, y.y, y.z, 0.f);
        m.ZAxis = float4(z.x, z.y, z.z, 0.f);
        m.Trans = float4(p.x, p.y, p.z, 1.f);
        return m;
    }
}

// The game's death ragdoll onto NM's body: each of its bodies kinematic, moved where its bone is on NM's pose (its
// offset from the bone taken when it was first seen).
// ponytail: the ragdoll's layout is HITMAN3's (offsets above); a body that isn't a ZPhysicsObject stops it.
void Delirium::Follow(Body& b) {
    void* s_Bodies[c_MaxRagdollBodies];
    int s_Bones[c_MaxRagdollBodies];
    void* s_Set = nullptr;
    const int n = ReadRagdoll(b.m_Char, &s_Set, s_Bodies, s_Bones);
    const size_t s_Rig = b.m_Bones.size();

    if (n <= 0 || b.m_RigWorld.size() != s_Rig * 7) {
        if (n < 0) {
            Logger::Info("[Delirium] {}: its ragdoll is unreadable; Delirium lets it go.", b.m_Name);
            b.m_FollowGone = true;
        }
        else if (b.m_FollowSet) {
            Logger::Info("[Delirium] {}: the game's ragdoll is off; Delirium keeps driving the body.", b.m_Name);
            b.m_FollowSet = nullptr;
        }

        // Billy: shot, the game took its ragdoll off ~30 s later (not through its deactivate, which is blocked) and
        // the green skeleton walked off on its AI while Homer lay there. Down is down: the game's ragdoll on again.
        if (n == 0 && b.m_Npc && b.m_Ragdoll && !b.m_Dead && Functions::ZHM5BaseCharacter_ActivateRagdoll) {
            static int s_Logs = 0;

            if (s_Logs++ < 10)
                Logger::Info("[Delirium] {}: the game's ragdoll went off while its body is down; on again.", b.m_Name);
            Functions::ZHM5BaseCharacter_ActivateRagdoll->Call(b.m_Char, true);
            GameTook(b.m_Char);

            // Billy run 7 "teleporting": the game remakes its ragdoll where its bodies were last left (Jonathon
            // Credle's 4.11 m off Homer) and it was shown there a frame before the next Follow put it on Homer. On
            // Homer the frame it's made.
            static bool s_Again = false;

            if (!s_Again) {
                s_Again = true;
                Follow(b);
                s_Again = false;
            }

            return;
        }

        if (n == 0 && !b.m_Ragdoll && !b.m_Dead)
            b.m_Follow = false;

        return;
    }

    if (s_Set != b.m_FollowSet || b.m_FollowRig.size() != static_cast<size_t>(n)) {
        b.m_FollowSet = s_Set;
        b.m_FollowRig.assign(n, -1);
        b.m_FollowOff.assign(n * 7, 0.f);
        std::string s_Log;
        // Pelican: offsets from the kill frame (the bodies as made, the bones as shown), not after a game step.
        const std::vector<SMatrix> s_Start = TakeRagdollStart(b.m_Char);
        const bool s_AtKill = s_Start.size() == static_cast<size_t>(n) && b.m_RigAtKill.size() == b.m_RigWorld.size();

        for (int i = 0; i < n; ++i) {
            const std::string s_Class = ClassOf(s_Bodies[i]);

            if (s_Class.find("PhysicsObject") == std::string::npos) {
                Logger::Warn("[Delirium] {}: ragdoll body {} is a {}; Delirium lets it go.", b.m_Name, i, s_Class);
                b.m_FollowGone = true;
                return;
            }

            const auto it = std::find(b.m_Bones.begin(), b.m_Bones.end(), s_Bones[i]);

            if (it == b.m_Bones.end()) {
                s_Log += std::format("{}:bone {} (no rig) ", i, s_Bones[i]);
                continue;
            }

            const int r = static_cast<int>(it - b.m_Bones.begin());
            const SMatrix s_M = s_AtKill ? s_Start[i] : static_cast<IPhysicsObject*>(s_Bodies[i])->GetTransform();
            const Q s_Bq = FromAxes(s_M.XAxis, s_M.YAxis, s_M.ZAxis);
            // Against the bone as the game made the ragdoll on it: this frame's animation (Posed ran just before),
            // not NM's last pose. A running NPC's NM pose trails its animation by a step or two (Billy's runner:
            // 0.2 m on every body, baked into the offsets; after her get-up her belly sat apart from her body).
            const bool s_Anim = !s_AtKill && b.m_World.size() >= static_cast<size_t>(s_Bones[i] + 1) * 7;
            const float* w = s_Anim ? &b.m_World[s_Bones[i] * 7] : &(s_AtKill ? b.m_RigAtKill : b.m_RigWorld)[r * 7];
            const Q s_Wq {w[3], w[4], w[5], w[6]};
            const Q s_Oq = Norm(Mul(Conj(s_Wq), s_Bq));
            const SVector3 s_Op = Rot(Conj(s_Wq), SVector3(s_M.Trans.x, s_M.Trans.y, s_M.Trans.z) - V(w));
            float* o = &b.m_FollowOff[i * 7];
            o[0] = s_Op.x, o[1] = s_Op.y, o[2] = s_Op.z, o[3] = s_Oq.x, o[4] = s_Oq.y, o[5] = s_Oq.z, o[6] = s_Oq.w;

            // A body's place on its bone is the ragdoll's own (a few cm). Made while NM's body lay metres from the
            // character (Billy: the skeleton floating off Homer, the NPC teleporting, a corpse apart from its body: 6 m
            // in his run), the measured one is that gap: the last good one for this bone instead.
            // Per body: bone numbers differ between skeletons (a woman's bone 7 isn't a man's).
            // Good is a few cm (0.00-0.01 m in the logs). The old 0.25 m let a runner's 0.2 m lag in, and with no
            // good one yet the bad one was used: Billy's shot NPCs glued 0.8-1.3 m off Homer, swinging in circles as
            // he rolled and pushed through the floor (the ragdoll's bodies not placed yet as the game turned it on).
            // Kept per skeleton too (keyed by its bone count, so a woman's bone 7 isn't a man's); none at all: on the bone.
            auto& s_Good = b.m_GoodOff;
            const int s_Key = static_cast<int>(b.m_World.size() / 7) << 16 | s_Bones[i];

            if (s_Op.Length() < 0.05f)
                std::copy_n(o, 7, s_Good[s_Bones[i]].begin()), std::copy_n(o, 7, m_GoodOffAll[s_Key].begin());
            else if (const auto g = s_Good.find(s_Bones[i]); g != s_Good.end())
                std::copy_n(g->second.begin(), 7, o), s_Log += "(own) ";
            else if (const auto a = m_GoodOffAll.find(s_Key); a != m_GoodOffAll.end())
                std::copy_n(a->second.begin(), 7, o), s_Log += "(skeleton's) ";
            else // the stale body's turn is as wrong as its place: the bone's own (good ones log their turn below)
                o[0] = o[1] = o[2] = o[3] = o[4] = o[5] = 0.f, o[6] = 1.f, s_Log += "(on the bone) ";

            b.m_FollowRig[i] = r;
            SetKinematic(s_Bodies[i], true);
            s_Log += std::format("{}:bone {} {:.2f}m {:.0f}deg ", i, s_Bones[i], s_Op.Length(),
                2.f * std::acos(std::min(1.f, std::abs(o[6]))) * 57.2958f);
        }

        Logger::Info("[Delirium] {}: following its ragdoll's {} bodies ({}, offsets {}): {}", b.m_Name, n, ClassOf(s_Bodies[0]),
            s_AtKill ? "from the kill frame" : "from now", s_Log);

    }

    int s_Lost = 0, s_Put = 0;

    for (int i = 0; i < n; ++i) {
        const int r = b.m_FollowRig[i];

        if (r < 0)
            continue;

        HookSetKinematic(s_Bodies[i]);
        Hold(s_Bodies[i], true);

        if (!IsKinematic(s_Bodies[i]))
            SetKinematic(s_Bodies[i], true), ++s_Lost;

        const float* w = &b.m_RigWorld[r * 7];
        const float* o = &b.m_FollowOff[i * 7];
        const Q s_Wq {w[3], w[4], w[5], w[6]};
        const Q q = Norm(Mul(s_Wq, Q {o[3], o[4], o[5], o[6]}));
        // The game makes its ragdoll where the NPC stands (its animation), up to metres from Homer. Moved there
        // kinematically in one frame, a body swept through everything at tens of m/s (Billy: bumped and shot NPCs
        // glitching, jittering, no-clipping): one that far off is put there instead, then followed.
        const SMatrix s_To = ToMatrix(q, V(w) + Rot(s_Wq, V(o)));
        IPhysicsObject* s_Obj = static_cast<IPhysicsObject*>(s_Bodies[i]);
        const SMatrix s_Now = s_Obj->GetTransform();

        // Billy: Homer looked choppy. A kinematic move lands at Hitman's next physics step, so the mesh on it moved at
        // the physics rate; every body is put on its place each frame (it's only the mesh's; NM does the physics).
        if ((SVector3(s_Now.Trans.x, s_Now.Trans.y, s_Now.Trans.z) - SVector3(s_To.Trans.x, s_To.Trans.y, s_To.Trans.z)).Length() > 0.3f)
            ++s_Put;

        // Billy: "teleports a couple feet up when shot", Homer's pelvis right in the logs. Where the game's pelvis
        // body was left (before this move), where it goes, the entity and the engine's root: which one jumps.
        if (i == 0 && b.m_FollowLogs < 12) {
            ++b.m_FollowLogs;
            float x[3], y[3], z[3], t[3];
            int s_L = 0;
            const bool s_Root = Bones::World(b.m_Anim, x, y, z, t, s_L);
            const float4 e = b.m_Spatial->GetObjectToWorldMatrix().Trans;
            Logger::Info("[Delirium] {} follow {}: pelvis body was z {:.2f} ({:.2f} m off), put at z {:.2f}; entity z {:.2f}, "
                "engine root z {:.2f}.", b.m_Name, b.m_FollowLogs, s_Now.Trans.z,
                (SVector3(s_Now.Trans.x, s_Now.Trans.y, s_Now.Trans.z) - SVector3(s_To.Trans.x, s_To.Trans.y, s_To.Trans.z)).Length(),
                s_To.Trans.z, e.z, s_Root ? t[2] : -999.f);
        }

        g_OurMove = true;
        s_Obj->SetTransform(s_To);
        s_Obj->MoveTransform(s_To); // its kinematic target too: the physics step doesn't take it back elsewhere
        g_OurMove = false;
    }

    static int s_PutLogs = 0;

    if (s_Put && s_PutLogs < 10) {
        ++s_PutLogs;
        Logger::Info("[Delirium] {}: {} of its ragdoll's bodies put onto Homer (over 0.3 m off).", b.m_Name, s_Put);
    }

    static int s_LostLogs = 0;

    if (s_Lost && s_LostLogs < 5) {
        ++s_LostLogs;
        Logger::Info("[Delirium] {}: the game took {} of its ragdoll's bodies off kinematic; put back.", b.m_Name, s_Lost);
    }
}

bool Delirium::Attach(ZHM5BaseCharacter* p_Char, ZSpatialEntity* p_Spatial, bool p_Npc, bool p_Female,
    const Ray& p_Ray, const std::string& p_Name) {
    if (!p_Char || !p_Spatial || !Start())
        return false;

    Body b;
    b.m_Char = p_Char;
    b.m_Spatial = p_Spatial;
    b.m_Npc = p_Npc;
    b.m_Name = p_Name;
    Laps s_Lap {p_Name};
    // The rig (names, parents) from the morpheme entity; the pose from what draws him (47: his own animator).
    void* s_Rig = Bones::Animator(p_Char);
    // ponytail: 47's own animator (Bones::HitmanAnimator) gave a pose that isn't his mesh's (arms up), so the morpheme's.
    b.m_Anim = s_Rig;

    // Not posed yet (the hook only sees it from its next frame): try again later.
    std::vector<Bones::SQV> s_Local, s_RigLocal;

    if (!b.m_Anim || !s_Rig || !Bones::Read(b.m_Anim, s_Local) || !Bones::Read(s_Rig, s_RigLocal))
        return false;

    const int n = static_cast<int>(s_Local.size());
    s_Lap("read");

    if (s_RigLocal.size() != s_Local.size()) {
        Logger::Warn("[Delirium] {}: animator has {} bones, morpheme {}.", p_Name, s_Local.size(), s_RigLocal.size());
        return false;
    }

    b.m_Parents = Bones::Parents(s_Rig, n);

    if (b.m_Parents.empty() || n < 59) {
        Logger::Warn("[Delirium] {}: couldn't read the skeleton ({} bones).", p_Name, n);
        return false;
    }

    // Its bones by name (Hitman's rig is its own: 233 bones in the suit, 231 in a disguise).
    std::string s_Where;
    const std::vector<std::string> s_Names = Bones::Names(s_Rig, n, s_Where);
    static bool s_Dumped = false;

    if (!s_Dumped) {
        s_Dumped = true;
        std::string s_List;

        for (int i = 0; i < n; ++i)
            s_List += std::format("{}:{}<{} ", i, i < static_cast<int>(s_Names.size()) ? s_Names[i] : "?", b.m_Parents[i]);

        Logger::Info("[Delirium] {}'s rig ({} bones; names {}): {}", p_Name, n, s_Names.empty() ? "NOT FOUND" : s_Where,
            s_List);
    }

    if (s_Names.empty()) {
        Logger::Warn("[Delirium] {}: no bone names found, no body.", p_Name);
        return false;
    }

    s_Lap("names");

    // Which way Glacier's bone turns go (once): the one that stands the skeleton up like a person wins.
    if (!g_ConjKnown) {
        const auto s_Find = [&](const char* p_Bone) {
            for (int i = 0; i < n; ++i)
                if (_stricmp(s_Names[i].c_str(), p_Bone) == 0)
                    return i;

            return -1;
        };
        const int s_Lf = s_Find("l_foot"), s_Rf = s_Find("r_foot"), s_Hd = s_Find("head"), s_Lh = s_Find("l_hand"),
                  s_Rh = s_Find("r_hand");

        if (s_Lf >= 0 && s_Rf >= 0 && s_Hd >= 0 && s_Lh >= 0 && s_Rh >= 0) {
            b.m_Layout = -1;
            Q s_Rq;
            SVector3 s_Rp;
            Root(b, &s_Rq.x, s_Rp);
            std::string s_Bind;
            const std::vector<Bones::SQV> s_BindPose = Bones::BindPose(s_Rig, n, s_Bind);
            float s_Score[2];
            std::vector<float> w;

            for (int c = 0; c < 2; ++c) {
                Fk(s_Local, b.m_Parents, s_Rq, s_Rp, c == 1, w);
                const float s_Floor = s_Rp.z;
                s_Score[c] = std::abs(w[s_Lf * 7 + 2] - s_Floor) + std::abs(w[s_Rf * 7 + 2] - s_Floor) +
                    std::abs(w[s_Hd * 7 + 2] - s_Floor - 1.65f);
                Logger::Info("[Delirium] Bone turns {}: feet z {:.2f} {:.2f}, head z {:.2f}, hands z {:.2f} {:.2f} (floor {:.2f}).",
                    c ? "inverted" : "as stored", w[s_Lf * 7 + 2], w[s_Rf * 7 + 2], w[s_Hd * 7 + 2], w[s_Lh * 7 + 2],
                    w[s_Rh * 7 + 2], s_Floor);

                if (s_BindPose.size() == static_cast<size_t>(n)) {
                    Fk(s_BindPose, b.m_Parents, Q {0.f, 0.f, 0.f, 1.f}, SVector3(0.f, 0.f, 0.f), c == 1, w);
                    s_Score[c] += std::abs(w[s_Lh * 7 + 2] - w[s_Rh * 7 + 2]) + std::abs(w[s_Lf * 7 + 2] - w[s_Rf * 7 + 2]);
                    Logger::Info("[Delirium]   bind pose: hands {:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f}, feet z {:.2f} {:.2f}, head z {:.2f}.",
                        w[s_Lh * 7], w[s_Lh * 7 + 1], w[s_Lh * 7 + 2], w[s_Rh * 7], w[s_Rh * 7 + 1], w[s_Rh * 7 + 2],
                        w[s_Lf * 7 + 2], w[s_Rf * 7 + 2], w[s_Hd * 7 + 2]);
                }
            }

            g_Conj = s_Score[1] < s_Score[0];
            g_ConjKnown = true;
            b.m_Layout = 0;
            Logger::Info("[Delirium] Bone turns: {} (scores {:.2f} as stored, {:.2f} inverted).",
                g_Conj ? "INVERTED" : "as stored", s_Score[0], s_Score[1]);
        }
    }

    s_Lap("turns");

    if (!p_Npc) {
        std::string s_Map;

        for (int s_Part = 0; s_Part < c_Parts; ++s_Part) {
            char s_Pn[64] = {};
            nm_part_name(s_Part, s_Pn, sizeof(s_Pn));
            s_Map += std::format("{}:", s_Pn);

            for (int i = 0; i < n; ++i)
                if (nm_rig_part_for_name(s_Names[i].c_str()) == s_Part && s_Part != 0)
                    s_Map += std::format(" {}", s_Names[i]);

            s_Map += "; ";
        }

        Logger::Info("[Delirium] 47's parts: {}", s_Map);
    }

    // Which bones drive the neck (11) and head (12): only neck/neck1 and head should.
    {
        std::string s_Heads;

        for (int i = 0; i < n; ++i)
            if (const int s_P = nm_rig_part_for_name(s_Names[i].c_str()); s_P == 11 || s_P == 12)
                s_Heads += std::format("{}->{} ", s_Names[i], s_P);

        Logger::Info("[Delirium] {}: neck/head bones: {}", p_Name, s_Heads);
    }

    // Animated or not: each limb bone's turn now against its bind pose (0 degrees every time: the buffer holds bind
    // values there, and the drawn pose comes from a later layer).
    {
        std::string w, s_Diff;
        const std::vector<Bones::SQV> s_BindPose = Bones::BindPose(s_Rig, n, w);

        for (int i = 0; i < n && s_BindPose.size() == static_cast<size_t>(n); ++i) {
            for (const char* s_Bone : {"spine1", "neck", "l_clavicle", "l_upperarm", "l_forearm", "r_upperarm", "l_thigh", "l_calf", "r_thigh"}) {
                if (_stricmp(s_Names[i].c_str(), s_Bone) != 0)
                    continue;

                const float* x = s_Local[i].q;
                const float* y = s_BindPose[i].q;
                const float d = std::min(1.f, std::abs(x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + x[3] * y[3]));
                s_Diff += std::format("{} {:.0f} ", s_Bone, 2.f * std::acos(d) * 57.2958f);
            }
        }

        Logger::Info("[Delirium] {}: degrees off the bind pose: {}", p_Name, s_Diff.empty() ? "no bind pose" : s_Diff);
    }

    // The rig: the hips and everything under them, parents first (Hitman's own order).
    int s_HipsBone = -1;

    for (int i = 0; i < n && s_HipsBone < 0; ++i)
        if (nm_rig_part_for_name(s_Names[i].c_str()) == 0)
            s_HipsBone = i;

    if (s_HipsBone < 0) {
        Logger::Warn("[Delirium] {}: no pelvis among its bones.", p_Name);
        return false;
    }

    std::vector<int> s_InRig(n, -1);
    std::vector<int32_t> s_RigParents, s_Map;

    for (int i = s_HipsBone; i < n; ++i) {
        const int s_Parent = b.m_Parents[i];

        if (i == s_HipsBone || (s_Parent >= 0 && s_InRig[s_Parent] >= 0)) {
            s_InRig[i] = static_cast<int>(b.m_Bones.size());
            s_RigParents.push_back(i == s_HipsBone ? -1 : s_InRig[s_Parent]);
            int s_Part = nm_rig_part_for_name(s_Names[i].c_str());
            std::string s_Low = s_Names[i];
            std::ranges::transform(s_Low, s_Low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            for (const char* s_Skip : {"twist", "roll", "fa_", "xtra", "cloth", "holster", "bag", "attach", "prop", "weapon",
                     "skirt", "coat", "tie", "hair", "face", "jaw", "eye", "lip", "brow", "cheek", "tongue", "finger_", "_end"})
                if (s_Low.find(s_Skip) != std::string::npos)
                    s_Part = -1;

            s_Map.push_back(i == s_HipsBone ? 0 : s_Part == 0 ? -1 : s_Part);
            b.m_Bones.push_back(i);
        }
    }

    std::fill(std::begin(b.m_PartRig), std::end(b.m_PartRig), -1);

    for (size_t r = 0; r < s_Map.size(); ++r)
        if (s_Map[r] >= 0 && s_Map[r] < c_Parts && b.m_PartRig[s_Map[r]] < 0)
            b.m_PartRig[s_Map[r]] = static_cast<int>(r);

    // The pose it stands in now is the rest pose (an idle's: arms down, as Homer stands; Pelican's advice).
    s_Lap("map");

    if (!Posed(b))
        return false;

    // Billy: NPCs killed by a first headshot, before they had a body, teleported up. Hitman's death ragdoll had
    // already moved its root (to the pelvis): the animated bones read 0.9 m over where the NPC is (Samir_Chandra,
    // Herbert_West: pelvis body z 5.91, bones' hips 6.79), and Homer was made there. Its own ragdoll's pelvis is
    // where the NPC is: the root moved back by the gap while this body has it.
    if (p_Npc && b.m_PartRig[0] >= 0) {
        void* s_Rb[c_MaxRagdollBodies];
        int s_Bn[c_MaxRagdollBodies];
        void* s_Set = nullptr;
        const int s_Bone = b.m_Bones[b.m_PartRig[0]];

        for (int i = 0, n = ReadRagdoll(b.m_Char, &s_Set, s_Rb, s_Bn); i < n; ++i)
            if (s_Bn[i] == s_Bone) {
                const float4 t = static_cast<IPhysicsObject*>(s_Rb[i])->GetTransform().Trans;
                const SVector3 s_Gap = SVector3(t.x, t.y, t.z) - V(&b.m_Now[b.m_PartRig[0] * 7]);

                if (s_Gap.Length() > 0.2f && s_Gap.Length() < 3.f) {
                    b.m_RootFix = s_Gap;
                    Logger::Info("[Delirium] {}: the game's ragdoll was on first; its bones {:.2f} m off its pelvis "
                        "(z {:+.2f}): moved onto it.", p_Name, s_Gap.Length(), s_Gap.z);

                    if (!Posed(b))
                        return false;
                }

                break;
            }
    }

    const int s_N = static_cast<int>(b.m_Bones.size());
    std::vector<float> s_Places(s_N * 3);

    // Fitted on the live pose, as in 8333914 (the one build where the bodies lined up). The T-pose bind put the hanging
    // arms past Fred's joint limits (2.06 of range), so keyframed arms sat on the limit ~45 deg out.
    const std::vector<float>& s_Rest = b.m_Now;

    for (int r = 0; r < s_N; ++r)
        std::copy_n(&s_Rest[r * 7], 3, &s_Places[r * 3]);

    char s_Report[2048] = {};

    s_Lap("pose");
    const int s_Problems = nm_rig_check(s_N, s_RigParents.data(), s_Places.data(), s_Map.data(), s_Report, sizeof(s_Report));
    Logger::Info("[Delirium] {}'s rig check: {} ({})", p_Name, s_Problems > 0 ? s_Report : "clean", s_Problems);
    {
        std::string s_Mapped;

        for (int r = 0; r < s_N; ++r)
            if (s_Map[r] >= 0)
                s_Mapped += std::format("{}>{} ", s_Names[b.m_Bones[r]], s_Map[r]);

        Logger::Info("[Delirium] {}'s map: {}", p_Name, s_Mapped);
    }

    {
        const float* f = &b.m_Now[0];
        float s_Lo = 1e9f, s_Hi = -1e9f;

        for (int r = 0; r < s_N; ++r)
            s_Lo = std::min(s_Lo, b.m_Now[r * 7 + 2]), s_Hi = std::max(s_Hi, b.m_Now[r * 7 + 2]);

        const SMatrix s_M = p_Spatial->GetObjectToWorldMatrix();
        Logger::Info("[Delirium] {}: hips {:.2f} {:.2f} {:.2f}, bones from z {:.2f} to {:.2f}, entity at {:.2f} {:.2f} {:.2f}; "
            "bind pose {}.", p_Name, f[0], f[1], f[2], s_Lo, s_Hi, s_M.Trans.x, s_M.Trans.y, s_M.Trans.z,
            [&] { std::string w; return Bones::BindPose(s_Rig, n, w).empty() ? std::string("not found") : w; }());
    }

    // Pelican: the head takes its neck's length (joint 11 to 12). With neck and neck1 both on 11, the upper one could
    // become joint 11: a short neck shrank and sank the head (Billy: his head in his neck). Only the lower neck on 11.
    for (int r = 0; r < s_N; ++r)
        if (s_Map[r] == 11 && _stricmp(s_Names[b.m_Bones[r]].c_str(), "neck1") == 0)
            for (int q = 0; q < s_N; ++q)
                if (s_Map[q] == 11 && _stricmp(s_Names[b.m_Bones[q]].c_str(), "neck") == 0) {
                    s_Map[r] = -1;
                    break;
                }

    float s_Joints[63];

    if (nm_rig_joints(s_N, s_RigParents.data(), s_Places.data(), s_Map.data(), s_Joints) < 0) {
        Logger::Warn("[Delirium] {}: {}", p_Name, LastError());
        return false;
    }

    // Homer (or Wilma) fitted to this character's own proportions (Pelican): the keyframe then puts every part
    // exactly on its bone, no length fight (shaking) or baked-in pelvis height (floating). REAL: one world, scale 1.
    // No joint changes: the fit scales each part by its bone's length, so raising joint 10 by 10 cm stretched the mid
    // chest (part 9: radius 0.132 > 0.218 m, Billy's "rack", on every NPC too). Each body fitted to its own skeleton.
    NmBuild* s_Base = p_Female && m_Wilma ? m_Wilma : m_Build;
    s_Lap("check");
    // A short spine (Hitman: 3 bones for Homer's 4 parts) is spread by Fred's own joint shares in nm_rig_joints
    // (delirium 04b8d40fe); evenly, it made his lower spine parts long and fat (Billy's huge belly).
    // Homer as the Playroom has him (every Custom slider 1, Pelican): width 0.65 thinned his arms and shoulders too.
    b.m_Build = nm_build_fit_sized(s_Base, s_Joints, NM_SIZE_REAL);

    if (!b.m_Build) {
        Logger::Warn("[Delirium] {}: fit failed: {}", p_Name, LastError());
        return false;
    }

    // Billy: the head sat in the neck (the same with only the lower neck on joint 11: the fit was unchanged). Hitman's
    // skull is taller than the fitted head, which sits at the jaw: Pelican's raise and growth for a tall one.
    nm_build_set_head(b.m_Build, 0.05f, 1.08f);

    Logger::Info("[Delirium] {}: Homer fitted to its skeleton ({:.0f} kg).", p_Name, nm_build_get_weight(b.m_Build));

    // Once: each part's radius / half length, Homer's and fitted (Pelican: a spine part much shorter than his with the
    // same radius means the fit put the spine joints too close).
    static bool s_Table;

    if (!std::exchange(s_Table, true)) {
        std::string s_T;

        for (int i = 0; i < c_Parts; ++i) {
            float m, r0, h0, r1, h1;
            int s_Par;
            nm_build_part_info(s_Base, i, &m, &r0, &h0, &s_Par);
            nm_build_part_info(b.m_Build, i, &m, &r1, &h1, &s_Par);
            s_T += std::format("{}: r {:.3f}>{:.3f} h {:.3f}>{:.3f}; ", i, r0, r1, h0, h1);
        }

        Logger::Info("[Delirium] {}: parts, Homer > fitted: {}", p_Name, s_T);
    }

    const SVector3 s_Hips = V(&b.m_Now[0]);
    float s_Floor = s_Hips.z;

    for (int i = 0; i < n; ++i)
        s_Floor = std::min(s_Floor, b.m_World[i * 7 + 2]);

    SVector3 s_Hit, s_Normal;

    if (p_Ray(s_Hips, s_Hips - SVector3(0.f, 0.f, 2.5f), s_Hit, s_Normal))
        s_Floor = s_Hit.z;

    const float s_At[3] = {s_Hips.x, s_Hips.y, s_Floor};
    s_Lap("fit+ray");
    b.m_Agent = nm_agent_create(b.m_Build, s_At, 0.f, 0.37f);

    if (b.m_Agent) {
        // The fitted build stands in the skeleton's bind pose (nm_build_part_frame): every part put there.
        float s_Worst = 0.f;
        int s_WorstPart = -1;

        for (int i = 0; i < c_Parts; ++i) {
            float s_P[3], s_R[4], s_AP[3], s_AR[4];
            const float s_Zero[3] = {0.f, 0.f, 0.f};

            if (nm_build_part_frame(b.m_Build, i, s_P, s_R) != 0) // 0 is success (this skipped every part: no bake)
                continue;

            nm_agent_get_part_transform(b.m_Agent, i, s_AP, s_AR);
            const float d = (V(s_P) - V(s_AP)).Length();

            if (d > s_Worst)
                s_Worst = d, s_WorstPart = i;

            nm_agent_set_part_state(b.m_Agent, i, s_P, s_R, s_Zero, s_Zero);
        }

        if (b.m_PartRig[14] >= 0 && b.m_PartRig[15] >= 0) {
            std::string s_Arm;

            for (int s_Pt : {14, 15}) {
                float p[3], q[4];
                nm_build_part_frame(b.m_Build, s_Pt, p, q);
                const float* w = &s_Rest[b.m_PartRig[s_Pt] * 7];
                const Q s_Wq {w[3], w[4], w[5], w[6]}, s_Pq {q[0], q[1], q[2], q[3]};
                const Q s_D = Norm(Mul(Conj(s_Wq), s_Pq));
                s_Arm += std::format("part {}: bone rot ({:.3f} {:.3f} {:.3f} {:.3f}) at ({:.3f} {:.3f} {:.3f}), part rot ({:.3f} {:.3f} "
                    "{:.3f} {:.3f}) at ({:.3f} {:.3f} {:.3f}), {:.0f} deg apart; ", s_Pt, w[3], w[4], w[5], w[6], w[0], w[1], w[2],
                    q[0], q[1], q[2], q[3], p[0], p[1], p[2], 2.f * std::acos(std::min(1.f, std::abs(s_D.w))) * 57.2958f);

                if (s_Pt == 14) {
                    const SVector3 s_Off = Rot(Conj(s_Wq), V(p) - V(w));
                    b.m_ArmOff[0] = s_Off.x, b.m_ArmOff[1] = s_Off.y, b.m_ArmOff[2] = s_Off.z;
                    b.m_ArmOff[3] = s_D.x, b.m_ArmOff[4] = s_D.y, b.m_ArmOff[5] = s_D.z, b.m_ArmOff[6] = s_D.w;
                    const SVector3 s_Line = (V(&s_Rest[b.m_PartRig[15] * 7]) - V(w)).Normalized();
                    float s_Best = 180.f;
                    char s_Ax = '?';

                    for (int k = 0; k < 3; ++k) {
                        const SVector3 s_Axis = Rot(s_Pq, SVector3(k == 0, k == 1, k == 2));
                        const float s_A = std::acos(std::min(1.f, std::abs(s_Axis.Dot(s_Line)))) * 57.2958f;

                        if (s_A < s_Best)
                            s_Best = s_A, s_Ax = "XYZ"[k];
                    }

                    const SVector3 s_Y = Rot(s_Pq, SVector3(0.f, 1.f, 0.f));
                    s_Arm += std::format("capsule Y {:.0f} deg off the shoulder-elbow line (nearest axis {} at {:.0f}); ",
                        std::acos(std::min(1.f, std::abs(s_Y.Dot(s_Line)))) * 57.2958f, s_Ax, s_Best);
                }
            }

            std::string s_J;

            for (int k = 0; k < c_Parts; ++k)
                s_J += std::format("{:.3f} {:.3f} {:.3f}, ", s_Joints[k * 3], s_Joints[k * 3 + 1], s_Joints[k * 3 + 2]);

            Logger::Info("[Delirium] {}: bake check: {}rest upper-arm bone's axes along its line: {}; joints fitted from: {}",
                p_Name, s_Arm, AlongAxis(&s_Rest[b.m_PartRig[14] * 7], &s_Rest[b.m_PartRig[15] * 7]), s_J);
        }

        // Pelican's check: the upper arm on the A-pose bind reads ~1.0 of its range; ~0.4 means the parts aren't on it.
        float s_Ang[3];
        const float s_Arm = nm_agent_get_joint_angle(b.m_Agent, 14, s_Ang);
        Logger::Info("[Delirium] {}: parts put on the bind pose before the rig (furthest was part {}, {:.2f} m off; "
            "upper arm at {:.2f} of its range).", p_Name, s_WorstPart, s_Worst, s_Arm);

        // Billy: head and neck the right shape but not lined up. Once a run: neck and head parts' centres and long
        // axes against joints 11 (neck) and 12 (head), in the neck's line's terms (along / off it).
        static bool s_HeadLogged;

        if (!std::exchange(s_HeadLogged, true)) {
            const SVector3 j11 = V(&s_Joints[11 * 3]), j12 = V(&s_Joints[12 * 3]);
            const SVector3 s_Line = (j12 - j11).Normalized();
            std::string s_H;

            for (int s_Pt : {10, 11, 12}) {
                float p[3], q[4];
                nm_build_part_frame(b.m_Build, s_Pt, p, q);
                const SVector3 d = V(p) - j12, s_Y = Rot(Q {q[0], q[1], q[2], q[3]}, SVector3(0.f, 1.f, 0.f));
                const float s_Along = d.Dot(s_Line);
                s_H += std::format("part {}: centre {:.3f} m along the neck past joint 12, {:.3f} m off its line, axis {:.0f} deg "
                    "off it; ", s_Pt, s_Along, (d - s_Line * s_Along).Length(),
                    std::acos(std::min(1.f, std::abs(s_Y.Dot(s_Line)))) * 57.2958f);
            }

            Logger::Info("[Delirium] {}: head check: neck line joint 11 > 12 {:.3f} m; {}", p_Name, (j12 - j11).Length(), s_H);
        }
    }

    s_Lap("agent");
    b.m_Rig = b.m_Agent ? nm_rig_create(b.m_Agent, s_N, s_RigParents.data(), s_Rest.data(), s_Map.data()) : nullptr;

    if (!b.m_Rig) {
        Logger::Warn("[Delirium] {}: {}", p_Name, LastError());

        if (b.m_Agent)
            nm_agent_destroy(b.m_Agent);

        nm_build_destroy(b.m_Build);
        return false;
    }

    s_Lap("rig");
    nm_rig_take(b.m_Rig, b.m_Agent, b.m_Now.data(), nullptr, 0.f);

    // On its animation exactly until hit or killed (GTA IV's way); a free half-ragdoll walking it sent them flying.
    // Keyframed parts can't move, so a keyframed NPC stays out of the other bodies' contacts until it ragdolls.
    nm_agent_keyframe(b.m_Agent, "fb", 1);
    // 47 is in them (Pelican: the Playroom's bump staggers by contact; the across-the-map fling was the
    // deactivate-is-activate bug, the game ragdoll switching on under NM, not his keyframed body).
    nm_agent_collide_agents(b.m_Agent, b.m_Npc ? 0 : 1);

    Logger::Info("[Delirium] {} has {}'s body ({} bones, {} rigged).", p_Name, s_Base == m_Wilma ? "Wilma" : "Homer", n,
        s_N);
    Own(b.m_Char, true, b.m_Npc);
    m_Bodies.push_back(std::move(b));
    // Its ground now: shot on its first frame it had none (the replays: "no ground within 4 m" in 10 of 17 shots).
    UpdateGround(p_Ray, false);
    return true;
}

void Delirium::Unhook() {
    AbsUnhook(g_RagdollAt, reinterpret_cast<const void*>(g_Ragdoll));
    AbsUnhook(g_PoweredAt, reinterpret_cast<const void*>(g_Powered));
    AbsUnhook(g_DeactivateAt, reinterpret_cast<const void*>(g_Deactivate));
}

void Delirium::Detach(Body& p_Body) {
    // Its game ragdoll's bodies let go (held, the game couldn't move them: a frozen corpse).
    if (p_Body.m_Npc && p_Body.m_Follow) {
        void* s_Bodies[c_MaxRagdollBodies];
        int s_Bones[c_MaxRagdollBodies];
        void* s_Set = nullptr;

        for (int i = 0, n = ReadRagdoll(p_Body.m_Char, &s_Set, s_Bodies, s_Bones); i < n; ++i)
            SetKinematic(s_Bodies[i], false);
    }

    Own(p_Body.m_Char, false, false);
    Down(p_Body.m_Char, false);
    Bones::Override(p_Body.m_Anim, {});

    if (p_Body.m_Rig)
        nm_rig_destroy(p_Body.m_Rig);

    if (p_Body.m_Agent)
        nm_agent_destroy(p_Body.m_Agent);

    if (p_Body.m_Build)
        nm_build_destroy(p_Body.m_Build);

    p_Body.m_Rig = nullptr;
    p_Body.m_Agent = nullptr;
    p_Body.m_Build = nullptr;
}

void Delirium::SetPortals(const DpFrame* p_Frames) {
    m_PortalsOpen = p_Frames != nullptr;

    if (p_Frames)
        m_Portals[0] = p_Frames[0], m_Portals[1] = p_Frames[1];
}

bool Delirium::HitmanDown() {
    const Body* b = Hitman();
    return b && (b->m_Ragdoll || b->m_Blend > 0.f);
}

bool Delirium::Dive(const SVector3& p_Vel) {
    Body* b = Hitman();

    if (!b || b->m_Ragdoll)
        return b != nullptr;

    SetHitmanRagdoll(true);
    const float s_V[3] = {p_Vel.x, p_Vel.y, p_Vel.z};
    nm_agent_add_velocity(b->m_Agent, s_V);
    Logger::Info("[Delirium] 47 into the floor portal as a ragdoll at {:.1f} m/s.", p_Vel.Length());
    return true;
}

void Delirium::SetHitmanRagdoll(bool p_On) {
    m_HitmanRagdoll = p_On;

    if (Body* b = Hitman())
        SetRagdoll(*b, p_On);
}

void Delirium::SetRagdoll(Body& p_Body, bool p_On) {
    HitchTimer s_T {p_On ? "Ragdoll on" : "Ragdoll off"};
    Body* b = &p_Body;

    if (b->m_Ragdoll == p_On)
        return;

    b->m_Ragdoll = p_On;

    if (p_On) {
        nm_agent_keyframe(b->m_Agent, "fb", 0);

        // Pelican (Bobby_Caretaker-1794866323984): a keyframed foot came in at 9.6 m/s from an animation jump and the
        // ragdoll started with it. Each part at most 3 m/s off the pelvis's speed, spin at most 10 rad/s.
        {
            float p[3], q[4], s_V0[3], w[3];
            nm_agent_get_part_velocity(b->m_Agent, 0, s_V0, w);

            for (int i = 0; i < c_Parts; ++i) {
                float v[3];
                nm_agent_get_part_transform(b->m_Agent, i, p, q);
                nm_agent_get_part_velocity(b->m_Agent, i, v, w);
                SVector3 d = V(v) - V(s_V0), s_W = V(w);

                if (d.Length() <= 3.f && s_W.Length() <= 10.f)
                    continue;

                if (d.Length() > 3.f)
                    d = d.Normalized() * 3.f;

                if (s_W.Length() > 10.f)
                    s_W = s_W.Normalized() * 10.f;

                const float s_NewV[3] = {s_V0[0] + d.x, s_V0[1] + d.y, s_V0[2] + d.z}, s_NewW[3] = {s_W.x, s_W.y, s_W.z};
                nm_agent_set_part_state(b->m_Agent, i, p, q, s_NewV, s_NewW);
            }
        }
        nm_agent_collide_agents(b->m_Agent, 1);
        b->m_HighFall = false;
        b->m_PoseA.clear();
        b->m_PoseB.clear();
        b->m_GettingUp = false;
        b->m_Dead ? nm_agent_task_relax(b->m_Agent, -1.f, -1.f) : nm_agent_task_balance(b->m_Agent, m_World, "*", nullptr);
        const SMatrix s_M = b->m_Spatial->GetObjectToWorldMatrix();
        b->m_HeldZ = s_M.Trans.z;
        b->m_HeldRot = s_M;

        // Billy: knocked down mid walk cycle, the game's AI walked the NPC on without its body. The game's own ragdoll
        // stops it; its bodies then follow NM's (Follow), as a kill's do. Off again at the get-up.
        if (b->m_Npc)
            Down(b->m_Char, true);

        if (b->m_Npc && !b->m_Follow && Functions::ZHM5BaseCharacter_ActivateRagdoll) {
            Functions::ZHM5BaseCharacter_ActivateRagdoll->Call(b->m_Char, true);
            b->m_Follow = true;
            b->m_FollowSet = nullptr;
            b->m_FollowLogs = 0;
        }
    }
    else {
        // Back on his animation from where it is now, blended from the ragdoll's last pose (the get-up).
        Bones::Override(b->m_Anim, {});
        b->m_RootFix = SVector3(0.f, 0.f, 0.f); // the game's ragdoll goes off: its root is its own again
        Down(b->m_Char, false);

        // The game's ragdoll (made at the knock-down) off: its bodies let go, then the game's own deactivate.
        if (b->m_Npc && b->m_Follow && !b->m_Dead) {
            void* s_Bodies[c_MaxRagdollBodies];
            int s_Bones[c_MaxRagdollBodies];
            void* s_Set = nullptr;

            for (int i = 0, n = ReadRagdoll(b->m_Char, &s_Set, s_Bodies, s_Bones); i < n; ++i)
                SetKinematic(s_Bodies[i], false);

            // Hitman's own get-up from where its ragdoll lies (on NM's body); no clip: just off. One that kept his
            // feet just goes back to his animation (Billy: Hitman's get-up from standing knocked two NPCs over).
            if (b->m_SettledT <= 1.5f && StartRecovery(b->m_Char)) {
                b->m_Recover = 10.f; // 6 s timed out mid get-up
                Logger::Info("[Delirium] {}: Hitman's get-up.", b->m_Name);
            }
            else
                OurDeactivate(b->m_Char);

            b->m_Follow = false;
            b->m_FollowSet = nullptr;
        }

        if (b->m_Npc) {
            float s_Pos[3], s_Rot[4];
            nm_agent_get_part_transform(b->m_Agent, 0, s_Pos, s_Rot);
            SMatrix s_M = b->m_HeldRot;
            float s_Head[3] = {};
            int s_FaceUp = 0;
            // Lying only: standing, its "head way" is any way (Billy: the body flipped sideways as it stood back up).
            const bool s_Lying = nm_agent_get_lying(b->m_Agent, s_Head, &s_FaceUp) > 0.f;
            SVector3 s_Face(s_Head[0], s_Head[1], 0.f);

            if (s_Lying && s_Face.Length() > 0.5f) {
                s_Face = (s_FaceUp ? s_Face * -1.f : s_Face).Normalized();
                s_M.YAxis = float4(s_Face.x, s_Face.y, 0.f, 0.f);
                s_M.XAxis = float4(s_Face.y, -s_Face.x, 0.f, 0.f); // forward x up
                s_M.ZAxis = float4(0.f, 0.f, 1.f, 0.f);
            }

            s_M.Trans.x = s_Pos[0];
            s_M.Trans.y = s_Pos[1];
            const auto s_G = m_LastGround.find(b->m_Agent);
            s_M.Trans.z = s_G != m_LastGround.end() ? s_G->second : b->m_HeldZ;
            b->m_Spatial->SetObjectToWorldMatrixFromEditor(s_M);

            // The blend into the animation starts from the lying pose: its hips put back under the new root (they were
            // worked out against the old one), so the NPC rises out of where it lay instead of popping upright.
            const size_t n = b->m_Bones.size();
            const int s_H = b->m_PoseHips;

            if (b->m_Recover > 0.f)
                b->m_Written.clear();
            else if (s_H >= 0 && s_H < static_cast<int>(n) && b->m_PoseB.size() == n * 4 + 3 &&
                b->m_Written.size() == b->m_Local.size()) {
                std::vector<float> s_W;
                Fk(b->m_Local, b->m_Parents, FromAxes(s_M.XAxis, s_M.YAxis, s_M.ZAxis),
                    SVector3(s_M.Trans.x, s_M.Trans.y, s_M.Trans.z), g_Conj, s_W);
                const int s_Bone = b->m_Bones[s_H], s_Parent = b->m_Parents[s_Bone];
                Q s_Pq = FromAxes(s_M.XAxis, s_M.YAxis, s_M.ZAxis);
                SVector3 s_Pp(s_M.Trans.x, s_M.Trans.y, s_M.Trans.z);

                if (s_Parent >= 0) {
                    const float* w = &s_W[s_Parent * 7];
                    s_Pq = {w[3], w[4], w[5], w[6]};
                    s_Pp = V(w);
                }

                const float* h = &b->m_PoseB[s_H * 4];
                const Q s_Lq = Norm(Mul(Conj(s_Pq), Q {h[0], h[1], h[2], h[3]}));
                const SVector3 s_Lp = Rot(Conj(s_Pq), V(&b->m_PoseB[n * 4]) - s_Pp);
                Bones::SQV& l = b->m_Written[s_Bone];
                l.q[0] = g_Conj ? -s_Lq.x : s_Lq.x, l.q[1] = g_Conj ? -s_Lq.y : s_Lq.y, l.q[2] = g_Conj ? -s_Lq.z : s_Lq.z;
                l.q[3] = s_Lq.w;
                l.p[0] = s_Lp.x, l.p[1] = s_Lp.y, l.p[2] = s_Lp.z;
            }
            else
                b->m_Written.clear();
        }

        b->m_Blend = b->m_Written.empty() ? 0.f : c_GetUp;

        if (!b->m_Now.empty())
            nm_rig_take(b->m_Rig, b->m_Agent, b->m_Now.data(), nullptr, 0.f);

        nm_agent_keyframe(b->m_Agent, "fb", 1);
        nm_agent_collide_agents(b->m_Agent, b->m_Npc ? 0 : 1);
    }

    Logger::Info("[Delirium] {} ragdoll {}.", b->m_Name, p_On ? "on" : "off");
}

// Delirium on Hitman's NPCs: the nearest few near 47 get a body, the far, gone and out-of-mode ones lose theirs.
void Delirium::UpdateNpcs(const Ray& p_Ray, const SVector3& p_Feet) {
    std::vector<std::pair<float, TEntityRef<ZActor>>> s_Near;

    if (m_NpcMode && Globals::ActorManager) {
        for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
            ZActor* s_Actor = s_Entry.m_pInterfaceRef;
            const auto s_Spatial = s_Entry.m_entityRef ? s_Entry.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

            if (!s_Actor || !s_Spatial)
                continue;

            const float4 t = s_Spatial->GetObjectToWorldMatrix().Trans;
            const float s_D = (SVector3(t.x, t.y, t.z) - p_Feet).Length();

            if (s_D < 25.f)
                s_Near.emplace_back(s_D, s_Entry);
        }
    }

    std::ranges::sort(s_Near, {}, &std::pair<float, TEntityRef<ZActor>>::first);

    // Billy: Numpad 1 has to give a body to whoever 47 walks up to, anywhere, not just those near him when he pressed
    // it. Bodies went to the first 6 within 25 m and stayed theirs while they were there: walking on, the ones he
    // reached got none. Only the 6 nearest (within 15 m) standing ones keep theirs.
    std::vector<void*> s_Want;

    for (const auto& [s_D, s_Actor] : s_Near)
        if (s_Want.size() < c_MaxNpcs && s_D <= 15.f && !s_Actor.m_pInterfaceRef->IsDead())
            s_Want.push_back(s_Actor.m_pInterfaceRef);

    std::erase_if(m_Bodies, [&](Body& b) {
        // A body that's down stays on (shot from afar).
        const bool s_Keep = !b.m_Npc || b.m_Ragdoll || b.m_Dead || b.m_Ko || std::ranges::find(s_Want, b.m_Char) != s_Want.end();

        if (!s_Keep)
            Detach(b);

        return !s_Keep;
    });

    // Billy: after enough kills, bumped NPCs stopped ragdolling. The dead (kept on) counted against the 6: only those up count.
    int s_Count = static_cast<int>(std::ranges::count_if(m_Bodies, [](const Body& b) { return b.m_Npc && !b.m_Dead; }));

    for (const auto& [s_D, s_Actor] : s_Near) {
        if (s_Count >= c_MaxNpcs || s_D > 15.f || m_Bodies.size() >= c_MaxMeshes - 1)
            break;

        ZActor* a = s_Actor.m_pInterfaceRef;

        if (std::ranges::any_of(m_Bodies, [&](const Body& b) { return b.m_Char == a; }) || a->IsDead())
            continue;

        const ZGlobalOutfitKit* s_Outfit = a->m_rOutfit.m_pInterfaceRef;
        const bool s_Female = s_Outfit && s_Outfit->m_bIsFemale;

        HitchTimer s_T {"NPC body made"};

        if (Attach(a, s_Actor.m_entityRef.QueryInterface<ZSpatialEntity>(), true, s_Female, p_Ray,
            std::string(a->m_sActorName.c_str(), a->m_sActorName.size())))
            ++s_Count;

        break; // the next one next frame
    }
}

void Delirium::Snapshot() {
    std::vector<std::vector<float>> s_Pose;
    std::vector<std::string> s_Names;
    std::vector<std::vector<float>> s_Sizes;
    const auto s_Add = [&](NmAgent* a, const NmBuild* p_Build = nullptr) {
        s_Pose.emplace_back(a ? c_Parts * 7 : 0);
        s_Sizes.emplace_back();

        if (!a)
            return;

        for (int i = 0; i < c_Parts; ++i) {
            float m, r = m_Radius[i], h = m_Half[i];
            int s_Parent;

            if (p_Build)
                nm_build_part_info(p_Build, i, &m, &r, &h, &s_Parent);

            float s_In[3], s_Box[3] = {};

            if (nm_agent_get_part_body(a, i, &m, s_In, s_Box) != 2)
                s_Box[0] = s_Box[1] = s_Box[2] = 0.f;

            s_Sizes.back().insert(s_Sizes.back().end(), {r, h, s_Box[0], s_Box[1], s_Box[2]});
        }

        auto& [s_Prev, s_Cur] = m_DrawPose[a];

        if (m_Stepped || s_Cur.size() != c_Parts * 7) {
            s_Prev = s_Cur;
            s_Cur.resize(c_Parts * 7);
            nm_agent_get_part_transforms(a, s_Cur.data(), c_Parts);
        }

        std::vector<float>& o = s_Pose.back();
        o = s_Cur;

        if (s_Prev.size() != o.size())
            return;

        // Billy: 47's Homer drawn behind him running (a "hump"): one step between the last two stepped poses is 1/60 s
        // late, ~10 cm at a run. A body on its animation is drawn at the newest step.
        const float t = std::ranges::any_of(m_Bodies, [&](const Body& b) { return b.m_Agent == a && !b.m_Ragdoll; }) ? 1.f : m_Alpha;

        for (int i = 0; i < c_Parts; ++i) {
            float* c = &o[i * 7];
            const float* p = &s_Prev[i * 7];

            if ((V(c) - V(p)).Length() > 1.f) // through a portal: no smear
                continue;

            for (int k = 0; k < 3; ++k)
                c[k] = p[k] + (c[k] - p[k]) * t;

            const float s = p[3] * c[3] + p[4] * c[4] + p[5] * c[5] + p[6] * c[6] < 0.f ? -1.f : 1.f;
            const Q m = Norm(Q {p[3] + (c[3] * s - p[3]) * t, p[4] + (c[4] * s - p[4]) * t, p[5] + (c[5] * s - p[5]) * t,
                p[6] + (c[6] * s - p[6]) * t});
            c[3] = m.x, c[4] = m.y, c[5] = m.z, c[6] = m.w;
        }
    };

    // In target order: 47, the peds, the NPCs.
    const Body* s_Hitman = Hitman();
    s_Add(s_Hitman ? s_Hitman->m_Agent : nullptr, s_Hitman ? s_Hitman->m_Build : nullptr);

    for (const Ped& s_Ped : m_Peds) {
        s_Add(s_Ped.m_Agent);
        s_Names.push_back(s_Ped.m_Name);
    }

    for (const Body& s_Body : m_Bodies) {
        if (s_Body.m_Npc) {
            s_Add(s_Body.m_Agent, s_Body.m_Build);
            s_Names.push_back(s_Body.m_Name + (s_Body.m_Ragdoll ? " (ragdoll)" : ""));
        }
    }

    // Hitman's skeletons as NM reads them (green): they should sit on the bodies.
    std::vector<SVector3> s_Lines;

    for (const Body& b : m_Bodies)
        for (size_t r = 0; r < b.m_Bones.size() && b.m_Now.size() == b.m_Bones.size() * 7; ++r)
            if (const int p = b.m_Parents[b.m_Bones[r]]; p >= 0 && b.m_World.size() > static_cast<size_t>(p) * 7 + 2)
                s_Lines.insert(s_Lines.end(), {V(&b.m_Now[r * 7]), V(&b.m_World[p * 7])});

    std::scoped_lock s_Lock(m_Mutex);
    m_Lines = std::move(s_Lines);
    m_Pose = std::move(s_Pose);
    m_Sizes = std::move(s_Sizes);
    m_Names = std::move(s_Names);
    m_PedsDrawn = m_Peds.size();
}

void Delirium::Update(float p_Dt, const Ray& p_Ray, const SVector3& p_Eye, const SVector3& p_Aim,
    const SVector3& p_Feet, const SVector3& p_Facing, ZHM5BaseCharacter* p_Hitman, ZSpatialEntity* p_HitmanSpatial) {
    m_Eye = p_Eye;
    m_Aim = p_Aim;
    m_Clock += p_Dt;
    Bones::Tick();

    // Hitman's get-up runs on its powered ragdoll: that isn't a kill (unless it died meanwhile).
    for (Body& b : m_Bodies)
        b.m_UpGrace = std::max(0.f, b.m_UpGrace - p_Dt);

    for (Body& b : m_Bodies)
        if (b.m_Recover > 0.f) {
            GameTook(b.m_Char);
            const bool s_On = GameRagdollOn(b.m_Char);

            if ((b.m_Recover -= p_Dt) <= 0.f || !s_On || b.m_Ragdoll || static_cast<ZActor*>(b.m_Char)->IsDead()) {
                // Its ragdoll already off: no deactivate (the same frame then saw it on again and knocked the NPC
                // straight back down, 5 of Billy's 13 get-ups).
                if (s_On && !b.m_Ragdoll && !static_cast<ZActor*>(b.m_Char)->IsDead())
                    OurDeactivate(b.m_Char);

                GameTook(b.m_Char);
                b.m_UpGrace = 1.f;
                Logger::Info("[Delirium] {}: up ({}).", b.m_Name, s_On ? "timed out" : "Hitman's get-up done");
                b.m_Recover = 0.f;
            }
        }

    // A kill: the game's death ragdoll runs (blocking it crashed), its bodies then follow NM's (Follow).
    for (Body& b : m_Bodies)
        if (b.m_Npc && !b.m_Follow &&
            ((b.m_Recover <= 0.f && b.m_UpGrace <= 0.f) || static_cast<ZActor*>(b.m_Char)->IsDead()) &&
            (GameTook(b.m_Char) || static_cast<ZActor*>(b.m_Char)->IsDead()) && GameRagdollOn(b.m_Char)) {
            // A fresh activation by the game (or a death), not just its flag on again (Hitman's get-up ending).
            // Only a ragdoll that's really on (the end of Hitman's get-up left a stale "took" that set this on a standing
            // NPC: its next knock-down didn't stop its AI, the skeleton walked on). The body is Delirium's at once.
            Logger::Info("[Delirium] {}: the game ragdolled it; its ragdoll follows Delirium's body.", b.m_Name);
            b.m_Follow = true;
            b.m_RigAtKill = b.m_RigWorld; // last frame's: what the mesh showed as the ragdoll was made

            if (!b.m_Ragdoll && !b.m_Dead && !static_cast<ZActor*>(b.m_Char)->IsDead()) {
                SetRagdoll(b, true);
                // Billy: a can thrown at a head; Hitman ragdolled him (dazed, out cold) with no hit points lost, and
                // Delirium took it for a pistol shot: Homer balanced. No damage: a thrown object's knock.
                const auto h = m_Hp.find(b.m_Char);
                const bool s_NoDamage = h != m_Hp.end() && static_cast<ZActor*>(b.m_Char)->m_fCurrentHitPoints >= h->second - 0.01f;

                // Billy: a fire extinguisher to the head still read as a shot (it costs hit points). No shot from 47's
                // gun in the last 0.5 s: thrown.
                if (s_NoDamage || Thrown())
                    Knock(b);
                else if (b.m_Hit == 0)
                    ShootAtAim(b);
            }

            if (!b.m_Dead && static_cast<ZActor*>(b.m_Char)->IsDead()) {
                b.m_Dead = true;
                SetRagdoll(b, true);

                if (b.m_Hit == 0)
                    ShootAtAim(b);

                if (b.m_Health >= 50.f)
                    nm_agent_set_dead(b.m_Agent, 1);
            }
        }

    std::erase_if(m_Bodies, [&](Body& b) {
        if (!b.m_FollowGone)
            return false;

        Detach(b);
        return true;
    });

    UpdateCripple();

    std::vector<void*> s_Hits;
    {
        std::scoped_lock s_Lock(m_HitMutex);
        s_Hits.swap(m_Hits);
    }

    // Billy's run: no YouGotHit for 47's bullets at all, so shots never reached Delirium. A drop in an NPC's hit
    // points (ZActor +0x1028) is a hit.
    // Billy: an object thrown at someone should knock him off balance and over, not hard. Hitman knocks him out
    // (pacified, not dead) with 47 out of reach (not a choke or a punch): a gentle push from 47's side, then down.
    std::vector<void*> s_Throws;

    if (Globals::ActorManager)
        for (const auto& e : Globals::ActorManager->m_activatedActors)
            if (ZActor* a = e.m_pInterfaceRef) {
                const bool s_Pac = a->IsPacified() && !a->IsDead();
                bool& s_Was = m_Pacified[a];

                // Billy: Numpad 1 or not, anyone on the map hit by something thrown gets Delirium. The game ragdolls him
                // (bodiless) with no gunshot from 47: a body and the knock.
                {
                    const bool s_Rag = GameRagdollOn(a);
                    bool& s_RagWas = m_GameRag[a];

                    if (s_Rag && !s_RagWas && !a->IsDead() && Thrown() &&
                        std::ranges::none_of(m_Bodies, [&](const Body& b) { return b.m_Char == static_cast<void*>(a); }))
                        s_Throws.push_back(a);

                    s_RagWas = s_Rag;
                }

                if (s_Pac && !s_Was && e.m_entityRef)
                    if (ZSpatialEntity* s = e.m_entityRef.QueryInterface<ZSpatialEntity>()) {
                        const float4 t = s->GetObjectToWorldMatrix().Trans;

                        if ((SVector3(t.x, t.y, m_Eye.z) - m_Eye).Length() > 2.5f)
                            s_Throws.push_back(a);
                    }

                s_Was = s_Pac;
            }

    if (Globals::ActorManager)
        for (const auto& e : Globals::ActorManager->m_activatedActors)
            if (ZActor* a = e.m_pInterfaceRef) {
                const float s_Hp = a->m_fCurrentHitPoints;
                const auto it = m_Hp.find(a);

                if (it != m_Hp.end() && s_Hp < it->second - 0.01f) {
                    static int s_Logs = 0;

                    if (s_Logs++ < 20)
                        Logger::Info("[Delirium] {}: hit ({:.0f} > {:.0f} hit points).", a->m_sActorName.c_str(), it->second, s_Hp);

                    s_Hits.push_back(a);
                }

                m_Hp[a] = s_Hp;
            }

    // Billy: the first hit on an NPC without a body didn't give it one (Attach failed, its bones unreadable that
    // frame); the next hit did. A failed one tries again each frame for a second.
    for (auto it = m_Pending.begin(); it != m_Pending.end();)
        if ((it->second -= p_Dt) <= 0.f || std::ranges::any_of(m_Bodies, [&](const Body& b) { return b.m_Char == it->first; }))
            it = m_Pending.erase(it);
        else
            s_Hits.push_back(it++->first);

    s_Hits.insert(s_Hits.end(), s_Throws.begin(), s_Throws.end());
    std::ranges::sort(s_Hits);
    s_Hits.erase(std::unique(s_Hits.begin(), s_Hits.end()), s_Hits.end());

    // Billy: after a while shots stopped making Delirium ragdolls. 29 bodies in his run (the dead stay on) filled the
    // 31 there's ground for: the dead body furthest from 47 goes back to the game for each new one.
    for (size_t k = 0; k < s_Hits.size() && m_Bodies.size() >= c_MaxMeshes - 1; ++k) {
        auto s_Far = m_Bodies.end();
        float s_Most = -1.f;

        for (auto it = m_Bodies.begin(); it != m_Bodies.end(); ++it) {
            float p[3], q[4];

            if (!it->m_Npc || !it->m_Dead || !it->m_Agent)
                continue;

            nm_agent_get_part_transform(it->m_Agent, 0, p, q);

            if (const float d = (V(p) - m_Eye).Length(); d > s_Most)
                s_Most = d, s_Far = it;
        }

        if (s_Far == m_Bodies.end())
            break;

        Logger::Info("[Delirium] {}: dead {:.0f} m away, back to the game's ragdoll (room for a new body).", s_Far->m_Name, s_Most);
        Detach(*s_Far);
        m_Bodies.erase(s_Far);
    }

    for (void* c : s_Hits) {
        // Billy: shot from afar (past the 15 m / 6 NPCs that get bodies), it reacted as Hitman's. Its body now.
        if (Globals::ActorManager && std::ranges::none_of(m_Bodies, [&](const Body& b) { return b.m_Char == c; }) &&
            std::ranges::count_if(m_Bodies, [](const Body& b) { return b.m_Npc && !b.m_Dead; }) < c_MaxNpcs + 4 &&
            m_Bodies.size() < c_MaxMeshes - 1)
            for (const auto& e : Globals::ActorManager->m_activatedActors)
                if (ZActor* a = e.m_pInterfaceRef; a && static_cast<void*>(a) == c && e.m_entityRef) {
                    const ZGlobalOutfitKit* s_Outfit = a->m_rOutfit.m_pInterfaceRef;
                    if (Attach(a, e.m_entityRef.QueryInterface<ZSpatialEntity>(), true, s_Outfit && s_Outfit->m_bIsFemale, p_Ray,
                        std::string(a->m_sActorName.c_str(), a->m_sActorName.size()))) {
                        Logger::Info("[Delirium] {}: shot without a body, given one{}.", a->m_sActorName.c_str(),
                            m_Pending.contains(c) ? " (on a retry)" : "");
                        m_Pending.erase(c);
                    }
                    else
                        m_Pending.try_emplace(c, 1.f);

                    break;
                }

        for (Body& b : m_Bodies)
            if (b.m_Char == c && b.m_Npc) {
                // Billy: killed in Hitman, Homer stayed standing (NM's own health, 200, wasn't down yet). The kill
                // shot kills NM too: health 1 first, so its shot reaction is the death (set_dead first skips it).
                ZActor* a = static_cast<ZActor*>(b.m_Char);
                const bool s_Kill = a->m_fCurrentHitPoints <= 0.f || a->IsDead();
                SetRagdoll(b, true);

                if (std::ranges::find(s_Throws, c) != s_Throws.end()) {
                    Knock(b);
                    continue;
                }

                if (s_Kill)
                    b.m_Health = std::min(b.m_Health, 1.f);

                ShootAtAim(b);

                if (s_Kill) {
                    if (b.m_Health > 0.f)
                        nm_agent_set_dead(b.m_Agent, 1);

                    b.m_Dead = true;
                    Logger::Info("[Delirium] {}: killed in Hitman; Homer dies.", b.m_Name);
                }
            }
    }

    // Billy: dead bodies float and 47 can't push them. Every 2 s (10 lines a run): a dead body's pelvis over its
    // ground, its speed, and how far 47 is.
    {
        static float s_Every = 0.f;
        static int s_Logs = 0;

        if ((s_Every -= p_Dt) <= 0.f && s_Logs < 10) {
            s_Every = 2.f;

            for (const Body& b : m_Bodies)
                if (b.m_Npc && b.m_Dead) {
                    float p[3], q[4], v[3], w[3];
                    nm_agent_get_part_transform(b.m_Agent, 0, p, q);
                    nm_agent_get_part_velocity(b.m_Agent, 0, v, w);
                    const auto g = m_LastGround.find(b.m_Agent);
                    // Pelican: what NM's own world has under the pelvis (a flat floor rests it at 0.13 m).
                    const float s_Down[3] = {0.f, 0.f, -1.f};
                    float s_Dist = -1.f, s_N[3] = {};
                    nm_world_raycast(m_World, p, s_Down, 3.f, &s_Dist, s_N);
                    Logger::Info("[Delirium] {}: dead, pelvis {:.2f} m over its ground (the game's), {:.2f} m over NM's (normal z "
                        "{:.2f}), {:.1f} m/s, 47 {:.1f} m away, awake {}.", b.m_Name, g != m_LastGround.end() ? p[2] - g->second : -1.f,
                        s_Dist, s_N[2], V(v).Length(), (V(p) - m_Eye).Length(), nm_agent_is_awake(b.m_Agent));
                    ++s_Logs;
                    break;
                }
        }
    }

    // Numpad 1: Delirium on NPCs. Numpad 2: 47 ragdolls. Numpad 5 / 6: shoot / push the body you aim at.
    static bool s_K1, s_K2, s_K5, s_K6;

    if (KeyPressed(VK_NUMPAD1, s_K1)) {
        m_NpcMode = !m_NpcMode;
        m_NpcTimer = 0.f;
        Logger::Info("[Delirium] NPC ragdolls {}.", m_NpcMode ? "on" : "off");
    }

    bool s_ToggleRagdoll = KeyPressed(VK_NUMPAD2, s_K2);
    const bool s_Shoot = KeyPressed(VK_NUMPAD5, s_K5);
    const bool s_Push = KeyPressed(VK_NUMPAD6, s_K6);

    bool s_Spawn, s_Clear, s_Npc;
    std::vector<std::pair<int, std::string>> s_Fire;
    {
        std::scoped_lock s_Lock(m_Mutex);
        s_Spawn = std::exchange(m_WantSpawn, false);
        s_Npc = std::exchange(m_WantNpc, false);
        s_Clear = std::exchange(m_WantClear, false);
        s_Fire = std::exchange(m_WantFire, {});
    }

    if (s_Npc)
        SpawnNpc(p_HitmanSpatial, p_Facing);

    if (s_Clear) {
        for (Ped& s_Ped : m_Peds)
            nm_agent_destroy(s_Ped.m_Agent);

        m_Peds.clear();
    }

    if (s_Spawn)
        Spawn(p_Ray, p_Feet, p_Facing);

    // 47's Homer body: on him while the menu says so (and to ragdoll him).
    Body* s_Me = Hitman();

    if (s_Me && (s_Me->m_Char != p_Hitman || !m_HitmanBody)) {
        Detach(*s_Me);
        std::erase_if(m_Bodies, [](const Body& b) { return !b.m_Npc; });
        m_HitmanRagdoll = false;
        s_Me = nullptr;
    }

    if (!s_Me && p_Hitman && (m_HitmanBody || s_ToggleRagdoll) && (m_HitmanTimer -= p_Dt) <= 0.f) {
        m_HitmanTimer = 1.f;
        m_HitmanBody = true;
        Attach(p_Hitman, p_HitmanSpatial, false, false, p_Ray, "47");
        s_Me = Hitman();
    }

    // Billy: 47 goes ragdoll only falling into a floor portal (Dive); Numpad 2 only puts his body on.

    if ((m_NpcTimer -= p_Dt) <= 0.f) {
        m_NpcTimer = 0.25f; // one new body a call: walking up to people, theirs came a second apart

        if (m_NpcMode || std::ranges::any_of(m_Bodies, [](const Body& b) { return b.m_Npc; }))
            UpdateNpcs(p_Ray, p_Feet);
    }

    if (!m_Ready || (m_Peds.empty() && m_Bodies.empty()))
        return;

    if (s_Shoot || s_Push)
        if (const int t = Aimed(); t >= 0)
            s_Fire.emplace_back(t, s_Shoot ? "Shot (pistol)" : "Push");

    for (const auto& [t, s_What] : s_Fire) {
        std::string* s_Name = nullptr;
        float* s_Health = nullptr;
        Body* s_Body = nullptr;
        NmAgent* a = Target(t, s_Name, s_Health, s_Body);

        if (!a)
            continue;

        // A body: hit, it goes; stood up, back on its animation.
        if (s_Body && !s_Body->m_Npc && s_What != "Stand up again")
            continue; // 47: floor portals only (Billy)

        if (s_Body && s_What != "Stand up again")
            SetRagdoll(*s_Body, true);

        if (s_Body && s_What == "Stand up again") {
            nm_agent_set_dead(a, 0);
            s_Body->m_Npc ? SetRagdoll(*s_Body, false) : SetHitmanRagdoll(false);

            m_Line = std::format("{} is back on the animation.", *s_Name);
            continue;
        }

        Fire(a, *s_Name, *s_Health, s_What);
    }

    // The animation's pose in: what keyframed and partial bodies follow.
    for (Body& b : m_Bodies) {
        if (!Posed(b))
            continue;

        if (!b.m_Ragdoll)
            nm_rig_incoming(b.m_Rig, b.m_Agent, b.m_Now.data());

        // Pelican's check, once a second: is the left upper arm (part 14) on its bone? Its centre's angle off the
        // shoulder-to-elbow line: ~0 pinned on the animation; ~35 the limit holding it out (not keyframed).
        if (!b.m_Ragdoll && !b.m_ArmTargetLogged && b.m_PartRig[14] >= 0) {
            b.m_ArmTargetLogged = true;
            float p[3], q[4];
            nm_agent_get_part_transform(b.m_Agent, 14, p, q);
            const float* w = &b.m_Now[b.m_PartRig[14] * 7];
            const Q s_Wq {w[3], w[4], w[5], w[6]};
            const Q s_Tq = Norm(Mul(s_Wq, Q {b.m_ArmOff[3], b.m_ArmOff[4], b.m_ArmOff[5], b.m_ArmOff[6]}));
            const SVector3 s_Tp = V(w) + Rot(s_Wq, V(b.m_ArmOff));
            const Q s_D = Norm(Mul(Conj(s_Tq), Q {q[0], q[1], q[2], q[3]}));
            Logger::Info("[Delirium] {}: first keyframed frame: part 14 at ({:.3f} {:.3f} {:.3f}) rot ({:.3f} {:.3f} {:.3f} {:.3f}); "
                "bone x bake offset gives ({:.3f} {:.3f} {:.3f}) rot ({:.3f} {:.3f} {:.3f} {:.3f}): {:.3f} m, {:.0f} deg apart "
                "(the step may not have run yet); live upper-arm bone's axes along its line: {}.", b.m_Name, p[0], p[1], p[2], q[0], q[1], q[2], q[3], s_Tp.x, s_Tp.y, s_Tp.z,
                s_Tq.x, s_Tq.y, s_Tq.z, s_Tq.w, (V(p) - s_Tp).Length(), 2.f * std::acos(std::min(1.f, std::abs(s_D.w))) * 57.2958f,
                b.m_PartRig[15] >= 0 ? AlongAxis(w, &b.m_Now[b.m_PartRig[15] * 7]) : std::string("?"));
        }

        if ((b.m_ArmCheck -= p_Dt) <= 0.f && b.m_PartRig[14] >= 0 && b.m_PartRig[15] >= 0) {
            b.m_ArmCheck = 1.f;
            float p[3], q[4], s_Ang[3];
            nm_agent_get_part_transform(b.m_Agent, 14, p, q);
            const SVector3 s_Sh = V(&b.m_Now[b.m_PartRig[14] * 7]), s_El = V(&b.m_Now[b.m_PartRig[15] * 7]);
            const SVector3 s_Bone = (s_El - s_Sh).Normalized(), s_Part = (V(p) - s_Sh).Normalized();
            const float s_Off = std::acos(std::clamp(s_Bone.Dot(s_Part), -1.f, 1.f)) * 57.2958f;
            Logger::Info("[Delirium] {}: arm check: {}, upper arm {:.0f} deg off its bone ({:.2f} m from the shoulder), "
                "joint at {:.2f} of its range.", b.m_Name, b.m_Ragdoll ? "ragdoll" : "keyframed on the animation", s_Off,
                (V(p) - s_Sh).Length(), nm_agent_get_joint_angle(b.m_Agent, 14, s_Ang));
        }

        // A kill in Hitman: he drops (Delirium's death, not the game's).
        // ponytail: the game's own death ragdoll still takes the body over; detour ActivateRagdoll to keep NM's.
        if (b.m_Npc && !b.m_Dead && (b.m_Shot || static_cast<ZActor*>(b.m_Char)->IsDead())) {
            b.m_Dead = true;
            SetRagdoll(b, true);

            // Shot: task_shot with health 0 kills and keeps the reaction (set_dead first would skip it).
            if (b.m_Shot && b.m_Hit == 0)
                ShootAtAim(b); // no hit seen (another way of killing): one shot's worth

            if (b.m_Health >= 50.f)
                nm_agent_set_dead(b.m_Agent, 1);
        }
    }

    // 47 walking or running into a body knocks it over (GTA IV's bump), pushed the way he goes.
    if (const Body* h = Hitman(); h && h->m_Now.size() >= 3 && p_Dt > 0.f) {
        const SVector3 s_Hips = V(h->m_Now.data());
        SVector3 s_Vel = m_HadHitmanHips ? (s_Hips - m_HitmanHips) * (1.f / p_Dt) : SVector3(0.f, 0.f, 0.f);
        s_Vel.z = 0.f;
        m_HitmanHips = s_Hips;
        m_HadHitmanHips = true;

        // The Playroom's bump (Pelican: niko.rs 287-292, main.rs 3948): no push at all. Faster than 2.5 m/s, within
        // 0.7 m, moving toward a standing NPC: NM balance looking at 47's head ("WEAK" under 140 health), and the
        // body staggers by itself; once per 2.5 s. Lying bodies get nothing (the old per-frame shove flung them).
        // GTA IV's own answer (Pelican: nm_player_bump, its player-bumps-ped handler), once, the frame 47 first
        // touches a standing NPC: a shove (balance and ~30 N s at the chest along his facing), a flinch, or nothing.
        // ponytail: 47 doesn't play a shove or reach his hands onto the chest (IK); add with Hitman's push clip.
        const auto s_Move = [](float v) { return v < 0.3f ? 1 : v < 2.5f ? 2 : v < 5.f ? 3 : 4; };
        const SMatrix s_HM = h->m_Spatial->GetObjectToWorldMatrix();
        // Facing: the way he walks when he does (GTA's shove needs the NPC in front: forward . (it - him) > 0). An
        // entity's Y axis points BACK (log: walking, his velocity . Y was -1.00): forward is -Y.
        const SVector3 s_HF = s_Vel.Length() > 0.3f ? s_Vel.Normalized() : SVector3(-s_HM.YAxis.x, -s_HM.YAxis.y, 0.f).Normalized();

        for (Body& b : m_Bodies) {
            b.m_Stagger = std::max(0.f, b.m_Stagger - p_Dt);

            if (!b.m_Npc || b.m_Now.size() < 3)
                continue;

            const SVector3 s_Np = V(b.m_Now.data());
            const float s_NpV = b.m_HadBumpHips ? (s_Np - b.m_BumpHips).Length() / p_Dt : 0.f;
            b.m_BumpHips = s_Np, b.m_HadBumpHips = true;
            SVector3 s_D = s_Np - s_Hips;
            s_D.z = 0.f;
            const bool s_Touch = s_D.Length() < 0.7f;

            // The bump's lean away ends after a second (the Playroom's lean_stop).
            if (b.m_LeanStop > 0.f && (b.m_LeanStop -= p_Dt) <= 0.f)
                nm_msg_send_text(b.m_Agent, m_World, "leanInDirection start=false");

            // Clear of 47 again: it meets his body again.
            if (b.m_NoAgents && s_D.Length() > 0.9f)
                nm_agent_collide_agents(b.m_Agent, b.m_Ragdoll || !b.m_Npc ? 1 : 0), b.m_NoAgents = false;

            // (The old test set m_Touching only while touching: after the first touch, no bump ever again.)
            if (std::exchange(b.m_Touching, s_Touch) || !s_Touch || b.m_Ragdoll || b.m_Recover > 0.f || b.m_Stagger > 0.f)
                continue;

            const SMatrix s_NM = b.m_Spatial->GetObjectToWorldMatrix();
            const float s_Me[6] = {s_Hips.x, s_Hips.y, s_Hips.z, s_HF.x, s_HF.y, 0.f};
            const float s_Other[6] = {s_Np.x, s_Np.y, s_Np.z, -s_NM.YAxis.x, -s_NM.YAxis.y, 0.f};
            int s_Side = -1, s_Clip = -1;
            const int s_R = nm_player_bump(m_World, s_Move(s_Vel.Length()), s_Move(s_NpV), s_Me, s_Other, b.m_Health, 1.3f, 1.3f,
                m_ShovedLast == b.m_Agent ? NM_BUMP_SHOVED_LAST : 0, &s_Side, &s_Clip);
            Logger::Info("[Delirium] 47 bumped {} at {:.1f} m/s: {} (side {}, clip {}; health {:.0f}, ahead {:.2f}, its facing "
                "vs his {:.2f}).", b.m_Name, s_Vel.Length(), std::array {"nothing", "shove", "flinch", "knocked over"}[std::clamp(s_R, 0, 3)],
                s_Side, s_Clip, b.m_Health, s_HF.Dot(s_D.Normalized()), s_HF.Dot(SVector3(-s_HM.YAxis.x, -s_HM.YAxis.y, 0.f).Normalized()));

            // GTA's answer is the player's; the bumped body is pushed by the touch itself. Hitman's NPC is on its
            // animation until then, so "nothing" still staggers it at a run (the guide's 2.5 m/s rule).
            if (s_R == NM_BUMP_NOTHING && (s_Vel.Length() < 2.5f || s_Vel.Dot(s_D) <= 0.f))
                continue;

            SetRagdoll(b, true);
            // The Playroom's way (niko.rs): the Hero's body never rams Homer; the bump is the balance reaction and the
            // shove's 30 N s. Here 47's keyframed body carried them off at his sprint (6 m/s pelvis in the bump watch):
            // Billy's flying bumps. Out of the other bodies until he's clear.
            nm_agent_collide_agents(b.m_Agent, 0), b.m_NoAgents = true;
            const float s_Look[3] = {s_Hips.x, s_Hips.y, s_Hips.z + 0.75f};
            nm_agent_task_balance(b.m_Agent, m_World, b.m_Health < 140.f ? "WEAK" : "*", s_Look);
            // The Playroom's bump (main.rs 3969, peds.rs NMBalance 82611900): lean 0.4 away from 47 for 1 s.
            {
                const SVector3 s_Away = s_D.Normalized();
                const std::string s_Lean = std::format("leanInDirection start=true applyAsForce=true leanAmount=0.4 dir={:.3f},{:.3f},0",
                    s_Away.x, s_Away.y);
                nm_msg_send_text(b.m_Agent, m_World, s_Lean.c_str());
                b.m_LeanStop = 1.f;
            }

            if (s_R == NM_BUMP_SHOVE) {
                float p[3], q[4];
                nm_agent_get_part_transform(b.m_Agent, 10, p, q);
                const float s_Push[3] = {s_HF.x * 30.f, s_HF.y * 30.f, 0.f};
                nm_agent_apply_impulse(b.m_Agent, 10, p, s_Push);
                m_ShovedLast = b.m_Agent;
            }

            b.m_Stagger = 2.5f;
            m_BumpWatch = 2.f, m_BumpAgent = b.m_Agent;
        }

        // Billy: shot NPCs don't stagger or grab like the Playroom's. Every 0.25 s for 6 s: the shot task running,
        // the pelvis over NM's ground, the feet touching (Pelican's checks: health, ground under the feet, task stopped).
        if (m_ShotWatch > 0.f && (m_ShotWatch -= p_Dt, (m_ShotLog -= p_Dt) <= 0.f)) {
            m_ShotLog = 0.25f;

            for (const Body& b : m_Bodies) {
                if (b.m_Agent != m_ShotAgent)
                    continue;

                float p[3], q[4], s_Dist = -1.f, s_N[3];
                const float s_Down[3] = {0.f, 0.f, -1.f};
                nm_agent_get_part_transform(b.m_Agent, 0, p, q);
                nm_world_raycast(m_World, p, s_Down, 3.f, &s_Dist, s_N);
                Logger::Info("[Delirium] shot watch {}: task {}, pelvis {:.2f} m over NM's ground, feet contact {:#x} {:#x}, "
                    "ragdoll {}, health {:.0f}.", b.m_Name, nm_agent_task_running(b.m_Agent), s_Dist,
                    nm_agent_get_part_contact(b.m_Agent, 3), nm_agent_get_part_contact(b.m_Agent, 6), b.m_Ragdoll, b.m_Health);
            }
        }

        // Pelican: Billy's bumps throw NPCs; a plain keyframed walk-into only shoves at the walker's speed (SDK
        // walked_into_staggers). Each frame for 2 s after a bump (150 lines a run): 47's fastest bone ((now - last) /
        // dt: a pose jump is a 6-15 m/s hand in NM), the frame time, and the bumped body's pelvis speed.
        static std::vector<float> s_Last;
        static int s_Lines = 0;
        float s_Fast = 0.f;
        size_t s_FastBone = 0;

        if (s_Last.size() == h->m_Now.size())
            for (size_t r = 0; r * 7 < h->m_Now.size(); ++r)
                if (const float v = (V(&h->m_Now[r * 7]) - V(&s_Last[r * 7])).Length() / p_Dt; v > s_Fast)
                    s_Fast = v, s_FastBone = r;

        s_Last = h->m_Now;

        if (m_BumpWatch > 0.f && s_Lines < 150) {
            m_BumpWatch -= p_Dt;
            ++s_Lines;
            float v[3] = {}, w[3];

            if (std::ranges::any_of(m_Bodies, [&](const Body& o) { return o.m_Agent == m_BumpAgent; }))
                nm_agent_get_part_velocity(m_BumpAgent, 0, v, w);

            Logger::Info("[Delirium] bump watch: dt {:.4f}, 47's fastest bone {} at {:.1f} m/s, hips {:.1f} m/s; bumped "
                "pelvis {:.1f} m/s.", p_Dt, h->m_Bones[s_FastBone], s_Fast, s_Vel.Length(), V(v).Length());
        }
    }

    // One body's patch a frame, each body about every 0.25 s.
    if ((m_GroundTimer -= p_Dt) <= 0.f) {
        m_GroundTimer = 0.25f / static_cast<float>(std::max<size_t>(1, m_Bodies.size() + m_Peds.size()));
        UpdateGround(p_Ray, true);
    }

    // NM's own 60 steps a second, at most 4 a frame.
    m_Accum = std::min(m_Accum + p_Dt, 4.f * c_Step);
    const int s_Steps = static_cast<int>(m_Accum / c_Step);
    m_Accum -= s_Steps * c_Step;
    m_Stepped = s_Steps > 0;
    m_Alpha = std::clamp(m_Accum / c_Step, 0.f, 1.f);
    std::vector<NmAgent*> s_Agents;

    for (const Ped& s_Ped : m_Peds)
        s_Agents.push_back(s_Ped.m_Agent);

    for (const Body& s_Body : m_Bodies)
        s_Agents.push_back(s_Body.m_Agent);

    if (s_Steps > 0) {
    {
        HitchTimer s_T {"NM step"};
        nm_world_step_agents(m_World, s_Agents.data(), static_cast<int>(s_Agents.size()), s_Steps * c_Step, 0);
    }
    }

    // Ragdolls through the portals, the Playroom's way (portal.rs funnel_agent, transit): Portal 2's funnel steers a
    // fall toward a floor portal, and when a body's centre crosses an opening inside it the whole body comes out of
    // the other, turned, its speed turned with it.
    // ponytail: no ghost copy at the far portal while part way in (the Playroom draws one; Hitman's mesh is one piece).
    for (Body& b : m_Bodies) {
        if (!m_PortalsOpen || !b.m_Ragdoll) {
            b.m_PortalHad = false;
            continue;
        }

        float s_Com[3], s_Vel[3];
        nm_agent_get_com(b.m_Agent, s_Com, s_Vel);
        float s_Steer[3] = {s_Vel[0], s_Vel[1], s_Vel[2]};
        dp_funnel(m_Portals, 2, 9.81f, &s_Com, &s_Steer, p_Dt);
        const float s_Dv[3] = {s_Steer[0] - s_Vel[0], s_Steer[1] - s_Vel[1], s_Steer[2] - s_Vel[2]};

        if (s_Dv[0] * s_Dv[0] + s_Dv[1] * s_Dv[1] + s_Dv[2] * s_Dv[2] > 1e-10f)
            nm_agent_add_velocity(b.m_Agent, s_Dv);

        const int s_Parts = nm_agent_part_count(b.m_Agent);
        float s_C[3] = {};

        for (int k = 0; k < s_Parts; ++k) {
            float s_P[3], s_Q[4];
            nm_agent_get_part_transform(b.m_Agent, k, s_P, s_Q);
            s_C[0] += s_P[0] / s_Parts, s_C[1] += s_P[1] / s_Parts, s_C[2] += s_P[2] / s_Parts;
        }

        const float s_Was[2] = {b.m_PortalLast[0], b.m_PortalLast[1]};
        const bool s_Had = b.m_PortalHad;
        b.m_PortalLast[0] = dp_local_z(&m_Portals[0], &s_C);
        b.m_PortalLast[1] = dp_local_z(&m_Portals[1], &s_C);
        b.m_PortalHad = true;

        for (int i = 0; i < 2 && s_Had; ++i) {
            // Floors and ceilings: his middle can go down just past the opening's 0.83 while he tips over its rim, and
            // then he fell through the map (Billy 11-16). Tested pulled 0.7 toward the portal: the whole opening counts.
            const DpFrame& s_F = m_Portals[i];
            const bool s_Flat = std::abs(Rotate(s_F.rot, SVector3(0.f, 0.f, 1.f)).z) > 0.7f;
            const float s_Pull = s_Flat ? 0.7f : 1.f;
            const float s_Cp[3] = {s_F.at[0] + (s_C[0] - s_F.at[0]) * s_Pull, s_F.at[1] + (s_C[1] - s_F.at[1]) * s_Pull,
                s_F.at[2] + (s_C[2] - s_F.at[2]) * s_Pull};

            // And a body that got under a floor portal's plane anyway (there's only void under it): through it.
            bool s_Under = false;

            if (s_Flat) {
                const float z = dp_local_z(&s_F, &s_C);
                const SVector3 n = Rotate(s_F.rot, SVector3(0.f, 0.f, 1.f));

                if (z < 0.f && z > -1.5f) { // not someone on the floor under it
                    const float s_Up[3] = {s_F.at[0] + (s_C[0] - n.x * (z + 0.05f) - s_F.at[0]) * 0.6f,
                        s_F.at[1] + (s_C[1] - n.y * (z + 0.05f) - s_F.at[1]) * 0.6f,
                        s_F.at[2] + (s_C[2] - n.z * (z + 0.05f) - s_F.at[2]) * 0.6f};
                    s_Under = dp_crossed(&s_F, 1.f, &s_Up);
                }
            }

            if (!s_Under && !dp_crossed(&s_F, s_Was[i], &s_Cp))
                continue;

            const DpFrame& s_From = m_Portals[i];
            const DpFrame& s_To = m_Portals[1 - i];

            for (int k = 0; k < s_Parts; ++k) {
                float s_P[3], s_Q[4], s_V[3], s_W[3];
                nm_agent_get_part_transform(b.m_Agent, k, s_P, s_Q);
                nm_agent_get_part_velocity(b.m_Agent, k, s_V, s_W);
                dp_through(&s_From, &s_To, &s_P, &s_V, &s_Q);
                dp_through(&s_From, &s_To, nullptr, &s_W, nullptr);
                nm_agent_set_part_state(b.m_Agent, k, s_P, s_Q, s_V, s_W);
            }

            nm_agent_get_com(b.m_Agent, s_Com, s_Vel);
            float s_Drop[3];
            dp_exit_gimme(&s_To, &s_Vel, &s_Drop);
            const float s_Back[3] = {-s_Drop[0], -s_Drop[1], -s_Drop[2]};

            if (s_Drop[0] != 0.f || s_Drop[1] != 0.f || s_Drop[2] != 0.f)
                nm_agent_add_velocity(b.m_Agent, s_Back);

            b.m_PortalHad = false;
            b.m_PoseA.clear();
            m_GroundTimer = 0.f;
            Logger::Info("[Delirium] {} through the {} portal at {:.1f} m/s.", b.m_Name, i ? "orange" : "blue",
                std::sqrt(s_Vel[0] * s_Vel[0] + s_Vel[1] * s_Vel[1] + s_Vel[2] * s_Vel[2]));
            break;
        }
    }

    // Down and still for a moment: up again (the dead stay down).
    for (Body& b : m_Bodies) {
        if (!b.m_Ragdoll || b.m_Dead) {
            b.m_GettingUp = false;
            b.m_SettledT = 0.f;
            continue;
        }

        if (!b.m_HighFall) {
            float s_C[3], s_V[3];
            nm_agent_get_com(b.m_Agent, s_C, s_V);

            if (s_V[2] < -6.f) {
                b.m_HighFall = true;
                nm_agent_task_high_fall(b.m_Agent, m_World, "*", b.m_Npc ? 0 : 1);
            }
        }

        // Up again with Hitman's own recovery (Billy: never the learned get-up / getup.net). SetRagdoll(false) puts an
        // NPC where its body lies, facing the way it got up, and deactivates the game's ragdoll.
        // Billy: one shot that kept his feet never left the balance. Standing still on his feet (the stagger's over):
        // back on his animation too.
        b.m_SettledT = !b.m_HighFall && nm_agent_is_settled(b.m_Agent) == 1 ? b.m_SettledT + p_Dt : 0.f;
        const bool s_Settled = b.m_SettledT > 1.5f;

        // Knocked out: dazed a moment (the push, his balance staggering), then out: limp, he falls over and stays
        // down (Billy: "they just lose balance and fall over"; Hitman has him unconscious). No get-up.
        // Billy: one only dazed gets up when it wears off, and a knocked-out one can be woken by whoever finds him:
        // limp while Hitman has him pacified; not pacified (2 s on), alive again and his get-up below.
        if (b.m_Ko) {
            b.m_KoT += p_Dt;
            const bool s_Out = b.m_Npc && static_cast<ZActor*>(b.m_Char)->IsPacified();

            if (!b.m_KoOut && b.m_KoT > 0.6f) {
                nm_agent_set_dead(b.m_Agent, 1);
                b.m_KoOut = true;
                Logger::Info("[Delirium] {}: out cold, falls.", b.m_Name);
            }

            if (!b.m_KoOut || s_Out || b.m_KoT < 6.f) // a daze lasts a few seconds
                continue;

            nm_agent_set_dead(b.m_Agent, 0);
            b.m_Ko = b.m_KoOut = false;
            Logger::Info("[Delirium] {}: comes to; gets up.", b.m_Name);
        }

        if (s_Settled || nm_agent_should_get_up(b.m_Agent, 1.f) == 1) {
            Logger::Info("[Delirium] {} {}.", b.m_Name, s_Settled ? "kept his feet; back on his animation" : "gets up");
            b.m_Npc ? SetRagdoll(b, false) : SetHitmanRagdoll(false);
        }
    }

    for (Body& b : m_Bodies) {
        if (b.m_Ragdoll || b.m_Blend <= 0.f || b.m_Written.size() != b.m_Local.size())
            continue;

        b.m_Blend = std::max(0.f, b.m_Blend - p_Dt);
        const float t = 1.f - b.m_Blend / c_GetUp;
        const float k = t * t * (3.f - 2.f * t);
        std::vector<Bones::SQV> s_Mix = b.m_Local;

        for (size_t i = 0; i < s_Mix.size(); ++i) {
            const float* a = b.m_Written[i].q;
            float* q = s_Mix[i].q;
            const float s_Sign = a[0] * q[0] + a[1] * q[1] + a[2] * q[2] + a[3] * q[3] < 0.f ? -1.f : 1.f;
            const Q m = Norm(Q {a[0] + (q[0] * s_Sign - a[0]) * k, a[1] + (q[1] * s_Sign - a[1]) * k,
                a[2] + (q[2] * s_Sign - a[2]) * k, a[3] + (q[3] * s_Sign - a[3]) * k});
            q[0] = m.x, q[1] = m.y, q[2] = m.z, q[3] = m.w;

            for (int c = 0; c < 3; ++c)
                s_Mix[i].p[c] = b.m_Written[i].p[c] + (s_Mix[i].p[c] - b.m_Written[i].p[c]) * k;
        }

        Bones::Override(b.m_Anim, b.m_Blend > 0.f ? std::move(s_Mix) : std::vector<Bones::SQV> {});
    }

    // NM's pose out onto the ragdolling ones.
    for (Body& b : m_Bodies) {
        // Ragdolling: the character goes where its body goes (no walking off out of it), then the pose onto it.
        if (b.m_Ragdoll) {
            // 47 only. An NPC's entity is the game's: moved (and lifted 2.5 m) here every frame, its AI moved it back,
            // and the pose (worked out against ours) jumped back and forth and up and down (Billy: "flying").
            // The game's ragdoll (on at the knock-down) holds it still and off 47's way instead.
            if (!b.m_Npc) {
                float s_Pos[3], s_Rot[4];
                nm_agent_get_part_transform(b.m_Agent, 0, s_Pos, s_Rot);
                SMatrix s_M = b.m_HeldRot;
                s_M.Trans.x = s_Pos[0];
                s_M.Trans.y = s_Pos[1];
                // His feet on the ground under the body (it comes out of the far portal at another height).
                const auto s_G = m_LastGround.find(b.m_Agent);
                s_M.Trans.z = s_G != m_LastGround.end() && std::abs(s_G->second - s_Pos[2]) < 2.f ? s_G->second : s_Pos[2] - 0.2f;
                b.m_Spatial->SetObjectToWorldMatrixFromEditor(s_M);
            }

            Posed(b);
            WritePose(b);

            if (b.m_Follow)
                Follow(b);
        }

        if (b.m_Ragdoll && (b.m_SpeedCheck -= p_Dt) <= 0.f) {
            b.m_SpeedCheck = 1.f;
            float s_Head[3];
            int s_Up;
            const float s_Lying = nm_agent_get_lying(b.m_Agent, s_Head, &s_Up);
            std::vector<float> s_T(c_Parts * 7);
            nm_agent_get_part_transforms(b.m_Agent, s_T.data(), c_Parts);

            if (b.m_SpeedPrev.size() == s_T.size()) {
                float s_Max = 0.f;
                int s_Part = 0;

                for (int k = 0; k < c_Parts; ++k) {
                    const float v = (V(&s_T[k * 7]) - V(&b.m_SpeedPrev[k * 7])).Length() / b.m_SpeedDt;

                    if (v > s_Max)
                        s_Max = v, s_Part = k;
                }

                if (s_Max > 5.f && s_Lying > 0.5f) {
                    char s_Pn[32] = {};
                    nm_part_name(s_Part, s_Pn, sizeof(s_Pn));
                    const Body* h = Hitman();
                    const float s_To47 = h && h->m_Now.size() >= 3 && !b.m_Now.empty()
                        ? (V(b.m_Now.data()) - V(h->m_Now.data())).Length() : -1.f;
                    Logger::Warn("[Delirium] {}: lying {:.1f} s but its {} moves {:.1f} m/s (map or joints wrong?); game "
                        "ragdoll {}, 47 {:.1f} m away (Pelican: a fling).", b.m_Name, s_Lying, s_Pn, s_Max,
                        GameRagdollOn(b.m_Char) ? "on" : "off", s_To47);
                }
            }

            b.m_SpeedPrev = std::move(s_T);
            b.m_SpeedDt = 0.f;

            float s_Worst = 0.f;
            int s_WorstPart = 0;

            for (int k = 1; k < c_Parts; ++k) {
                float s_Ang[3];
                const float s_Into = nm_agent_get_joint_angle(b.m_Agent, k, s_Ang);

                if (s_Into > s_Worst)
                    s_Worst = s_Into, s_WorstPart = k;
            }

            char s_Jn[32] = {};
            nm_part_name(s_WorstPart, s_Jn, sizeof(s_Jn));

            if (s_Worst > 1.2f)
                Logger::Warn("[Delirium] {}: {} joint {:.2f} of its range (past the limit: map or bind for this NPC type).",
                    b.m_Name, s_Jn, s_Worst);
            else
                Logger::Info("[Delirium] {}: worst joint {} at {:.2f} of its range.", b.m_Name, s_Jn, s_Worst);
        }

        b.m_SpeedDt += p_Dt;
    }

    Snapshot();
}

void Delirium::Draw3D(IRenderer* p_Renderer) {
    std::scoped_lock s_Lock(m_Mutex);

    if (m_ShowBodies)
        for (size_t i = 0; i + 1 < m_Lines.size(); i += 2)
            p_Renderer->DrawLine3D(m_Lines[i], m_Lines[i + 1], SVector4(0.2f, 1.f, 0.3f, 1.f), SVector4(0.2f, 1.f, 0.3f, 1.f));

    for (size_t p = 0; p < m_Pose.size(); ++p) {
        if (m_Pose[p].empty() || (!m_ShowBodies && (p == 0 || p > m_PedsDrawn)))
            continue;

        const bool s_Picked = static_cast<int>(p) == m_Target;

        for (int i = 0; i < c_Parts && i < static_cast<int>(m_Half.size()); ++i) {
            const float* s_T = &m_Pose[p][i * 7];
            const SVector3 s_C = V(s_T);
            const SVector3 s_Y = Rotate(s_T + 3, SVector3(0.f, 1.f, 0.f));
            const SVector3 s_X = Rotate(s_T + 3, SVector3(1.f, 0.f, 0.f));
            const SVector3 s_Z = Rotate(s_T + 3, SVector3(0.f, 0.f, 1.f));
            const bool s_Own = p < m_Sizes.size() && m_Sizes[p].size() == c_Parts * 5;
            const float* z = s_Own ? &m_Sizes[p][i * 5] : nullptr;
            const float r = z ? z[0] : m_Radius[i], h = z ? z[1] : m_Half[i];

            // Solid and shaded like the Playroom's (Billy: the outline's end rings read as two big circles in the
            // torso; the spine parts are short wide capsules that only look like a body drawn whole). Its colours:
            // one side blue, the other red, the trunk and head light grey; the body you aim at orange.
            const SVector3 s_Base = s_Picked ? SVector3(1.f, 0.6f, 0.15f)
                : (i >= 1 && i <= 3) || (i >= 13 && i <= 16) ? SVector3(0.25f, 0.45f, 0.95f)
                : (i >= 4 && i <= 6) || (i >= 17 && i <= 20) ? SVector3(0.95f, 0.35f, 0.25f)
                : SVector3(0.85f, 0.85f, 0.85f);
            const SVector3 s_Light = SVector3(0.3f, 0.2f, 0.93f).Normalized();
            const auto s_Shade = [&](const SVector3& n) {
                const float k = 0.45f + 0.55f * std::max(0.f, n.Dot(s_Light));
                return SVector4(s_Base.x * k, s_Base.y * k, s_Base.z * k, 0.8f);
            };

            if (z && (z[2] > 0.f || z[3] > 0.f || z[4] > 0.f)) { // a foot: NM's box
                const SVector3 e[3] = {s_X * z[2], s_Y * z[3], s_Z * z[4]};

                for (int f = 0; f < 3; ++f)
                    for (const float sg : {-1.f, 1.f}) {
                        const SVector3 n = e[f] * sg, u = e[(f + 1) % 3], v = e[(f + 2) % 3];
                        const SVector4 c = s_Shade(n.Normalized());
                        if (m_Wire)
                            for (const auto& [a, z2] : {std::pair {-u - v, u - v}, {u - v, u + v}})
                                p_Renderer->DrawLine3D(s_C + n + a, s_C + n + z2, c, c);
                        else
                            p_Renderer->DrawQuad3D(s_C + n - u - v, c, s_C + n + u - v, c, s_C + n + u + v, c, s_C + n - u + v, c);
                    }

                continue;
            }

            // A capsule: two hemispheres h either side of the centre along its Y, joined by a cylinder.
            constexpr int c_Seg = 10, c_Lat = 3;
            const auto s_Point = [&](int k, int l, SVector3& n) {
                const bool s_Top = l > c_Lat; // rings 0..c_Lat the bottom cap, the rest the top
                const float s_Phi = std::numbers::pi_v<float> * 0.5f * (s_Top ? l - c_Lat - 1 : l - c_Lat) / c_Lat;
                const float a = 2.f * std::numbers::pi_v<float> * k / c_Seg;
                n = (s_X * std::cos(a) + s_Z * std::sin(a)) * std::cos(s_Phi) + s_Y * std::sin(s_Phi);
                return s_C + s_Y * (s_Top ? h : -h) + n * r;
            };

            for (int l = 0; l < 2 * c_Lat + 1; ++l)
                for (int k = 0; k < c_Seg; ++k) {
                    SVector3 n0, n1, n2, n3;
                    const SVector3 p0 = s_Point(k, l, n0), p1 = s_Point(k + 1, l, n1);
                    const SVector3 p2 = s_Point(k + 1, l + 1, n2), p3 = s_Point(k, l + 1, n3);
                    if (m_Wire) { // Billy: the outline as an option (rings and lines along the capsule)
                        const SVector4 c = s_Shade(n0);
                        p_Renderer->DrawLine3D(p0, p1, c, c);

                        if (k % 3 == 0)
                            p_Renderer->DrawLine3D(p0, p3, c, c);
                    }
                    else
                        p_Renderer->DrawQuad3D(p0, s_Shade(n0), p1, s_Shade(n1), p2, s_Shade(n2), p3, s_Shade(n3));
                }
        }
    }
}

void Delirium::DrawMenu() {
    ImGui::TextWrapped("%s", m_Status.c_str());
    ImGui::Checkbox("47's Homer body (bumps peds; ragdolls only in floor portals)", &m_HitmanBody);
    ImGui::SameLine();
    ImGui::Checkbox("Show bodies", &m_ShowBodies);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &m_Wire);

    if (ImGui::Checkbox("47 invincible", &m_Invincible))
        m_CrippleDirty = true;

    ImGui::SameLine();

    if (ImGui::Checkbox("Never wanted (AI ignores 47)", &m_Hidden))
        m_CrippleDirty = true;
    ImGui::TextDisabled("Num1 NPCs: %s   Num2 47 ragdoll: %s   Num5/6 shoot/push", m_NpcMode ? "ON" : "off",
        m_HitmanRagdoll ? "ON" : "off");
    ImGui::Separator();

    std::scoped_lock s_Lock(m_Mutex);

    if (ImGui::Button("Spawn NPC"))
        m_WantNpc = true;

    ImGui::SameLine();

    if (ImGui::Button("Spawn outline ped"))
        m_WantSpawn = true;

    ImGui::SameLine();

    if (ImGui::Button("Remove peds"))
        m_WantClear = true;

    // Who: 47, a ped or an NPC.
    const auto s_NameOf = [&](int t) {
        return t == 0 ? std::string("47") : t - 1 < static_cast<int>(m_Names.size()) ? m_Names[t - 1] : "?";
    };
    m_Target = std::clamp(m_Target, 0, static_cast<int>(m_Names.size()));
    ImGui::SetNextItemWidth(-1.f);

    if (ImGui::BeginCombo("##target", ("Target: " + s_NameOf(m_Target)).c_str())) {
        for (int t = 0; t <= static_cast<int>(m_Names.size()); ++t)
            if (ImGui::Selectable(std::format("{}##t{}", s_NameOf(t), t).c_str(), m_Target == t))
                m_Target = t;

        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##strength", &m_Strength, 0.2f, 4.f, "Strength %.1fx");

    if (ImGui::BeginTable("##actions", 3, ImGuiTableFlags_SizingStretchSame)) {
        for (const char* s_Action : c_Actions) {
            ImGui::TableNextColumn();

            if (ImGui::Button(s_Action, ImVec2(-1.f, 0.f)))
                m_WantFire.emplace_back(m_Target, s_Action);
        }

        ImGui::EndTable();
    }

    // Every NM behaviour by name, started with its defaults; or any message line.
    if (!m_Messages.empty()) {
        m_Message = std::clamp(m_Message, 0, static_cast<int>(m_Messages.size()) - 1);
        ImGui::SetNextItemWidth(-60.f);

        if (ImGui::BeginCombo("##behaviour", m_Messages[m_Message].c_str())) {
            for (int m = 0; m < static_cast<int>(m_Messages.size()); ++m)
                if (ImGui::Selectable(m_Messages[m].c_str(), m == m_Message))
                    m_Message = m;

            ImGui::EndCombo();
        }

        ImGui::SameLine();

        if (ImGui::Button("Start", ImVec2(-1.f, 0.f)))
            m_WantFire.emplace_back(m_Target, "msg:" + m_Messages[m_Message] + " start=true");
    }

    ImGui::SetNextItemWidth(-60.f);
    ImGui::InputText("##line", m_Text, sizeof(m_Text));
    ImGui::SameLine();

    if (ImGui::Button("Send", ImVec2(-1.f, 0.f)))
        m_WantFire.emplace_back(m_Target, std::string("msg:") + m_Text);

    if (!m_Line.empty())
        ImGui::TextWrapped("%s", m_Line.c_str());
}

// A real Hitman NPC 3 m in front of 47, as the SDK's Assets mod spawns one (npcactor.entitytemplate), dressed as the
// nearest NPC (its outfit, charset, variation). Delirium takes it like any NPC near 47 (Num1).
void Delirium::SpawnNpc(ZSpatialEntity* p_HitmanSpatial, const SVector3& p_Facing) {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene || !p_HitmanSpatial || !Globals::ActorManager) {
        m_Line = "Can't spawn an NPC: no level.";
        return;
    }

    // The outfit of the nearest NPC.
    const SMatrix s_Me = p_HitmanSpatial->GetObjectToWorldMatrix();
    const SVector3 s_MyPos(s_Me.Trans.x, s_Me.Trans.y, s_Me.Trans.z);
    ZActor* s_Model = nullptr;
    float s_Best = 1e9f;

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        ZActor* a = s_Entry.m_pInterfaceRef;
        const auto s_Spatial = s_Entry.m_entityRef ? s_Entry.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

        if (!a || !s_Spatial || a->IsDead())
            continue;

        const float4 t = s_Spatial->GetObjectToWorldMatrix().Trans;
        const float d = (SVector3(t.x, t.y, t.z) - s_MyPos).Length();

        if (d < s_Best)
            s_Best = d, s_Model = a;
    }

    if (!s_Model) {
        m_Line = "Can't spawn an NPC: no NPC in the level to copy the outfit of.";
        return;
    }

    TResourcePtr<ZTemplateEntityFactory> s_Resource;
    Globals::ResourceManager->GetResourcePtr(
        s_Resource, ResId<"[assembly:/templates/gameplay/ai2/actors.template?/npcactor.entitytemplate].pc_entitytype">, 0);

    if (!s_Resource) {
        m_Line = "Can't spawn an NPC: the NPC template isn't loaded in this level.";
        return;
    }

    ZEntityRef s_New;
    SExternalReferences s_Refs;
    Functions::ZEntityManager_NewEntity->Call(Globals::EntityManager, s_New, "", s_Resource, s_Scene.m_entityRef, s_Refs, -1);
    ZActor* s_Actor = s_New ? s_New.QueryInterface<ZActor>() : nullptr;
    ZSpatialEntity* s_Spatial = s_New ? s_New.QueryInterface<ZSpatialEntity>() : nullptr;

    if (!s_Actor || !s_Spatial) {
        m_Line = "Can't spawn an NPC: the game didn't make one.";
        return;
    }

    static int s_Count = 0;
    m_SpawnedNpcs.push_back(s_New);
    s_Actor->m_sActorName = std::format("Delirium NPC {}", ++s_Count);
    s_Actor->m_bStartEnabled = true;
    s_Actor->m_nOutfitCharset = s_Model->m_nOutfitCharset;
    s_Actor->m_nOutfitVariation = s_Model->m_nOutfitVariation;
    s_Actor->m_OutfitRepositoryID = s_Model->m_OutfitRepositoryID;
    s_Actor->m_eRequiredVoiceVariation = EActorVoiceVariation::eAVV_Undefined;
    s_Actor->Activate(0);

    // 3 m ahead of 47, facing him.
    SMatrix s_M = s_Me;
    const SVector3 f = SVector3(p_Facing.x, p_Facing.y, 0.f).Normalized();
    s_M.Trans.x += f.x * 3.f;
    s_M.Trans.y += f.y * 3.f;
    s_M.YAxis = float4(-f.x, -f.y, 0.f, 0.f);
    s_M.XAxis = float4(-f.y, f.x, 0.f, 0.f);
    s_M.ZAxis = float4(0.f, 0.f, 1.f, 0.f);
    s_Spatial->SetObjectToWorldMatrixFromEditor(s_M);

    m_Line = std::format("Spawned {} (dressed as {}).", s_Actor->m_sActorName.c_str(), s_Model->m_sActorName.c_str());
    Logger::Info("[Delirium] {}", m_Line);
}

// A bullet into the part nearest 47's aim (the game's shot ragdolls it underneath; NM's body is what shows).
// The part a ray from 47's eye along `p_Dir` passes nearest, and how near (m).
static float NearestPart(NmAgent* a, const SVector3& p_Eye, const SVector3& p_Dir, int& p_Part, float* p_At) {
    float s_Best = 1e9f;

    for (int i = 0; i < c_Parts; ++i) {
        float p[3], q[4];
        nm_agent_get_part_transform(a, i, p, q);
        const SVector3 d = V(p) - p_Eye;
        const float t = std::max(0.f, d.x * p_Dir.x + d.y * p_Dir.y + d.z * p_Dir.z);
        const float s_Off = (d - p_Dir * t).Length();

        if (s_Off < s_Best)
            s_Best = s_Off, p_Part = i, std::copy_n(p, 3, p_At);
    }

    return s_Best;
}

void Delirium::Knock(Body& b) {
    // A push at the chest from 47's side (about a shove's): NM's balance staggers.
    float p[3], q[4];
    nm_agent_get_part_transform(b.m_Agent, 9, p, q);
    SVector3 d(p[0] - m_Eye.x, p[1] - m_Eye.y, 0.f);
    d = d.Length() > 0.01f ? d.Normalized() : SVector3(1.f, 0.f, 0.f);
    const float s_Imp[3] = {d.x * 45.f, d.y * 45.f, 0.f};
    nm_agent_apply_push(b.m_Agent, p, s_Imp);
    b.m_Ko = true;
    b.m_KoT = 0.f;
    Logger::Info("[Delirium] {}: knocked by a thrown object; off balance (Hitman: pacified {}, health {:.0f}).", b.m_Name,
        b.m_Npc && static_cast<ZActor*>(b.m_Char)->IsPacified(), b.m_Npc ? static_cast<ZActor*>(b.m_Char)->m_fCurrentHitPoints : 0.f);
}

void Delirium::ShootAtAim(Body& b) {
    // Billy: one pistol shot sent them flying. A first hit could be shot twice in one frame: by the game's ragdoll
    // (m_Hit still 0) and then by its hit-point drop. One shot per body per 0.1 s.
    if (m_Clock - b.m_ShotAt < 0.1f)
        return;

    b.m_ShotAt = m_Clock;
    int s_Part = 0;
    float s_At[3] = {};
    NearestPart(b.m_Agent, m_Eye, m_Aim, s_Part, s_At);

    // 47's gun as GTA IV's (weaponinfo.xml, loaded at start; the SDK's four defaults without it): its push, damage and
    // reaction set. ponytail: knives and throws count as the last gun fired.
    const int t = m_Weapon;
    // Billy's pistol came through as an AK (anything not 0/3/4/7 was "ak47"): only an assault rifle is one now.
    const char* s_Gun = t == 2 ? "ak47" : t == 3 ? "mp5" : t == 4 ? "sniperrifle" : t == 7 ? "shotgun" : "pistol";
    {
        static int s_Last = -1;

        if (std::exchange(s_Last, t) != t)
            Logger::Info("[Delirium] 47's gun: Hitman weapon type {}, Delirium {}.", t, s_Gun);
    }
    int s_W = nm_weapon_find(s_Gun);

    if (s_W < 0) // no game table: the defaults
        s_W = nm_weapon_find(s_Gun = t == 0 ? "pistol" : t == 3 ? "smg" : t == 7 ? "shotgun" : "rifle");

    const float s_Dir[3] = {m_Aim.x, m_Aim.y, m_Aim.z};
    char s_Set[64] = {};
    float v[7] = {};
    nm_weapon_get(s_W, nullptr, 0, s_Set, sizeof(s_Set), v);

    // A shotgun's pellets fanned as the game does; those that pass within 25 cm of a part hit it.
    if (v[2] > 1.f) {
        static int s_Seed = 12345;
        std::vector<float> s_Fan((static_cast<size_t>(v[2]) + 1) * 3);
        const int n = nm_shotgun_fan(s_Dir, v[5], v[6], static_cast<int>(v[2]), &s_Seed, s_Fan.data());
        std::vector<float> s_Hits;

        for (int k = 1; k < n; ++k) {
            const SVector3 d = SVector3(s_Fan[k * 3] - m_Eye.x, s_Fan[k * 3 + 1] - m_Eye.y, s_Fan[k * 3 + 2] - m_Eye.z).Normalized();
            int p = 0;
            float at[3];

            if (NearestPart(b.m_Agent, m_Eye, d, p, at) < 0.25f)
                s_Hits.insert(s_Hits.end(), {static_cast<float>(p), at[0], at[1], at[2], d.x, d.y, d.z});
        }

        if (!s_Hits.empty()) {
            b.m_Health = std::max(0.f, b.m_Health - v[1] * (s_Hits.size() / 7)); // v[1] is one pellet's
            nm_agent_task_shotgun(b.m_Agent, m_World, s_Set, s_Hits.data(), static_cast<int>(s_Hits.size() / 7), b.m_Health, v[0]);
        }
    }
    else {
        // Pelican's replay (nm_scene_save): the shot moment to a file, replayed without the game to check a fix.
        {
            static int s_Saved = 0;

            if (s_Saved++ < 20) {
                std::string s_Name = b.m_Name;
                std::ranges::replace_if(s_Name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
                const std::string s_Path = std::format("F:/Users/billy/Projects/hitman-scenes/{}-{}.txt", s_Name,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                nm_scene_save(b.m_Agent, m_World, s_Path.c_str(), s_Gun, s_Part, s_At, s_Dir, b.m_Health);
            }
        }

        const float s_Left = nm_agent_shoot_weapon(b.m_Agent, m_World, s_W, s_Part, s_At, s_Dir, b.m_Health);
        // Pelican: under 100 he goes down, under 50 dead; a stagger needs ~200. The health in and out, then 6 s of the task.
        Logger::Info("[Delirium] {}: shot ({} on part {}), health {:.0f} > {:.0f}.", b.m_Name, s_Gun, s_Part, b.m_Health, s_Left);
        m_ShotWatch = 6.f, m_ShotAgent = b.m_Agent, m_ShotLog = 0.f;

        if (s_Left >= 0.f)
            b.m_Health = s_Left;
    }

    ++b.m_Hit;

    if (b.m_Health < 50.f)
        b.m_Dead = true;
}

// 47 invincible / hidden from the AI, as the SDK's Player mod does it (an AI cripple entity's pins), once per level.
void Delirium::UpdateCripple() {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene)
        return;

    if (s_Scene.m_pInterfaceRef != m_CrippleScene) {
        m_CrippleScene = s_Scene.m_pInterfaceRef;
        m_Cripple = {};
        m_CrippleDirty = m_Invincible || m_Hidden;
    }

    if (!m_CrippleDirty)
        return;

    m_CrippleDirty = false;

    if (!m_Cripple) {
        TResourcePtr<ZTemplateEntityFactory> s_Factory;
        Globals::ResourceManager->LoadResource(s_Factory, ResId<"[modules:/zaicrippleentity.class].pc_entitytype">);

        if (!s_Factory) {
            m_Line = "Can't make 47 invincible / hidden: the AI cripple class didn't load.";
            return;
        }

        SExternalReferences s_Refs;
        Functions::ZEntityManager_NewEntity->Call(Globals::EntityManager, m_Cripple, "", s_Factory, s_Scene.m_entityRef, s_Refs, -1);

        if (!m_Cripple) {
            m_Line = "Can't make 47 invincible / hidden: the game didn't make the entity.";
            return;
        }
    }

    m_Cripple.SignalInputPin(std::string(m_Invincible ? "SetHeroInvincible" : "SetHeroVulnerable"));
    m_Cripple.SignalInputPin(std::string(m_Hidden ? "SetHeroHidden" : "SetHeroVisible"));
    Logger::Info("[Delirium] 47 {}, {}.", m_Invincible ? "invincible" : "vulnerable", m_Hidden ? "hidden from the AI" : "seen by the AI");
}

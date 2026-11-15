#include "Delirium.h"

#include <Windows.h>
#include "Hooks.h"
#include <Glacier/ZActor.h>
#include <Glacier/ZOutfit.h>
#include <Glacier/ZSpatialEntity.h>
#include <Globals.h>
#include <Functions.h>
#include <MinHook.h>

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

namespace {
    constexpr float c_Step = 1.f / 60.f;
    constexpr int c_Parts = 21;
    constexpr int c_MaxPeds = 8;
    constexpr int c_Chest = 10;
    constexpr int c_MaxNpcs = 6;
    constexpr int c_MaxMeshes = 32;
    constexpr float c_GetUp = 1.2f; // s from lying to the animation
    // ponytail: a downed NPC's game capsule is lifted over 47's head so he walks over it; the game's own switch for
    // character collision (not in the SDK) would be the real fix.
    constexpr float c_CapsuleLift = 2.5f;

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

    void HkRagdoll(ZHM5BaseCharacter* c, bool p_InMotion) {
        if (GameRagdolls(c))
            g_Ragdoll(c, p_InMotion);
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

    // An EngineFunction's address (its only member).
    void* AddressOf(const void* p_Function) { return p_Function ? *static_cast<void* const*>(p_Function) : nullptr; }

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
        MH_Initialize();
        g_RagdollAt = AddressOf(Functions::ZHM5BaseCharacter_ActivateRagdoll);
        g_PoweredAt = AddressOf(Functions::ZHM5BaseCharacter_ActivatePoweredRagdoll);

        if (g_RagdollAt && MH_CreateHook(g_RagdollAt, reinterpret_cast<void*>(&HkRagdoll), reinterpret_cast<void**>(&g_Ragdoll)) == MH_OK)
            MH_EnableHook(g_RagdollAt);

        if (g_PoweredAt && MH_CreateHook(g_PoweredAt, reinterpret_cast<void*>(&HkPowered), reinterpret_cast<void**>(&g_Powered)) == MH_OK)
            MH_EnableHook(g_PoweredAt);

        Logger::Info("[Delirium] Game ragdoll hooks: {} {}.", g_Ragdoll ? "ragdoll" : "NO ragdoll",
            g_Powered ? "powered" : "NO powered");
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

void Delirium::Clear() {
    for (Ped& s_Ped : m_Peds)
        nm_agent_destroy(s_Ped.m_Agent);

    for (Body& s_Body : m_Bodies)
        Detach(s_Body);

    m_Bodies.clear();
    m_HitmanRagdoll = false;

    if (m_World)
        for (int i = 0; i < c_MaxMeshes; ++i)
            nm_world_set_mesh(m_World, i, nullptr, 0, nullptr, 0, nullptr);

    m_Peds.clear();
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
    constexpr int c_N = 7;
    constexpr float c_Spacing = 0.5f;

    std::vector<NmAgent*> s_Agents;

    for (const Ped& s_Ped : m_Peds)
        s_Agents.push_back(s_Ped.m_Agent);

    for (const Body& s_Body : m_Bodies)
        s_Agents.push_back(s_Body.m_Agent);

    const size_t s_One = p_One && !s_Agents.empty() ? m_GroundNext++ % s_Agents.size() : SIZE_MAX;

    for (size_t p = 0; p < s_Agents.size() && p < c_MaxMeshes; ++p) {
        if (s_One != SIZE_MAX && p != s_One)
            continue;

        if (std::ranges::any_of(m_Bodies, [&](const Body& b) { return b.m_Agent == s_Agents[p] && b.m_GettingUp; }))
            continue;

        float s_Pos[3], s_Rot[4];
        nm_agent_get_part_transform(s_Agents[p], 0, s_Pos, s_Rot);

        std::vector<float> s_Verts;
        std::vector<uint32_t> s_Tris;
        const float s_Half = c_Spacing * (c_N - 1) * 0.5f;

        const auto s_Last = m_LastGround.try_emplace(s_Agents[p], s_Pos[2] - 1.f).first;

        for (int y = 0; y < c_N; ++y) {
            for (int x = 0; x < c_N; ++x) {
                const SVector3 s_Top(s_Pos[0] - s_Half + x * c_Spacing, s_Pos[1] - s_Half + y * c_Spacing, s_Pos[2] + 1.f);
                SVector3 s_Hit, s_Normal;
                float s_Z = s_Last->second;

                if (p_Ray(s_Top, s_Top - SVector3(0.f, 0.f, 4.f), s_Hit, s_Normal)) {
                    s_Z = s_Hit.z;

                    if (x == c_N / 2 && y == c_N / 2)
                        s_Last->second = s_Z;
                }

                s_Verts.insert(s_Verts.end(), {s_Top.x, s_Top.y, s_Z});
            }
        }

        // A cell over an open portal in the floor is a hole: bodies fall in.
        const auto s_OverPortal = [&](uint32_t a, uint32_t d) {
            if (!m_PortalsOpen)
                return false;

            const float s_C[3] = {(s_Verts[a * 3] + s_Verts[d * 3]) * 0.5f, (s_Verts[a * 3 + 1] + s_Verts[d * 3 + 1]) * 0.5f,
                (s_Verts[a * 3 + 2] + s_Verts[d * 3 + 2]) * 0.5f};

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

        for (int y = 0; y + 1 < c_N; ++y) {
            for (int x = 0; x + 1 < c_N; ++x) {
                const uint32_t a = y * c_N + x, b = a + 1, c = a + c_N, d = c + 1;

                if (!s_OverPortal(a, d))
                    s_Tris.insert(s_Tris.end(), {a, b, d, a, d, c});
            }
        }

        // The walls round it: level rays out at knee and chest height, a wall panel stood where each hits (none in an
        // open portal: bodies go into it).
        const float s_Floor = s_Last->second;

        for (int k = 0; k < 16; ++k) {
            const float s_Ang = k * 6.2831853f / 16.f;
            const SVector3 s_Dir(std::cos(s_Ang), std::sin(s_Ang), 0.f);

            for (const float s_H : {0.4f, 1.2f}) {
                const SVector3 s_From(s_Pos[0], s_Pos[1], s_Floor + s_H);
                SVector3 s_Hit, s_N;

                if (!p_Ray(s_From, s_From + s_Dir * 2.5f, s_Hit, s_N))
                    continue;

                SVector3 n(s_N.x, s_N.y, 0.f);

                if (n.Length() < 0.7f)
                    continue;

                bool s_NearSomeone = false;

                for (const Body& o : m_Bodies) {
                    if (o.m_Now.size() < 3 || o.m_Agent == s_Agents[p])
                        continue;

                    const float dx = o.m_Now[0] - s_Hit.x, dy = o.m_Now[1] - s_Hit.y;
                    float s_P[3], s_R[4];
                    nm_agent_get_part_transform(o.m_Agent, 0, s_P, s_R);
                    const float ex = s_P[0] - s_Hit.x, ey = s_P[1] - s_Hit.y;
                    s_NearSomeone = s_NearSomeone || dx * dx + dy * dy < 0.8f * 0.8f || ex * ex + ey * ey < 0.8f * 0.8f;
                }

                if (s_NearSomeone)
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

                const SVector3 s_Side(-n.y, n.x, 0.f);
                const uint32_t s_Base = static_cast<uint32_t>(s_Verts.size() / 3);

                for (const auto [sx, sz] : {std::pair {-0.4f, -0.3f}, {0.4f, -0.3f}, {0.4f, 2.2f}, {-0.4f, 2.2f}}) {
                    const SVector3 v = s_Hit + s_Side * sx;
                    s_Verts.insert(s_Verts.end(), {v.x, v.y, s_Floor + sz});
                }

                // A slab 30 cm thick (a fast body went through a flat face between steps): front, back, top, ends.
                for (const auto [sx, sz] : {std::pair {-0.4f, -0.3f}, {0.4f, -0.3f}, {0.4f, 2.2f}, {-0.4f, 2.2f}}) {
                    const SVector3 v = s_Hit + s_Side * sx - n * 0.3f;
                    s_Verts.insert(s_Verts.end(), {v.x, v.y, s_Floor + sz});
                }

                // NM's triangles collide on both sides (Pelican): one winding each.
                const uint32_t f0 = s_Base, f1 = s_Base + 1, f2 = s_Base + 2, f3 = s_Base + 3;
                const uint32_t b0 = s_Base + 4, b1 = s_Base + 5, b2 = s_Base + 6, b3 = s_Base + 7;
                s_Tris.insert(s_Tris.end(), {f0, f1, f2, f0, f2, f3, b0, b2, b1, b0, b3, b2, f3, f2, b2, f3, b2, b3, f0, f3, b3,
                    f0, b3, b0, f1, b1, b2, f1, b2, f2});
            }
        }

        if (s_Tris.empty())
            s_Tris = {0, 1, 2};

        nm_world_set_mesh(m_World, static_cast<int>(p), s_Verts.data(), static_cast<int>(s_Verts.size() / 3), s_Tris.data(),
            static_cast<int>(s_Tris.size() / 3), nullptr);
    }
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

    if (Bones::World(p_Body.m_Anim, x, y, z, t, s_Layout)) {
        p_Q = FromAxes(float4(x[0], x[1], x[2], 0.f), float4(y[0], y[1], y[2], 0.f), float4(z[0], z[1], z[2], 0.f));
        p_P = SVector3(t[0], t[1], t[2]);
    }
    else {
        const SMatrix s_M = p_Body.m_Spatial->GetObjectToWorldMatrix();
        p_Q = FromAxes(s_M.XAxis, s_M.YAxis, s_M.ZAxis);
        p_P = SVector3(s_M.Trans.x, s_M.Trans.y, s_M.Trans.z);
    }

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
    // The rig (names, parents) from the morpheme entity; the pose from what draws him (47: his own animator).
    void* s_Rig = Bones::Animator(p_Char);
    // ponytail: 47's own animator (Bones::HitmanAnimator) gave a pose that isn't his mesh's (arms up), so the morpheme's.
    b.m_Anim = s_Rig;

    // Not posed yet (the hook only sees it from its next frame): try again later.
    std::vector<Bones::SQV> s_Local, s_RigLocal;

    if (!b.m_Anim || !s_Rig || !Bones::Read(b.m_Anim, s_Local) || !Bones::Read(s_Rig, s_RigLocal))
        return false;

    const int n = static_cast<int>(s_Local.size());

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

    // The pose it stands in now is the rest pose (an idle's: arms down, as Homer stands; Pelican's advice).
    if (!Posed(b))
        return false;

    const int s_N = static_cast<int>(b.m_Bones.size());
    std::vector<float> s_Places(s_N * 3);

    std::vector<float> s_Rest = b.m_Now;
    {
        std::string w;
        const std::vector<Bones::SQV> s_BindL = Bones::BindPose(s_Rig, n, w);

        if (s_BindL.size() == static_cast<size_t>(n)) {
            Q s_Rq;
            SVector3 s_Rp;
            Root(b, &s_Rq.x, s_Rp);
            std::vector<float> s_W;
            Fk(s_BindL, b.m_Parents, s_Rq, s_Rp, g_Conj, s_W);

            for (int r = 0; r < s_N; ++r)
                std::copy_n(&s_W[b.m_Bones[r] * 7], 7, &s_Rest[r * 7]);

            Logger::Info("[Delirium] {}: fitted on the bind pose.", p_Name);
        }
    }

    for (int r = 0; r < s_N; ++r)
        std::copy_n(&s_Rest[r * 7], 3, &s_Places[r * 3]);

    char s_Report[2048] = {};

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

    float s_Joints[63];

    if (nm_rig_joints(s_N, s_RigParents.data(), s_Places.data(), s_Map.data(), s_Joints) < 0) {
        Logger::Warn("[Delirium] {}: {}", p_Name, LastError());
        return false;
    }

    // Homer (or Wilma) fitted to this character's own proportions (Pelican): the keyframe then puts every part
    // exactly on its bone, no length fight (shaking) or baked-in pelvis height (floating). REAL: one world, scale 1.
    NmBuild* s_Base = p_Female && m_Wilma ? m_Wilma : m_Build;
    b.m_Build = nm_build_fit_sized(s_Base, s_Joints, NM_SIZE_REAL);

    if (!b.m_Build) {
        Logger::Warn("[Delirium] {}: fit failed: {}", p_Name, LastError());
        return false;
    }

    const SVector3 s_Hips = V(&b.m_Now[0]);
    float s_Floor = s_Hips.z;

    for (int i = 0; i < n; ++i)
        s_Floor = std::min(s_Floor, b.m_World[i * 7 + 2]);

    SVector3 s_Hit, s_Normal;

    if (p_Ray(s_Hips, s_Hips - SVector3(0.f, 0.f, 2.5f), s_Hit, s_Normal))
        s_Floor = s_Hit.z;

    const float s_At[3] = {s_Hips.x, s_Hips.y, s_Floor};
    b.m_Agent = nm_agent_create(b.m_Build, s_At, 0.f, 0.37f);

    if (b.m_Agent) {
        // The fitted build stands in the skeleton's bind pose (nm_build_part_frame): every part put there.
        float s_Worst = 0.f;
        int s_WorstPart = -1;

        for (int i = 0; i < c_Parts; ++i) {
            float s_P[3], s_R[4], s_AP[3], s_AR[4];
            const float s_Zero[3] = {0.f, 0.f, 0.f};

            if (!nm_build_part_frame(b.m_Build, i, s_P, s_R))
                continue;

            nm_agent_get_part_transform(b.m_Agent, i, s_AP, s_AR);
            const float d = (V(s_P) - V(s_AP)).Length();

            if (d > s_Worst)
                s_Worst = d, s_WorstPart = i;

            nm_agent_set_part_state(b.m_Agent, i, s_P, s_R, s_Zero, s_Zero);
        }

        Logger::Info("[Delirium] {}: parts put on the bind pose before the rig (furthest was part {}, {:.2f} m off).",
            p_Name, s_WorstPart, s_Worst);
    }

    b.m_Rig = b.m_Agent ? nm_rig_create(b.m_Agent, s_N, s_RigParents.data(), s_Rest.data(), s_Map.data()) : nullptr;

    if (!b.m_Rig) {
        Logger::Warn("[Delirium] {}: {}", p_Name, LastError());

        if (b.m_Agent)
            nm_agent_destroy(b.m_Agent);

        nm_build_destroy(b.m_Build);
        return false;
    }

    nm_rig_take(b.m_Rig, b.m_Agent, b.m_Now.data(), nullptr, 0.f);

    // On its animation exactly until hit or killed (GTA IV's way); a free half-ragdoll walking it sent them flying.
    // Keyframed parts can't move, so a keyframed NPC stays out of the other bodies' contacts until it ragdolls.
    nm_agent_keyframe(b.m_Agent, "fb", 1);
    nm_agent_collide_agents(b.m_Agent, p_Npc ? 0 : 1);

    Logger::Info("[Delirium] {} has {}'s body ({} bones, {} rigged).", p_Name, s_Base == m_Wilma ? "Wilma" : "Homer", n,
        s_N);
    Own(b.m_Char, true, b.m_Npc);
    m_Bodies.push_back(std::move(b));
    return true;
}

void Delirium::Unhook() {
    for (void* p : {g_RagdollAt, g_PoweredAt})
        if (p)
            MH_DisableHook(p);
}

void Delirium::Detach(Body& p_Body) {
    Own(p_Body.m_Char, false, false);
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
        nm_agent_collide_agents(b->m_Agent, 1);
        b->m_HighFall = false;
        b->m_PoseA.clear();
        b->m_PoseB.clear();
        b->m_GettingUp = false;
        b->m_Dead ? nm_agent_task_relax(b->m_Agent, -1.f, -1.f) : nm_agent_task_balance(b->m_Agent, m_World, "*", nullptr);
        const SMatrix s_M = b->m_Spatial->GetObjectToWorldMatrix();
        b->m_HeldZ = s_M.Trans.z;
        b->m_HeldRot = s_M;
    }
    else {
        // Back on his animation from where it is now, blended from the ragdoll's last pose (the get-up).
        Bones::Override(b->m_Anim, {});

        if (b->m_Npc) {
            float s_Pos[3], s_Rot[4];
            nm_agent_get_part_transform(b->m_Agent, 0, s_Pos, s_Rot);
            SMatrix s_M = b->m_HeldRot;
            float s_Head[3] = {};
            int s_FaceUp = 0;
            nm_agent_get_lying(b->m_Agent, s_Head, &s_FaceUp);
            SVector3 s_Face(s_Head[0], s_Head[1], 0.f);

            if (s_Face.Length() > 0.5f) {
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

    std::erase_if(m_Bodies, [&](Body& b) {
        const bool s_Keep = !b.m_Npc || std::ranges::any_of(s_Near, [&](const auto& n) {
            return n.second.m_pInterfaceRef == b.m_Char;
        });

        if (!s_Keep)
            Detach(b);

        return !s_Keep;
    });

    int s_Count = static_cast<int>(std::ranges::count_if(m_Bodies, [](const Body& b) { return b.m_Npc; }));

    for (const auto& [s_D, s_Actor] : s_Near) {
        if (s_Count >= c_MaxNpcs || s_D > 15.f)
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

            s_Sizes.back().insert(s_Sizes.back().end(), {r, h});
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

        const float t = m_Alpha;

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
    Bones::Tick();

    std::erase_if(m_Bodies, [&](Body& b) {
        if (!b.m_Npc || !GameTook(b.m_Char))
            return false;

        Logger::Info("[Delirium] {}: the game ragdolled it (shot or killed); Delirium lets it go.", b.m_Name);
        Detach(b);
        return true;
    });

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

    bool s_Spawn, s_Clear;
    std::vector<std::pair<int, std::string>> s_Fire;
    {
        std::scoped_lock s_Lock(m_Mutex);
        s_Spawn = std::exchange(m_WantSpawn, false);
        s_Clear = std::exchange(m_WantClear, false);
        s_Fire = std::exchange(m_WantFire, {});
    }

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

    if (s_ToggleRagdoll)
        SetHitmanRagdoll(!m_HitmanRagdoll);

    if ((m_NpcTimer -= p_Dt) <= 0.f) {
        m_NpcTimer = 1.f;

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
        if (s_Body && s_What != "Stand up again")
            s_Body->m_Npc ? SetRagdoll(*s_Body, true) : SetHitmanRagdoll(true);

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

        // A kill in Hitman: he drops (Delirium's death, not the game's).
        // ponytail: the game's own death ragdoll still takes the body over; detour ActivateRagdoll to keep NM's.
        if (b.m_Npc && !b.m_Dead && static_cast<ZActor*>(b.m_Char)->IsDead()) {
            b.m_Dead = true;
            SetRagdoll(b, true);
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

        for (Body& b : m_Bodies) {
            if (!b.m_Npc || b.m_Ragdoll || b.m_Now.size() < 3)
                continue;

            SVector3 s_D = V(b.m_Now.data()) - s_Hips;
            s_D.z = 0.f;

            if (s_D.Length() < 0.95f && s_Vel.Length() > 0.6f && s_Vel.Dot(s_D) > 0.f) {
                SetRagdoll(b, true);
                const SVector3 s_Push = s_Vel.Normalized() * 100.f;
                const float s_F[3] = {s_Push.x, s_Push.y, 0.f};
                const float s_At[3] = {b.m_Now[0], b.m_Now[1], b.m_Now[2] + 0.3f};
                nm_agent_apply_push(b.m_Agent, s_At, s_F);
                Logger::Info("[Delirium] 47 bumped {} at {:.1f} m/s.", b.m_Name, s_Vel.Length());
            }
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
            if (!dp_crossed(&m_Portals[i], s_Was[i], &s_C))
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
            continue;
        }

        // Lying still: Delirium's learned get-up (the Playroom's, on his own muscles); stood up, back on the animation.
        if (b.m_GettingUp) {
            if (!nm_agent_learned_getup_running(b.m_Agent) || (b.m_GetUpTime += p_Dt) > 9.f) {
                b.m_GettingUp = false;
                Logger::Info("[Delirium] {} is up.", b.m_Name);
                b.m_Npc ? SetRagdoll(b, false) : SetHitmanRagdoll(false);
            }

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

        if (nm_agent_should_get_up(b.m_Agent, 1.f) == 1) {
            const std::string s_Net = (m_Dir / "getup.net").string();

            if (nm_agent_learned_getup(b.m_Agent, s_Net.c_str()) >= 0 && nm_agent_learned_getup_running(b.m_Agent)) {
                b.m_GettingUp = true;
                b.m_GetUpTime = 0.f;
                Logger::Info("[Delirium] {} gets up.", b.m_Name);
            }
            else {
                Logger::Info("[Delirium] {} gets up (no learned get-up: {}).", b.m_Name, LastError());
                b.m_Npc ? SetRagdoll(b, false) : SetHitmanRagdoll(false);
            }
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
        if (b.m_Ragdoll)
            WritePose(b);

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
                    Logger::Warn("[Delirium] {}: lying {:.1f} s but its {} moves {:.1f} m/s (map or joints wrong?).", b.m_Name,
                        s_Lying, s_Pn, s_Max);
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

        // Ragdolling: the character goes where its body goes (no walking off out of it).
        if (b.m_Ragdoll) {
            float s_Pos[3], s_Rot[4];
            nm_agent_get_part_transform(b.m_Agent, 0, s_Pos, s_Rot);
            SMatrix s_M = b.m_HeldRot;
            s_M.Trans.x = s_Pos[0];
            s_M.Trans.y = s_Pos[1];
            s_M.Trans.z = b.m_HeldZ + (b.m_Npc ? c_CapsuleLift : 0.f);
            b.m_Spatial->SetObjectToWorldMatrixFromEditor(s_M);
        }
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
        const SVector4 s_Color = s_Picked ? SVector4(1.f, 0.6f, 0.1f, 1.f) : SVector4(0.9f, 0.9f, 0.9f, 1.f);

        for (int i = 0; i < c_Parts && i < static_cast<int>(m_Half.size()); ++i) {
            const float* s_T = &m_Pose[p][i * 7];
            const SVector3 s_C = V(s_T);
            const SVector3 s_Y = Rotate(s_T + 3, SVector3(0.f, 1.f, 0.f));
            const SVector3 s_X = Rotate(s_T + 3, SVector3(1.f, 0.f, 0.f));
            const SVector3 s_Z = Rotate(s_T + 3, SVector3(0.f, 0.f, 1.f));
            const bool s_Own = p < m_Sizes.size() && m_Sizes[p].size() == c_Parts * 2;
            const float r = s_Own ? m_Sizes[p][i * 2] : m_Radius[i], h = s_Own ? m_Sizes[p][i * 2 + 1] : m_Half[i];

            // Each part's capsule: its two end rings and four lines between them.
            constexpr int c_Seg = 10;

            for (const float s_End : {-h, h}) {
                for (int k = 0; k < c_Seg; ++k) {
                    const float a0 = 2.f * std::numbers::pi_v<float> * k / c_Seg;
                    const float a1 = 2.f * std::numbers::pi_v<float> * (k + 1) / c_Seg;
                    const SVector3 s_P0 = s_C + s_Y * s_End + (s_X * std::cos(a0) + s_Z * std::sin(a0)) * r;
                    const SVector3 s_P1 = s_C + s_Y * s_End + (s_X * std::cos(a1) + s_Z * std::sin(a1)) * r;
                    p_Renderer->DrawLine3D(s_P0, s_P1, s_Color, s_Color);
                }
            }

            for (const SVector3& s_Off : {s_X * r, s_X * -r, s_Z * r, s_Z * -r})
                p_Renderer->DrawLine3D(s_C - s_Y * h + s_Off, s_C + s_Y * h + s_Off, s_Color, s_Color);
        }
    }
}

void Delirium::DrawMenu() {
    ImGui::TextWrapped("%s", m_Status.c_str());
    ImGui::Checkbox("47's Homer body", &m_HitmanBody);
    ImGui::SameLine();
    ImGui::Checkbox("Show bodies", &m_ShowBodies);
    ImGui::TextDisabled("Num1 NPCs: %s   Num2 47 ragdoll: %s   Num5/6 shoot/push", m_NpcMode ? "ON" : "off",
        m_HitmanRagdoll ? "ON" : "off");
    ImGui::Separator();

    std::scoped_lock s_Lock(m_Mutex);

    if (ImGui::Button("Spawn ped"))
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

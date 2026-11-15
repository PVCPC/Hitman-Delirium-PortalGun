#include "Delirium.h"

#include <Windows.h>
#include "Hooks.h"
#include <Glacier/ZActor.h>
#include <Glacier/ZOutfit.h>
#include <Glacier/ZSpatialEntity.h>
#include <Globals.h>

#include "PortalLog.h"
#define Logger PortalLog

#include "IRenderer.h"


#include <imgui.h>
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "gta4nm.h"

namespace {
    constexpr float c_Step = 1.f / 60.f;
    constexpr int c_Parts = 21;
    constexpr int c_MaxPeds = 8;
    constexpr int c_Chest = 10;
    constexpr int c_MaxNpcs = 6;
    constexpr int c_MaxMeshes = 32;

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
void Delirium::UpdateGround(const Ray& p_Ray) {
    constexpr int c_N = 7;
    constexpr float c_Spacing = 0.5f;

    std::vector<NmAgent*> s_Agents;

    for (const Ped& s_Ped : m_Peds)
        s_Agents.push_back(s_Ped.m_Agent);

    for (const Body& s_Body : m_Bodies)
        s_Agents.push_back(s_Body.m_Agent);

    for (size_t p = 0; p < s_Agents.size() && p < c_MaxMeshes; ++p) {
        float s_Pos[3], s_Rot[4];
        nm_agent_get_part_transform(s_Agents[p], 0, s_Pos, s_Rot);

        std::vector<float> s_Verts;
        std::vector<uint32_t> s_Tris;
        const float s_Half = c_Spacing * (c_N - 1) * 0.5f;

        for (int y = 0; y < c_N; ++y) {
            for (int x = 0; x < c_N; ++x) {
                const SVector3 s_Top(s_Pos[0] - s_Half + x * c_Spacing, s_Pos[1] - s_Half + y * c_Spacing, s_Pos[2] + 1.f);
                SVector3 s_Hit, s_Normal;
                float s_Z = s_Pos[2] - 1.f;

                if (p_Ray(s_Top, s_Top - SVector3(0.f, 0.f, 4.f), s_Hit, s_Normal))
                    s_Z = s_Hit.z;

                s_Verts.insert(s_Verts.end(), {s_Top.x, s_Top.y, s_Z});
            }
        }

        for (int y = 0; y + 1 < c_N; ++y) {
            for (int x = 0; x + 1 < c_N; ++x) {
                const uint32_t a = y * c_N + x, b = a + 1, c = a + c_N, d = c + 1;
                s_Tris.insert(s_Tris.end(), {a, b, d, a, d, c});
            }
        }

        nm_world_set_mesh(m_World, static_cast<int>(p), s_Verts.data(), c_N * c_N, s_Tris.data(),
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
    const size_t n = p_Body.m_Local.size();
    p_Body.m_World.resize(n * 7);

    for (size_t i = 0; i < n; ++i) {
        const int s_Parent = p_Body.m_Parents[i];
        const float* w = s_Parent >= 0 ? &p_Body.m_World[s_Parent * 7] : nullptr;
        const Q s_Pq = w ? Q {w[3], w[4], w[5], w[6]} : s_ObjQ;
        const SVector3 s_Pp = w ? V(w) : s_ObjP;
        const Bones::SQV& l = p_Body.m_Local[i];
        const Q q = Norm(Mul(s_Pq, Q {l.q[0], l.q[1], l.q[2], l.q[3]}));
        const SVector3 p = s_Pp + Rot(s_Pq, V(l.p));
        float* o = &p_Body.m_World[i * 7];
        o[0] = p.x, o[1] = p.y, o[2] = p.z, o[3] = q.x, o[4] = q.y, o[5] = q.z, o[6] = q.w;
    }

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

    std::vector<float> s_Out(n * 4 + 3);
    const int s_Hips = nm_rig_pose(p_Body.m_Rig, p_Body.m_Agent, s_Out.data());

    if (s_Hips < 0 || s_Hips >= static_cast<int>(n))
        return;

    std::vector<Bones::SQV> s_Local = p_Body.m_Local;

    for (size_t r = 0; r < n; ++r)
        std::copy_n(&s_Out[r * 4], 4, s_Local[p_Body.m_Bones[r]].q);

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
    l.q[0] = s_Lq.x, l.q[1] = s_Lq.y, l.q[2] = s_Lq.z, l.q[3] = s_Lq.w;
    l.p[0] = s_Lp.x, l.p[1] = s_Lp.y, l.p[2] = s_Lp.z;
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
    b.m_Anim = Bones::Animator(p_Char);

    // Not posed yet (the hook only sees it from its next frame): try again later.
    std::vector<Bones::SQV> s_Local;

    if (!b.m_Anim || !Bones::Read(b.m_Anim, s_Local))
        return false;

    const int n = static_cast<int>(s_Local.size());
    b.m_Parents = Bones::Parents(b.m_Anim, n);

    if (b.m_Parents.empty() || n < 59) {
        Logger::Warn("[Delirium] {}: couldn't read the skeleton ({} bones).", p_Name, n);
        return false;
    }

    // Its bones by name (Hitman's rig is its own: 233 bones in the suit, 231 in a disguise).
    std::string s_Where;
    const std::vector<std::string> s_Names = Bones::Names(b.m_Anim, n, s_Where);
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
            const int s_Part = nm_rig_part_for_name(s_Names[i].c_str());
            s_Map.push_back(i == s_HipsBone ? 0 : s_Part == 0 ? -1 : s_Part);
            b.m_Bones.push_back(i);
        }
    }

    // The pose it stands in now is the rest pose (an idle's: arms down, as Homer stands; Pelican's advice).
    if (!Posed(b))
        return false;

    const int s_N = static_cast<int>(b.m_Bones.size());
    std::vector<float> s_Places(s_N * 3);

    for (int r = 0; r < s_N; ++r)
        std::copy_n(&b.m_Now[r * 7], 3, &s_Places[r * 3]);

    char s_Report[2048] = {};

    if (nm_rig_check(s_N, s_RigParents.data(), s_Places.data(), s_Map.data(), s_Report, sizeof(s_Report)) > 0)
        Logger::Warn("[Delirium] {}'s skeleton: {}", p_Name, s_Report);

    {
        const float* f = &b.m_Now[0];
        float s_Lo = 1e9f, s_Hi = -1e9f;

        for (int r = 0; r < s_N; ++r)
            s_Lo = std::min(s_Lo, b.m_Now[r * 7 + 2]), s_Hi = std::max(s_Hi, b.m_Now[r * 7 + 2]);

        const SMatrix s_M = p_Spatial->GetObjectToWorldMatrix();
        Logger::Info("[Delirium] {}: hips {:.2f} {:.2f} {:.2f}, bones from z {:.2f} to {:.2f}, entity at {:.2f} {:.2f} {:.2f}; "
            "bind pose {}.", p_Name, f[0], f[1], f[2], s_Lo, s_Hi, s_M.Trans.x, s_M.Trans.y, s_M.Trans.z,
            [&] { std::string w; return Bones::BindPose(b.m_Anim, n, w).empty() ? std::string("not found") : w; }());
    }

    float s_Joints[63];

    if (nm_rig_joints(s_N, s_RigParents.data(), s_Places.data(), s_Map.data(), s_Joints) < 0) {
        Logger::Warn("[Delirium] {}: {}", p_Name, LastError());
        return false;
    }

    // Homer's (or Wilma's) own body, unfitted, turned to face where the skeleton faces, its feet on the ground.
    NmBuild* s_Build = p_Female && m_Wilma ? m_Wilma : m_Build;
    float s_L[3], s_R[3], s_Rot[4];
    nm_build_part_frame(s_Build, 1, s_L, s_Rot);
    nm_build_part_frame(s_Build, 4, s_R, s_Rot);
    const auto s_PartBone = [&](int p_Part) {
        for (int r = 0; r < s_N; ++r)
            if (s_Map[r] == p_Part)
                return r;

        return 0;
    };
    const float s_Yaw = Facing(V(&b.m_Now[s_PartBone(1) * 7]), V(&b.m_Now[s_PartBone(4) * 7])) - Facing(V(s_L), V(s_R));

    const SVector3 s_Hips = V(&b.m_Now[0]);
    float s_Floor = s_Hips.z;

    for (int i = 0; i < n; ++i)
        s_Floor = std::min(s_Floor, b.m_World[i * 7 + 2]);

    SVector3 s_Hit, s_Normal;

    if (p_Ray(s_Hips, s_Hips - SVector3(0.f, 0.f, 2.5f), s_Hit, s_Normal))
        s_Floor = s_Hit.z;

    const float s_At[3] = {s_Hips.x, s_Hips.y, s_Floor};
    b.m_Agent = nm_agent_create(s_Build, s_At, s_Yaw, 0.37f);
    b.m_Rig = b.m_Agent ? nm_rig_create(b.m_Agent, s_N, s_RigParents.data(), b.m_Now.data(), s_Map.data()) : nullptr;

    if (!b.m_Rig) {
        Logger::Warn("[Delirium] {}: {}", p_Name, LastError());

        if (b.m_Agent)
            nm_agent_destroy(b.m_Agent);

        return false;
    }

    nm_rig_take(b.m_Rig, b.m_Agent, b.m_Now.data(), nullptr, 0.f);

    if (p_Npc)
        nm_agent_partial(b.m_Agent, NM_PARTIAL_LEGS);
    else
        nm_agent_keyframe(b.m_Agent, "fb", 1);

    Logger::Info("[Delirium] {} has {}'s body ({} bones, {} rigged).", p_Name, s_Build == m_Wilma ? "Wilma" : "Homer", n,
        s_N);
    m_Bodies.push_back(std::move(b));
    return true;
}

void Delirium::Detach(Body& p_Body) {
    Bones::Override(p_Body.m_Anim, {});

    if (p_Body.m_Rig)
        nm_rig_destroy(p_Body.m_Rig);

    if (p_Body.m_Agent)
        nm_agent_destroy(p_Body.m_Agent);

    p_Body.m_Rig = nullptr;
    p_Body.m_Agent = nullptr;
}

void Delirium::SetHitmanRagdoll(bool p_On) {
    m_HitmanRagdoll = p_On;
    Body* b = Hitman();

    if (!b || b->m_Ragdoll == p_On)
        return;

    b->m_Ragdoll = p_On;

    if (p_On) {
        nm_agent_keyframe(b->m_Agent, "fb", 0);
    }
    else {
        // Back on his animation from where it is now.
        Bones::Override(b->m_Anim, {});

        if (!b->m_Now.empty())
            nm_rig_take(b->m_Rig, b->m_Agent, b->m_Now.data(), nullptr, 0.f);

        nm_agent_keyframe(b->m_Agent, "fb", 1);
    }

    Logger::Info("[Delirium] 47 ragdoll {}.", p_On ? "on" : "off");
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

        if (Attach(a, s_Actor.m_entityRef.QueryInterface<ZSpatialEntity>(), true, s_Female, p_Ray,
            std::string(a->m_sActorName.c_str(), a->m_sActorName.size())))
            ++s_Count;
    }
}

void Delirium::Snapshot() {
    std::vector<std::vector<float>> s_Pose;
    std::vector<std::string> s_Names;
    const auto s_Add = [&](NmAgent* a) {
        s_Pose.emplace_back(a ? c_Parts * 7 : 0);

        if (a)
            nm_agent_get_part_transforms(a, s_Pose.back().data(), c_Parts);
    };

    // In target order: 47, the peds, the NPCs.
    const Body* s_Hitman = Hitman();
    s_Add(s_Hitman ? s_Hitman->m_Agent : nullptr);

    for (const Ped& s_Ped : m_Peds) {
        s_Add(s_Ped.m_Agent);
        s_Names.push_back(s_Ped.m_Name);
    }

    for (const Body& s_Body : m_Bodies) {
        if (s_Body.m_Npc) {
            s_Add(s_Body.m_Agent);
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
    m_Names = std::move(s_Names);
    m_PedsDrawn = m_Peds.size();
}

void Delirium::Update(float p_Dt, const Ray& p_Ray, const SVector3& p_Eye, const SVector3& p_Aim,
    const SVector3& p_Feet, const SVector3& p_Facing, ZHM5BaseCharacter* p_Hitman, ZSpatialEntity* p_HitmanSpatial) {
    m_Eye = p_Eye;
    m_Aim = p_Aim;

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
        if (s_Body && !s_Body->m_Npc && s_What != "Stand up again")
            SetHitmanRagdoll(true);

        if (s_Body && s_What == "Stand up again") {
            if (!s_Body->m_Npc) {
                SetHitmanRagdoll(false);
            }
            else if (!s_Body->m_Now.empty()) {
                Bones::Override(s_Body->m_Anim, {});
                s_Body->m_Ragdoll = false;
                nm_agent_set_dead(a, 0);
                nm_rig_take(s_Body->m_Rig, a, s_Body->m_Now.data(), nullptr, 0.f);
                nm_agent_partial(a, NM_PARTIAL_LEGS);
            }

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
            nm_agent_set_dead(b.m_Agent, 1);
        }
    }

    if ((m_GroundTimer -= p_Dt) <= 0.f) {
        m_GroundTimer = 0.25f;
        UpdateGround(p_Ray);
    }

    // NM's own 60 steps a second, at most 4 a frame.
    m_Accum = std::min(m_Accum + p_Dt, 4.f * c_Step);
    std::vector<NmAgent*> s_Agents;

    for (const Ped& s_Ped : m_Peds)
        s_Agents.push_back(s_Ped.m_Agent);

    for (const Body& s_Body : m_Bodies)
        s_Agents.push_back(s_Body.m_Agent);

    while (m_Accum >= c_Step) {
        m_Accum -= c_Step;
        nm_world_step_agents(m_World, s_Agents.data(), static_cast<int>(s_Agents.size()), c_Step, 0);
    }

    // NM's pose out onto the ragdolling ones.
    for (Body& b : m_Bodies) {
        if (b.m_Npc) {
            switch (nm_agent_partial_event(b.m_Agent)) {
                case NM_PARTIAL_KNOCKED:
                case NM_PARTIAL_FELL:
                case NM_PARTIAL_DROPPED:
                    b.m_Ragdoll = true;
                    break;
                case NM_PARTIAL_RESUMED:
                case NM_PARTIAL_RECOVERED:
                    b.m_Ragdoll = false;
                    Bones::Override(b.m_Anim, {});
                    break;
                default:
                    break;
            }
        }

        if (b.m_Ragdoll)
            WritePose(b);

        // 47 ragdolling: he goes where his body goes (no walking off out of it).
        if (!b.m_Npc && b.m_Ragdoll) {
            float s_Pos[3], s_Rot[4];
            nm_agent_get_part_transform(b.m_Agent, 0, s_Pos, s_Rot);
            SMatrix s_M = b.m_Spatial->GetObjectToWorldMatrix();
            s_M.Trans.x = s_Pos[0];
            s_M.Trans.y = s_Pos[1];
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
            const float r = m_Radius[i], h = m_Half[i];

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

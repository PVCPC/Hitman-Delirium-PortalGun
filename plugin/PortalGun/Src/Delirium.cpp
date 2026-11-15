#include "Delirium.h"

#include <Windows.h>
#include "Hooks.h"

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

    SVector3 V(const float* p) { return SVector3(p[0], p[1], p[2]); }

    // q (x, y, z, w) applied to v.
    SVector3 Rotate(const float* q, const SVector3& v) {
        const SVector3 u(q[0], q[1], q[2]);
        const float w = q[3];
        const SVector3 t = u.Cross(v) * 2.f;
        return v + t * w + u.Cross(t);
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

    if (std::filesystem::exists(m_Dir / "taskparams.txt"))
        nm_load_task_params((m_Dir / "taskparams.txt").string().c_str());

    m_Build = nm_build_load((m_Dir / "build.txt").string().c_str());

    if (!m_Build) {
        m_Status = "No build.txt: export it once with Delirium (delirium-room --game <GTA IV> --export <mods/Delirium>).";
        return false;
    }

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
    m_Status = std::format("Ready (gta4nm {}, {} behaviours).", nm_version(), m_Messages.size());
    Logger::Info("[Delirium] {}", m_Status);
    return true;
}

void Delirium::Clear() {
    for (Ped& s_Ped : m_Peds)
        nm_agent_destroy(s_Ped.m_Agent);

    if (m_World)
        for (int i = 0; i < c_MaxPeds; ++i)
            nm_world_set_mesh(m_World, i, nullptr, 0, nullptr, 0, nullptr);

    m_Peds.clear();
    std::scoped_lock s_Lock(m_Mutex);
    m_Pose.clear();
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

    for (size_t p = 0; p < m_Peds.size(); ++p) {
        float s_Pos[3], s_Rot[4];
        nm_agent_get_part_transform(m_Peds[p].m_Agent, 0, s_Pos, s_Rot);

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

int Delirium::Aimed() const {
    int s_Best = -1;
    float s_BestDist = 1e9f;
    const float s_From[3] = {m_Eye.x, m_Eye.y, m_Eye.z};
    const SVector3 s_End = m_Eye + m_Aim * 100.f;
    const float s_To[3] = {s_End.x, s_End.y, s_End.z};

    for (size_t i = 0; i < m_Peds.size(); ++i) {
        float s_Point[3], s_Normal[3];

        if (nm_agent_raycast(m_Peds[i].m_Agent, s_From, s_To, s_Point, s_Normal) >= 0) {
            const float s_D = (V(s_Point) - m_Eye).Length();

            if (s_D < s_BestDist) {
                s_BestDist = s_D;
                s_Best = static_cast<int>(i);
            }
        }
    }

    return s_Best;
}

void Delirium::Fire(int p_Ped, const std::string& p_What) {
    if (p_Ped < 0 || p_Ped >= static_cast<int>(m_Peds.size()))
        return;

    Ped& s_Ped = m_Peds[p_Ped];
    NmAgent* a = s_Ped.m_Agent;
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
        s_Ped.m_Health = w >= 0 ? nm_agent_shoot_weapon(a, m_World, w, s_Part, s_At, s_D, s_Ped.m_Health) : s_Ped.m_Health;
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
    else if (p_What == "Set on fire") s_Result = nm_agent_task_on_fire(a, m_World, s_Ped.m_Health);
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
            nm_agent_destroy(a);
            s_Ped.m_Agent = s_New;
            s_Ped.m_Health = 200.f;
            s_Result = nm_agent_task_balance(s_New, m_World, "*", s_Eye);
        }
    }
    else if (p_What.starts_with("msg:")) s_Result = nm_msg_send_text(a, m_World, p_What.c_str() + 4);

    m_Line = s_Result < 0 ? std::format("{} on {}: {}", p_What, s_Ped.m_Name, LastError())
                          : std::format("{} on {} (health {:.0f}).", p_What, s_Ped.m_Name, s_Ped.m_Health);
    Logger::Info("[Delirium] {}", m_Line);
}

void Delirium::Snapshot() {
    std::vector<std::vector<float>> s_Pose(m_Peds.size(), std::vector<float>(c_Parts * 7));

    for (size_t i = 0; i < m_Peds.size(); ++i)
        nm_agent_get_part_transforms(m_Peds[i].m_Agent, s_Pose[i].data(), c_Parts);

    std::scoped_lock s_Lock(m_Mutex);
    m_Pose = std::move(s_Pose);
}

void Delirium::Update(float p_Dt, const Ray& p_Ray, const SVector3& p_Eye, const SVector3& p_Aim,
    const SVector3& p_Feet, const SVector3& p_Facing) {
    m_Eye = p_Eye;
    m_Aim = p_Aim;

    // Numpad 1: Delirium on NPCs. Numpad 2: 47 ragdolls. Numpad 5 / 6: shoot / push the ped you aim at.
    static bool s_K1, s_K2, s_K5, s_K6;

    if (KeyPressed(VK_NUMPAD1, s_K1)) {
        m_NpcMode = !m_NpcMode;
        Logger::Info("[Delirium] NPC ragdolls {}.", m_NpcMode ? "on" : "off");
    }

    if (KeyPressed(VK_NUMPAD2, s_K2)) {
        m_HitmanRagdoll = !m_HitmanRagdoll;
        Logger::Info("[Delirium] 47 ragdoll {}.", m_HitmanRagdoll ? "on" : "off");
    }

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

    if (s_Clear)
        Clear();

    if (s_Spawn)
        Spawn(p_Ray, p_Feet, p_Facing);

    if (!m_Ready || m_Peds.empty())
        return;

    if (s_Shoot || s_Push)
        if (const int s_Ped = Aimed(); s_Ped >= 0)
            s_Fire.emplace_back(s_Ped, s_Shoot ? "Shot (pistol)" : "Push");

    for (const auto& [s_Ped, s_What] : s_Fire)
        Fire(s_Ped, s_What);

    if ((m_GroundTimer -= p_Dt) <= 0.f) {
        m_GroundTimer = 0.25f;
        UpdateGround(p_Ray);
    }

    // NM's own 60 steps a second, at most 4 a frame.
    m_Accum = std::min(m_Accum + p_Dt, 4.f * c_Step);
    std::vector<NmAgent*> s_Agents;

    for (const Ped& s_Ped : m_Peds)
        s_Agents.push_back(s_Ped.m_Agent);

    while (m_Accum >= c_Step) {
        m_Accum -= c_Step;
        nm_world_step_agents(m_World, s_Agents.data(), static_cast<int>(s_Agents.size()), c_Step, 0);
    }

    Snapshot();
}

void Delirium::Draw3D(IRenderer* p_Renderer) {
    std::scoped_lock s_Lock(m_Mutex);

    for (size_t p = 0; p < m_Pose.size(); ++p) {
        const bool s_Picked = static_cast<int>(p) + 1 == m_Target;
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
    if (!ImGui::CollapsingHeader("DELIRIUM TRAINER", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextWrapped("%s", m_Status.c_str());
    ImGui::Text("Numpad 1  Delirium on NPCs: %s", m_NpcMode ? "ON" : "off");
    ImGui::Text("Numpad 2  47 ragdoll: %s", m_HitmanRagdoll ? "ON" : "off");
    ImGui::TextDisabled("47's Homer body and Hitman's own NPCs: being hooked up (needs Hitman's skeleton).");
    ImGui::TextDisabled("Numpad 5 / 6: shoot / push the Delirium ped you aim at.");
    ImGui::Separator();

    std::scoped_lock s_Lock(m_Mutex);

    if (ImGui::Button("Spawn Delirium ped"))
        m_WantSpawn = true;

    ImGui::SameLine();

    if (ImGui::Button("Remove all"))
        m_WantClear = true;

    // Who: 47, or one of the peds.
    std::string s_Who = m_Target == 0 ? "47 (Homer body)" : std::format("Ped {}", m_Target);

    if (ImGui::BeginCombo("Target", s_Who.c_str())) {
        if (ImGui::Selectable("47 (Homer body)", m_Target == 0))
            m_Target = 0;

        for (size_t i = 0; i < m_Pose.size(); ++i)
            if (ImGui::Selectable(std::format("Ped {}", i + 1).c_str(), m_Target == static_cast<int>(i) + 1))
                m_Target = static_cast<int>(i) + 1;

        ImGui::EndCombo();
    }

    ImGui::SliderFloat("Strength", &m_Strength, 0.2f, 4.f, "%.1fx");

    if (m_Target == 0) {
        ImGui::TextDisabled("Pick a ped: 47's body isn't hooked up yet.");
    }
    else {
        for (int i = 0; i < static_cast<int>(std::size(c_Actions)); ++i) {
            if (i % 3)
                ImGui::SameLine();

            if (ImGui::Button(c_Actions[i], ImVec2(118.f, 0.f)))
                m_WantFire.emplace_back(m_Target - 1, c_Actions[i]);
        }

        // Every NM behaviour by name (the game's and Delirium's extras), started with its defaults.
        if (!m_Messages.empty()) {
            m_Message = std::clamp(m_Message, 0, static_cast<int>(m_Messages.size()) - 1);

            if (ImGui::BeginCombo("Behaviour", m_Messages[m_Message].c_str())) {
                for (int m = 0; m < static_cast<int>(m_Messages.size()); ++m)
                    if (ImGui::Selectable(m_Messages[m].c_str(), m == m_Message))
                        m_Message = m;

                ImGui::EndCombo();
            }

            ImGui::SameLine();

            if (ImGui::Button("Start"))
                m_WantFire.emplace_back(m_Target - 1, "msg:" + m_Messages[m_Message] + " start=true");
        }

        ImGui::InputText("##line", m_Text, sizeof(m_Text));
        ImGui::SameLine();

        if (ImGui::Button("Send"))
            m_WantFire.emplace_back(m_Target - 1, std::string("msg:") + m_Text);
    }

    if (!m_Line.empty())
        ImGui::TextWrapped("%s", m_Line.c_str());
}

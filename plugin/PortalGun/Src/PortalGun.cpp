#include <Glacier/ZActor.h>
#include "PortalGun.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <format>
#include <fstream>

#include <Windows.h>
#include <shobjidl.h>

#include <thread>

#include "Hooks.h"
#include "Logging.h"
#include "IRenderer.h"
#include "IconsMaterialDesign.h"

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZCameraEntity.h>
#include <Glacier/ZApplicationEngineWin32.h>
#include <Glacier/ZInputActionManager.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZCollision.h>
#include <Glacier/ZItem.h>
#include <Glacier/ZInventory.h>
#include <Glacier/ZPhysics.h>
#include <Glacier/ZRepository.h>
#include <Glacier/ZResource.h>
#include <Glacier/EntityFactory.h>
#include <Glacier/SExternalReferences.h>
#include <Glacier/CompileReflection.h>

#include "PortalLog.h"
#include "PortalViewJson.h"
#include "Sfx.h"
#include "Bones.h"
#define Logger PortalLog

namespace {
    // Source units to metres, at Portal's player scale (Chell is 72 units tall = 1.83m, like 47).
    constexpr float c_Unit = 0.0254f;

    // Oval half extents: Portal's portals are 64x108 units.
    constexpr float c_HalfWidth = 32.f * c_Unit;
    constexpr float c_HalfHeight = 54.f * c_Unit;

    // Portal 2's movement values, read from its server.dll (sv_gravity, sv_maxvelocity,
    // sv_airaccelerate, sv_speed_normal, CGameMovement's air speed cap and jump impulse).
    constexpr float c_Gravity = 600.f * c_Unit;
    constexpr float c_MaxVelocity = 3500.f * c_Unit;
    constexpr float c_AirAccelerate = 10.f;
    constexpr float c_AirSpeedCap = 30.f * c_Unit;
    constexpr float c_WalkSpeed = 175.f * c_Unit;
    constexpr float c_JumpSpeed = 268.3281573f * c_Unit;

    // 47's capsule, for flight collision.
    constexpr float c_CapsuleRadius = 0.3f;
    constexpr float c_CapsuleHeight = 1.85f;

    // How far in front of 47 a hand-placed portal goes.
    constexpr float c_PlaceDistance = 2.5f;

    // Exit offset along the out-portal normal, so 47's capsule (radius ~0.3m) clears the surface.
    constexpr float c_ExitPush = 0.6f;

    constexpr float c_TeleportCooldown = 0.25f;

    // Trigger volume in front of the portal plane. A solid wall stops 47's capsule about 0.3m out,
    // so the volume has to reach further than that or he could never enter it.
    constexpr float c_TriggerDepth = 0.45f;
    constexpr float c_TriggerBehind = 0.5f;

    // How far the portal gun reaches, and how far off the surface the portal sits.
    constexpr float c_MaxRange = 150.f;
    constexpr float c_SurfaceOffset = 0.02f;

    // Normals with |z| above this count as floors or ceilings.
    constexpr float c_FloorNormalZ = 0.7f;

    // Pick-up, from Portal 2: use reach 80 units (PLAYER_USE_RADIUS); the object is held
    // player_held_object_distance (15) beyond the player's hull (16) plus a cube's half size (18),
    // player_held_object_offset_up_cube (-10) lower.
    constexpr float c_UseRange = 80.f * c_Unit;
    constexpr float c_HoldDistance = (15.f + 16.f + 18.f) * c_Unit;
    constexpr float c_HoldOffsetUp = -10.f * c_Unit;
    constexpr float c_HoldMaxSpeed = 1000.f * c_Unit;
    constexpr float c_HoldBreakDistance = 1.5f;

    // Physics objects through portals: trigger depth in front of the portal, and how far out of the exit
    // surface they reappear.
    constexpr float c_BodyTriggerDepth = 0.5f;
    constexpr float c_BodyExitClear = 0.5f;
    constexpr size_t c_MaxTrackedBodies = 16;

    // When set, RayCast appends a line per query step here (self-test diagnostics).
    std::string* g_RayTrace = nullptr;

    const SVector4 c_BlueColor(0.25f, 0.6f, 1.f, 1.f);
    const SVector4 c_OrangeColor(1.f, 0.55f, 0.1f, 1.f);

    SVector3 ToVec3(const float4& p_Value) {
        return SVector3(p_Value.x, p_Value.y, p_Value.z);
    }

    // Portal frame from a centre and an outward normal, with world Z as up.
    SMatrix MakePortalFrame(const SVector3& p_Centre, const SVector3& p_Normal) {
        const SVector3 s_Up(0.f, 0.f, 1.f);
        const SVector3 s_Right = p_Normal.Cross(s_Up);

        return SMatrix(
            float4(s_Right, 0.f),
            float4(p_Normal, 0.f),
            float4(s_Up, 0.f),
            float4(p_Centre, 1.f)
        );
    }

    float Distance(const float4& p_A, const float4& p_B) {
        return (ToVec3(p_A) - ToVec3(p_B)).Length();
    }

    // Removes the component along p_Axis, then normalizes. Returns false if almost nothing is left.
    bool FlattenOnto(const SVector3& p_Vec, const SVector3& p_Axis, SVector3& p_Out) {
        const SVector3 s_Flat = p_Vec - p_Axis * p_Vec.Dot(p_Axis);

        if (s_Flat.Length() < 0.1f)
            return false;

        p_Out = s_Flat.Normalized();
        return true;
    }

    // Height above 47's feet of the point tested against a portal's trigger volume:
    // feet for floor portals, head for ceiling portals, waist for walls.
    float ProbeHeight(const SMatrix& p_Portal) {
        if (p_Portal.YAxis.z > c_FloorNormalZ)
            return 0.05f;

        if (p_Portal.YAxis.z < -c_FloorNormalZ)
            return 1.8f;

        return 1.f;
    }

    // Point in a portal's local frame.
    SVector3 ToLocal(const SMatrix& p_Portal, const SVector3& p_World) {
        return ToVec3(p_Portal.Inverse().WVectorTransform(float4(p_World, 1.f)));
    }

    bool g_Square = false; // rectangle portals instead of ovals (the Portals tab)

    bool InsideOval(const SVector3& p_Local, float p_Margin = 0.f) {
        const float s_Ex = p_Local.x / (c_HalfWidth + p_Margin);
        const float s_Ez = p_Local.z / (c_HalfHeight + p_Margin);

        if (g_Square)
            return std::abs(s_Ex) <= 1.f && std::abs(s_Ez) <= 1.f;

        return s_Ex * s_Ex + s_Ez * s_Ez <= 1.f;
    }

    // The opening's half width at height z (its own axes).
    float HalfWidthAt(float p_Z) {
        if (g_Square)
            return std::abs(p_Z) <= c_HalfHeight ? c_HalfWidth : 0.f;

        return c_HalfWidth * std::sqrt(std::max(0.f, 1.f - (p_Z / c_HalfHeight) * (p_Z / c_HalfHeight)));
    }

    // Rotation about world Z that faces along p_Aim, flattened (X = right, Y = forward, Z = up).
    SMatrix MakeYaw(const SVector3& p_Aim) {
        const SVector3 s_Up(0.f, 0.f, 1.f);
        SVector3 s_Fwd;

        if (!FlattenOnto(p_Aim, s_Up, s_Fwd))
            s_Fwd = SVector3(0.f, 1.f, 0.f);

        return SMatrix(float4(s_Fwd.Cross(s_Up), 0.f), float4(s_Fwd, 0.f), float4(s_Up, 0.f), float4(0.f, 0.f, 0.f, 1.f));
    }

    // Source's CGameMovement::AirAccelerate.
    void AirAccelerate(SVector3& p_Vel, const SVector3& p_WishDir, float p_WishSpeed, float p_Dt) {
        const float s_WishSpd = std::min(p_WishSpeed, c_AirSpeedCap);
        const float s_AddSpeed = s_WishSpd - p_Vel.Dot(p_WishDir);

        if (s_AddSpeed <= 0.f)
            return;

        const float s_AccelSpeed = std::min(c_AirAccelerate * p_WishSpeed * p_Dt, s_AddSpeed);
        p_Vel = p_Vel + p_WishDir * s_AccelSpeed;
    }

    // Source's CGameMovement::CheckVelocity clamps each axis.
    void ClampVelocity(SVector3& p_Vel) {
        p_Vel.x = std::clamp(p_Vel.x, -c_MaxVelocity, c_MaxVelocity);
        p_Vel.y = std::clamp(p_Vel.y, -c_MaxVelocity, c_MaxVelocity);
        p_Vel.z = std::clamp(p_Vel.z, -c_MaxVelocity, c_MaxVelocity);
    }

    // Source's ClipVelocity with an overbounce of 1: slide along the surface.
    void ClipVelocity(SVector3& p_Vel, const SVector3& p_Normal) {
        const float s_Into = p_Vel.Dot(p_Normal);

        if (s_Into < 0.f)
            p_Vel = p_Vel - p_Normal * s_Into;
    }
}

PortalGun::PortalGun() :
    m_PlaceBlueAction("PlaceBluePortal"),
    m_PlaceOrangeAction("PlaceOrangePortal"),
    m_GunOrangeAction("PortalGunOrange"),
    m_JumpAction("PortalGunJump"),
    m_FwdAction("PortalGunFwd"),
    m_BackAction("PortalGunBack"),
    m_LeftAction("PortalGunLeft"),
    m_RightAction("PortalGunRight"),
    m_GunId(ZString("9a7e5c1b-6f0d-4c2a-8b3e-50a7a1c0de42")),
    m_UseAction("PortalGunUse"),
    m_SpawnCubeAction("PortalGunSpawnCube"),
    m_CubeId(ZString("bb42bbb6-289b-46f6-83ae-201385d0deb1")) {}

PortalGun::~PortalGun() {
    Bones::Uninstall();
    Delirium::Unhook();
    const ZMemberDelegate<PortalGun, void(const SGameUpdateEvent&)> s_Delegate(this, &PortalGun::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

void PortalGun::OnEngineInitialized() {
    const ZMemberDelegate<PortalGun, void(const SGameUpdateEvent&)> s_Delegate(this, &PortalGun::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    const char* s_Binds = "PortalGunInput={"
        "PlaceBluePortal=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,b);"
        "PlaceOrangePortal=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,o);"
        "PortalGunOrange=tap(kb,r);"
        "PortalGunJump=tap(kb,space);"
        "PortalGunFwd=hold(kb,w);"
        "PortalGunBack=hold(kb,s);"
        "PortalGunLeft=hold(kb,a);"
        "PortalGunRight=hold(kb,d);"
        "PortalGunUse=tap(kb,f);"
        "PortalGunSpawnCube=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,j);};";

    // Find the folder this DLL lives in, and check for the self-test flag file.
    HMODULE s_Module = nullptr;
    wchar_t s_Path[MAX_PATH] = {};

    if (GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&MakePortalFrame), &s_Module
    ) && GetModuleFileNameW(s_Module, s_Path, MAX_PATH)) {
        m_ModDir = std::filesystem::path(s_Path).parent_path();
    }

    if (!m_ModDir.empty() && std::filesystem::exists(m_ModDir / "PortalGun.selftest")) {
        std::ofstream(m_ModDir / "PortalGun_selftest.txt", std::ios::trunc) << "[PortalGun] Self-test armed.\n";
        m_SelfTest = SelfTestStage::Waiting;
        Logger::Info("[PortalGun] Self-test armed.");
    }

    PortalLog::Open(m_ModDir);
    Logger::Info("[PortalGun] Loaded, built " __DATE__ " " __TIME__ ".");
    dp_set_half(c_HalfWidth, c_HalfHeight);
    FindGames();
    LoadSetup();

    if (ZInputActionManager::AddBindings(s_Binds))
        Logger::Info("[PortalGun] Bindings added (Ctrl+B blue, Ctrl+O orange).");
    else
        Logger::Warn("[PortalGun] Failed to add bindings.");
}

void PortalGun::Init() {
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &PortalGun::OnClearScene);
    Hooks::ZHM5ItemWeapon_FireProjectiles->AddDetour(this, &PortalGun::ZHM5ItemWeapon_FireProjectiles);
    Hooks::ZHM5ItemWeapon_IsFiring->AddDetour(this, &PortalGun::ZHM5ItemWeapon_IsFiring);
    Hooks::ZActor_YouGotHit->AddDetour(this, &PortalGun::ZActor_YouGotHit);
    Hooks::ZHM5ItemWeapon_SetBulletsInMagazine->AddDetour(this, &PortalGun::ZHM5ItemWeapon_SetBulletsInMagazine);
    Hooks::ZInputAction_Digital->AddDetour(this, &PortalGun::ZInputAction_Digital);
    Hooks::ZInputAction_Analog->AddDetour(this, &PortalGun::ZInputAction_Analog);
}

// The user's own installs (none of their files ship with the mod): saved next to the DLL or by Delirium,
// else found in the Steam libraries.
void PortalGun::FindGames() {
    const std::string s_Dir = m_ModDir.string();
    char s_Path[1024] = {};

    m_Portal2Path = dp_find_game(0, s_Dir.c_str(), s_Path, sizeof(s_Path)) ? s_Path : "";
    m_Gta4Path = dp_find_game(1, s_Dir.c_str(), s_Path, sizeof(s_Path)) ? s_Path : "";

    Logger::Info(
        "[PortalGun] Portal 2: {}. GTA IV: {}.", m_Portal2Path.empty() ? "not found" : m_Portal2Path,
        m_Gta4Path.empty() ? "not found" : m_Gta4Path
    );

    m_Delirium.Configure(m_ModDir / "Delirium", m_Gta4Path);
    Sfx::Load(m_Portal2Path);
}

// PortalGun.seen next to the DLL: there once the window has been closed, with a line per game the user skipped.
void PortalGun::LoadSetup() {
    std::ifstream s_File(m_ModDir / "PortalGun.seen");
    const bool s_Seen = s_File.is_open();
    std::string s_Line;

    while (std::getline(s_File, s_Line)) {
        if (s_Line == "skip portal2") m_Skip[0] = true;
        if (s_Line == "skip gta4") m_Skip[1] = true;
    }

    // First start, or a game it needs is missing and wasn't skipped.
    m_SetupOpen = !s_Seen || (m_Portal2Path.empty() && !m_Skip[0]) || (m_Gta4Path.empty() && !m_Skip[1]);
}

void PortalGun::SaveSetup() {
    std::ofstream s_File(m_ModDir / "PortalGun.seen", std::ios::trunc);
    if (m_Skip[0]) s_File << "skip portal2\n";
    if (m_Skip[1]) s_File << "skip gta4\n";
}

// Windows' own folder picker, on its own thread so the game keeps running. The folder is checked and saved there;
// the next frame picks it up.
void PortalGun::BrowseFolder(int p_Game) {
    if (m_Browsing.exchange(true))
        return;

    std::thread([this, p_Game] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog* s_Dialog = nullptr;

        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&s_Dialog)))) {
            DWORD s_Options = 0;
            s_Dialog->GetOptions(&s_Options);
            s_Dialog->SetOptions(s_Options | FOS_PICKFOLDERS);
            s_Dialog->SetTitle(p_Game == 0 ? L"Your Portal 2 folder" : L"Your GTA IV folder");
            IShellItem* s_Item = nullptr;

            if (SUCCEEDED(s_Dialog->Show(nullptr)) && SUCCEEDED(s_Dialog->GetResult(&s_Item))) {
                PWSTR s_Path = nullptr;

                if (SUCCEEDED(s_Item->GetDisplayName(SIGDN_FILESYSPATH, &s_Path))) {
                    const std::u8string s_U8 = std::filesystem::path(s_Path).u8string();
                    const std::string s_Utf8(reinterpret_cast<const char*>(s_U8.data()), s_U8.size());
                    m_BrowseResult = dp_set_game(p_Game, m_ModDir.string().c_str(), s_Utf8.c_str()) ? 1 : 2 + p_Game;
                    CoTaskMemFree(s_Path);
                }

                s_Item->Release();
            }

            s_Dialog->Release();
        }

        CoUninitialize();
        m_Browsing = false;
    }).detach();
}

// The startup window, laid out like Delirium Physics Playroom's: each game found, or a box to paste or pick its
// folder, or skip it. The portal gun works without either.
void PortalGun::OnDrawUI(bool p_HasFocus) {
    if (const int s_Result = m_BrowseResult.exchange(0)) {
        m_SetupError = s_Result == 1 ? "" : s_Result == 2 ? "That folder isn't Portal 2." : "That folder isn't GTA IV.";
        FindGames();
    }

    // Numpad 3 (while the game's window is in front) opens and closes our menu.
    DWORD s_Pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Pid);
    const bool s_Key = s_Pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_NUMPAD3) & 0x8000);

    if (s_Key && !m_MenuKeyDown)
        m_MenuOpen = !m_MenuOpen;

    m_MenuKeyDown = s_Key;

    // Our windows take the mouse while they're open, and give it back after.
    const bool s_Want = m_MenuOpen || m_SetupOpen;

    if (s_Want && !m_HaveFocus)
        SDK()->RequestUIFocus();
    else if (!s_Want && m_HaveFocus)
        SDK()->ReleaseUIFocus();

    m_HaveFocus = s_Want;

    if (m_MenuOpen) {
        ImGui::SetNextWindowSize(ImVec2(420.f, 560.f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Hitman-Delirium (Numpad 3)", &m_MenuOpen, ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Portals")) {
                DrawPortalTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Delirium")) {
                m_Delirium.DrawMenu();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    if (m_SetupOpen)
        DrawSetup(p_HasFocus);

    m_ScreenW = ImGui::GetIO().DisplaySize.x;
    m_ScreenH = ImGui::GetIO().DisplaySize.y;
    DrawViews();
    DrawRims();
}

// The Playroom's portal rim (portal/surface.wesl): a burning band from 0.85 of the opening out to its edge, noise
// tongues licking out, a soft glow past it; a new portal grows open over 0.5 s with its edge flashing. Drawn over the
// screen (ImGui), as the see-through is.
// ponytail: drawn over everything (no depth): a wall in front doesn't hide it; a real Glacier mesh would.
void PortalGun::DrawRims() {
    if (!m_PlayroomRims)
        return;

    const float s_Now = static_cast<float>(ImGui::GetTime());
    bool s_Any = false;

    for (int i = 0; i < 2; ++i) {
        const Portal& p = i == 0 ? m_Blue : m_Orange;

        if (!p.m_Placed) {
            m_RimSince[i] = -1.f;
            continue;
        }

        const SVector3 s_At = ToVec3(p.m_Transform.Trans);

        if (m_RimSince[i] < 0.f || (s_At - m_RimAt[i]).Length() > 0.01f)
            m_RimSince[i] = s_Now, m_RimAt[i] = s_At;

        s_Any = true;
    }

    if (!s_Any)
        return;

    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##PortalRims", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav);
    ImDrawList* s_List = ImGui::GetWindowDrawList();
    const ImVec2 s_White = ImGui::GetFontTexUvWhitePixel();

    const auto s_Col = [](const SVector4& c, float k, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.f, c.x * k), std::min(1.f, c.y * k), std::min(1.f, c.z * k), a));
    };

    for (int i = 0; i < 2; ++i) {
        const Portal& p = i == 0 ? m_Blue : m_Orange;

        if (!p.m_Placed)
            continue;

        const SMatrix& s_M = p.m_Transform;
        const SVector4& c = i == 0 ? c_BlueColor : c_OrangeColor;

        // From in front only.
        const auto s_Cam = Functions::GetCurrentCamera->Call();

        if (!s_Cam || (ToVec3(s_Cam->GetObjectToWorldMatrix().Trans) - ToVec3(s_M.Trans)).Dot(ToVec3(s_M.YAxis)) <= 0.f)
            continue;

        const float t = std::clamp((s_Now - m_RimSince[i]) / 0.5f, 0.f, 1.f);
        const float s_Grow = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
        const float s_Flash = 1.f + 1.5f * (1.f - t);

        constexpr int c_Seg = 64;
        // Rings out from the opening: radius (of the rim) and colour strength / alpha at each.
        constexpr float c_R[] = {0.80f, 0.85f, 0.92f, 1.0f, 1.12f};
        constexpr float c_K[] = {0.5f, 1.8f, 1.4f, 1.f, 1.f};
        constexpr float c_A[] = {0.f, 0.95f, 0.8f, 0.45f, 0.f};
        constexpr int c_Rings = 5;

        ImVec2 s_P[c_Rings][c_Seg + 1];
        bool s_Ok = true;

        for (int k = 0; k <= c_Seg && s_Ok; ++k) {
            const float a = 2.f * std::numbers::pi_v<float> * k / c_Seg;
            float x = std::cos(a), z = std::sin(a);

            if (g_Square) {
                const float m = std::max(std::abs(x), std::abs(z));
                x /= m, z /= m;
            }

            // Tongues: the outer rings flicker out by a moving noise.
            const float s_Lick = 0.5f + 0.5f * std::sin(a * 7.f + s_Now * 5.3f) * std::sin(a * 3.f - s_Now * 3.1f + i);

            for (int r = 0; r < c_Rings && s_Ok; ++r) {
                const float s_R = (c_R[r] + (r >= 3 ? 0.06f * s_Lick : 0.f)) * s_Grow;
                SVector2 s_S;
                s_Ok = SDK()->WorldToScreen(
                    ToVec3(s_M.WVectorTransform(float4(x * c_HalfWidth * s_R, 0.01f, z * c_HalfHeight * s_R, 1.f))), s_S);
                s_P[r][k] = ImVec2(s_S.x, s_S.y);
            }
        }

        if (!s_Ok)
            continue;

        for (int r = 0; r + 1 < c_Rings; ++r) {
            const ImU32 s_In = s_Col(c, c_K[r] * s_Flash, c_A[r]);
            const ImU32 s_Out = s_Col(c, c_K[r + 1] * s_Flash, c_A[r + 1]);
            s_List->PrimReserve(c_Seg * 6, c_Seg * 4);

            for (int k = 0; k < c_Seg; ++k) {
                const ImDrawIdx b = static_cast<ImDrawIdx>(s_List->_VtxCurrentIdx);
                s_List->PrimWriteVtx(s_P[r][k], s_White, s_In);
                s_List->PrimWriteVtx(s_P[r][k + 1], s_White, s_In);
                s_List->PrimWriteVtx(s_P[r + 1][k + 1], s_White, s_Out);
                s_List->PrimWriteVtx(s_P[r + 1][k], s_White, s_Out);
                s_List->PrimWriteIdx(b), s_List->PrimWriteIdx(b + 1), s_List->PrimWriteIdx(b + 2);
                s_List->PrimWriteIdx(b), s_List->PrimWriteIdx(b + 2), s_List->PrimWriteIdx(b + 3);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void PortalGun::DrawSetup(bool p_HasFocus) {
    const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(720.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(s_Screen.x * 0.5f, s_Screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Hitman-Delirium Portal Gun", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    if (!p_HasFocus)
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "Press Numpad 3 to use this window.");

    ImGui::TextWrapped("The portal gun works on its own. Your own games add to it (they're not included):");
    ImGui::Spacing();

    struct Row {
        const char* m_Name;
        const char* m_Adds;
        const char* m_Need;
        const char* m_Hint;
    };

    static constexpr Row c_Rows[2] = {
        {"Portal 2", "Adds Portal 2's gun and cube models, its sounds and its portal effects.",
         "You need your own Portal 2 for PC (Steam).", "e.g. C:\\Program Files (x86)\\Steam\\steamapps\\common\\Portal 2"},
        {"GTA IV", "Adds Delirium's ragdoll physics to NPCs and bodies: they stumble, brace and fall for real.",
         "You need your own GTA IV: The Complete Edition for PC (Steam), the folder with GTAIV.exe in it. "
         "A disc (.iso): open it once in Delirium Physics Playroom and it's found here.",
         "e.g. C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto IV\\GTAIV"},
    };

    for (int i = 0; i < 2; ++i) {
        const Row& s_Row = c_Rows[i];
        const std::string& s_Path = i == 0 ? m_Portal2Path : m_Gta4Path;
        char* s_Typed = i == 0 ? m_Portal2Typed : m_Gta4Typed;

        ImGui::PushID(i);
        ImGui::BeginChild("row", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::Text("%s", s_Row.m_Name);
        ImGui::SameLine();

        if (!s_Path.empty()) {
            ImGui::TextColored(ImVec4(0.47f, 0.9f, 0.59f, 1.f), "found: %s", s_Path.c_str());
        }
        else if (m_Skip[i]) {
            ImGui::TextDisabled("skipped");
            ImGui::SameLine();

            if (ImGui::SmallButton("Add it"))
                m_Skip[i] = false;
        }
        else {
            ImGui::TextColored(ImVec4(0.94f, 0.47f, 0.39f, 1.f), "not found");
        }

        ImGui::TextWrapped("%s", s_Row.m_Adds);

        if (s_Path.empty() && !m_Skip[i]) {
            ImGui::TextWrapped("%s", s_Row.m_Need);
            ImGui::TextDisabled("Paste where it is, or pick it:");
            ImGui::SetNextItemWidth(420.f);
            ImGui::InputTextWithHint("##path", s_Row.m_Hint, s_Typed, 512);
            ImGui::SameLine();

            if (ImGui::Button("Use it")) {
                if (dp_set_game(i, m_ModDir.string().c_str(), s_Typed)) {
                    m_SetupError.clear();
                    FindGames();
                }
                else {
                    m_SetupError = std::format("That isn't {}: {}", s_Row.m_Name, s_Typed);
                }
            }

            ImGui::SameLine();

            if (ImGui::Button(m_Browsing ? "..." : "Browse"))
                BrowseFolder(i);

            ImGui::SameLine();

            if (ImGui::Button("Skip"))
                m_Skip[i] = true;
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

    if (!m_SetupError.empty())
        ImGui::TextColored(ImVec4(0.94f, 0.47f, 0.39f, 1.f), "%s", m_SetupError.c_str());

    ImGui::TextDisabled("You only do this once: it's remembered. Change it later from the menu (Numpad 3).");

    if (ImGui::Button("Done")) {
        m_SetupOpen = false;
        m_SetupError.clear();
        SaveSetup();
    }

    ImGui::End();
}

// The SDK's own menu: just the way into ours.
void PortalGun::OnDrawMenu() {
    if (ImGui::Button(ICON_MD_MENU " HITMAN-DELIRIUM (NUMPAD 3)"))
        m_MenuOpen = true;
}

void PortalGun::DrawPortalTab() {
    if (ImGui::Button(ICON_MD_FOLDER " YOUR GAMES (PORTAL 2 / GTA IV)"))
        m_SetupOpen = true;

    if (ImGui::Button(ICON_MD_ADD " GIVE PORTAL GUN"))
        m_RequestGive = true;

    if (ImGui::Button(ICON_MD_CIRCLE " BLUE"))
        m_RequestBlue = true;

    if (ImGui::Button(ICON_MD_CIRCLE " ORANGE"))
        m_RequestOrange = true;

    if (ImGui::Button(ICON_MD_CIRCLE " BLUE IN FRONT"))
        m_RequestBlueHere = true;

    if (ImGui::Button(ICON_MD_CIRCLE " ORANGE IN FRONT"))
        m_RequestOrangeHere = true;

    if (ImGui::Button(ICON_MD_ADD " SPAWN CUBE"))
        m_RequestCube = true;

    if (ImGui::Button(ICON_MD_CLEAR " CLEAR"))
        m_RequestClear = true;

    static bool s_Easy = true;

    if (ImGui::Checkbox("Easy placement (open on any wall)", &s_Easy))
        dp_set_easy(s_Easy ? 1 : 0);

    if (ImGui::Checkbox("Rectangle portals", &g_Square))
        dp_set_rect(g_Square ? 1 : 0);
    ImGui::Checkbox("Playroom portal rims", &m_PlayroomRims);
    ImGui::Checkbox("See-through portals", &m_SeeThrough);
    ImGui::SameLine();
    ImGui::Checkbox("Show portal cameras", &m_ShowCameras);
    ImGui::SliderFloat("Volume", &m_Volume, 0.f, 1.f, "%.2f");
}

void PortalGun::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    if (m_RequestClear) {
        m_RequestClear = false;
        ClearPortals();
    }

    auto s_LocalHitman = SDK()->GetLocalPlayer();

    if (!s_LocalHitman)
        return;

    const auto s_HitmanSpatial = s_LocalHitman.m_entityRef.QueryInterface<ZSpatialEntity>();

    if (!s_HitmanSpatial)
        return;

    const SMatrix s_PlayerTrans = s_HitmanSpatial->GetObjectToWorldMatrix();
    const float s_Dt = static_cast<float>(p_UpdateEvent.m_GameTimeDelta.ToSeconds());
    ++m_Frame;
    m_SceneTime += s_Dt;

    // Hand 47 the portal gun once per level, after things have settled.
    if (!m_GaveThisScene && m_SceneTime > 3.f) {
        m_GaveThisScene = true;
        m_RequestGive = true;
    }

    if (m_RequestGive) {
        m_RequestGive = false;
        GivePortalGun();
    }

    // R fires orange while the portal gun is in 47's hands. (The game checks the held weapon every frame.)
    const bool s_GunInHand = GunInHand();

    if (Functions::ZInputAction_Digital->Call(&m_GunOrangeAction, -1) && s_GunInHand)
        m_RequestOrange = true;

    // Right click (Hitman's aim, swallowed below) fires orange, like Portal.
    const bool s_AimHeld = m_AimSeenFrame != 0 && m_AimSeenFrame + 1 >= m_Frame;

    DWORD s_Fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Fg);
    const bool s_RightDown = !m_MenuOpen && s_Fg == GetCurrentProcessId() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);

    if (s_RightDown && !m_RightWasDown && s_GunInHand)
        m_RequestOrange = true;

    m_RightWasDown = s_RightDown;

    m_AimWasHeld = s_AimHeld;

    if (Functions::ZInputAction_Digital->Call(&m_PlaceBlueAction, -1))
        m_RequestBlue = true;

    if (Functions::ZInputAction_Digital->Call(&m_PlaceOrangeAction, -1))
        m_RequestOrange = true;

    if (Functions::ZInputAction_Digital->Call(&m_SpawnCubeAction, -1))
        m_RequestCube = true;

    if (m_RequestCube) {
        m_RequestCube = false;
        SpawnCube(s_PlayerTrans);
    }

    if (m_Delirium.HitmanDown())
        m_RequestBlue = m_RequestOrange = false;

    if (m_RequestBlue) {
        m_RequestBlue = false;
        ShootPortal(m_Blue, m_Orange, s_LocalHitman.m_entityRef);
    }

    if (m_RequestOrange) {
        m_RequestOrange = false;
        ShootPortal(m_Orange, m_Blue, s_LocalHitman.m_entityRef);
    }

    if (m_RequestBlueHere) {
        m_RequestBlueHere = false;
        PlacePortalInFrontOfPlayer(m_Blue, s_PlayerTrans);
    }

    if (m_RequestOrangeHere) {
        m_RequestOrangeHere = false;
        PlacePortalInFrontOfPlayer(m_Orange, s_PlayerTrans);
    }

    if (m_Cooldown > 0.f)
        m_Cooldown -= s_Dt;

    if (m_SelfTest != SelfTestStage::Off && m_SelfTest != SelfTestStage::Done)
        RunSelfTest(s_HitmanSpatial, s_PlayerTrans, s_Dt);

    if (s_Dt <= 0.f)
        return;

    m_Dt = s_Dt;
    UpdatePickup(s_LocalHitman.m_entityRef, s_Dt);
    UpdateBodies(s_Dt);

    // 47's walking velocity, measured from how far the game moved him since last frame.
    // Jumps of more than 20 m/s are cutscenes, respawns or our own teleports, not walking.
    const SVector3 s_Feet = ToVec3(s_PlayerTrans.Trans);

    if (!m_Flying) {
        m_WalkVel = SVector3(0.f, 0.f, 0.f);

        if (m_HasPrevFeet) {
            const SVector3 s_Vel = (s_Feet - m_PrevFeet) * (1.f / s_Dt);

            if (s_Vel.Length() < 20.f)
                m_WalkVel = s_Vel;
        }

        if (Functions::ZInputAction_Digital->Call(&m_JumpAction, -1)) {
            // Portal's jump: keep the run-up speed, add the jump impulse.
            StartFlight(s_Feet, SVector3(m_WalkVel.x, m_WalkVel.y, c_JumpSpeed));
        }
        else if (m_Blue.m_Placed && m_Orange.m_Placed) {
            // Standing over a floor portal: there's no floor there any more, so he falls in.
            for (const Portal* s_Portal : { &m_Blue, &m_Orange }) {
                if (s_Portal->m_Transform.YAxis.z <= c_FloorNormalZ)
                    continue;

                const SVector3 s_Local = ToLocal(s_Portal->m_Transform, s_Feet);

                if (std::abs(s_Local.y) < 0.3f && InsideOval(s_Local, -c_CapsuleRadius * 0.5f)) {
                    StartFlight(s_Feet, SVector3(m_WalkVel.x, m_WalkVel.y, 0.f));
                    break;
                }
            }
        }
    }

    SMatrix s_Trans = s_PlayerTrans;

    if (m_Flying)
        UpdateFlight(s_HitmanSpatial, s_Trans, s_Dt);

    m_PrevFeet = ToVec3(s_Trans.Trans);
    m_HasPrevFeet = true;

    if (m_Blue.m_Placed && m_Orange.m_Placed) {
        const DpFrame s_Pair[2] = {ToDpFrame(m_Blue.m_Transform), ToDpFrame(m_Orange.m_Transform)};
        m_Delirium.SetPortals(s_Pair);
    }
    else {
        m_Delirium.SetPortals(nullptr);
    }

    if (m_Blue.m_Placed && m_Orange.m_Placed && !TryTeleport(m_Blue, m_Orange, s_HitmanSpatial, s_Trans))
        TryTeleport(m_Orange, m_Blue, s_HitmanSpatial, s_Trans);

    UpdateViews(s_LocalHitman.m_entityRef);

    if (const auto s_Camera = Functions::GetCurrentCamera->Call()) {
        const SMatrix s_Cam = s_Camera->GetObjectToWorldMatrix();
        const ZEntityRef s_Me = s_LocalHitman.m_entityRef;
        m_Delirium.Update(
            s_Dt,
            [&](const SVector3& p_From, const SVector3& p_To, SVector3& p_Hit, SVector3& p_Normal) {
                return RayCastLevel(p_From, p_To, s_Me, p_Hit, p_Normal);
            },
            ToVec3(s_Cam.Trans), (ToVec3(s_Cam.CameraBackward) * -1.f).Normalized(), ToVec3(s_PlayerTrans.Trans),
            ToVec3(s_PlayerTrans.YAxis), s_LocalHitman.m_pInterfaceRef, s_HitmanSpatial
        );
    }
}

void PortalGun::StartFlight(const SVector3& p_Feet, const SVector3& p_Vel) {
    m_Flying = true;
    m_FlyFeet = p_Feet;
    m_FlyVel = p_Vel;
    ClampVelocity(m_FlyVel);
    m_FlyTime = 0.f;
}

bool PortalGun::IsCarved(const SVector3& p_Point) const {
    // Surfaces inside a placed portal's oval don't block anything, as if a hole were cut there.
    for (const Portal* s_Portal : { &m_Blue, &m_Orange }) {
        if (!s_Portal->m_Placed || !m_Blue.m_Placed || !m_Orange.m_Placed)
            continue;

        const SVector3 s_Local = ToLocal(s_Portal->m_Transform, p_Point);

        if (std::abs(s_Local.y) < 0.15f && InsideOval(s_Local))
            return true;
    }

    return false;
}

bool PortalGun::FlightRayCast(const SVector3& p_From, const SVector3& p_To, SVector3& p_Hit, SVector3& p_Normal) const {
    const ZEntityRef s_Hitman = SDK()->GetLocalPlayer().m_entityRef;
    const SVector3 s_Dir = (p_To - p_From).Normalized();
    SVector3 s_From = p_From;

    // Pass through carved surfaces and look for whatever is behind them.
    for (int i = 0; i < 4; ++i) {
        if (!RayCast(s_From, p_To, s_Hitman, p_Hit, p_Normal))
            return false;

        if (!IsCarved(p_Hit))
            return true;

        s_From = p_Hit + s_Dir * 0.05f;

        if ((p_To - s_From).Dot(s_Dir) <= 0.f)
            return false;
    }

    return false;
}

void PortalGun::UpdateFlight(ZSpatialEntity* p_Hitman, SMatrix& p_Trans, float p_Dt) {
    const SVector3 s_Up(0.f, 0.f, 1.f);
    m_FlyTime += p_Dt;

    // Air control, like Portal: WASD relative to where the camera looks, at walking speed.
    // (The game snaps an airborne 47 to the nearest floor, so his own movement can't be used.)
    const float s_FMove = (Functions::ZInputAction_Digital->Call(&m_FwdAction, -1) ? 1.f : 0.f) -
        (Functions::ZInputAction_Digital->Call(&m_BackAction, -1) ? 1.f : 0.f);
    const float s_SMove = (Functions::ZInputAction_Digital->Call(&m_RightAction, -1) ? 1.f : 0.f) -
        (Functions::ZInputAction_Digital->Call(&m_LeftAction, -1) ? 1.f : 0.f);

    SVector3 s_Wish(0.f, 0.f, 0.f);

    if (const auto s_Camera = Functions::GetCurrentCamera->Call()) {
        const SMatrix s_Cam = s_Camera->GetObjectToWorldMatrix();
        SVector3 s_Fwd, s_Right;

        if (FlattenOnto(ToVec3(s_Cam.CameraBackward) * -1.f, s_Up, s_Fwd)) {
            s_Right = s_Fwd.Cross(s_Up);
            s_Wish = s_Fwd * s_FMove + s_Right * s_SMove;
        }
    }

    const float s_WishSpeed = s_Wish.Length() > 0.01f ? c_WalkSpeed : 0.f;

    // CGameMovement::FullWalkMove in the air: half gravity, AirMove, move, half gravity.
    m_FlyVel.z -= c_Gravity * 0.5f * p_Dt;

    if (s_WishSpeed > 0.01f)
        AirAccelerate(m_FlyVel, s_Wish.Normalized(), s_WishSpeed, p_Dt);

    ClampVelocity(m_FlyVel);

    const SVector3 s_Old = m_FlyFeet;
    SVector3 s_New = s_Old + m_FlyVel * p_Dt;
    SVector3 s_Hit, s_Normal;

    // Walls: sweep the capsule's edge at knee, waist and head height, and slide along what it hits.
    SVector3 s_Horiz = s_New - s_Old;
    s_Horiz.z = 0.f;

    if (s_Horiz.Length() > 0.001f) {
        const SVector3 s_HDir = s_Horiz.Normalized();

        for (const float s_Height : { 0.4f, 1.f, 1.6f }) {
            const SVector3 s_From = s_Old + s_Up * s_Height;
            const SVector3 s_To = SVector3(s_New.x, s_New.y, s_Old.z + s_Height) + s_HDir * c_CapsuleRadius;

            if (FlightRayCast(s_From, s_To, s_Hit, s_Normal) && std::abs(s_Normal.z) < c_FloorNormalZ) {
                SVector3 s_WallN(s_Normal.x, s_Normal.y, 0.f);
                s_WallN = s_WallN.Normalized();
                ClipVelocity(m_FlyVel, s_WallN);

                // Stop the capsule's edge at the wall, keep the vertical part of the move.
                const SVector3 s_Stop = s_Hit - s_HDir * (c_CapsuleRadius + 0.02f);
                s_New.x = s_Stop.x;
                s_New.y = s_Stop.y;
                break;
            }
        }
    }

    // Ceiling.
    if (m_FlyVel.z > 0.f &&
        FlightRayCast(s_Old + s_Up * (c_CapsuleHeight - 0.1f), s_New + s_Up * c_CapsuleHeight, s_Hit, s_Normal) &&
        s_Normal.z < -c_FloorNormalZ) {
        m_FlyVel.z = 0.f;
        s_New.z = s_Hit.z - c_CapsuleHeight;
    }

    m_FlyVel.z -= c_Gravity * 0.5f * p_Dt;

    // Landing: search from above where his feet were down to where they are now, so a fast fall
    // can't tunnel through the floor.
    bool s_Landed = false;

    if (m_FlyVel.z <= 0.f) {
        const SVector3 s_From(s_New.x, s_New.y, std::max(s_Old.z, s_New.z) + 0.5f);
        const SVector3 s_To(s_New.x, s_New.y, s_New.z - 0.02f);

        if (FlightRayCast(s_From, s_To, s_Hit, s_Normal)) {
            if (s_Normal.z > c_FloorNormalZ) {
                s_New.z = s_Hit.z;
                s_Landed = true;
            }
            else {
                // Steep slope: slide down it.
                ClipVelocity(m_FlyVel, s_Normal);
                s_New.z = std::max(s_New.z, s_Hit.z);
            }
        }
    }

    m_FlyFeet = s_New;
    p_Trans.Trans = float4(s_New, 1.f);
    p_Hitman->SetObjectToWorldMatrixFromEditor(p_Trans);

    if (s_Landed) {
        Logger::Info("[PortalGun] Landed after {:.2f}s, {:.1f} m/s.", m_FlyTime, m_FlyVel.Length());
        m_Flying = false;
        m_FlyVel = SVector3(0.f, 0.f, 0.f);
    }
}

void PortalGun::PlacePortalInFrontOfPlayer(Portal& p_Portal, const SMatrix& p_PlayerTrans) {
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    if (!s_Camera)
        return;

    // Camera looks down its -Z axis. Flatten that onto the ground plane.
    const SMatrix s_CameraTrans = s_Camera->GetObjectToWorldMatrix();
    SVector3 s_Forward = ToVec3(s_CameraTrans.CameraBackward) * -1.f;
    s_Forward.z = 0.f;

    if (s_Forward.Length() < 0.01f)
        return;

    s_Forward = s_Forward.Normalized();

    const SVector3 s_Up(0.f, 0.f, 1.f);
    // The portal faces back toward 47, so walking forward goes into it.
    const SVector3 s_Normal = s_Forward * -1.f;
    const SVector3 s_Right = s_Normal.Cross(s_Up);

    // 47's origin is at his feet. Put the bottom of the oval just below floor level.
    const SVector3 s_Feet = ToVec3(p_PlayerTrans.Trans);
    const SVector3 s_Centre = s_Feet + s_Forward * c_PlaceDistance + s_Up * (c_HalfHeight - 0.05f);

    p_Portal.m_Transform = SMatrix(
        float4(s_Right, 0.f),
        float4(s_Normal, 0.f),
        float4(s_Up, 0.f),
        float4(s_Centre, 1.f)
    );
    p_Portal.m_Placed = true;
    p_Portal.m_HasPrevSide = false;
    p_Portal.m_WasInside = false;

    Logger::Info("[PortalGun] Portal placed at ({:.2f}, {:.2f}, {:.2f}).", s_Centre.x, s_Centre.y, s_Centre.z);
}

// Delirium's ground rays: the level only, never a character's collider (an NPC's own capsule laid its ground at
// body height and the contacts threw it off; Pelican).
static thread_local bool g_LevelOnly = false;

static bool IsCharacter(const ZEntityRef& p_Hit, const SVector3& p_At) {
    if (p_Hit.QueryInterface<ZActor>() || p_Hit.QueryInterface<ZHM5BaseCharacter>())
        return true;

    if (!Globals::ActorManager)
        return false;

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        const auto s_Spatial = s_Entry.m_entityRef ? s_Entry.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

        if (!s_Spatial)
            continue;

        const float4 t = s_Spatial->GetObjectToWorldMatrix().Trans;

        if ((SVector3(t.x, t.y, t.z) - p_At).Length() < 3.f && (p_Hit == s_Entry.m_entityRef || p_Hit.IsAnyParent(s_Entry.m_entityRef)))
            return true;
    }

    return false;
}

bool PortalGun::RayCastLevel(const SVector3& p_From, const SVector3& p_To, const ZEntityRef& p_Ignore, SVector3& p_Hit,
    SVector3& p_Normal) {
    g_LevelOnly = true;
    const bool s_Ok = RayCast(p_From, p_To, p_Ignore, p_Hit, p_Normal);
    g_LevelOnly = false;
    return s_Ok;
}

bool PortalGun::RayCast(
    const SVector3& p_From, const SVector3& p_To, const ZEntityRef& p_Ignore, SVector3& p_Hit, SVector3& p_Normal
) {
    if (!*Globals::CollisionManager)
        return false;

    const SVector3 s_Dir = (p_To - p_From).Normalized();
    SVector3 s_From = p_From;

    const float s_Length = (p_To - p_From).Length();
    float s_Travelled = 0.f;

    // Step past anything that belongs to the ignored entity (47 himself, when the camera ray clips him).
    // A ray that starts inside a collider reports it at t=0, so this can take a few steps to get out of his capsule.
    for (int i = 0; i < 24; ++i) {
        ZRayQueryInput s_Input {
            .m_vFrom = float4(s_From, 1.f),
            .m_vTo = float4(p_To, 1.f),
        };

        ZRayQueryOutput s_Output {};

        const bool s_Ok = (*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Output);

        if (g_RayTrace) {
            *g_RayTrace += std::format(
                "    step {}: ret={} hasHit={} t={:.3f} pos=({:.2f}, {:.2f}, {:.2f}) entity={}\n",
                i, s_Ok, s_Output.m_bHasHit, s_Output.m_nT, s_Output.m_vPosition.x, s_Output.m_vPosition.y,
                s_Output.m_vPosition.z, s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef != nullptr
            );
        }

        // m_bHasHit isn't reliable on this game build (it stays false on real hits). A hit is
        // the return value plus a parameter t within the segment.
        if (!s_Ok || s_Output.m_nT < 0.f || s_Output.m_nT > 1.f)
            return false;

        bool s_Ignored = false;

        if (p_Ignore && s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef) {
            const ZEntityRef& s_Hit = s_Output.m_pBlockingSpatialEntity.m_entityRef;
            s_Ignored = s_Hit == p_Ignore || s_Hit.IsAnyParent(p_Ignore);
        }

        if (!s_Ignored && g_LevelOnly && s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef)
            s_Ignored = IsCharacter(s_Output.m_pBlockingSpatialEntity.m_entityRef, ToVec3(s_Output.m_vPosition));

        if (!s_Ignored) {
            p_Hit = ToVec3(s_Output.m_vPosition);
            p_Normal = ToVec3(s_Output.m_vNormal).Normalized();
            return true;
        }

        const float s_Step = std::max((ToVec3(s_Output.m_vPosition) - s_From).Length(), 0.f) + 0.05f;
        s_Travelled += s_Step;

        if (s_Travelled >= s_Length)
            return false;

        s_From = s_From + s_Dir * s_Step;
    }

    return false;
}

// Hitman portal matrices: X right (normal x up), Y the normal, Z up. Delirium frames: rotation columns
// (up x normal, up, normal), so right is flipped.
DpFrame PortalGun::ToDpFrame(const SMatrix& p_Frame) {
    const SVector3 s_Normal = ToVec3(p_Frame.YAxis);
    const SVector3 s_Up = ToVec3(p_Frame.ZAxis);
    const SVector3 s_Right = s_Up.Cross(s_Normal);
    const SVector3 s_At = ToVec3(p_Frame.Trans) - s_Normal * c_SurfaceOffset;

    // Rotation matrix (columns right, up, normal) to quaternion.
    const float m00 = s_Right.x, m01 = s_Up.x, m02 = s_Normal.x;
    const float m10 = s_Right.y, m11 = s_Up.y, m12 = s_Normal.y;
    const float m20 = s_Right.z, m21 = s_Up.z, m22 = s_Normal.z;
    float x, y, z, w;
    const float s_Trace = m00 + m11 + m22;

    if (s_Trace > 0.f) {
        const float s = std::sqrt(s_Trace + 1.f) * 2.f;
        w = 0.25f * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        w = (m21 - m12) / s; x = 0.25f * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
    }
    else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25f * s; z = (m12 + m21) / s;
    }
    else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25f * s;
    }

    return DpFrame {{s_At.x, s_At.y, s_At.z}, {x, y, z, w}};
}

SMatrix PortalGun::FromDpFrame(const DpFrame& p_Frame) {
    const float x = p_Frame.rot[0], y = p_Frame.rot[1], z = p_Frame.rot[2], w = p_Frame.rot[3];
    const SVector3 s_Up(2.f * (x * y - w * z), 1.f - 2.f * (x * x + z * z), 2.f * (y * z + w * x));
    const SVector3 s_Normal(2.f * (x * z + w * y), 2.f * (y * z - w * x), 1.f - 2.f * (x * x + y * y));
    // dp_shoot's frames sit 1 cm off the surface; ours sit c_SurfaceOffset off.
    const SVector3 s_At = SVector3(p_Frame.at[0], p_Frame.at[1], p_Frame.at[2]) + s_Normal * (c_SurfaceOffset - 0.01f);

    return SMatrix(
        float4(s_Normal.Cross(s_Up), 0.f),
        float4(s_Normal, 0.f),
        float4(s_Up, 0.f),
        float4(s_At, 1.f)
    );
}

bool PortalGun::DpRayCast(void* p_Ctx, const float (*p_Origin)[3], const float (*p_Dir)[3], float p_Max, DpHit* p_Hit) {
    const SVector3 s_From((*p_Origin)[0], (*p_Origin)[1], (*p_Origin)[2]);
    const SVector3 s_Dir((*p_Dir)[0], (*p_Dir)[1], (*p_Dir)[2]);
    SVector3 s_Hit, s_Normal;

    if (!RayCast(s_From, s_From + s_Dir * p_Max, *static_cast<const ZEntityRef*>(p_Ctx), s_Hit, s_Normal))
        return false;

    // Some meshes have flipped normals; the rules want it facing the ray's start.
    if (s_Normal.Dot(s_Dir) > 0.f)
        s_Normal = s_Normal * -1.f;

    *p_Hit = DpHit {{s_Hit.x, s_Hit.y, s_Hit.z}, {s_Normal.x, s_Normal.y, s_Normal.z}, (s_Hit - s_From).Length()};
    return true;
}

SMatrix PortalGun::MakeSurfaceFrame(const SVector3& p_Hit, const SVector3& p_Normal, const SVector3& p_AimDir) {
    const SVector3 s_WorldUp(0.f, 0.f, 1.f);
    SVector3 s_Up;

    // Walls: the oval stands upright. Floors and ceilings: its top points the way the shot was travelling,
    // like Portal does.
    if (std::abs(p_Normal.z) < c_FloorNormalZ) {
        if (!FlattenOnto(s_WorldUp, p_Normal, s_Up))
            s_Up = s_WorldUp;
    }
    else if (!FlattenOnto(p_AimDir, p_Normal, s_Up) && !FlattenOnto(SVector3(1.f, 0.f, 0.f), p_Normal, s_Up)) {
        s_Up = SVector3(0.f, 1.f, 0.f);
    }

    return SMatrix(
        float4(p_Normal.Cross(s_Up), 0.f),
        float4(p_Normal, 0.f),
        float4(s_Up, 0.f),
        float4(p_Hit + p_Normal * c_SurfaceOffset, 1.f)
    );
}

bool PortalGun::ShootPortal(
    Portal& p_Portal, const Portal& p_Other, const ZEntityRef& p_Hitman, const SVector3* p_Origin, const SVector3* p_Aim,
    int* p_Result
) {
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    if (!s_Camera && !p_Origin)
        return false;

    // Camera looks down its -Z axis.
    SVector3 s_Origin = p_Origin ? *p_Origin : ToVec3(s_Camera->GetObjectToWorldMatrix().Trans);
    const SVector3 s_Aim = p_Aim ? p_Aim->Normalized() : (ToVec3(s_Camera->GetObjectToWorldMatrix().CameraBackward) * -1.f).Normalized();

    // Sometimes the camera reports its position as the world origin. Shoot from 47's head instead.
    if (s_Origin.Length() < 0.01f) {
        if (const auto s_HitmanSpatial = p_Hitman.QueryInterface<ZSpatialEntity>())
            s_Origin = ToVec3(s_HitmanSpatial->GetObjectToWorldMatrix().Trans) + SVector3(0.f, 0.f, 1.6f);
    }

    Logger::Info(
        "[PortalGun] Shooting from ({:.2f}, {:.2f}, {:.2f}) along ({:.2f}, {:.2f}, {:.2f}).",
        s_Origin.x, s_Origin.y, s_Origin.z, s_Aim.x, s_Aim.y, s_Aim.z
    );

    // Portal 2's own placement rules and shot trace (Delirium's portal-core): fitting, floor snap, overlap,
    // backed corners, and shots bending through a linked pair.
    const float s_From[3] = {s_Origin.x, s_Origin.y, s_Origin.z};
    const float s_Dir[3] = {s_Aim.x, s_Aim.y, s_Aim.z};
    const DpFrame s_Other = ToDpFrame(p_Other.m_Transform);
    const DpFrame s_Linked[2] = {ToDpFrame(m_Blue.m_Transform), ToDpFrame(m_Orange.m_Transform)};
    const bool s_BothOpen = m_Blue.m_Placed && m_Orange.m_Placed;
    DpFrame s_Out {};
    DpHit s_ShotHit {};
    ZEntityRef s_Ignore = p_Hitman;

    const bool s_IsBlue = &p_Portal == &m_Blue;
    Sfx::Play(s_IsBlue ? Sfx::ShootBlue : Sfx::ShootOrange, m_Volume);
    const int s_Result = dp_shoot(
        &DpRayCast, &s_Ignore, &s_From, &s_Dir, c_MaxRange, p_Other.m_Placed ? &s_Other : nullptr,
        s_BothOpen ? s_Linked : nullptr, &s_Out, &s_ShotHit
    );

    if (p_Result)
        *p_Result = s_Result;

    if (s_Result != 0) {
        Logger::Info(
            "[PortalGun] Shot rejected: {}.",
            s_Result == 1 ? "nothing in range" : s_Result == 2 ? "the portal doesn't fit there" : "it overlaps the other portal"
        );
        return false;
    }

    const SMatrix s_Frame = FromDpFrame(s_Out);
    const SVector3 s_Normal = ToVec3(s_Frame.YAxis);

    p_Portal = {};
    p_Portal.m_Transform = s_Frame;
    Sfx::Play(s_IsBlue ? Sfx::OpenBlue : Sfx::OpenOrange, m_Volume);
    p_Portal.m_Placed = true;

    Logger::Info(
        "[PortalGun] Portal shot onto surface at ({:.2f}, {:.2f}, {:.2f}), normal ({:.2f}, {:.2f}, {:.2f}).",
        s_Frame.Trans.x, s_Frame.Trans.y, s_Frame.Trans.z, s_Normal.x, s_Normal.y, s_Normal.z
    );

    return true;
}

bool PortalGun::TryTeleport(Portal& p_In, Portal& p_Out, ZSpatialEntity* p_Hitman, const SMatrix& p_PlayerTrans) {
    // Probe point in the in-portal's local frame.
    const SMatrix& s_InTrans = p_In.m_Transform;
    const float4 s_Probe = p_PlayerTrans.Trans + float4(0.f, 0.f, ProbeHeight(s_InTrans), 0.f);
    const float4 s_Local = s_InTrans.Inverse().WVectorTransform(float4(s_Probe.x, s_Probe.y, s_Probe.z, 1.f));

    const SVector3 s_Local3 = ToVec3(s_Local);
    const bool s_Inside = InsideOval(s_Local3, 0.25f) && s_Local.y < c_TriggerDepth && s_Local.y > -c_TriggerBehind;

    const bool s_HadPrev = p_In.m_HasPrevSide;
    const bool s_WasInside = p_In.m_WasInside;
    const float s_PrevSide = p_In.m_PrevSide;
    const SVector3 s_PrevLocal = p_In.m_PrevLocal;

    p_In.m_PrevSide = s_Local.y;
    p_In.m_PrevLocal = s_Local3;
    p_In.m_WasInside = s_Inside;
    p_In.m_HasPrevSide = true;

    if (!s_HadPrev || m_Cooldown > 0.f)
        return false;

    // Walls: fire on entering the volume from the front, never from behind the surface.
    // Floors: he drops through the carved hole and fires when his feet cross the plane.
    const bool s_FloorPortal = s_InTrans.YAxis.z > c_FloorNormalZ;
    bool s_Enter = !s_FloorPortal && s_Inside && !s_WasInside && s_PrevSide >= -0.05f;

    // In flight he can move further than the volume is deep in one frame (a long fall into a floor
    // portal). Then fire if the step from last frame crossed the plane inside the oval.
    if (!s_Enter && m_Flying && s_PrevSide >= 0.f && s_Local.y < 0.f) {
        const float s_T = s_PrevSide / (s_PrevSide - s_Local.y);
        s_Enter = InsideOval(s_PrevLocal + (s_Local3 - s_PrevLocal) * s_T);
    }

    if (!s_Enter)
        return false;

    // Portal keeps the player's speed and turns his velocity with the portal pair.
    const SVector3 s_InVel = m_Flying ? m_FlyVel : m_WalkVel;

    SVector3 s_OutVel = MapDirection(s_InVel, s_InTrans, p_Out.m_Transform);

    SMatrix s_NewTrans = ExitTransform(p_PlayerTrans, s_InTrans, p_Out.m_Transform);

    // Leaving a floor portal: at least enough upward speed to clear the opening.
    const SVector3 s_OutNormal = ToVec3(p_Out.m_Transform.YAxis);

    if (s_OutNormal.z > c_FloorNormalZ)
        s_OutVel.z = std::max(s_OutVel.z, 2.f);

    p_Hitman->SetObjectToWorldMatrixFromEditor(s_NewTrans);
    m_LastTeleportTarget = s_NewTrans;
    StartFlight(ToVec3(s_NewTrans.Trans), s_OutVel);
    Logger::Info(
        "[PortalGun] Through at {:.1f} m/s, out ({:.1f}, {:.1f}, {:.1f}).", s_InVel.Length(), s_OutVel.x, s_OutVel.y,
        s_OutVel.z
    );

    // 47 now stands at the exit. He has to leave its volume before it can fire back.
    p_Out.m_WasInside = true;
    p_Out.m_HasPrevSide = false;
    p_In.m_HasPrevSide = false;

    m_Cooldown = c_TeleportCooldown;
    ++m_TeleportCount;

    Logger::Info("[PortalGun] Teleport #{}.", m_TeleportCount);

    return true;
}

SMatrix PortalGun::ExitTransform(const SMatrix& p_PlayerTrans, const SMatrix& p_In, const SMatrix& p_Out) {
    const SMatrix s_Mapped = MapThroughPortal(p_PlayerTrans, p_In, p_Out);
    const SVector3 s_WorldUp(0.f, 0.f, 1.f);

    // 47 always stays upright. Keep his mapped heading when it's horizontal-ish. Otherwise
    // (wall -> floor, floor -> wall) face along the direction he was travelling, which maps to
    // minus his mapped up axis, or along the exit portal's up.
    SVector3 s_Forward;

    if (!FlattenOnto(ToVec3(s_Mapped.YAxis), s_WorldUp, s_Forward) &&
        !FlattenOnto(ToVec3(s_Mapped.ZAxis) * -1.f, s_WorldUp, s_Forward) &&
        !FlattenOnto(ToVec3(p_Out.ZAxis), s_WorldUp, s_Forward)) {
        s_Forward = SVector3(0.f, 1.f, 0.f);
    }

    const SVector3 s_OutCentre = ToVec3(p_Out.Trans);
    const SVector3 s_OutNormal = ToVec3(p_Out.YAxis);
    SVector3 s_Feet;

    if (s_OutNormal.z > c_FloorNormalZ) {
        // Floor portal: pop out standing on it.
        s_Feet = s_OutCentre + s_WorldUp * 0.1f;
    }
    else if (s_OutNormal.z < -c_FloorNormalZ) {
        // Ceiling portal: drop out with his head just below it.
        s_Feet = s_OutCentre - s_WorldUp * 1.9f;
    }
    else {
        // Wall portal: keep his offset across the oval, but put him in front of the surface
        // and no lower than the bottom of the oval.
        const float s_Bottom = s_OutCentre.z - c_HalfHeight + 0.02f;

        if (std::abs(p_In.YAxis.z) > c_FloorNormalZ) {
            // Coming from a floor or ceiling: his offset across that oval means nothing here.
            // Step out of the bottom of this one.
            s_Feet = s_OutCentre + s_OutNormal * c_ExitPush;
            s_Feet.z = s_Bottom;
        }
        else {
            s_Feet = ToVec3(s_Mapped.Trans);
            const float s_Depth = (s_Feet - s_OutCentre).Dot(s_OutNormal);
            s_Feet = s_Feet + s_OutNormal * (c_ExitPush - s_Depth);
            s_Feet.z = std::clamp(s_Feet.z, s_Bottom, s_OutCentre.z);
        }
    }

    return SMatrix(
        float4(s_Forward.Cross(s_WorldUp), 0.f),
        float4(s_Forward, 0.f),
        float4(s_WorldUp, 0.f),
        float4(s_Feet, 1.f)
    );
}

SMatrix PortalGun::MapThroughPortal(const SMatrix& p_World, const SMatrix& p_In, const SMatrix& p_Out) {
    // world -> in-portal local -> turn 180 degrees about the portal's up axis -> out-portal world.
    const SMatrix s_HalfTurn(
        float4(-1.f, 0.f, 0.f, 0.f),
        float4(0.f, -1.f, 0.f, 0.f),
        float4(0.f, 0.f, 1.f, 0.f),
        float4(0.f, 0.f, 0.f, 1.f)
    );

    return p_World * p_In.Inverse() * s_HalfTurn * p_Out;
}

SVector3 PortalGun::MapDirection(const SVector3& p_Dir, const SMatrix& p_In, const SMatrix& p_Out) {
    SMatrix s_Point;
    s_Point.Trans = float4(0.f, 0.f, 0.f, 1.f);
    const float4 s_Origin = MapThroughPortal(s_Point, p_In, p_Out).Trans;

    s_Point.Trans = float4(p_Dir, 1.f);
    const float4 s_Tip = MapThroughPortal(s_Point, p_In, p_Out).Trans;

    return ToVec3(s_Tip) - ToVec3(s_Origin);
}

void PortalGun::OnDraw3D(IRenderer* p_Renderer) {
    m_Delirium.Draw3D(p_Renderer);
    if (!m_PlayroomRims) {
        DrawPortal(p_Renderer, m_Blue, c_BlueColor);
        DrawPortal(p_Renderer, m_Orange, c_OrangeColor);
    }
}

void PortalGun::DrawPortal(IRenderer* p_Renderer, const Portal& p_Portal, const SVector4& p_Color) {
    if (!p_Portal.m_Placed)
        return;

    constexpr int c_Segments = 48;
    const SMatrix& s_M = p_Portal.m_Transform;

    // A few concentric rings to fake a thick rim.
    for (int s_Ring = 0; s_Ring < 4; ++s_Ring) {
        const float s_Scale = 1.f - 0.025f * s_Ring;
        const float s_Offset = 0.01f * s_Ring;

        if (g_Square) {
            const float w = c_HalfWidth * s_Scale, h = c_HalfHeight * s_Scale;
            const float s_C[5][2] = {{-w, -h}, {w, -h}, {w, h}, {-w, h}, {-w, -h}};

            for (int k = 0; k < 4; ++k)
                p_Renderer->DrawLine3D(ToVec3(s_M.WVectorTransform(float4(s_C[k][0], s_Offset, s_C[k][1], 1.f))),
                    ToVec3(s_M.WVectorTransform(float4(s_C[k + 1][0], s_Offset, s_C[k + 1][1], 1.f))), p_Color, p_Color);

            continue;
        }

        for (int i = 0; i < c_Segments; ++i) {
            const float s_A0 = 2.f * std::numbers::pi_v<float> * i / c_Segments;
            const float s_A1 = 2.f * std::numbers::pi_v<float> * (i + 1) / c_Segments;

            const float4 s_P0 = s_M.WVectorTransform(
                float4(std::cos(s_A0) * c_HalfWidth * s_Scale, s_Offset, std::sin(s_A0) * c_HalfHeight * s_Scale, 1.f)
            );
            const float4 s_P1 = s_M.WVectorTransform(
                float4(std::cos(s_A1) * c_HalfWidth * s_Scale, s_Offset, std::sin(s_A1) * c_HalfHeight * s_Scale, 1.f)
            );

            p_Renderer->DrawLine3D(ToVec3(s_P0), ToVec3(s_P1), p_Color, p_Color);
        }
    }

    // Normal indicator, so it's obvious which side is the entrance.
    const float4 s_Centre = s_M.Trans;
    const float4 s_Tip = s_Centre + s_M.YAxis * 0.5f;
    p_Renderer->DrawLine3D(ToVec3(s_Centre), ToVec3(s_Tip), p_Color, SVector4(1.f, 1.f, 1.f, 1.f));
}

void PortalGun::RunSelfTest(ZSpatialEntity* p_Hitman, const SMatrix& p_PlayerTrans, float p_Dt) {
    const SVector3 s_Up(0.f, 0.f, 1.f);
    const SVector3 s_Feet = ToVec3(m_SelfTestStart.Trans);

    switch (m_SelfTest) {
        case SelfTestStage::Waiting: {
            // Give the level a few seconds to settle after 47 spawns.
            m_SelfTestTimer += p_Dt;

            if (m_SelfTestTimer < 5.f)
                return;

            m_SelfTestStart = p_PlayerTrans;
            const SVector3 s_Start = ToVec3(p_PlayerTrans.Trans);

            SelfTestLog(std::format("start feet=({:.2f}, {:.2f}, {:.2f})", s_Start.x, s_Start.y, s_Start.z));

            // Raycasts: straight down (should find the floor, not 47), sideways, and the real camera shot.
            const ZEntityRef s_HitmanRef = SDK()->GetLocalPlayer().m_entityRef;
            SelfTestLog(std::format("collision manager: {}", *Globals::CollisionManager != nullptr));

            auto s_TestRay = [&](const char* p_Name, const SVector3& p_From, const SVector3& p_To, const ZEntityRef& p_Ignore) {
                std::string s_Trace;
                g_RayTrace = &s_Trace;
                SVector3 s_Hit, s_Normal;
                const bool s_Ok = RayCast(p_From, p_To, p_Ignore, s_Hit, s_Normal);
                g_RayTrace = nullptr;

                SelfTestLog(std::format(
                    "ray {}: from=({:.2f}, {:.2f}, {:.2f}) to=({:.2f}, {:.2f}, {:.2f}) -> {} hit=({:.2f}, {:.2f}, {:.2f}) normal=({:.2f}, {:.2f}, {:.2f})\n{}",
                    p_Name, p_From.x, p_From.y, p_From.z, p_To.x, p_To.y, p_To.z, s_Ok ? "HIT" : "MISS",
                    s_Hit.x, s_Hit.y, s_Hit.z, s_Normal.x, s_Normal.y, s_Normal.z, s_Trace
                ));
                return std::make_pair(s_Ok, s_Hit);
            };

            const auto [s_DownOk, s_DownHit] = s_TestRay("down", s_Start + s_Up * 2.5f, s_Start - s_Up * 3.f, s_HitmanRef);
            s_TestRay("down-noignore", s_Start + s_Up * 2.5f, s_Start - s_Up * 3.f, ZEntityRef());
            s_TestRay("down-short", s_Start + s_Up * 0.3f, s_Start - s_Up * 1.f, ZEntityRef());

            for (int i = 0; i < 4; ++i) {
                const float s_A = i * std::numbers::pi_v<float> * 0.5f;
                const SVector3 s_Dir(std::cos(s_A), std::sin(s_A), 0.f);
                s_TestRay(std::format("side{}", i).c_str(), s_Start + s_Up * 1.f, s_Start + s_Up * 1.f + s_Dir * 50.f, s_HitmanRef);
            }

            if (const auto s_Camera = Functions::GetCurrentCamera->Call()) {
                const SMatrix s_Cam = s_Camera->GetObjectToWorldMatrix();
                const SVector3 s_Origin = ToVec3(s_Cam.Trans);
                const SVector3 s_Aim = (ToVec3(s_Cam.CameraBackward) * -1.f).Normalized();
                s_TestRay("camera", s_Origin, s_Origin + s_Aim * c_MaxRange, s_HitmanRef);
            }

            const bool s_FloorOk = s_DownOk && std::abs(s_DownHit.z - s_Start.z) < 0.3f;
            SelfTestLog(std::format("raycast floor check -> {}", s_FloorOk ? "PASS" : "FAIL"));
            m_SelfTestPasses += s_FloorOk ? 1 : 0;

            m_SelfTestPhase = 1;
            m_SelfTest = SelfTestStage::Setup;
            return;
        }

        case SelfTestStage::Setup: {
            // Orange is a wall-style portal 4m to the side, facing +Y.
            m_Orange = {};
            m_Orange.m_Transform = MakePortalFrame(s_Feet + SVector3(0.f, 4.f, 0.f) + s_Up * c_HalfHeight, SVector3(0.f, 1.f, 0.f));
            m_Orange.m_Placed = true;

            m_Blue = {};
            m_Blue.m_Placed = true;

            if (m_SelfTestPhase == 1) {
                // Wall portal 1.5m ahead along +X, facing 47: he starts outside its volume.
                m_Blue.m_Transform = MakePortalFrame(s_Feet + SVector3(1.5f, 0.f, 0.f) + s_Up * c_HalfHeight, SVector3(-1.f, 0.f, 0.f));
            }
            else {
                // Floor portal 2m to the side of 47, top pointing along +X.
                m_Blue.m_Transform = MakeSurfaceFrame(s_Feet + SVector3(-2.f, 0.f, 0.f), s_Up, SVector3(1.f, 0.f, 0.f));
            }

            m_Cooldown = 0.f;
            m_TeleportCount = 0;
            m_SelfTest = SelfTestStage::Arm;
            return;
        }

        case SelfTestStage::Arm: {
            if (!m_Blue.m_HasPrevSide || m_Blue.m_WasInside) {
                SelfTestLog(std::format(
                    "phase {} FAIL arm: hasPrev={} wasInside={}", m_SelfTestPhase, m_Blue.m_HasPrevSide, m_Blue.m_WasInside
                ));
                ClearPortals();
                m_SelfTest = SelfTestStage::Done;
                return;
            }

            SelfTestLog(std::format("phase {} arm: blue side = {:.3f}, outside volume", m_SelfTestPhase, m_Blue.m_PrevSide));

            // Move blue so 47 is now inside its volume, as if he had walked up to it.
            if (m_SelfTestPhase == 1)
                m_Blue.m_Transform = MakePortalFrame(s_Feet + SVector3(0.3f, 0.f, 0.f) + s_Up * c_HalfHeight, SVector3(-1.f, 0.f, 0.f));
            else
                m_Blue.m_Transform = MakeSurfaceFrame(s_Feet, s_Up, SVector3(1.f, 0.f, 0.f));

            m_SelfTest = SelfTestStage::Cross;
            return;
        }

        case SelfTestStage::Cross: {
            if (m_TeleportCount == 0) {
                SelfTestLog(std::format(
                    "phase {} FAIL cross: no teleport fired (blue side {:.3f}, inside {})",
                    m_SelfTestPhase, m_Blue.m_PrevSide, m_Blue.m_WasInside
                ));
                ClearPortals();
                m_SelfTest = SelfTestStage::Done;
                return;
            }

            const float4& s_T = m_LastTeleportTarget.Trans;
            SelfTestLog(std::format(
                "phase {} cross: teleport fired, target=({:.2f}, {:.2f}, {:.2f})", m_SelfTestPhase, s_T.x, s_T.y, s_T.z
            ));

            m_SelfTestFrames = 0;
            m_SelfTest = SelfTestStage::Verify;
            [[fallthrough]];
        }

        case SelfTestStage::Verify: {
            ++m_SelfTestFrames;

            if (m_SelfTestFrames == 1 || m_SelfTestFrames == 10 || m_SelfTestFrames == 60) {
                const float s_Dist = Distance(p_PlayerTrans.Trans, m_LastTeleportTarget.Trans);
                const float s_FromStart = Distance(p_PlayerTrans.Trans, m_SelfTestStart.Trans);
                const SVector3 s_Fwd = ToVec3(p_PlayerTrans.YAxis);
                const SVector3 s_ExpFwd = ToVec3(m_LastTeleportTarget.YAxis);

                SelfTestLog(std::format(
                    "phase {} frame {}: pos=({:.2f}, {:.2f}, {:.2f}) err={:.3f}m fromStart={:.2f}m axisDot={:.3f} upZ={:.3f} teleports={}",
                    m_SelfTestPhase, m_SelfTestFrames, p_PlayerTrans.Trans.x, p_PlayerTrans.Trans.y, p_PlayerTrans.Trans.z,
                    s_Dist, s_FromStart, s_Fwd.Dot(s_ExpFwd), p_PlayerTrans.ZAxis.z, m_TeleportCount
                ));

                if (m_SelfTestFrames == 60) {
                    // Exactly one teleport: the exit must not bounce him straight back.
                    // He now flies out with his momentum, so allow drift from the exit point.
                    const bool s_Pass = s_Dist < 3.f && s_FromStart > 1.5f && m_TeleportCount == 1;
                    SelfTestLog(std::format("phase {} RESULT: {}", m_SelfTestPhase, s_Pass ? "PASS" : "FAIL"));
                    m_SelfTestPasses += s_Pass ? 1 : 0;

                    // Put 47 back and tidy up.
                    p_Hitman->SetObjectToWorldMatrixFromEditor(m_SelfTestStart);
                    ClearPortals();

                    if (m_SelfTestPhase == 1) {
                        m_SelfTestPhase = 2;
                        m_SelfTest = SelfTestStage::Setup;
                    }
                    else {
                        m_SelfTest = SelfTestStage::Shoot;
                    }
                }
            }
            return;
        }

        case SelfTestStage::Shoot: {
            // Shots from 47's head through Portal 2's placement rules (the real ShootPortal, aimed by the test).
            const ZEntityRef s_HitmanRef = SDK()->GetLocalPlayer().m_entityRef;
            const SVector3 s_Head = s_Feet + s_Up * 1.6f;

            // A wall: the nearest upright surface 1.5-15 m away in eight directions.
            SVector3 s_WallDir(1.f, 0.f, 0.f);
            float s_WallDist = 0.f;

            for (int i = 0; i < 8; ++i) {
                const float s_A = i * std::numbers::pi_v<float> * 0.25f;
                const SVector3 s_Dir(std::cos(s_A), std::sin(s_A), 0.f);
                SVector3 s_Hit, s_Normal;

                if (!RayCast(s_Head, s_Head + s_Dir * 15.f, s_HitmanRef, s_Hit, s_Normal) || std::abs(s_Normal.z) > 0.3f)
                    continue;

                const float s_D = (s_Hit - s_Head).Length();

                if (s_D > 1.5f && (s_WallDist == 0.f || s_D < s_WallDist)) {
                    s_WallDist = s_D;
                    s_WallDir = s_Dir;
                }
            }

            const auto s_Shot = [&](const char* p_Name, Portal& p_Portal, const Portal& p_Other, const SVector3& p_Aim) {
                int s_Result = -1;
                const bool s_Ok = ShootPortal(p_Portal, p_Other, s_HitmanRef, &s_Head, &p_Aim, &s_Result);
                const SVector3 s_At = ToVec3(p_Portal.m_Transform.Trans);
                const SVector3 s_N = ToVec3(p_Portal.m_Transform.YAxis);
                SelfTestLog(std::format(
                    "shot {}: result={} at=({:.2f}, {:.2f}, {:.2f}) normal=({:.2f}, {:.2f}, {:.2f})", p_Name, s_Result,
                    s_At.x, s_At.y, s_At.z, s_N.x, s_N.y, s_N.z
                ));
                return std::make_tuple(s_Ok, s_Result, s_At, s_N);
            };

            const auto s_Result = [&](const char* p_Name, bool p_Pass) {
                SelfTestLog(std::format("{} RESULT: {}", p_Name, p_Pass ? "PASS" : "FAIL"));
                m_SelfTestPasses += p_Pass ? 1 : 0;
            };

            ClearPortals();

            if (s_WallDist == 0.f) {
                SelfTestLog("shot wall: no wall within 15 m, skipped (counts as FAIL)");
            }
            else {
                const auto [s_Ok, s_R, s_At, s_N] = s_Shot("wall", m_Blue, m_Orange, s_WallDir);
                // On the wall's plane, facing 47.
                s_Result("shot wall", s_Ok && std::abs(s_N.z) < 0.3f && s_N.Dot(s_WallDir) < -0.7f &&
                    std::abs((s_At - s_Head).Dot(s_WallDir) - s_WallDist) < 0.3f);

                // The same spot for orange: Portal 2 refuses a portal overlapping the other.
                Portal s_Second;
                const auto [s_Ok2, s_R2, s_At2, s_N2] = s_Shot("overlap", s_Second, m_Blue, s_WallDir);
                s_Result("shot overlap refused", !s_Ok2 && s_R2 == 3);
            }

            // The floor 3 m away, the other way from the wall.
            const SVector3 s_FloorAim = s_Feet - s_WallDir * 3.f - s_Head;
            const auto [s_FOk, s_FR, s_FAt, s_FN] = s_Shot("floor", m_Orange, m_Blue, s_FloorAim);
            s_Result("shot floor", s_FOk && s_FN.z > 0.9f && std::abs(s_FAt.z - s_Feet.z) < 0.3f);

            ClearPortals();

            // A cube dropped 1.5 m in front of 47, for the pick-up test.
            SpawnCube(m_SelfTestStart);
            m_SelfTestTimer = 0.f;
            m_SelfTest = SelfTestStage::CubeGrab;
            return;
        }

        case SelfTestStage::CubeGrab: {
            m_SelfTestTimer += p_Dt;

            if (m_SelfTestTimer < 3.f)
                return;

            const ZEntityRef s_HitmanRef = SDK()->GetLocalPlayer().m_entityRef;
            const SVector3 s_Head = s_Feet + s_Up * 1.6f;
            const SVector3 s_Cube = s_Feet + ToVec3(m_SelfTestStart.YAxis).Normalized() * 1.5f + s_Up * 0.3f;
            const SVector3 s_Aim = s_Cube - s_Head;
            TryGrab(s_HitmanRef, &s_Head, &s_Aim);

            const bool s_Pass = static_cast<bool>(m_Held);
            SelfTestLog(std::format("cube grab RESULT: {}", s_Pass ? "PASS" : "FAIL"));
            m_SelfTestPasses += s_Pass ? 1 : 0;

            if (!s_Pass) {
                m_SelfTest = SelfTestStage::Done;
                SelfTestLog(std::format("SUMMARY: {}/9 passed, 47 restored", m_SelfTestPasses));
                return;
            }

            m_SelfTestCube = m_Held;
            m_SelfTestFrames = 0;
            m_SelfTest = SelfTestStage::CubeHold;
            return;
        }

        case SelfTestStage::CubeHold: {
            if (++m_SelfTestFrames < 60)
                return;

            const auto s_Body = GetBody(m_SelfTestCube);
            const float s_Z = s_Body ? s_Body->m_pPhysicsObject->GetTransform().Trans.z : 0.f;
            // Held up in front of him, well off the floor.
            const bool s_Pass = s_Body && m_Held && s_Z > s_Feet.z + 0.6f;
            SelfTestLog(std::format("cube hold: z={:.2f} (feet {:.2f}) RESULT: {}", s_Z, s_Feet.z, s_Pass ? "PASS" : "FAIL"));
            m_SelfTestPasses += s_Pass ? 1 : 0;

            Release();
            m_SelfTestCubeZ = s_Z;
            m_SelfTestFrames = 0;
            m_SelfTest = SelfTestStage::CubeDrop;
            return;
        }

        case SelfTestStage::CubeDrop: {
            if (++m_SelfTestFrames < 90)
                return;

            const auto s_Body = GetBody(m_SelfTestCube);
            const float s_Z = s_Body ? s_Body->m_pPhysicsObject->GetTransform().Trans.z : 0.f;
            // Let go, it falls.
            const bool s_Pass = s_Body && !m_Held && s_Z < m_SelfTestCubeZ - 0.3f;
            SelfTestLog(std::format("cube drop: z={:.2f} (was {:.2f}) RESULT: {}", s_Z, m_SelfTestCubeZ, s_Pass ? "PASS" : "FAIL"));
            m_SelfTestPasses += s_Pass ? 1 : 0;

            m_SelfTest = SelfTestStage::Done;
            SelfTestLog(std::format("SUMMARY: {}/9 passed, 47 restored", m_SelfTestPasses));
            return;
        }

        default:
            return;
    }
}

void PortalGun::SelfTestLog(const std::string& p_Line) {
    Logger::Info("[PortalGun] [SelfTest] {}", p_Line);

    if (!m_ModDir.empty())
        std::ofstream(m_ModDir / "PortalGun_selftest.txt", std::ios::app) << p_Line << "\n";
}

void PortalGun::GivePortalGun() {
    auto s_LocalHitman = SDK()->GetLocalPlayer();

    if (!s_LocalHitman || !s_LocalHitman.m_pInterfaceRef->m_pCharacter)
        return;

    const auto s_Character = s_LocalHitman.m_pInterfaceRef->m_pCharacter.m_pInterfaceRef;
    const auto s_Controllers = &s_Character->m_rSubcontrollerContainer.m_pInterfaceRef->m_aReferencedControllers;

    if (s_Controllers->size() <= 6)
        return;

    const auto s_Inventory = static_cast<ZCharacterSubcontrollerInventory*>((*s_Controllers)[6].m_pInterfaceRef);

    Functions::ZCharacterSubcontrollerInventory_CreateItem->Call(
        s_Inventory, m_GunId, "", {}, ZCharacterSubcontrollerInventory::ECreateItemType::ECIT_ContractItem
    );

    Logger::Info("[PortalGun] Gave 47 the portal gun.");
}

bool PortalGun::IsOurGun(const ZHM5ItemWeapon* p_Weapon) const {
    if (!p_Weapon || !p_Weapon->m_pItemConfigDescriptor)
        return false;

    const auto s_LocalHitman = SDK()->GetLocalPlayer();

    return s_LocalHitman && p_Weapon->m_pOwner == s_LocalHitman.m_entityRef &&
        p_Weapon->m_pItemConfigDescriptor->m_ItemID == m_GunId;
}

DEFINE_PLUGIN_DETOUR(PortalGun, bool, ZHM5ItemWeapon_FireProjectiles, ZHM5ItemWeapon* th, bool bMayStartSound) {
    if (!IsOurGun(th)) {
        // 47's real gun: Delirium's hits push and react as that kind of gun.
        if (th && th->m_pOwner == SDK()->GetLocalPlayer().m_entityRef)
            m_Delirium.Fired(static_cast<int>(th->m_WeaponType));

        return HookResult<bool>(HookAction::Continue());
    }

    // Primary fire shoots blue. No bullet ever leaves the gun.
    m_RequestBlue = true;

    return HookResult<bool>(HookAction::Return(), true);
}

// Every hit on an NPC (each bullet of a burst), for Delirium's body.
DEFINE_PLUGIN_DETOUR(PortalGun, bool, ZActor_YouGotHit, IBaseCharacter* th, const SHitInfo& hitInfo) {
    m_Delirium.Hit(th);
    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(PortalGun, bool, ZHM5ItemWeapon_IsFiring, IFirearm* th) {
    if (IsOurGun(static_cast<ZHM5ItemWeapon*>(th)))
        m_GunSeenFrame = m_Frame;

    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(PortalGun, void, ZHM5ItemWeapon_SetBulletsInMagazine, IFirearm* th, int32_t nBullets) {
    // The portal gun never runs dry.
    if (IsOurGun(static_cast<ZHM5ItemWeapon*>(th))) {
        p_Hook->CallOriginal(th, 25);
        return HookResult<void>(HookAction::Return());
    }

    return HookResult<void>(HookAction::Continue());
}

ZDynamicPhysicsAspect* PortalGun::GetBody(const ZEntityRef& p_Entity) {
    if (!p_Entity)
        return nullptr;

    const auto s_Body = p_Entity.QueryInterface<ZDynamicPhysicsAspect>();

    return s_Body && s_Body->m_pPhysicsObject ? s_Body : nullptr;
}

// The entity that owns the dynamic physics body behind a ray hit, if any.
ZEntityRef PortalGun::FindBody(const ZEntityRef& p_Hit) {
    ZEntityRef s_Entity = p_Hit;

    for (int i = 0; i < 8 && s_Entity; ++i) {
        if (GetBody(s_Entity))
            return s_Entity;

        s_Entity = s_Entity.GetLogicalParent();
    }

    return {};
}

TrackedBody* PortalGun::Track(const ZEntityRef& p_Entity) {
    for (auto& s_Body : m_Bodies) {
        if (s_Body.m_Entity == p_Entity)
            return &s_Body;
    }

    if (m_Bodies.size() >= c_MaxTrackedBodies)
        m_Bodies.erase(m_Bodies.begin());

    m_Bodies.push_back({ .m_Entity = p_Entity });
    return &m_Bodies.back();
}

// Puts a body at a pose with a velocity. There's no velocity setter on Hitman's physics objects, so the
// body is placed one frame behind and driven forward with MoveTransform, which moves it like a
// kinematic target and leaves it with that speed.
void PortalGun::SetBodyMotion(ZDynamicPhysicsAspect* p_Body, const SMatrix& p_Pose, const SVector3& p_Vel, float p_Dt) const {
    SMatrix s_Behind = p_Pose;
    s_Behind.Trans = float4(ToVec3(p_Pose.Trans) - p_Vel * p_Dt, 1.f);

    p_Body->m_pPhysicsObject->SetTransform(s_Behind);
    p_Body->m_pPhysicsObject->MoveTransform(p_Pose);
}

void PortalGun::TryGrab(const ZEntityRef& p_Hitman, const SVector3* p_Origin, const SVector3* p_Aim) {
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    if (!s_Camera && !p_Origin)
        return;

    const SVector3 s_Origin = p_Origin ? *p_Origin : ToVec3(s_Camera->GetObjectToWorldMatrix().Trans);
    const SVector3 s_Aim = p_Aim ? p_Aim->Normalized() : (ToVec3(s_Camera->GetObjectToWorldMatrix().CameraBackward) * -1.f).Normalized();

    // Portal reaches player_use_radius-style: the camera sits behind 47 in third person, so the reach
    // is measured from 47's head rather than from the camera.
    float s_Skip = 0.f;

    if (const auto s_Spatial = p_Hitman.QueryInterface<ZSpatialEntity>(); s_Spatial && !p_Origin) {
        const SVector3 s_Head = ToVec3(s_Spatial->GetObjectToWorldMatrix().Trans) + SVector3(0.f, 0.f, 1.6f);
        s_Skip = std::max(0.f, (s_Head - s_Origin).Dot(s_Aim));
    }

    ZRayQueryInput s_Input {
        .m_vFrom = float4(s_Origin + s_Aim * s_Skip, 1.f),
        .m_vTo = float4(s_Origin + s_Aim * (s_Skip + c_UseRange), 1.f),
    };

    ZRayQueryOutput s_Output {};

    if (!(*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Output) || s_Output.m_nT < 0.f || s_Output.m_nT > 1.f) {
        Logger::Info("[PortalGun] Grab: nothing in reach.");
        return;
    }

    ZEntityRef s_Hit = s_Output.m_BlockingEntity;

    if (!s_Hit && s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef)
        s_Hit = s_Output.m_pBlockingSpatialEntity.m_entityRef;

    const ZEntityRef s_BodyEntity = FindBody(s_Hit);

    if (!s_BodyEntity || s_BodyEntity == p_Hitman || s_BodyEntity.IsAnyParent(p_Hitman)) {
        Logger::Info("[PortalGun] Grab: the thing in reach isn't a loose physics object.");
        return;
    }

    const auto s_Body = GetBody(s_BodyEntity);
    const SMatrix s_Pose = s_Body->m_pPhysicsObject->GetTransform();

    // Portal keeps the object's orientation fixed relative to the player's view yaw.
    SMatrix s_Rot = s_Pose;
    s_Rot.Trans = float4(0.f, 0.f, 0.f, 1.f);
    SMatrix s_Yaw = MakeYaw(s_Aim);

    m_HeldRelRot = s_Rot * s_Yaw.Inverse();
    m_Held = s_BodyEntity;
    m_HeldVel = SVector3(0.f, 0.f, 0.f);
    m_HeldHasCommand = false;
    m_HoldStuckFrames = 0;

    Track(s_BodyEntity);

    Logger::Info(
        "[PortalGun] Picked up an object at ({:.2f}, {:.2f}, {:.2f}), kinematic={}.",
        s_Pose.Trans.x, s_Pose.Trans.y, s_Pose.Trans.z, s_Body->m_bKinematic
    );
}

void PortalGun::Release() {
    if (!m_Held)
        return;

    if (const auto s_Body = GetBody(m_Held)) {
        // Portal 2 drops the object with whatever speed it was being carried at.
        SetBodyMotion(s_Body, s_Body->m_pPhysicsObject->GetTransform(), m_HeldVel, m_Dt);

        if (TrackedBody* s_Tracked = Track(m_Held)) {
            s_Tracked->m_ReleaseVel = m_HeldVel;
            s_Tracked->m_LogFrames = 30;
            s_Tracked->m_HasPrev = false;
        }
    }

    Logger::Info("[PortalGun] Dropped the object at {:.2f} m/s.", m_HeldVel.Length());
    m_Held = {};
}

void PortalGun::UpdatePickup(const ZEntityRef& p_Hitman, float p_Dt) {
    if (Functions::ZInputAction_Digital->Call(&m_UseAction, -1)) {
        if (m_Held)
            Release();
        else
            TryGrab(p_Hitman);
    }

    if (!m_Held)
        return;

    const auto s_Body = GetBody(m_Held);
    const auto s_Camera = Functions::GetCurrentCamera->Call();
    const auto s_Spatial = p_Hitman.QueryInterface<ZSpatialEntity>();

    // Picked up into the inventory, destroyed, or the camera's gone: let go.
    if (!s_Body || !s_Camera || !s_Spatial) {
        m_Held = {};
        return;
    }

    const SMatrix s_CameraTrans = s_Camera->GetObjectToWorldMatrix();
    const SVector3 s_Aim = (ToVec3(s_CameraTrans.CameraBackward) * -1.f).Normalized();
    const SVector3 s_Head = ToVec3(s_Spatial->GetObjectToWorldMatrix().Trans) + SVector3(0.f, 0.f, 1.6f);

    const SMatrix s_Pose = s_Body->m_pPhysicsObject->GetTransform();
    const SVector3 s_Pos = ToVec3(s_Pose.Trans);

    // Check last frame's MoveTransform actually moved it.
    if (m_HeldHasCommand && !m_HoldUseSet) {
        if ((s_Pos - m_HeldCommanded).Length() > 0.25f && (m_HeldVel.Length() * p_Dt) > 0.01f) {
            if (++m_HoldStuckFrames > 10) {
                m_HoldUseSet = true;
                Logger::Warn("[PortalGun] MoveTransform doesn't move held objects; holding with SetTransform instead.");
            }
        }
        else {
            m_HoldStuckFrames = 0;
        }
    }

    // Too far from where it should be (snagged on something): Portal drops it.
    const SVector3 s_Target = s_Head + s_Aim * c_HoldDistance + SVector3(0.f, 0.f, c_HoldOffsetUp);

    if ((s_Pos - s_Target).Length() > c_HoldBreakDistance) {
        Logger::Info("[PortalGun] The object snagged, so it was dropped.");
        Release();
        return;
    }

    SVector3 s_Vel = (s_Target - s_Pos) * (1.f / std::max(p_Dt, 0.001f));

    if (s_Vel.Length() > c_HoldMaxSpeed)
        s_Vel = s_Vel.Normalized() * c_HoldMaxSpeed;

    SMatrix s_New = m_HeldRelRot * MakeYaw(s_Aim);
    s_New.Trans = float4(s_Pos + s_Vel * p_Dt, 1.f);

    if (m_HoldUseSet)
        s_Body->m_pPhysicsObject->SetTransform(s_New);
    else
        s_Body->m_pPhysicsObject->MoveTransform(s_New);

    m_HeldVel = s_Vel;
    m_HeldCommanded = ToVec3(s_New.Trans);
    m_HeldHasCommand = true;
}

void PortalGun::UpdateBodies(float p_Dt) {
    for (auto it = m_Bodies.begin(); it != m_Bodies.end();) {
        const auto s_Body = GetBody(it->m_Entity);

        if (!s_Body) {
            it = m_Bodies.erase(it);
            continue;
        }

        TrackedBody& s_Tracked = *it++;
        const SMatrix s_Pose = s_Body->m_pPhysicsObject->GetTransform();
        const SVector3 s_Pos = ToVec3(s_Pose.Trans);

        if (s_Tracked.m_HasPrev)
            s_Tracked.m_Vel = (s_Pos - s_Tracked.m_PrevPos) * (1.f / p_Dt);

        s_Tracked.m_PrevPos = s_Pos;
        const bool s_HadPrev = s_Tracked.m_HasPrev;
        s_Tracked.m_HasPrev = true;

        if (s_Tracked.m_LogFrames > 0 && s_HadPrev) {
            --s_Tracked.m_LogFrames;

            if (s_Tracked.m_LogFrames % 10 == 0) {
                Logger::Info(
                    "[PortalGun] After release: speed {:.2f} m/s (dropped at {:.2f}).", s_Tracked.m_Vel.Length(),
                    s_Tracked.m_ReleaseVel.Length()
                );
            }
        }

        if (s_Tracked.m_Cooldown > 0.f) {
            s_Tracked.m_Cooldown -= p_Dt;
            continue;
        }

        // The held object is carried, not flying; it goes through once it's let go.
        if (!s_HadPrev || s_Tracked.m_Entity == m_Held || !m_Blue.m_Placed || !m_Orange.m_Placed)
            continue;

        for (auto [s_In, s_Out] : { std::pair { &m_Blue, &m_Orange }, std::pair { &m_Orange, &m_Blue } }) {
            const SMatrix& s_InTrans = s_In->m_Transform;
            const SVector3 s_Local = ToLocal(s_InTrans, s_Pos);
            const SVector3 s_Normal = ToVec3(s_InTrans.YAxis);
            const float s_Into = -s_Tracked.m_Vel.Dot(s_Normal);

            if (s_Local.y > c_BodyTriggerDepth || s_Local.y < -c_TriggerBehind || !InsideOval(s_Local))
                continue;

            // Moving into the portal, or resting on a floor portal (there's no floor there).
            SVector3 s_Vel = s_Tracked.m_Vel;
            const bool s_Resting = s_Tracked.m_Vel.Length() < 0.3f && s_InTrans.YAxis.z > c_FloorNormalZ;

            if (s_Into < 0.1f && !s_Resting)
                continue;

            if (s_Resting)
                s_Vel = s_Normal * -1.f;

            SMatrix s_Exit = MapThroughPortal(s_Pose, s_InTrans, s_Out->m_Transform);
            const SVector3 s_OutNormal = ToVec3(s_Out->m_Transform.YAxis);
            const SVector3 s_ExitLocal = ToLocal(s_Out->m_Transform, ToVec3(s_Exit.Trans));

            // Come out clear of the exit surface.
            s_Exit.Trans = float4(ToVec3(s_Exit.Trans) + s_OutNormal * std::max(0.f, c_BodyExitClear - s_ExitLocal.y), 1.f);

            SVector3 s_OutVel = MapDirection(s_Vel, s_InTrans, s_Out->m_Transform);
            ClampVelocity(s_OutVel);

            SetBodyMotion(s_Body, s_Exit, s_OutVel, p_Dt);

            s_Tracked.m_Cooldown = c_TeleportCooldown;
            s_Tracked.m_HasPrev = false;
            s_Tracked.m_ReleaseVel = s_OutVel;
            s_Tracked.m_LogFrames = 30;

            Logger::Info("[PortalGun] An object went through a portal at {:.2f} m/s.", s_OutVel.Length());
            break;
        }
    }
}

void PortalGun::SpawnCube(const SMatrix& p_PlayerTrans) {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene)
        return;

    TResourcePtr<ZTemplateEntityFactory> s_SpawnerRes, s_KeyRes;
    Globals::ResourceManager->GetResourcePtr(s_SpawnerRes, ResId<"[modules:/zitemspawner.class].pc_entitytype">, 0);
    Globals::ResourceManager->GetResourcePtr(s_KeyRes, ResId<"[modules:/zitemrepositorykeyentity.class].pc_entitytype">, 0);

    if (!s_SpawnerRes || !s_KeyRes) {
        Logger::Warn("[PortalGun] Can't spawn a cube: the item spawner isn't loaded.");
        return;
    }

    ZEntityRef s_Spawner, s_Key;
    SExternalReferences s_ExternalRefs;

    Functions::ZEntityManager_NewEntity->Call(Globals::EntityManager, s_Spawner, "", s_SpawnerRes, s_Scene.m_entityRef, s_ExternalRefs, -1);
    Functions::ZEntityManager_NewEntity->Call(Globals::EntityManager, s_Key, "", s_KeyRes, s_Scene.m_entityRef, s_ExternalRefs, -1);

    if (!s_Spawner || !s_Key) {
        Logger::Warn("[PortalGun] Can't spawn a cube: entity creation failed.");
        return;
    }

    const auto s_ItemSpawner = s_Spawner.QueryInterface<ZItemSpawner>();
    const auto s_KeyEntity = s_Key.QueryInterface<ZItemRepositoryKeyEntity>();

    if (!s_ItemSpawner || !s_KeyEntity)
        return;

    s_ItemSpawner->m_ePhysicsMode = ZItemSpawner::EPhysicsMode::EPM_DYNAMIC;
    s_ItemSpawner->m_rMainItemKey.m_entityRef = s_Key;
    s_ItemSpawner->m_rMainItemKey.m_pInterfaceRef = s_KeyEntity;
    s_KeyEntity->m_RepositoryId = m_CubeId;
    s_ItemSpawner->m_bUsePlacementAttach = false;

    // A little in front of 47, dropped from waist height, like Portal's cube dropper.
    SMatrix s_Trans = p_PlayerTrans;
    s_Trans.Trans = float4(ToVec3(p_PlayerTrans.Trans) + ToVec3(p_PlayerTrans.YAxis).Normalized() * 1.5f + SVector3(0.f, 0.f, 1.f), 1.f);
    s_ItemSpawner->SetObjectToWorldMatrixFromEditor(s_Trans);

    Functions::ZItemSpawner_RequestContentLoad->Call(s_ItemSpawner);
    Logger::Info("[PortalGun] Spawned a cube.");
}

bool PortalGun::GunInHand() const {
    return m_GunSeenFrame != 0 && m_GunSeenFrame + 3 >= m_Frame;
}

bool PortalGun::OnGameAction(const char* p_Name, bool p_Active, bool p_Analog) {
    if (!p_Name)
        return false;

    const std::string_view s_Name(p_Name);

    // Our own bindings.
    if (s_Name.starts_with("PortalGun") || s_Name.starts_with("PlaceBlue") || s_Name.starts_with("PlaceOrange"))
        return false;

    std::string s_Lower(s_Name);
    std::ranges::transform(s_Lower, s_Lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const bool s_IsAim = s_Lower.find("aim") != std::string::npos && s_Lower.find("assist") == std::string::npos;
    // Down as a ragdoll: no walking him off out of his body (it shook).
    const bool s_Move = s_Lower.find("move") != std::string::npos;
    const bool s_Block = (s_IsAim && GunInHand()) || (s_Move && m_Delirium.HitmanDown());

    if (s_Block && p_Active)
        m_AimSeenFrame = m_Frame;

    // Record every game action seen active, once, so the real aim action names can be checked.
    if (p_Active && !m_ModDir.empty() && m_LoggedActions.insert(std::string(s_Name)).second) {
        std::ofstream(m_ModDir / "PortalGun_actions.txt", std::ios::app) << std::format(
            "{} {} gun={} {}\n", s_Name, p_Analog ? "analog" : "digital", GunInHand() ? 1 : 0,
            s_Block ? "BLOCKED" : ""
        );
    }

    return s_Block;
}

DEFINE_PLUGIN_DETOUR(PortalGun, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId) {
    const char* s_Name = th ? th->m_szName : nullptr;

    if (s_Name && (std::string_view(s_Name).starts_with("PortalGun") || std::string_view(s_Name).starts_with("Place")))
        return HookResult<bool>(HookAction::Continue());

    const bool s_Result = p_Hook->CallOriginal(th, controllerId);

    if (OnGameAction(s_Name, s_Result, false))
        return HookResult<bool>(HookAction::Return(), false);

    return HookResult<bool>(HookAction::Return(), s_Result);
}

DEFINE_PLUGIN_DETOUR(PortalGun, float, ZInputAction_Analog, ZInputAction* th, int32_t controllerId) {
    const char* s_Name = th ? th->m_szName : nullptr;
    const float s_Result = p_Hook->CallOriginal(th, controllerId);

    if (OnGameAction(s_Name, std::abs(s_Result) > 0.01f, true))
        return HookResult<float>(HookAction::Return(), 0.f);

    return HookResult<float>(HookAction::Return(), s_Result);
}

void PortalGun::ClearPortals() {
    m_Held = {};
    m_Bodies.clear();
    m_Blue = {};
    m_Orange = {};
    m_Cooldown = 0.f;
    m_Flying = false;
    m_HasPrevFeet = false;
}

DEFINE_PLUGIN_DETOUR(PortalGun, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene) {
    DeleteViews();
    m_Delirium.Clear();
    ClearPortals();
    m_GaveThisScene = false;
    m_SceneTime = 0.f;
    m_GunSeenFrame = 0;
    m_AimSeenFrame = 0;
    m_AimWasHeld = false;

    return HookResult<void>(HookAction::Continue());
}

void PortalGun::SpawnViews() {
    const auto s_Start = std::chrono::steady_clock::now();
    struct Done {
        std::chrono::steady_clock::time_point t;
        ~Done() {
            Logger::Info("[Hitch] Portal cameras spawned in {:.1f} ms.",
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t).count());
        }
    } s_Done {s_Start};
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene || !(*Globals::ApplicationEngineWin32)->m_bSceneLoaded)
        return;

    // Half the screen's resolution: same shape as the screen, so screen positions are texture positions.
    std::string s_Json = c_PortalViewJson;
    const auto s_Put = [&](const char* p_Key, uint32_t p_Value) {
        s_Json.replace(s_Json.find(p_Key), 3, std::to_string(p_Value));
    };
    s_Put("@W@", std::max(64u, static_cast<uint32_t>(m_ScreenW * 0.5f)));
    s_Put("@H@", std::max(64u, static_cast<uint32_t>(m_ScreenH * 0.5f)));

    if (!m_ViewFactory && !SDK()->LoadQnEntity(ZString(s_Json), m_ViewBlueprint, m_ViewFactory)) {
        Logger::Error("[PortalGun] See-through portals: couldn't load the camera entity.");
        m_ViewsFailed = true;
        return;
    }

    std::scoped_lock s_Lock(m_ViewMutex);

    for (int i = 0; i < 2; ++i) {
        PortalView& s_View = m_Views[i];
        SExternalReferences s_ExternalRefs;
        Functions::ZEntityManager_NewEntity->Call(
            Globals::EntityManager, s_View.m_Root, i == 0 ? ZString("PortalViewBlue") : ZString("PortalViewOrange"), m_ViewFactory,
            s_Scene.m_entityRef, s_ExternalRefs, -1
        );

        const auto s_Bp = m_ViewBlueprint.GetResource();

        if (!s_View.m_Root || !s_Bp) {
            Logger::Error("[PortalGun] See-through portals: couldn't spawn the camera.");
            m_ViewsFailed = true;
            return;
        }

        if (const int s_Idx = s_Bp->GetSubEntityIndex(0xfeedb6fc4f5626ea); s_Idx != -1)
            if (const auto s_Ent = s_Bp->GetSubEntity(s_View.m_Root.m_pObj, s_Idx))
                s_View.m_Camera = TEntityRef<ZCameraEntity>(ZEntityRef(s_Ent));

        if (const int s_Idx = s_Bp->GetSubEntityIndex(0xfeedbf5a41eb9c48); s_Idx != -1)
            if (const auto s_Ent = s_Bp->GetSubEntity(s_View.m_Root.m_pObj, s_Idx))
                s_View.m_Texture = TEntityRef<ZRenderDestinationTextureEntity>(ZEntityRef(s_Ent));

        if (const int s_Idx = s_Bp->GetSubEntityIndex(0xfeed678791f1b3e1); s_Idx != -1)
            if (const auto s_Ent = s_Bp->GetSubEntity(s_View.m_Root.m_pObj, s_Idx))
                s_View.m_Tablet = ZEntityRef(s_Ent).QueryInterface<ZSpatialEntity>();

        if (!s_View.m_Camera || !s_View.m_Texture) {
            Logger::Error("[PortalGun] See-through portals: the camera or its texture is missing.");
            m_ViewsFailed = true;
            return;
        }
    }

    Logger::Info("[PortalGun] See-through portals: cameras up ({}x{}).", m_ScreenW, m_ScreenH);
}

void PortalGun::DeleteViews() {
    std::scoped_lock s_Lock(m_ViewMutex);

    for (PortalView& s_View : m_Views) {
        if (s_View.m_Root)
            Functions::ZEntityManager_DeleteEntity->Call(Globals::EntityManager, s_View.m_Root, {});

        s_View = {};
    }
}

void PortalGun::UpdateViews(const ZEntityRef& p_Hitman) {
    const bool s_Both = m_Blue.m_Placed && m_Orange.m_Placed;

    if (m_SeeThrough && !m_Views[0].m_Root && !m_ViewsFailed)
        SpawnViews();

    std::scoped_lock s_Lock(m_ViewMutex);
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    for (int i = 0; i < 2; ++i) {
        PortalView& s_View = m_Views[i];

        if (!s_View.m_Camera || !s_View.m_Texture)
            continue;

        s_View.m_Show = false;
        const std::string s_WasWhy = s_View.m_Why;
        struct LogWhy {
            PortalView& v; const std::string& was; int i;
            ~LogWhy() {
                if (v.m_Why != was)
                    Logger::Info("[PortalGun] See-through {}: {}.", i ? "orange" : "blue", v.m_Why);
            }
        } s_LogWhy {s_View, s_WasWhy, i};
        s_View.m_Why = "off";
        const bool s_On = s_Both && m_SeeThrough && s_Camera;
        s_View.m_Camera.m_entityRef.SetProperty("m_bVisible", s_On);
        s_View.m_Texture.m_entityRef.SetProperty("m_bVisible", s_On);

        if (!s_On)
            continue;

        const Portal& s_In = i == 0 ? m_Blue : m_Orange;

        // The texture's tablet just inside the wall behind its portal: in view whenever the portal is (Glacier only
        // renders a texture something on screen shows; at z -999 it was black), hidden by the wall, out of 47's way.
        if (s_View.m_Tablet) {
            SMatrix s_M = s_In.m_Transform;
            s_M.Trans = s_M.Trans - s_M.YAxis * 0.25f;
            s_View.m_Tablet->SetObjectToWorldMatrixFromEditor(s_M);
        }

        const Portal& s_Out = i == 0 ? m_Orange : m_Blue;
        const SMatrix s_Cam = s_Camera->GetObjectToWorldMatrix();
        const SVector3 s_Eye = ToVec3(s_Cam.Trans);

        // Only from in front of it, and only when nothing is in the way.
        const SVector3 s_Centre = ToVec3(s_In.m_Transform.Trans);
        const SVector3 s_Normal = ToVec3(s_In.m_Transform.YAxis).Normalized();

        if ((s_Eye - s_Centre).Dot(s_Normal) <= 0.02f) {
            s_View.m_Why = "behind it";
            continue;
        }

        SVector3 s_Hit, s_HitNormal;
        const SVector3 s_Near = s_Centre + s_Normal * 0.05f;

        // From the portal out to the eye: what's within 1.5 m of the eye (47 himself, over his shoulder) doesn't count.
        const float s_Far = (s_Eye - s_Near).Length();

        // ponytail: off; something invisible ~2.3 m in front of portals blocked it every time. Shown through walls now.
        if (false && RayCast(s_Near, s_Eye, p_Hitman, s_Hit, s_HitNormal) && (s_Hit - s_Near).Length() < s_Far - 1.5f) {
            s_View.m_Why = std::format("blocked {:.2f} m from it", (s_Hit - s_Near).Length());
            continue;
        }

        // The player's camera put through the portal, looking out of the other one.
        const SMatrix s_Virtual = MapThroughPortal(s_Cam, s_In.m_Transform, s_Out.m_Transform);
        s_View.m_Camera.m_pInterfaceRef->SetObjectToWorldMatrixFromEditor(s_Virtual);

        // Clip away the wall behind the exit portal: start the view just short of it.
        // ponytail: flat near plane, an oblique projection would clip exactly at the exit's surface.
        const SVector3 s_Look = (ToVec3(s_Virtual.CameraBackward) * -1.f).Normalized();
        const float s_Depth = (ToVec3(s_Out.m_Transform.Trans) - ToVec3(s_Virtual.Trans)).Dot(s_Look);
        s_View.m_Camera.m_entityRef.SetProperty("m_fNearZ", std::clamp(s_Depth - 0.1f, 0.05f, 100.f));
        s_View.m_Camera.m_entityRef.SetProperty("m_fFovYDeg", s_Camera->GetFovYDeg());
        s_View.m_Camera.m_entityRef.SetProperty("m_fAspectWByH", m_ScreenW / std::max(1.f, m_ScreenH));
        s_View.m_Show = true;
        s_View.m_Why = "shown";
    }
}

void PortalGun::DrawViews() {
    std::scoped_lock s_Lock(m_ViewMutex);

    // What each portal camera sees, plainly (is there a picture at all?).
    if (m_ShowCameras) {
        ImGui::SetNextWindowSize(ImVec2(340.f, 420.f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Portal cameras", &m_ShowCameras);

        for (int i = 0; i < 2; ++i) {
            const PortalView& v = m_Views[i];
            ImGui::Text("%s: %s", i ? "Orange" : "Blue", v.m_Why.c_str());

            if (v.m_Texture && v.m_Texture.m_pInterfaceRef)
                if (const auto s_Rt = reinterpret_cast<ZRenderDestination*>(v.m_Texture.m_pInterfaceRef->GetRenderDestination()))
                    SDK()->ImGuiGameRenderTarget(s_Rt, ImVec2(320.f, 180.f));
        }

        ImGui::End();
    }

    if (!m_Views[0].m_Show && !m_Views[1].m_Show)
        return;

    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(m_ScreenW, m_ScreenH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize, 0.f);
    ImGui::Begin(
        "##PortalViews", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav
    );

    // The oval as horizontal strips; each strip is the texture's quad with its corners moved onto the strip and
    // its texture positions set to where they are on screen (the texture is the screen as seen through the portal).
    constexpr int c_Strips = 24;

    for (int i = 0; i < 2; ++i) {
        const PortalView& s_View = m_Views[i];

        if (!s_View.m_Show)
            continue;

        const auto s_Rt = reinterpret_cast<ZRenderDestination*>(s_View.m_Texture.m_pInterfaceRef->GetRenderDestination());

        if (!s_Rt) {
            if (s_View.m_Why != "no render target")
                Logger::Warn("[PortalGun] See-through {}: no render target.", i ? "orange" : "blue");

            continue;
        }

        const SMatrix& s_M = (i == 0 ? m_Blue : m_Orange).m_Transform;
        const auto s_Screen = [&](float p_X, float p_Z, ImVec2& p_Out) {
            SVector2 s_P;

            if (!SDK()->WorldToScreen(ToVec3(s_M.WVectorTransform(float4(p_X, 0.f, p_Z, 1.f))), s_P))
                return false;

            p_Out = ImVec2(s_P.x, s_P.y);
            return true;
        };

        for (int k = 0; k < c_Strips; ++k) {
            const float s_Z0 = c_HalfHeight * (-1.f + 2.f * k / c_Strips);
            const float s_Z1 = c_HalfHeight * (-1.f + 2.f * (k + 1) / c_Strips);
            const float s_W0 = HalfWidthAt(s_Z0);
            const float s_W1 = HalfWidthAt(s_Z1);
            ImVec2 s_P[4];

            if (!s_Screen(-s_W0, s_Z0, s_P[0]) || !s_Screen(s_W0, s_Z0, s_P[1]) ||
                !s_Screen(s_W1, s_Z1, s_P[2]) || !s_Screen(-s_W1, s_Z1, s_P[3]))
                continue;

            ImGui::SetCursorScreenPos(ImVec2(0.f, 0.f));
            SDK()->ImGuiGameRenderTarget(s_Rt, ImVec2(1.f, 1.f));

            ImDrawList* s_List = ImGui::GetWindowDrawList();

            if (s_List->VtxBuffer.Size < 4)
                continue;

            ImDrawVert* s_Vtx = s_List->VtxBuffer.Data + s_List->VtxBuffer.Size - 4;

            for (int v = 0; v < 4; ++v) {
                s_Vtx[v].pos = s_P[v];
                s_Vtx[v].uv = ImVec2(s_P[v].x / m_ScreenW, s_P[v].y / m_ScreenH);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}

DEFINE_ZHM_PLUGIN(PortalGun);

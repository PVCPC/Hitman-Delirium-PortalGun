#include "PortalGun.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <format>
#include <fstream>

#include <Windows.h>

#include "Hooks.h"
#include "Logging.h"
#include "IRenderer.h"
#include "IconsMaterialDesign.h"

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZCameraEntity.h>
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

    bool InsideOval(const SVector3& p_Local, float p_Margin = 0.f) {
        const float s_Ex = p_Local.x / (c_HalfWidth + p_Margin);
        const float s_Ez = p_Local.z / (c_HalfHeight + p_Margin);
        return s_Ex * s_Ex + s_Ez * s_Ez <= 1.f;
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

    dp_set_half(c_HalfWidth, c_HalfHeight);
    FindGames();

    if (ZInputActionManager::AddBindings(s_Binds))
        Logger::Info("[PortalGun] Bindings added (Ctrl+B blue, Ctrl+O orange).");
    else
        Logger::Warn("[PortalGun] Failed to add bindings.");
}

void PortalGun::Init() {
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &PortalGun::OnClearScene);
    Hooks::ZHM5ItemWeapon_FireProjectiles->AddDetour(this, &PortalGun::ZHM5ItemWeapon_FireProjectiles);
    Hooks::ZHM5ItemWeapon_IsFiring->AddDetour(this, &PortalGun::ZHM5ItemWeapon_IsFiring);
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
}

void PortalGun::OnDrawMenu() {
    // Each game: found, or a box to paste its folder into.
    const auto s_GameRow = [&](int p_Game, const char* p_Name, const char* p_Hint, std::string& p_Path, char* p_Typed) {
        if (!p_Path.empty()) {
            ImGui::Text("%s: %s", p_Name, p_Path.c_str());
            return;
        }

        ImGui::Text("%s not found. Paste its folder (%s):", p_Name, p_Hint);
        ImGui::PushID(p_Game);
        ImGui::InputText("##path", p_Typed, 512);
        ImGui::SameLine();

        if (ImGui::Button("USE")) {
            if (dp_set_game(p_Game, m_ModDir.string().c_str(), p_Typed))
                FindGames();
            else
                Logger::Warn("[PortalGun] That folder isn't {}.", p_Name);
        }

        ImGui::PopID();
    };

    s_GameRow(0, "Portal 2", "the one with portal2\\pak01_dir.vpk", m_Portal2Path, m_Portal2Typed);
    s_GameRow(1, "GTA IV", "the one with GTAIV.exe", m_Gta4Path, m_Gta4Typed);

    if (ImGui::Button(ICON_MD_ADD " PORTAL: GIVE PORTAL GUN"))
        m_RequestGive = true;

    if (ImGui::Button(ICON_MD_CIRCLE " PORTAL: BLUE"))
        m_RequestBlue = true;

    if (ImGui::Button(ICON_MD_CIRCLE " PORTAL: ORANGE"))
        m_RequestOrange = true;

    if (ImGui::Button(ICON_MD_CIRCLE " PORTAL: BLUE IN FRONT"))
        m_RequestBlueHere = true;

    if (ImGui::Button(ICON_MD_CIRCLE " PORTAL: ORANGE IN FRONT"))
        m_RequestOrangeHere = true;

    if (ImGui::Button(ICON_MD_ADD " PORTAL: SPAWN CUBE"))
        m_RequestCube = true;

    if (ImGui::Button(ICON_MD_CLEAR " PORTAL: CLEAR"))
        m_RequestClear = true;
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

    if (s_AimHeld && !m_AimWasHeld && s_GunInHand)
        m_RequestOrange = true;

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

    if (!m_Blue.m_Placed || !m_Orange.m_Placed)
        return;

    if (!TryTeleport(m_Blue, m_Orange, s_HitmanSpatial, s_Trans))
        TryTeleport(m_Orange, m_Blue, s_HitmanSpatial, s_Trans);
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

bool PortalGun::ShootPortal(Portal& p_Portal, const Portal& p_Other, const ZEntityRef& p_Hitman) {
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    if (!s_Camera)
        return false;

    // Camera looks down its -Z axis.
    const SMatrix s_CameraTrans = s_Camera->GetObjectToWorldMatrix();
    SVector3 s_Origin = ToVec3(s_CameraTrans.Trans);
    const SVector3 s_Aim = (ToVec3(s_CameraTrans.CameraBackward) * -1.f).Normalized();

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

    const int s_Result = dp_shoot(
        &DpRayCast, &s_Ignore, &s_From, &s_Dir, c_MaxRange, p_Other.m_Placed ? &s_Other : nullptr,
        s_BothOpen ? s_Linked : nullptr, &s_Out, &s_ShotHit
    );

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
    const bool s_Inside = InsideOval(s_Local3) && s_Local.y < c_TriggerDepth && s_Local.y > -c_TriggerBehind;

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
    DrawPortal(p_Renderer, m_Blue, c_BlueColor);
    DrawPortal(p_Renderer, m_Orange, c_OrangeColor);
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
                        m_SelfTest = SelfTestStage::Done;
                        SelfTestLog(std::format("SUMMARY: {}/3 passed, 47 restored", m_SelfTestPasses));
                    }
                }
            }
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
    if (!IsOurGun(th))
        return HookResult<bool>(HookAction::Continue());

    // Primary fire shoots blue. No bullet ever leaves the gun.
    m_RequestBlue = true;

    return HookResult<bool>(HookAction::Return(), true);
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

void PortalGun::TryGrab(const ZEntityRef& p_Hitman) {
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    if (!s_Camera)
        return;

    const SMatrix s_CameraTrans = s_Camera->GetObjectToWorldMatrix();
    const SVector3 s_Origin = ToVec3(s_CameraTrans.Trans);
    const SVector3 s_Aim = (ToVec3(s_CameraTrans.CameraBackward) * -1.f).Normalized();

    // Portal reaches player_use_radius-style: the camera sits behind 47 in third person, so the reach
    // is measured from 47's head rather than from the camera.
    float s_Skip = 0.f;

    if (const auto s_Spatial = p_Hitman.QueryInterface<ZSpatialEntity>()) {
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
    const bool s_Block = s_IsAim && GunInHand();

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
    ClearPortals();
    m_GaveThisScene = false;
    m_SceneTime = 0.f;
    m_GunSeenFrame = 0;
    m_AimSeenFrame = 0;
    m_AimWasHeld = false;

    return HookResult<void>(HookAction::Continue());
}

DEFINE_ZHM_PLUGIN(PortalGun);

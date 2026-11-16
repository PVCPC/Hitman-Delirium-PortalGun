#pragma once

#include "IPluginInterface.h"
#include "Glacier/ZEntity.h"
#include "Glacier/ZInput.h"
#include "Glacier/ZMath.h"
#include "Glacier/ZSpatialEntity.h"
#include "Glacier/ZPrimitives.h"

#include "delirium_portal.h"
#include "Delirium.h"

#include <Glacier/ZCameraEntity.h>
#include <Glacier/ZGeomEntity.h>
#include <Glacier/ZResource.h>
#include <Glacier/EntityFactory.h>
#include <mutex>

#include <atomic>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

class ZDynamicPhysicsAspect;

// Phase 3: portals are shot onto surfaces with a camera raycast and teleport 47 between them.
//
// Portal frame convention (Glacier row-major, v_world = v_local * M):
//   XAxis = portal right, YAxis = portal normal (points out of the surface), ZAxis = portal up.
//   The origin is the centre of the portal oval.
struct Portal {
    bool m_Placed = false;
    SMatrix m_Transform;

    // Signed distance of 47's probe point from the portal plane on the previous frame.
    float m_PrevSide = 0.f;
    bool m_HasPrevSide = false;

    // Whether 47's probe point was inside the trigger volume on the previous frame.
    bool m_WasInside = false;

    // 47's probe point in this portal's frame on the previous frame.
    SVector3 m_PrevLocal;
};

// A physics object the portal gun has handled. Its motion is tracked so it can pass through portals.
// What one portal shows: a hidden camera looking out of the other portal, rendering to a texture that's drawn
// in this portal's oval on screen (screen-space, so the camera matches the player's).
struct PortalView {
    ZEntityRef m_Root;
    TEntityRef<ZCameraEntity> m_Camera;
    TEntityRef<ZRenderDestinationTextureEntity> m_Texture;
    ZSpatialEntity* m_Tablet = nullptr; // the prop showing the texture: Glacier only renders a texture someone can see
    // Whether to draw it this frame (both portals up, 47's camera in front of it and nothing in the way).
    bool m_Show = false;
    std::string m_Why; // why it is or isn't shown (logged when it changes)
};

struct TrackedBody {
    ZEntityRef m_Entity;
    SVector3 m_PrevPos;
    bool m_HasPrev = false;
    SVector3 m_Vel;
    float m_Cooldown = 0.f;

    // Release diagnostics: the speed it was let go at, and how many frames to log afterwards.
    SVector3 m_ReleaseVel;
    int m_LogFrames = 0;
};

class PortalGun : public IPluginInterface {
public:
    PortalGun();
    ~PortalGun() override;

    void OnEngineInitialized() override;
    void Init() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;
    void DrawSetup(bool p_HasFocus);
    void OnDraw3D(IRenderer* p_Renderer) override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    void PlacePortalInFrontOfPlayer(Portal& p_Portal, const SMatrix& p_PlayerTrans);
    // From the camera, or from p_Origin along p_Aim (the self-test). p_Result: dp_shoot's answer.
    bool ShootPortal(Portal& p_Portal, const Portal& p_Other, const ZEntityRef& p_Hitman, const SVector3* p_Origin = nullptr,
        const SVector3* p_Aim = nullptr, int* p_Result = nullptr);
    bool TryTeleport(Portal& p_In, Portal& p_Out, ZSpatialEntity* p_Hitman, const SMatrix& p_PlayerTrans);
    void ClearPortals();
    void StartFlight(const SVector3& p_Feet, const SVector3& p_Vel);
    void UpdateFlight(ZSpatialEntity* p_Hitman, SMatrix& p_Trans, float p_Dt);
    bool IsCarved(const SVector3& p_Point) const;
    bool FlightRayCast(const SVector3& p_From, const SVector3& p_To, SVector3& p_Hit, SVector3& p_Normal) const;
    static SVector3 MapDirection(const SVector3& p_Dir, const SMatrix& p_In, const SMatrix& p_Out);
    void TurnCamera(const SMatrix& p_In, const SMatrix& p_Out);
    void RunSelfTest(ZSpatialEntity* p_Hitman, const SMatrix& p_PlayerTrans, float p_Dt);
    void SelfTestLog(const std::string& p_Line);

    static SMatrix MapThroughPortal(const SMatrix& p_World, const SMatrix& p_In, const SMatrix& p_Out);
    static SMatrix ExitTransform(const SMatrix& p_PlayerTrans, const SMatrix& p_In, const SMatrix& p_Out);
    static bool RayCastLevel(const SVector3& p_From, const SVector3& p_To, const ZEntityRef& p_Ignore, SVector3& p_Hit,
        SVector3& p_Normal);
    static bool RayCast(
        const SVector3& p_From, const SVector3& p_To, const ZEntityRef& p_Ignore, SVector3& p_Hit, SVector3& p_Normal
    );

    // Pick-up, hold and drop, and physics objects through portals.
    void UpdatePickup(const ZEntityRef& p_Hitman, float p_Dt);
    void TryGrab(const ZEntityRef& p_Hitman, const SVector3* p_Origin = nullptr, const SVector3* p_Aim = nullptr);
    void Release();
    void UpdateBodies(float p_Dt);
    void SpawnCube(const SMatrix& p_PlayerTrans);
    TrackedBody* Track(const ZEntityRef& p_Entity);
    static ZDynamicPhysicsAspect* GetBody(const ZEntityRef& p_Entity);
    static ZEntityRef FindBody(const ZEntityRef& p_Hit);
    void SetBodyMotion(ZDynamicPhysicsAspect* p_Body, const SMatrix& p_Pose, const SVector3& p_Vel, float p_Dt) const;

    static DpFrame ToDpFrame(const SMatrix& p_Frame);
    static SMatrix FromDpFrame(const DpFrame& p_Frame);
    static bool DpRayCast(void* p_Ctx, const float (*p_Origin)[3], const float (*p_Dir)[3], float p_Max, DpHit* p_Hit);
    void FindGames();
    void LoadSetup();
    void SaveSetup();
    void BrowseFolder(int p_Game);
    static SMatrix MakeSurfaceFrame(const SVector3& p_Hit, const SVector3& p_Normal, const SVector3& p_AimDir);
    static void DrawPortal(IRenderer* p_Renderer, const Portal& p_Portal, const SVector4& p_Color);

    void GivePortalGun();
    bool IsOurGun(const ZHM5ItemWeapon* p_Weapon) const;

    bool m_CornerPicture = false;
    DECLARE_PLUGIN_DETOUR(PortalGun, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene);
    DECLARE_PLUGIN_DETOUR(PortalGun, bool, ZHM5ItemWeapon_FireProjectiles, ZHM5ItemWeapon* th, bool bMayStartSound);
    DECLARE_PLUGIN_DETOUR(PortalGun, bool, ZHM5ItemWeapon_IsFiring, IFirearm* th);
    DECLARE_PLUGIN_DETOUR(PortalGun, bool, ZActor_YouGotHit, IBaseCharacter* th, const SHitInfo& hitInfo);
    DECLARE_PLUGIN_DETOUR(PortalGun, void, ZHM5ItemWeapon_SetBulletsInMagazine, IFirearm* th, int32_t nBullets);
    DECLARE_PLUGIN_DETOUR(PortalGun, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId);
    DECLARE_PLUGIN_DETOUR(PortalGun, float, ZInputAction_Analog, ZInputAction* th, int32_t controllerId);

    bool GunInHand() const;
    // Whether the game is asking about its own aim input (blocked while the portal gun is held).
    bool OnGameAction(const char* p_Name, bool p_Active, bool p_Analog);

private:
    Portal m_Blue;
    Portal m_Orange;

    // See-through portals: [0] is drawn in blue (looks out of orange), [1] in orange.
    void SpawnViews();
    void DeleteViews();
    void UpdateViews(const ZEntityRef& p_Hitman);
    void DrawViews();
    void DrawRims();
    bool m_PlayroomRims = true;
    float m_RimSince[2] = {-1.f, -1.f};
    SVector3 m_RimAt[2];
    PortalView m_Views[2];
    Delirium m_Delirium;
    float m_Volume = 0.3f;
    bool m_ShowCameras = false;
    bool m_RightWasDown = false;
    void DrawPortalTab();
    TResourcePtr<ZTemplateEntityFactory> m_ViewFactory;
    TResourcePtr<ZTemplateEntityBlueprintFactory> m_ViewBlueprint;
    bool m_ViewsFailed = false;
    bool m_SeeThrough = true;
    float m_ScreenW = 1920.f, m_ScreenH = 1080.f;
    std::mutex m_ViewMutex;

    // Requests raised from the UI thread, consumed on the game thread.
    bool m_RequestBlue = false;
    bool m_RequestOrange = false;
    bool m_RequestClear = false;
    bool m_RequestBlueHere = false;
    bool m_RequestOrangeHere = false;

    // Seconds remaining before another teleport is allowed.
    float m_Cooldown = 0.f;
    int m_TeleportCount = 0;

    // Automated self-test, enabled by a PortalGun.selftest file next to the DLL.
    enum class SelfTestStage { Off, Waiting, Setup, Arm, Cross, Verify, Shoot, CubeGrab, CubeHold, CubeDrop, Done };
    SelfTestStage m_SelfTest = SelfTestStage::Off;
    float m_SelfTestTimer = 0.f;
    int m_SelfTestFrames = 0;
    ZEntityRef m_SelfTestCube;
    float m_SelfTestCubeZ = 0.f;
    // 1 = wall portal walk-in, 2 = floor portal step-in.
    int m_SelfTestPhase = 1;
    int m_SelfTestPasses = 0;
    SMatrix m_SelfTestStart;
    SMatrix m_LastTeleportTarget;
    std::filesystem::path m_ModDir;
    // The user's own Portal 2 and GTA IV folders (empty until found or given), and the menu's paste boxes.
    std::string m_Portal2Path;
    std::string m_Gta4Path;
    char m_Portal2Typed[512] = {};
    char m_Gta4Typed[512] = {};
    // The startup window: open, games skipped (Portal 2, GTA IV), a Browse running, its result (1 saved, 2/3 wrong
    // folder for Portal 2 / GTA IV), the last error shown.
    bool m_SetupOpen = false;
    // Our menu window (Numpad 3), the key's state last frame, and whether we hold the SDK's UI focus.
    bool m_MenuOpen = false;
    bool m_MenuKeyDown = false;
    bool m_HaveFocus = false;
    bool m_Skip[2] = {};
    std::atomic<bool> m_Browsing = false;
    std::atomic<int> m_BrowseResult = 0;
    std::string m_SetupError;

    ZInputAction m_PlaceBlueAction;
    ZInputAction m_PlaceOrangeAction;
    ZInputAction m_GunOrangeAction;
    ZInputAction m_JumpAction;
    ZInputAction m_FwdAction;
    ZInputAction m_BackAction;
    ZInputAction m_LeftAction;
    ZInputAction m_RightAction;

    // Portal-style flight. Hitman has no airborne 47 (he snaps to the nearest floor), so from a jump
    // or a portal exit until he lands, his position comes from Portal's movement code instead.
    bool m_Flying = false;
    SVector3 m_FlyFeet;
    SVector3 m_FlyVel;
    float m_FlyTime = 0.f;

    // 47's measured walking velocity while the game moves him.
    SVector3 m_PrevFeet;
    bool m_HasPrevFeet = false;
    SVector3 m_WalkVel;

    // The portal gun item (SMF repository entry, see smf/PortalGun).
    ZRepositoryID m_GunId;
    bool m_RequestGive = false;
    bool m_GaveThisScene = false;
    float m_SceneTime = 0.f;

    // Frame counter, and the last frame the game asked whether our gun (in 47's hands) is firing.
    uint64_t m_Frame = 0;
    uint64_t m_GunSeenFrame = 0;

    // Portal has no aim-down-sights: while the portal gun is held, Hitman's aim input is swallowed
    // (so NPCs never see 47 aiming a firearm) and pressing it fires orange instead.
    uint64_t m_AimSeenFrame = 0;
    bool m_AimWasHeld = false;
    std::unordered_set<std::string> m_LoggedActions;

    ZInputAction m_UseAction;
    ZInputAction m_SpawnCubeAction;
    bool m_RequestCube = false;
    // Stand-in for the Weighted Storage Cube until the Portal 2 model is packed: Crate_Styrofoam.
    ZRepositoryID m_CubeId;

    // The object 47 is holding, its rotation relative to his view yaw, and its carry velocity.
    ZEntityRef m_Held;
    SMatrix m_HeldRelRot;
    SVector3 m_HeldVel;
    SVector3 m_HeldCommanded;
    bool m_HeldHasCommand = false;
    // MoveTransform didn't move the object, so the hold falls back to SetTransform.
    bool m_HoldUseSet = false;
    int m_HoldStuckFrames = 0;
    float m_Dt = 1.f / 60.f;

    std::vector<TrackedBody> m_Bodies;
};

DECLARE_ZHM_PLUGIN(PortalGun)

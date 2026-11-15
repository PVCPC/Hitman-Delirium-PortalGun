// Delirium (gta4nm, the user's own GTA IV's NM) in Hitman: its trainer, its peds, and the ragdoll hotkeys.
// gta4nm.dll is loaded from mods/Delirium on first use (delay-loaded), with the NM data the user's GTA IV gave.
#pragma once

#include <unordered_map>

#include <delirium_portal.h>

#include <Glacier/ZMath.h>

#include "Bones.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class IRenderer;
class ZHM5BaseCharacter;
class ZSpatialEntity;
struct NmRig;
struct NmWorld;
struct NmBuild;
struct NmAgent;

class Delirium {
public:
    // Hitman's level: nearest solid hit from `from` to `to`.
    using Ray = std::function<bool(const SVector3& p_From, const SVector3& p_To, SVector3& p_Hit, SVector3& p_Normal)>;

    // `p_Dir`: mods/Delirium (gta4nm.dll, character.xrt, build.txt, taskparams.txt). `p_Gta4`: the user's GTA IV.
    void Configure(const std::filesystem::path& p_Dir, const std::string& p_Gta4);

    // Each frame (game thread). `p_Eye` / `p_Aim`: the camera; `p_Feet` / `p_Facing`: 47.
    // `p_Hitman` / `p_HitmanSpatial`: 47 (his Homer body rides his skeleton).
    void Update(float p_Dt, const Ray& p_Ray, const SVector3& p_Eye, const SVector3& p_Aim, const SVector3& p_Feet,
        const SVector3& p_Facing, ZHM5BaseCharacter* p_Hitman, ZSpatialEntity* p_HitmanSpatial);
    void Draw3D(IRenderer* p_Renderer);
    void DrawMenu();
    void Clear();
    // The linked pair (null: not both open), for ragdolls going through and the holes in the ground.
    void SetPortals(const DpFrame* p_Frames);


    // 47 is down (ragdolled or getting up): his own movement is off.
    bool HitmanDown();

    static void Unhook(); // unloading: the game's ragdoll back as it was

    // The hotkeys' states (shown in the menu): Delirium on NPCs, and 47 ragdolling.
    bool m_NpcMode = false;
    bool m_HitmanRagdoll = false;
    bool m_HitmanBody = true;
    bool m_ShowBodies = true;

private:
    struct Ped {
        NmAgent* m_Agent = nullptr;
        std::string m_Name;
        float m_Health = 200.f;
    };

    // A Hitman character with an NM body on its skeleton (Homer's, Wilma's for women): 47 keyframed on his
    // animation (ragdolling on Numpad 2), NPCs a partial ragdoll (legs on the animation) knocked into a full one.
    struct Body {
        ZHM5BaseCharacter* m_Char = nullptr;
        ZSpatialEntity* m_Spatial = nullptr;
        void* m_Anim = nullptr;
        NmAgent* m_Agent = nullptr;
        NmRig* m_Rig = nullptr;
        NmBuild* m_Build = nullptr; // fitted to it, freed with it
        float m_PortalLast[2] = {};
        bool m_PortalHad = false;
        std::vector<Bones::SQV> m_Written; // the ragdoll's last pose
        float m_Blend = 0.f; // s left blending back to the animation
        bool m_GettingUp = false; // the learned get-up running
        float m_GetUpTime = 0.f;
        bool m_HighFall = false;
        std::vector<float> m_PoseA, m_PoseB; // the last two stepped poses (nm_rig_pose)
        int m_PoseHips = -1;
        SMatrix m_HeldRot; // its entity as it went down (held there, xy following the body)
        float m_HeldZ = 0.f;
        std::vector<int> m_Bones;      // Hitman bone of each rig bone (the pelvis and everything under it)
        std::vector<int32_t> m_Parents; // Hitman's parents, every bone
        std::vector<float> m_Now;      // the rig bones' world frames this frame (7 floats each)
        std::vector<Bones::SQV> m_Local; // the animation's pose this frame (Hitman's, every bone)
        std::vector<float> m_World;    // every bone's world frame this frame (7 floats each)
        bool m_Dead = false;
        int m_Layout = -1;
        bool m_Npc = false;
        bool m_Ragdoll = false;
        std::string m_Name;
        float m_Health = 200.f;
    };

    bool Start();
    bool Attach(ZHM5BaseCharacter* p_Char, ZSpatialEntity* p_Spatial, bool p_Npc, bool p_Female, const Ray& p_Ray,
        const std::string& p_Name);
    void Detach(Body& p_Body);
    void Root(Body& p_Body, float* p_Turn, SVector3& p_P);
    bool Posed(Body& p_Body);
    void WritePose(Body& p_Body);
    void UpdateNpcs(const Ray& p_Ray, const SVector3& p_Feet);
    void SetHitmanRagdoll(bool p_On);
    void SetRagdoll(Body& p_Body, bool p_On);
    DpFrame m_Portals[2] {};
    bool m_PortalsOpen = false;
    SVector3 m_HitmanHips;
    bool m_HadHitmanHips = false;
    Body* Hitman();
    // Target `t` (0 47's body, then the peds, then the NPCs' bodies): its agent, name and health; null for none.
    NmAgent* Target(int t, std::string*& p_Name, float*& p_Health, Body*& p_Body);
    void Spawn(const Ray& p_Ray, const SVector3& p_Feet, const SVector3& p_Facing);
    void UpdateGround(const Ray& p_Ray, bool p_One = false);
    void Fire(NmAgent* a, const std::string& p_Name, float& p_Health, const std::string& p_What);
    int Aimed();
    void Snapshot();

    std::filesystem::path m_Dir;
    std::string m_Gta4;
    bool m_Tried = false;
    bool m_Ready = false;
    std::string m_Status = "Not started.";

    NmWorld* m_World = nullptr;
    NmBuild* m_Build = nullptr;
    NmBuild* m_Wilma = nullptr;
    std::vector<Body> m_Bodies;
    float m_NpcTimer = 0.f;
    float m_HitmanTimer = 0.f;
    std::vector<Ped> m_Peds;
    std::vector<std::string> m_Messages;
    std::vector<float> m_Radius, m_Half;
    std::vector<std::vector<float>> m_Sizes; // per drawn body: each part's radius and half length

    // Requests from the menu (UI thread), done on the game thread.
    std::mutex m_Mutex;
    bool m_WantSpawn = false;
    bool m_WantClear = false;
    std::vector<std::pair<int, std::string>> m_WantFire; // target (0 = 47), action

    // What the menu picked.
    int m_Target = 0; // 0 = 47, 1.. = peds
    int m_Message = 0;
    float m_Strength = 1.f;
    std::string m_Line;
    char m_Text[256] = "bodyBalance start=true";

    // The last step's eye ray and the parts to draw (render thread reads these).
    SVector3 m_Eye, m_Aim;
    std::vector<std::vector<float>> m_Pose; // per ped and body: 21 x 7 floats
    size_t m_PedsDrawn = 0;
    std::vector<SVector3> m_Lines; // skeleton lines, in pairs
    std::vector<std::string> m_Names; // the targets after 47, for the menu
    float m_Accum = 0.f; // time toward the next 1/60 step
    float m_Alpha = 0.f; // how far into it (drawing between steps)
    bool m_Stepped = false;
    std::unordered_map<NmAgent*, std::pair<std::vector<float>, std::vector<float>>> m_DrawPose; // last two steps, for drawing
    std::unordered_map<NmAgent*, float> m_LastGround; // the last ground under each body
    float m_GroundTimer = 0.f;
    size_t m_GroundNext = 0;
};

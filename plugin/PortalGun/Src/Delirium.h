// Delirium (gta4nm, the user's own GTA IV's NM) in Hitman: its trainer, its peds, and the ragdoll hotkeys.
// gta4nm.dll is loaded from mods/Delirium on first use (delay-loaded), with the NM data the user's GTA IV gave.
#pragma once

#include <Glacier/ZMath.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class IRenderer;
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
    void Update(float p_Dt, const Ray& p_Ray, const SVector3& p_Eye, const SVector3& p_Aim, const SVector3& p_Feet,
        const SVector3& p_Facing);
    void Draw3D(IRenderer* p_Renderer);
    void DrawMenu();
    void Clear();

    // The hotkeys' states (shown in the menu): Delirium on NPCs, and 47 ragdolling.
    bool m_NpcMode = false;
    bool m_HitmanRagdoll = false;

private:
    struct Ped {
        NmAgent* m_Agent = nullptr;
        std::string m_Name;
        float m_Health = 200.f;
    };

    bool Start();
    void Spawn(const Ray& p_Ray, const SVector3& p_Feet, const SVector3& p_Facing);
    void UpdateGround(const Ray& p_Ray);
    void Fire(int p_Ped, const std::string& p_What);
    int Aimed() const;
    void Snapshot();

    std::filesystem::path m_Dir;
    std::string m_Gta4;
    bool m_Tried = false;
    bool m_Ready = false;
    std::string m_Status = "Not started.";

    NmWorld* m_World = nullptr;
    NmBuild* m_Build = nullptr;
    std::vector<Ped> m_Peds;
    std::vector<std::string> m_Messages;
    std::vector<float> m_Radius, m_Half;

    // Requests from the menu (UI thread), done on the game thread.
    std::mutex m_Mutex;
    bool m_WantSpawn = false;
    bool m_WantClear = false;
    std::vector<std::pair<int, std::string>> m_WantFire;

    // What the menu picked.
    int m_Target = 0; // 0 = 47, 1.. = peds
    int m_Message = 0;
    float m_Strength = 1.f;
    std::string m_Line;
    char m_Text[256] = "bodyBalance start=true";

    // The last step's eye ray and the parts to draw (render thread reads these).
    SVector3 m_Eye, m_Aim;
    std::vector<std::vector<float>> m_Pose; // per ped: 21 x 7 floats
    float m_Accum = 0.f;
    float m_GroundTimer = 0.f;
};

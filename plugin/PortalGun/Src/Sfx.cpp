#include "Sfx.h"

#include <Windows.h>
#include <xaudio2.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "delirium_portal.h"

#pragma comment(lib, "xaudio2.lib")

namespace {
    struct Sound {
        std::vector<unsigned char> m_Fmt;
        std::vector<unsigned char> m_Data;
    };

    std::mutex g_Mutex;
    IXAudio2* g_Audio = nullptr;
    IXAudio2MasteringVoice* g_Master = nullptr;
    Sound g_Sounds[Sfx::Count];
    bool g_Tried = false;

    // A RIFF WAVE's fmt and data chunks (PCM or ADPCM: XAudio2 plays both).
    bool Parse(const unsigned char* p, size_t n, Sound& s) {
        if (n < 12 || std::memcmp(p, "RIFF", 4) || std::memcmp(p + 8, "WAVE", 4))
            return false;

        for (size_t i = 12; i + 8 <= n;) {
            uint32_t s_Len;
            std::memcpy(&s_Len, p + i + 4, 4);
            const size_t s_End = std::min(n, i + 8 + s_Len);

            if (!std::memcmp(p + i, "fmt ", 4))
                s.m_Fmt.assign(p + i + 8, p + s_End);
            else if (!std::memcmp(p + i, "data", 4))
                s.m_Data.assign(p + i + 8, p + s_End);

            i += 8 + s_Len + (s_Len & 1);
        }

        if (s.m_Fmt.size() < sizeof(WAVEFORMATEX))
            s.m_Fmt.resize(sizeof(WAVEFORMATEX)); // a PCMWAVEFORMAT: cbSize 0

        return s.m_Fmt.size() >= 16 && !s.m_Data.empty();
    }
}

void Sfx::Load(const std::string& p_Portal2) {
    std::scoped_lock s_Lock(g_Mutex);

    if (g_Tried || p_Portal2.empty())
        return;

    g_Tried = true;

    if (FAILED(XAudio2Create(&g_Audio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_Audio->CreateMasteringVoice(&g_Master)))
        return;

    constexpr const char* c_Files[Count] = {
        "sound/weapons/portalgun/portalgun_shoot_blue1.wav", "sound/weapons/portalgun/portalgun_shoot_red1.wav",
        "sound/weapons/portalgun/portal_open_blue_01.wav", "sound/weapons/portalgun/portal_open_red_01.wav",
    };

    for (int i = 0; i < Count; ++i) {
        unsigned char* s_Data = nullptr;
        size_t s_Len = 0;

        if (dp_vpk_read(p_Portal2.c_str(), c_Files[i], &s_Data, &s_Len)) {
            Parse(s_Data, s_Len, g_Sounds[i]);
            dp_free(s_Data, s_Len);
        }
    }
}

void Sfx::Play(Id p_Id, float p_Volume) {
    std::scoped_lock s_Lock(g_Mutex);

    if (!g_Audio || g_Sounds[p_Id].m_Data.empty())
        return;

    const Sound& s = g_Sounds[p_Id];
    IXAudio2SourceVoice* s_Voice = nullptr;

    // ponytail: a new voice per sound, never freed until exit (a handful per shot); pool them if it ever matters.
    if (FAILED(g_Audio->CreateSourceVoice(&s_Voice, reinterpret_cast<const WAVEFORMATEX*>(s.m_Fmt.data()))))
        return;

    XAUDIO2_BUFFER s_Buf {};
    s_Buf.AudioBytes = static_cast<UINT32>(s.m_Data.size());
    s_Buf.pAudioData = s.m_Data.data();
    s_Buf.Flags = XAUDIO2_END_OF_STREAM;
    s_Voice->SetVolume(p_Volume);
    s_Voice->SubmitSourceBuffer(&s_Buf);
    s_Voice->Start();
}

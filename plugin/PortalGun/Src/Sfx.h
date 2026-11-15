// Portal 2's own portal gun sounds, read from the user's install (as Delirium's Playroom does) and played with XAudio2.
#pragma once

#include <string>

namespace Sfx {
    enum Id { ShootBlue, ShootOrange, OpenBlue, OpenOrange, Count };

    // Reads them the first time (nothing without Portal 2).
    void Load(const std::string& p_Portal2);
    void Play(Id p_Id, float p_Volume = 1.f);
}

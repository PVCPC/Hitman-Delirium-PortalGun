// Delirium's Portal 2 portal code (portal-core, Rust) for C/C++. Metres, Z up; rotations are quaternions x, y, z, w.
// A portal frame's local +Z points out of the surface and +Y is its up (right = up x normal).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DpFrame { float at[3]; float rot[4]; } DpFrame;
typedef struct DpHit { float point[3]; float normal[3]; float distance; } DpHit;

// Nearest solid hit along origin + t * dir, 0 <= t <= max: fill hit and return true. Should miss characters and props.
typedef bool (*DpRay)(void* ctx, const float (*origin)[3], const float (*dir)[3], float max, DpHit* hit);

void dp_set_half(float w, float h);
// 0 placed (out filled), 1 hit nothing, 2 doesn't fit, 3 overlaps the other. other / linked[2] / hit may be null.
int dp_shoot(DpRay ray, void* ctx, const float (*origin)[3], const float (*dir)[3], float range,
    const DpFrame* other, const DpFrame* linked, DpFrame* out, DpHit* hit);
// pos / vel / rot may be null.
void dp_through(const DpFrame* from, const DpFrame* to, float (*pos)[3], float (*vel)[3], float (*rot)[4]);
float dp_local_z(const DpFrame* f, const float (*p)[3]);
bool dp_crossed(const DpFrame* f, float last, const float (*p)[3]);
void dp_funnel(const DpFrame* portals, size_t n, float gravity, const float (*centre)[3], float (*vel)[3], float dt);
void dp_exit_gimme(const DpFrame* exit, const float (*vel)[3], float (*out)[3]);

// game: 0 Portal 2, 1 GTA IV. Saved in config_dir (or by Delirium), else found in the Steam libraries.
size_t dp_find_game(int game, const char* config_dir, char* out, size_t cap);
bool dp_set_game(int game, const char* config_dir, const char* path);
bool dp_vpk_read(const char* portal2, const char* path, unsigned char** data, size_t* len);
void dp_free(unsigned char* data, size_t len);

#ifdef __cplusplus
}
#endif

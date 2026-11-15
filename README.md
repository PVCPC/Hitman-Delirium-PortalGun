# Hitman-Delirium-PortalGun

A Portal gun for HITMAN World of Assassination: blue and orange portals on any surface, 47 and objects go through with
their speed kept. The portal code is [Delirium Physics](https://github.com/PVCPC/delirium-physics)' Portal 2 rewrite
(`portal-core/`, Rust), shared with Delirium's portal mode.

**Needs your own copies.** Nothing from Portal 2 or GTA IV is in this repository or its downloads. The mod reads them
from your installs: it finds them in your Steam libraries (or the folders Delirium already saved), and if it can't,
you paste or pick the folder in the mod's window.

Not affiliated with or endorsed by IO Interactive, Valve, Rockstar Games or NaturalMotion.

## Install

1. [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) and [Simple Mod Framework](https://github.com/atampy25/simple-mod-framework).
2. `PortalGun.dll` into `HITMAN 3\Retail\mods`; `smf/PortalGun` into SMF's `Mods` folder, enable it, deploy.
3. Start the game. The first time, a window shows which of your games it found (press `~` to use it). Each one adds
   something, and either can be skipped:

   | You provide | You get |
   |---|---|
   | Nothing | The portal gun, with stand-in looks (the SMG model, plain portal edges, no Portal sounds) |
   | Portal 2 | Portal 2's gun and cube models, sounds and portal effects |
   | GTA IV | Delirium's ragdoll physics for NPCs and bodies |
   | Both | Everything |

   Reopen it any time from the mod menu: **PORTAL: YOUR GAMES**.

Controls: fire = blue, right click = orange (gun in hand); Ctrl+B / Ctrl+O shoot without it; F picks up and drops.

## Build

Visual Studio 2022, CMake, Rust (stable). With the ZHMModSDK DevPkg unpacked somewhere:

```
cmake -S plugin -B plugin/build -G "Visual Studio 17 2022" -A x64 -DZHM_SDK_DIR=<DevPkg folder>
cmake --build plugin/build --config Release
```

`cargo test` in `portal-core` runs the portal maths tests (`-- --ignored` also checks it finds and reads your Portal 2).

## Credits

See [CREDITS.md](CREDITS.md).

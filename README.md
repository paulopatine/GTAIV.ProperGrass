# GTA IV Proper Grass

GPU-instanced procedural vegetation for **Grand Theft Auto IV** (**1.2.0.59** Complete Edition and **1.0.8.0**).
It fixes the game's long-standing grass problem: many grass surfaces never get plants, and the ones that do run out of
plant objects, pop in and vanish at some camera angles. The mod keeps the game's own plants and its own look, but draws them
with hardware instancing, so a far denser, longer-reaching lawn costs a handful of draw calls.

Inspired by *SA Proper Shaders* (GTA SA). Works alongside FusionFix, DXVK and DLSS-IV.

## What it does
- **Grass where the game left none.** The grass materials (`GRASS`, `SHORT_GRASS`, `GRASS_PATCHY`, `GRASS_LONG`) ship without a procedural tag, so parks, medians and slopes stay bare. The mod gives them one.
- **No more object limit.** The plant triangle pool grows from 512 to 16384, and plants are drawn as instances instead of one game object each.
- **Longer reach with a soft fade.** Plants grow in over the last metres instead of popping in.
- **Clean borders.** Plants no longer spill onto asphalt or sidewalks.
- **Plants follow the terrain.** On slopes they grow perpendicular to the ground instead of sticking straight up (adjustable).
- **View-cone culling** keeps the GPU cost low.

## How it differs from "Grass and Procedural Props Fix"
[Grass and Procedural Props Fix](https://www.nexusmods.com/gta4/mods/1230) (iammrmikeman) is the established fix for the same symptom, and the two take different routes. Based on its Nexus page:

| | Grass and Procedural Props Fix | Proper Grass |
|---|---|---|
| Approach | Raises the limits of the game's procedural-object storage (pool size, distances, density), so the game keeps creating and drawing one object per plant | Takes the plants out of the game's object system and draws them as GPU instances; also raises the triangle pool |
| Cost per plant | Same as vanilla: every extra plant is another object and draw call | A few draw calls for all plants of a model |
| Bare grass surfaces | Not covered by its description | Gives the untagged grass materials (`GRASS`, `SHORT_GRASS`, `GRASS_PATCHY`, `GRASS_LONG`) a plant tag, so parks, medians and slopes that never had plants get them |
| Edges and fade | Not covered by its description | Keeps plants off asphalt and sidewalks, grows them in at the edge of the radius, culls them outside the camera view |
| Other procedural objects | Plants, clutter, litter and more, with per-class density filters | Vegetation only |
| Supported builds | 1.0.4.0 to Complete Edition 1.2.0.59 and VCNGE | 1.2.0.59 only |
| Safety net | Hash check, startup validation, rollback | Signature check of the executable, disables itself on mismatch |

In short: that mod makes the game's own system hold more; this one replaces how plants are drawn and fills in the grass the game never populated. In the author's tests, even with Various Fixes installed, some surfaces stay bare in vanilla and are only filled by Proper Grass, for example the garage lawn and sidewalk strip near Meadows Park and a lawn at a building in the Firefly projects. If you only want more litter and props, or run another game build, use that mod. Both change the plant limits, so run one at a time unless you have tested the combination; they have not been tested together.

## Requirements
- GTA IV **1.2.0.59** (Complete Edition, Steam) or **1.0.8.0**. The mod recognises the executable and picks the right game addresses. Other builds of the Complete Edition (for example 1.2.0.43) are tried through code patterns and should work if the game code is the same, but they are untested; if the mod disables itself, send `ProperGrass.log`.
- An ASI loader ([FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) includes one).
- `d3dx9_43.dll` (installed by the DirectX redistributable that ships with the game).

## Install
Copy `GTAIV.ProperGrass.asi` and `ProperGrass.ini` into the `plugins` folder next to `GTAIV.exe`, or next to `GTAIV.exe` itself if that is where your other ASI plugins live. Keep both files together: the INI and the log (`ProperGrass.log`) are read from and written to the folder of the `.asi`. Uninstall by deleting both files.

## Settings (`ProperGrass.ini`)
| Key | Default | Meaning |
|---|---|---|
| `RadiusMultiplier` | 1.5 | Plant radius = (20 + 0.5 x Detail Distance) x this value |
| `DrawDistance` | 0 | Fixed radius in metres (0 = use the formula above) |
| `FadeWidth` | 12 | Metres over which plants grow in at the edge of the radius |
| `DensityMultiplier` | 1.0 | Scales plant density |
| `PlantScale` | 1.0 | Size of all plants, width and height (0.25 - 3.0) |
| `SlopeAlign` | 1 | How much plants follow the slope: 1 = perpendicular to the ground, 0 = straight up like vanilla |
| `TriCapacity` | 16384 | Active plant triangles |
| `EdgeMargin` / `EdgeSoft` | 0.35 / 1.0 | Keeps plants away from the border of a grass area |
| `FrustumCull` | 1 | Skip plants outside the camera view |
| `GrassMaterialFix` | 1 | Give plants to grass surfaces the game ships without any. Set 0 if another mod (e.g. Various Fixes) already fixes them |
| `GrassMaterials` | see file | `NAME@TAG` list of grass materials (plants for them, and edge handling) |

Diagnostics: `Debug=1` writes extra detail to `ProperGrass.log`. `Profile=1` shows an on-screen performance panel;
with `ToggleKey=0x76` (F7) you can switch live between **MOD**, **VANILLA** and **NO-INSTANCING**, and `CullKey=0x77` (F8) toggles culling.

## Performance
Measured with the on-screen profiler in Meadows Park (RTX 5070 Ti, i7-14700KF, 1440p). Frame rate varies a lot with traffic and pedestrians, so use the profiler on your own system.

| Settings | Vanilla | Mod |
|---|---|---|
| View/Detail/Vehicle 100/100/100 | 70-117 fps | 71-119 fps (draw calls about 300 lower) |
| FusionFix recommended 27/31/50 | ~200 fps | ~170 fps, 1% lows unchanged, grass reaches ~1.5x farther |

The mod costs roughly 0.2-0.4 ms on the main thread, under 0.1 ms on the render thread and 0.5-0.8 ms of GPU time.

## Known limitations
- The airport-highway median has concrete collision under its grass, so it stays bare.
- A few plant models cannot be instanced and remain ordinary game objects.
- Grass looks as glossy under car headlights as in vanilla; custom lighting is not part of this release.
- Only 1.2.0.59 and 1.0.8.0 are supported.

## Build
```
sudo apt install g++-mingw-w64-i686
./build.sh        # produces build/GTAIV.ProperGrass.asi
```

## How it works (short)
The plant manager's candidate-add function is hooked so each plant is stored as a compact instance instead of a game entity. Every frame the visible instances are packed into a vertex buffer and drawn at the start of the deferred G-buffer pass by replaying the plant model's own entity draw command with a patched vertex shader (`SetStreamSourceFreq` instancing, d3dx9 assembler at runtime).

## License
MIT. Not affiliated with Rockstar Games. Use at your own risk and keep a backup of your game.

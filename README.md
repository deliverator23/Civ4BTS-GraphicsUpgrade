# GraphicsUpgrade for Civilization IV: Beyond the Sword

A drop-in graphics add-on for **Beyond the Sword 3.19** that adds reflective, dynamic water (like Civilization IV: Colonization), real sun shadows and
adjustable lighting, with a togglable day–night cycle (like Civilization VI).

It's not a mod in the usual sense: it changes no game files, no XML, no Python and no gameplay, and it works alongside
whatever mod you play. It's a single `d3d9.dll` that sits next to the game, its settings file, and a folder of water
textures. It doesn't affect saves or multiplayer, and you can remove it at any time.

Mods can include their own GraphicsUpgrade.ini settings file, to configure their own lighting and other settings such as models that shouldn't
cast shadows. These apply only while that mod is running. See [For modders](#for-modders) and the modding guide, [MODDING.md](MODDING.md).

## What it changes

| | Plain BtS | With GraphicsUpgrade |
|---|---|---|
| **Water** | Flat, animated surface | Ripples that reflect the sky, show the sea floor through the waves, and sparkle in the sun |
| **Coasts** | Flat water up to the shore | Sunlight shimmering on the shallow sea floor, and a sun glint that follows the sun |
| **Ship reflections** | None | Ships and sea resources (whales, fish, clams...) mirrored in the water |
| **Under the water** | Hulls show through the semi-transparent sea | They still show through, fading out with depth like real shallows, and so do sea resources' underwater parts |
| **Unit, ship and tree shadows** | Round blobs under units; nothing for trees | Real shadows cast by the sun, weapons and rigging included |
| **Hill and peak shadows** | None | Hills and peaks shade the ground behind them |
| **Ground detail** | Painted on, lit the same whatever the light | Small bumps (sand ripples, cracks, stones) catch the sun and turn with it |
| **Building shadows** | Painted on, always pointing the same way | Real shadows from buildings, improvements and resources |
| **Lighting** | BtS's white sun and grey shade | [Without day-night cycle] A warm sun and cool blue shade; colours adjustable for units, ships, trees, buildings, terrain and rivers |
| **Day and night** | None | [With day-night cycle] The light changes colour and the sun circles the sky, with shadows turning to follow it |

Everything is on by default. Water, shadows and lighting can be switched off in game (see the hotkeys below), and every
part can be turned off in the settings. Shadows fall on the terrain, rivers, roads and the water's surface. Unit flags and other markers don't cast shadows.

## What you need

- Civilization IV: Beyond the Sword 3.19 on Windows Vista or later.
- A graphics card with Shader Model 3 (any from about 2005 on). Without it, hill and mountain shadows are left out;
  everything else works.

## Install

1. Download [GraphicsUpgrade.zip](https://github.com/deliverator23/Civ4BTS-GraphicsUpgrade/releases/latest/download/GraphicsUpgrade.zip)
   from the latest release (all versions are on the [Releases](../../releases) page).
2. Copy these from it into your **Beyond the Sword** folder, the one with `Civ4BeyondSword.exe` in it:

   ```
   d3d9.dll
   GraphicsUpgrade.ini
   GraphicsUpgrade\      (the folder, with the water and caustic textures)
   presets\              (optional lighting presets)
   ```

   The zip also has this guide, as `GraphicsUpgrade.md`, and the modding guide, as `GraphicsUpgrade-Modding.md`.

   Also copy the contents of the zip's `CustomAssets` folder (an `art` folder) into the game's CustomAssets folder,
   adding to anything already there:

   ```
   Documents\My Games\Beyond the Sword\CustomAssets\
   ```

   These are coast waves that fade in and out as they roll in, with whiter foam, and a rippled river surface for the
   rivers' shimmer. The game loads them itself, in plain BtS and in mods that allow custom assets. GraphicsUpgrade
   works without them, with BtS's own waves and rivers.

   For Steam, that's usually:

   ```
   C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization IV Beyond the Sword\Beyond the Sword\
   ```

3. Start the game as usual. The first time water comes into view there's a short pause while it loads.

If the folder already has a `d3d9.dll` (ReShade, DXVK...), read [Using with ReShade or DXVK](#using-with-reshade-or-dxvk) first.

**To remove it,** delete `d3d9.dll`, `GraphicsUpgrade.ini`, `GraphicsUpgrade\` and `presets\` from that folder, and
`art\terrain\waves\` and `art\terrain\routes\rivers\AllRiversNormal.dds` from the CustomAssets folder.

## In game

| Keys | What it does                                 |
|---|----------------------------------------------|
| Ctrl+Shift+1 | The water, to compare with BtS default water |
| Ctrl+Shift+2 | Sun shadows                                  |
| Ctrl+Shift+3 | Lighting                                     |
| Ctrl+Shift+4 | Pauses and resumes the day–night cycle       |
| Ctrl+Shift+5 | Sharper distant textures (anisotropic filtering) |
| Ctrl+Shift+6 | Bumps on the ground that catch the sun (detail normals) |
| Ctrl+Shift+7 | Next look for the waves, if there's more than one (`water_normal_1.dds`...) |
| Ctrl+Shift+/ | Saves a before/after pair of screenshots     |

Ctrl+Shift+/ saves two screenshots from two frames in a row, so every unit, tree and wave is in the same place:
one with GraphicsUpgrade's effects as you have them (`..._on.png`) and one with none of them, plain BtS
(`..._off.png`). They go in `%LOCALAPPDATA%\GraphicsUpgrade\bts\screenshots`.

To use other keys, change them in `GraphicsUpgrade.ini`: `ToggleKey` in the `[water]`, `[shadows]` and `[lighting]`
sections, `LightCycleToggleKey` in `[lighting]` for pausing the day–night cycle, `ToggleKey` and
`DetailNormalsToggleKey` in `[textures]` for the filtering and the ground's bumps, and `CompareKey` in `[general]` for the
screenshots. Each can be a number, a letter, a key name such as `F11` or `Numpad5`, a symbol such as `/`, or `none`.
`KeyModifiers` in `[general]` sets the Ctrl+Shift part.

These keys are kept from the game: BtS doesn't see Ctrl+Shift+1 as Ctrl+1 or Shift+1 (its unit groups), nor
Ctrl+Shift+/ as / (the next worker). The same number or `/` with any other modifiers is the game's as usual. This
holds for mods too, BUG among them. Their own keys are letters and F-keys with Ctrl, Alt or Shift (BUG's
Alt+G, Ctrl+Shift+K, Ctrl+X...), so none of them clash with the number keys and `/`. If you give one of GraphicsUpgrade's
keys the same keys as a mod command, GraphicsUpgrade wins and the mod's command stops working.

## Settings

Everything is in `GraphicsUpgrade.ini`, and each setting has a comment explaining it. Restart the game after editing.
The ones you're most likely to want:

| Setting | What it does |
|---|---|
| `[shadows] Darkness` | How dark shadows are (0 to 1) |
| `[shadows] Softness` | How soft shadow edges are (0 = hard) |
| `[shadows] BuildingShadows` | Real building shadows on (1) or off (0) |
| `[shadows] SunDirection` | Which way the sunlight travels when `[lighting] SunMiddayFrom` is empty. With `SunMiddayFrom` set, the sun follows it and the height settings; with the day–night cycle off, it stays where it is at `LightCycleStartStep` |
| `[shadows] HillShadowLength` | How long hill and mountain shadows are (1 = true to the terrain's heights) |
| `[shadows] WaterShadow` | How much shadow falls on the water's surface |
| `[water] UnderwaterDepth` | How deep you can see into the water (0 = not at all) |
| `[water] Caustics` | How strong the shimmer on the shallow sea floor is (0 = off) |
| `[water] WaveShading` | How strongly the sun or moon lights the waves, and the sparkle around the glint (0 = off, up to 255) |
| `[water] FoamLight` | How much of the light's colour and brightness the coast waves' foam takes (0 = always white, 1 = like the ground) |
| `[water] RiverGlint` | How bright the glint off the rivers is (0 = off, 128 = normal, up to 255) |
| `[water] RiverShimmer` | How far the rivers' ripples sway back and forth along the channel (0 = still, up to 3) |
| `[reflection] Size` | Reflection sharpness: 1024, or 512 to save a little speed |
| `[lighting] Enabled` | GraphicsUpgrade's lighting (1), or BtS's own (0) |
| `[lighting] LightCycleOn` | The day–night cycle on (1) or off (0) |
| `[textures] Anisotropy` | How sharp distant ground, roads and water stay: 2 to 16, or 1 for BtS's own filtering |
| `[textures] DetailNormals` | Bumps on the ground that catch the sun (1) or not (0); needs the lighting on |
| `[textures] DetailBumpiness` | How bumpy the ground looks (0 = only where a mod gives a normal map) |
| `[textures] BlendTileClamp` | No thin lines along square edges where terrains blend (1), or blends read as BtS reads them (0) |
| `[textures] AreaBorderTextures` | The settler's city-site guide laid over the hills and valleys (BtS's guide texture, the default), or BtS's flat guide (empty) |
| `[lighting] BuildingLightRange` | How much brighter than their textures buildings, improvements and resources can be lit: 1 (BtS's look) to 2 |

**Settings files from 1.0.** Two things have moved, and a `GraphicsUpgrade.ini` (yours or a mod's) that still has them
the old way keeps working, with a line in the log saying what to change: the group colours (`UnitSun`, `TerrainAmbient`
and so on) are now in `[lighting groups]` rather than `[lighting]`, and `[water] WaterConstants` is now three settings,
`ReflectionEvenness`, `ReflectionRipple` and `RefractionRipple`.

### Lighting and day–night

Colours are written as Red,Green,Blue from 0 to 255. You can give each group its own in `[lighting groups]`: units,
ships and vehicles, terrain, rivers, trees, buildings, and the sun on the water. The `presets` folder has complete
`[lighting]` and `[lighting groups]` sections to paste in place of the two in `GraphicsUpgrade.ini`:

- **`lighting-bts.ini`**: BtS's own lighting, close to how the game looks without GraphicsUpgrade. A good starting point for your own colours.
- **`lighting-warm.ini`**: a warm sun and cool blue shade, fixed (the default colours with the day–night cycle off).
- **`lighting-day-night.ini`**: a day–night cycle in muted, natural colours, the sun circling the sky once a day.

The day–night cycle runs through a day over `LightChangeCycleTime` seconds: golden hour, sunset, a silvery
moonlit night and dawn. The sun travels round the sky rather than swinging back and forth, so the shadows turn with it.
You can set:
- the colours for each hour
- the compass direction of the midday sun
- how high the sun and moon climb

The colours for each hour are a small picture, `GraphicsUpgrade\day-night.png` (`[lighting] LightCycleImage`): one
pixel wide per hour, from midnight on the left to 23:00 on the right, the sun's colour along the top row and the
shade's along the second, blended in between. Paint over it in any image editor to change the day, or point
`LightCycleImage` at a picture of your own (PNG or BMP) in the `GraphicsUpgrade` folder, or in a mod's own
`GraphicsUpgrade` folder. A 24×2 picture gives a 24-hour day, and a wider one gives finer steps. The colours can be
written as numbers instead, with `LightCycleSteps` and `LightCycleAmbientSteps` as in `presets\lighting-day-night.ini`.

**Buildings under a bright light.** BtS lights buildings, improvements and resources in a way that stops at the colour
of their textures. When the sun and the ambient add up to more than that, as they do in BtS's own light and in bright
day–night cycles, whatever faces the sky shows its texture's colour whatever the time of day, while the ground around
it darkens and changes colour. `[lighting] BuildingLightRange` lets them be lit brighter than their textures, up to 2
times, as the game lights the ground, so they follow the light as the ground does. The default of 1 keeps BtS's look.
Units are not changed.

### The settler's city-site guide

BtS draws the guide (the square or cross of plots a city there would work) as a flat sheet at one height, drawn over
everything, so on high or low ground it shows away from the plots it marks. GraphicsUpgrade lays it over the terrain's
hills and valleys instead, and hills in front of it hide it; anything standing on it (units, trees, buildings) shows in
front. `[textures] AreaBorderTextures` lists the guide's textures, BtS's own by default; empty it for BtS's flat guide.

## For modders

A mod can fit GraphicsUpgrade to itself without the player changing anything: a `GraphicsUpgrade.ini` in the mod's
own `GraphicsUpgrade` folder (`Mods\<your mod>\GraphicsUpgrade\`) replaces any of the player's settings while the mod
runs. With it a mod can set its own lighting and day–night cycle, ship its own water textures, keep markers from
casting shadows, have effects follow the light, and use the experimental terrain features (finer heightmaps, terrain
stamps, river layers).

All of this is in the modding guide, [MODDING.md](MODDING.md) (`GraphicsUpgrade-Modding.md` in the download).

## Using with ReShade or DXVK

Both of these also install as `d3d9.dll`. To use one with GraphicsUpgrade:
1. Rename theirs, for example to `reshade_d3d9.dll`.
2. Put GraphicsUpgrade's `d3d9.dll` in its place.
3. Set `ChainDll` in `GraphicsUpgrade.ini` to the full path of the renamed file.

GraphicsUpgrade then passes everything on to it.

## If something goes wrong

GraphicsUpgrade writes a short log to `%LOCALAPPDATA%\GraphicsUpgrade\bts\session.log`. It never writes into the game
folders. If the game won't start or something looks wrong, remove `d3d9.dll`, and include that log when you report it.

GraphicsUpgrade only acts on `Civ4BeyondSword.exe` and the mods it runs; in any other program it passes everything
straight through.

## Known limitations

- **Civilopedia previews keep BtS's water.** The 3D pictures in the Civilopedia (a ship on its patch of sea, for
  example) are drawn separately from the map, and GraphicsUpgrade leaves them as BtS draws them.

## How it works

GraphicsUpgrade sits between the game and Direct3D 9. It changes how BtS's own scene is drawn, frame by frame:

- **Water:** the ships are redrawn into a mirrored reflection. BtS's own water is then drawn with a new water shader.
  Water on models, such as an oasis' pool, gets the same ripples, sky reflection
  and sun glint drawn over it. BtS draws sea resources after the water, too late for its reflection and its view under
  the surface: each is also kept and drawn into the next frame's, so there they run one frame (a sixtieth of a second)
  late.
- **Shadows:** units, ships, trees and buildings are redrawn from the sun's point of view into a shadow map, units and
  ships only above the water line (the shadow map has no depth, so an underwater part would shade the surface). Hill
  shadows come from the terrain's own heights. The ground is then drawn with BtS's own shading plus both.
- **Lighting:** BtS's light colours are swapped as each thing is drawn. The terrain's lighting is re-coloured to match,
  and re-lit as the sun moves.

Everything else is drawn by the game exactly as before.

## Building from source

You need:
- Visual Studio 2022 with the *Desktop development with C++* workload, which includes the Windows SDK and its shader
  compiler
- [uv](https://docs.astral.sh/uv/)

```bash
build.bat
```

That builds `build\d3d9.dll`. To build and package a release into `dist\`:

```bash
uv run tools/package.py
```

To run the tests (no game needed; a few use installed games' files and are skipped without them):

```bash
uv run tests/run_tests.py
```

## Credits

GraphicsUpgrade by Deliverator.

The water and caustic textures in the `GraphicsUpgrade` folder and the coast waves in the `CustomAssets` folder are
from other Sid Meier's Civilization games, © Firaxis Games / 2K (`water_env.dds`'s sun glint redrawn to remove a seam,
the waves' foam whitened). GraphicsUpgrade is an unofficial fan
project, not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.

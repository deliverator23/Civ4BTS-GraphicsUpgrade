# GraphicsUpgrade for Civilization IV: Beyond the Sword

A drop-in graphics add-on for **Beyond the Sword 3.19** that adds reflective, dynamic water (like Civilization IV: Colonization), real sun shadows and
adjustable lighting, with a togglable day–night cycle (like Civilization VI).

It's not a mod in the usual sense: it changes no game files, no XML, no Python and no gameplay, and it works alongside
whatever mod you play. It's a single `d3d9.dll` that sits next to the game, its settings file, and a folder of water
textures. It doesn't affect saves or multiplayer, and you can remove it at any time.

Mods can include their own GraphicsUpgrade.ini settings file, to configure their own lighting and other settings such as models that shouldn't
cast shadows. These apply only while that mod is running. See [For modders](#for-modders).

## What it changes

| | Plain BtS | With GraphicsUpgrade |
|---|---|---|
| **Water** | Flat, animated surface | Ripples that reflect the sky, show the sea floor through the waves, and sparkle in the sun |
| **Ship reflections** | None | Ships mirrored in the water |
| **Under the water** | Hulls show through the semi-transparent sea | They still show through, fading out with depth like real shallows |
| **Unit, ship and tree shadows** | Round blobs under units; nothing for trees | Real shadows cast by the sun, weapons and rigging included |
| **Hill and peak shadows** | None | Hills and peaks shade the ground behind them |
| **Building shadows** | Painted on, always pointing the same way | Real shadows from buildings, improvements and resources |
| **Lighting** | BtS's white sun and grey shade | [Without day-night cycle] A warm sun and cool blue shade, as in Colonization; colours adjustable for units, ships, trees, buildings, terrain and rivers |
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
   GraphicsUpgrade\      (the folder, with water_normal.dds and water_env.dds)
   presets\              (optional lighting presets)
   ```

   The zip also has this guide, as `GraphicsUpgrade.md`.

   For Steam, that's usually:

   ```
   C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization IV Beyond the Sword\Beyond the Sword\
   ```

3. Start the game as usual. The first time water comes into view there's a short pause while it loads.

If the folder already has a `d3d9.dll` (ReShade, DXVK...), read [Using with ReShade or DXVK](#using-with-reshade-or-dxvk) first.

**To remove it,** delete `d3d9.dll`, `GraphicsUpgrade.ini`, `GraphicsUpgrade\` and `presets\` from that folder.

## In game

| Keys | What it does                                 |
|---|----------------------------------------------|
| Ctrl+Shift+1 | The water, to compare with BtS default water |
| Ctrl+Shift+2 | Sun shadows                                  |
| Ctrl+Shift+3 | Lighting                                     |
| Ctrl+Shift+4 | Pauses and resumes the day–night cycle       |
| Ctrl+Shift+5 | Sharper distant textures (anisotropic filtering) |
| Ctrl+Shift+/ | Saves a before/after pair of screenshots     |

Ctrl+Shift+/ saves two screenshots from two frames in a row, so every unit, tree and wave is in the same place:
one with GraphicsUpgrade's effects as you have them (`..._on.png`) and one with none of them, plain BtS
(`..._off.png`). They go in `%LOCALAPPDATA%\GraphicsUpgrade\bts\screenshots`.

To use other keys, change them in `GraphicsUpgrade.ini`: `ToggleKey` in the `[water]`, `[shadows]` and `[lighting]`
sections, `LightCycleToggleKey` in `[lighting]` for pausing the day–night cycle, and `CompareKey` in `[general]` for the
screenshots. Each can be a number, a letter, a key name such as `F11` or `Numpad5`, a symbol such as `/`, or `none`.
`KeyModifiers` in `[general]` sets the Ctrl+Shift part. Letters are best avoided: many mods' own keys ignore extra
modifiers, so a letter can set off a mod's command too. No mod we know of uses the number keys or `/`.

## Settings

Everything is in `GraphicsUpgrade.ini`, and each setting has a comment explaining it. Restart the game after editing.
The ones you're most likely to want:

| Setting | What it does |
|---|---|
| `[shadows] Darkness` | How dark shadows are (0 to 1) |
| `[shadows] Softness` | How soft shadow edges are (0 = hard) |
| `[shadows] BuildingShadows` | Real building shadows on (1) or off (0) |
| `[shadows] SunDirection` | Which way the sunlight travels while the day–night cycle is off (while it runs, the sun follows `[lighting] SunMiddayFrom` and the height settings) |
| `[shadows] HillShadowLength` | How long hill and mountain shadows are (1 = true to the terrain's heights) |
| `[shadows] WaterShadow` | How much shadow falls on the water's surface |
| `[water] UnderwaterDepth` | How deep you can see into the water (0 = not at all) |
| `[reflection] Size` | Reflection sharpness: 1024, or 512 to save a little speed |
| `[lighting] Enabled` | GraphicsUpgrade's lighting (1), or BtS's own (0) |
| `[lighting] LightCycleOn` | The day–night cycle on (1) or off (0) |
| `[textures] Anisotropy` | How sharp distant ground, roads and water stay: 2 to 16, or 1 for BtS's own filtering |

### Lighting and day–night

Colours are written as Red,Green,Blue from 0 to 255. You can give each group its own: units, ships and vehicles,
terrain, rivers, trees, buildings, and the sun on the water. The `presets` folder has complete `[lighting]` sections to
paste in place of the one in `GraphicsUpgrade.ini`:

- **`lighting-bts.ini`**: BtS's own lighting, close to how the game looks without GraphicsUpgrade. A good starting point for your own colours.
- **`lighting-colonization.ini`**: Colonization's warm sun and cool blue shade.
- **`lighting-civ6.ini`**: a day–night cycle with Civilization VI's colours, the sun circling the sky once a day.

The day–night cycle runs through a day over `LightChangeCycleTime` seconds: golden hour, sunset, a silvery
moonlit night and dawn. The sun travels round the sky rather than swinging back and forth, so the shadows turn with it.
You can set:
- the colours for each hour
- the compass direction of the midday sun
- how high the sun and moon climb

## For modders

**Your own settings.** Put a `GraphicsUpgrade` folder in your mod's folder (`Mods\<your mod>\GraphicsUpgrade\`) with a
`GraphicsUpgrade.ini` in it. While your mod is running, each setting in that file replaces the same setting in the
player's own `GraphicsUpgrade.ini`; anything you leave out stays as the player has it. List only what you change, under
the usual section names:

```ini
[lighting]
Enabled=1
SunColour=255,220,170
AmbientColour=80,100,90
```

**Your own water textures.** Put `water_normal.dds` (the ripples' normal map) and/or `water_env.dds` (the sky the water
reflects, with the sun's glint in its alpha) in your mod's `Assets\Art\GraphicsUpgrade\`, loose or packed in your
`.FPK` files. Each one is looked for on its own, so you can ship just the one you change. A `GraphicsUpgrade` folder
in your mod's folder still works for these too, and there the normal map is also found under its old name,
`water_001.dds`.

GraphicsUpgrade finds the running mod in two ways:
- from the game's command line (`mod= "Mods\<your mod>"`, the way BtS starts a mod)
- from the `Mod =` line in `CivilizationIV.ini`

The log notes which mod it found.

**Models that shouldn't cast shadows.** `[shadows] NoShadowTextures` lists textures whose models cast no shadow. By
default these are the unit flags. Entries are separated by `;`, and each one is a `.dds` file or a folder of them. Write
them as paths under `Assets`, just as in your XML:

```ini
[shadows]
NoShadowTextures=Art\Units\Flags; Art\Units\Flag\Medallions.dds; Art\Units\MyMarker\marker.dds
```

GraphicsUpgrade looks in your mod's `Assets` first, then the game's (loose files or inside `.FPK` archives), and
recognises the textures in game by their content. If your mod replaces BtS's unit blob shadow
(`Art\Units\01_UnitShadows\unit_shadows.dds`), your version is picked up automatically.

**Water on your models.** `[water] SurfaceTextures` lists textures whose models are water. They keep their own
look and get the water's ripples, sky reflection and sun glint on top, and cast no shadow. The texture's alpha (times
the vertex or material alpha) says where the water is. By default this is the oasis' pool. Entries work as in
`NoShadowTextures`:

```ini
[water]
SurfaceTextures=Art\Terrain\Features\Oasis\oasis_water.dds; Art\Effects\Flood\flood_water.dds
```

**Effects that follow the light.** BtS draws most particle effects (dust, smoke, glows) unlit, so they keep the
same colour by day and by night. `[lighting] LitEffectTextures` lists textures whose effects should follow the light
instead. They then darken and change colour with the day–night cycle and the lighting presets, as the ground under them
does. This applies to unlit, blended effects in the map: additive ones (source alpha or one onto one) and alpha-blended
ones. Multiplying effects, which darken what is behind them, already follow the light through the lit ground, so they
are left as they are. The interface and the Civilopedia are never changed. Entries work as in `NoShadowTextures`, and
`*` means every such effect. By default the list is empty, so fires and magic glows stay as bright at night as by day:

```ini
[lighting]
LitEffectTextures=Art\Effects\Sandstorm_P; Art\Effects\Dust\dust_cloud.dds
```

**Your own terrain lighting.** BtS bakes the terrain's light from `Art\Terrain\Lights\SunLight.nif`, and
GraphicsUpgrade needs that light to re-light the terrain and roads correctly. If your mod has its own `SunLight.nif`
(loose or in an `.FPK`), it is read automatically, and the log notes what was found. The `BakedTerrain*` lines under
`[lighting]` override it.

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

- **Water:** the ships are redrawn into a mirrored reflection. BtS's own water is then drawn with a new water shader,
  reverse engineered from Colonization's. Water on models, such as an oasis' pool, gets the same ripples, sky reflection
  and sun glint drawn over it.
- **Shadows:** units, ships, trees and buildings are redrawn from the sun's point of view into a shadow map. Hill
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

To run the tests (no game needed; a few use the installed BtS and Colonization files and are skipped without them):

```bash
uv run tests/run_tests.py
```

## Credits

GraphicsUpgrade by Deliverator.

The water shader is reverse engineered from the one in Sid Meier's Civilization IV: Colonization. The water textures
(`water_normal.dds`, which is Colonization's `water_001.dds`, and `water_env.dds`) are from Colonization, © Firaxis
Games / 2K; `water_env.dds`'s sun glint has been redrawn to remove a seam. GraphicsUpgrade is an unofficial fan project,
not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.

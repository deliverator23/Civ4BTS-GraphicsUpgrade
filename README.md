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

- Civilization IV: Beyond the Sword 3.19 on Windows.
- A graphics card that runs BtS at its higher graphics settings (pixel shader 2.0).

## Install

1. Download [GraphicsUpgrade.zip](https://github.com/deliverator23/Civ4BTS-GraphicsUpgrade/releases/latest/download/GraphicsUpgrade.zip)
   from the latest release (all versions are on the [Releases](../../releases) page).
2. Copy these from it into your **Beyond the Sword** folder, the one with `Civ4BeyondSword.exe` in it:

   ```
   d3d9.dll
   GraphicsUpgrade.ini
   GraphicsUpgrade\      (the folder, with water_001.dds and water_env.dds)
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

| Keys | Switches on and off                          |
|---|----------------------------------------------|
| Ctrl+Alt+Shift+W | The water, to compare with BtS default water |
| Ctrl+Alt+Shift+S | Sun shadows                                  |
| Ctrl+Alt+Shift+L | Lighting                                     |
| Ctrl+Alt+Shift+P | Pauses and resumes the day–night cycle       |

To use other keys, change `ToggleKey` in that section of `GraphicsUpgrade.ini`. It can be a letter, a key name such as `F11` or
`Numpad5`, or `none`. `KeyModifiers` in `[general]` sets the Ctrl+Alt+Shift part.

## Settings

Everything is in `GraphicsUpgrade.ini`, and each setting has a comment explaining it. Restart the game after editing.
The ones you're most likely to want:

| Setting | What it does |
|---|---|
| `[shadows] Darkness` | How dark shadows are (0 to 1) |
| `[shadows] Softness` | How soft shadow edges are (0 = hard) |
| `[shadows] BuildingShadows` | Real building shadows on (1) or off (0) |
| `[shadows] SunDirection` | Where the sun shines from |
| `[shadows] WaterShadow` | How much shadow falls on the water's surface |
| `[water] UnderwaterDepth` | How deep you can see into the water (0 = not at all) |
| `[reflection] Size` | Reflection sharpness: 512, or 1024 for sharper |
| `[lighting] Enabled` | GraphicsUpgrade's lighting (1), or BtS's own (0) |
| `[lighting] LightCycleOn` | The day–night cycle on (1) or off (0) |

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

That folder can also hold your own water textures (`water_001.dds`, `water_env.dds`).

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

**Your own terrain lighting.** If your mod replaces BtS's `SunLight.nif`, set the `BakedTerrain*` lines under
`[lighting]` to the colours and direction of your light. GraphicsUpgrade needs them to re-light the terrain correctly.

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

## How it works

GraphicsUpgrade sits between the game and Direct3D 9. It changes how BtS's own scene is drawn, frame by frame:

- **Water:** the ships are redrawn into a mirrored reflection. BtS's own water is then drawn with a new water shader,
  reverse engineered from Colonization's.
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

## Credits

GraphicsUpgrade by Deliverator.

The water shader is reverse engineered from the one in Sid Meier's Civilization IV: Colonization. The water textures
(`water_001.dds`, `water_env.dds`) are from Colonization, © Firaxis Games / 2K. GraphicsUpgrade is an unofficial fan
project, not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.

# GraphicsUpgrade for modders

GraphicsUpgrade works with any mod as it is: nothing here is needed for it to run. This guide is for modders who want
to fit it to their mod: their own lighting and water, models that shouldn't cast shadows, effects that follow the
day–night cycle, and the experimental terrain features. Players' settings are in the README (`GraphicsUpgrade.md` in
the download).

## Your mod's own settings

Put a `GraphicsUpgrade` folder in your mod's folder (`Mods\<your mod>\GraphicsUpgrade\`) with a `GraphicsUpgrade.ini`
in it. While your mod is running, each setting in that file replaces the same setting in the player's own
`GraphicsUpgrade.ini`; anything you leave out stays as the player has it. List only what you change, under the usual
section names:

```ini
[lighting]
Enabled=1
SunColour=255,220,170
AmbientColour=80,100,90
```

GraphicsUpgrade finds the running mod in two ways:
- from the game's command line (`mod= "Mods\<your mod>"`, the way BtS starts a mod)
- from the `Mod =` line in `CivilizationIV.ini`

The log (`%LOCALAPPDATA%\GraphicsUpgrade\bts\session.log`) notes which mod it found and whether its
`GraphicsUpgrade.ini` is used. A setting written twice in one file is pointed out there: only the first counts, except
for the lists below, where each line adds to the others.

## Texture lists

Several settings pick out models or effects by the textures they're drawn with. They all work the same way:

- Entries are separated by `;`, or written on more lines of the same setting (each line adds to the others).
- Each entry is a `.dds` file or a folder of them, written as a path under `Assets`, just as in your XML.
- GraphicsUpgrade looks in your mod's `Assets` first, then the game's (loose files or inside `.FPK` archives), and
  recognises the textures in game by their content, so it doesn't matter how the game loads them.
- A list in your mod's file replaces the player's whole list, so repeat any default entries you still want (the unit
  flags in `NoShadowTextures`, the oasis in `SurfaceTextures`, BtS's guide in `AreaBorderTextures`).

```ini
[shadows]
NoShadowTextures=Art\Units\Flags; Art\Units\Flag\Medallions.dds
NoShadowTextures=Art\Units\MyMarker\marker.dds
```

## Shadows

**Models that shouldn't cast shadows.** `[shadows] NoShadowTextures` lists textures whose models cast no shadow:
markers, glows, flags. By default these are the unit flags.

**Your own blob shadow.** If your mod replaces BtS's unit blob shadow (`Art\Units\01_UnitShadows\unit_shadows.dds`),
your version is picked up automatically. With `[shadows] HideBlobShadows=1` (the default) the blobs are hidden while
the units cast real shadows.

**Painted shadows.** BtS's buildings and improvements have shadows painted on the ground. GraphicsUpgrade hides them
while the buildings cast real shadows (`[shadows] BuildingShadows=1`); it recognises them as black textures under the
model. A painted shadow with bright colours in its see-through parts isn't taken for one, and stays.

## Water

**Your own water textures.** Put `water_normal.dds` (the ripples' normal map) and/or `water_env.dds` (the sky the water
reflects, with the sun's glint in its alpha) in your mod's `Assets\Art\GraphicsUpgrade\`, loose or packed in your
`.FPK` files. Each one is looked for on its own, so you can ship just the one you change. A `GraphicsUpgrade` folder
in your mod's folder works for these too, and there the normal map is also found under its old name, `water_001.dds`.
The caustics (`water_caustics1.dds`, `water_caustics2.dds`) can be replaced from that folder.

**More looks for the waves.** More normal maps can go in the same places, as `water_normal_1.dds`,
`water_normal_2.dds` and so on (numbered from 1, without gaps). Each covers 1440 world units (8 plots) before it
repeats, where `water_normal.dds` covers 480. Ctrl+Shift+7 (`[water] NormalMapToggleKey`) cycles through them,
`water_normal.dds` first, and `[water] NormalMap` picks the one the game starts with.

Any of these water normal maps can be DXT5nm instead of plain DXT5: x in the alpha channel and y in green (red and blue
unused), marked with the `DDPF_NORMAL` flag as NVIDIA's texture tools write it. At the same size it keeps the waves'
shape about 2.5 times more accurately.

**Water on your models.** `[water] SurfaceTextures` lists textures whose models are water, such as flooded tiles or
ponds. They keep their own look and get the water's ripples, sky reflection and sun glint on top, and cast no shadow.
The texture's alpha (times the vertex or material alpha) says where the water is. By default this is the oasis' pool:

```ini
[water]
SurfaceTextures=Art\Terrain\Features\Oasis\oasis_water.dds; Art\Effects\Flood\flood_water.dds
```

**Ships and sea units.** Ships are found by how and when BtS draws them (after the terrain, before the water), with or
without a unit shader, so a mod's ships are reflected and show under the water without any setting. Sea resources,
which BtS draws after the water, are recognised as models standing in it.

## Lighting

**Your own terrain lighting.** BtS bakes the terrain's light from `Art\Terrain\Lights\SunLight.nif`, and
GraphicsUpgrade needs that light to re-light the terrain and roads correctly. If your mod has its own `SunLight.nif`
(loose or in an `.FPK`), it is read automatically, and the log notes what was found. The `BakedTerrainSun`,
`BakedTerrainAmbient` and `BakedTerrainDirection` lines under `[lighting]` override it.

**Your own day–night cycle.** Everything under `[lighting]` and `[lighting groups]` can go in your mod's file: the
colours for each hour (`LightCycleSteps`, `LightCycleAmbientSteps`), the midday sun's direction and height
(`SunMiddayFrom`, `SunMiddayHeight`, `SunriseHeight`, `MoonHeight`), and the colours for each group (units, ships and
vehicles, terrain, rivers, trees, buildings, the sun on the water). The colours for each hour can also be painted as a
small picture, `[lighting] LightCycleImage=cycle.png` (PNG or BMP), in your mod's `GraphicsUpgrade` folder: one pixel
wide per hour, the sun's colour along the top row and the ambient's along the second. The `presets` folder in the
download has complete examples.

**Effects that follow the light.** BtS draws most particle effects (dust, smoke, glows) unlit, so they keep the same
colour by day and by night. `[lighting] LitEffectTextures` lists textures whose effects should follow the light
instead. They then darken and change colour with the day–night cycle and the lighting, as the ground under them does.
This applies to unlit, blended effects in the map: additive ones (source alpha or one onto one) and alpha-blended ones.
Multiplying effects, which darken what is behind them, already follow the light through the lit ground, so they are
left as they are. The interface and the Civilopedia are never changed. `*` means every such effect. By default the list
is empty, so fires and magic glows stay as bright at night as by day:

```ini
[lighting]
LitEffectTextures=Art\Effects\Smoke; Art\Effects\Dust\dust_cloud.dds
```

**Models that are part of terrain.** The game adds the light to the terrain's colours but multiplies a model's colours by
it. Under a strongly coloured light such as a red sunset, the same light takes far more colour from the ground than
from a model standing on it, so rocks, cliffs and other models that belong to the terrain can look out of place.
`[lighting] GroundLitTextures` lists textures whose models add the light as the ground does. It applies to models
whose first texture stage is the plain texture times the light:

```ini
[lighting]
GroundLitTextures=Art\Terrain\Features\Rocks\rock_base.dds
```

**Buildings under a bright light.** `[lighting] BuildingLightRange` (1 to 2) lets buildings, improvements and resources
be lit brighter than their textures, as the game lights the ground, so they follow a bright day–night cycle instead of
staying at their texture's colour. The README describes it for players; a mod with a bright light may want to set it.

**Models under the culture borders.** BtS tints the ground inside a civilization's borders, then draws the models on
top, so a model that is part of the landscape stands out of the tint. `[textures] ModelsUnderBorders` lists textures
whose models take the tint instead, the same as the ground around them, as if they were under it. Nothing moves, and
units in front still cover them. Off unless something is listed:

```ini
[textures]
ModelsUnderBorders=Art\Terrain\Features\Crater\crater.dds
```

## Terrain

**Normal maps for your terrain.** With the lighting on, the ground's small bumps are lit by the sun as it moves. By
default they come from each terrain's detail texture (the `<Detail>` in `CIV4ArtDefines_Terrain.xml`), its light and
dark areas read as high and low, and `[textures] DetailBumpiness` sets how strong they are. That works well for
textures like rock or soil. It works less well for a detail texture with light and shadow painted into it, where a
painted shadow would read as a dip. For those, ship a normal map: the same path and name as the detail texture with
`_normal` added, loose or in your `.FPK` files:

```
Art\Terrain\Textures\Detail\DesertDetail.dds
Art\Terrain\Textures\Detail\DesertDetail_normal.dds
```

The normal map is the usual kind (DirectX): red along the texture's width, green up the image, blue out of the ground.
It is used instead of the detail texture's light and dark areas, at `[textures] DetailNormalStrength` (1 = as made).
The log lists which detail textures have one.

**The settler's city-site guide.** `[textures] AreaBorderTextures` lists the guide's textures (BtS's
`Art\Terrain\PlotTextures\AreaBorder.dds` by default). If your mod draws the guide with its own texture, list it there,
with BtS's if your mod still uses that too, so the guide lies on the terrain.

## Experimental terrain features

These change how BtS builds the terrain itself, so they're for mods with their own terrain art. They're experimental
and off by default, and they work only with BtS 3.19's own `Civ4BeyondSword.exe` (Steam's or GOG's). On any other exe,
or one that has been changed, GraphicsUpgrade checks the code it would patch, finds it different, writes nothing, and
the game builds its terrain as BtS does. Everything they do is logged, in lines starting `terrain patch:`,
`terrain tile groups:`, `terrain tiles:`, `terrain land blends:` and `terrain river layer:`.

Put the settings in your mod's own `GraphicsUpgrade.ini`, so they apply only to your mod. Terrain and feature names are
the XML types (`TERRAIN_GRASS`, `FEATURE_FOREST`), separated by commas, semicolons or spaces. A folder is a path under
`Assets` (your mod's, then the game's, loose files before `.FPK` archives), or a full path.

### Finer heightmaps: `[debug] TerrainVertsPerPlot`

BtS's terrain has 17 height samples across each plot, from 16×16 heightmap tiles. `TerrainVertsPerPlot=33` or `65`
gives it 33 or 65, from 32×32 or 64×64 tiles: up to four times finer relief. 17, the default, changes nothing; other
values are refused. `CIV4TerrainSettings.xml` needs no change.

Ship the bigger tiles beside BtS's, under the same names, in `Art\Terrain\heightmap32\` (for 33) or
`Art\Terrain\heightmap64\` (for 65):

```
Ocean1_1.tga ...
flats\flat1_1.tga ...
hills\hill1_1.tga ...
peaks\Peak_1.tga ...
Coasts\CoastTile1_01.tga ...  CoastTile3_0<n>, CoastTile5_0<n>, CoastTile7_0<n>
CoastBlendMasks\CoastScaleMask00.tga ... CoastScaleMask15.tga
```

- Tiles are 8-bit greyscale TGA, (TerrainVertsPerPlot − 1) pixels square, numbered from 1 without gaps: the game reads
  each list until a number is missing.
- Each list is read from the new folder if its first file is there, else from `Art\Terrain\heightmap\` as before. Ship
  every list at the new size, and keep the 16-pixel tiles in `heightmap\` for players without it.
- Coast scale masks smaller than the tiles are resampled to fit (the log says so).
- Rivers' canyons read `Art\Terrain\Routes\Rivers\Canyons.tga` in larger windows, so ship it scaled up 2 times (for 33)
  or 4 times (for 65). Nothing resamples it.
- Culture borders and the settler's city-site guide are laid over the finer terrain, so it doesn't rise through them.

### Tile groups: `TilePicker`, `TileGroups`, `TileGroup1`..`8`

Different terrains can use different heightmap tiles. Each square takes its tiles from the group that most of its four
corner plots belong to.

```ini
[terrain tiles]
TilePicker=1
TileGroups=2
TileGroup2=TERRAIN_DESERT
```

- `TileGroup1` is every land terrain not listed in another group. Water terrains not listed don't count. Ties go to the
  lower group.
- There are no folders per group: each list (ocean, flats, each coast class) is split into `TileGroups` equal blocks in
  file order. With 12 `flat1_` tiles and 3 groups, group 1 has `flat1_1`..`4`, group 2 `flat1_5`..`8`, group 3
  `flat1_9`..`12`. Make each list a multiple of the number of groups: tiles left over are never used.
- Hills and peaks can have variants: `hill1_1`..`15` is one variant (one tile per corner arrangement, in BtS's order),
  `hill1_16`..`30` the next, and so on. Variants are shared out among the groups the same way.
- More coast tiles than BtS has wave models and water transition cells: set `CoastAtlasCells` and `CoastWaveModels` to
  the art's counts for the four coast classes (default `8,8,6,7`), and the extra tiles reuse them in turn.

### Coast blends: `StandardBlendOnLand`, `MixedCoastBlend`

The game picks a coast square's heightmap tile and its terrain's `TextureBlend` entry with the same random pick. So a
`TextureBlend` list exactly as long as that coast class's tile list pairs entry n with tile n: a painted cliff for each
coast shape. Such paired lists would also be used where two land terrains meet, where they don't fit.

- `StandardBlendOnLand=TERRAIN_A, TERRAIN_B`: on squares with no water, those terrains take BtS's standard blend layout
  instead of their paired lists. `StandardBlendOnLand1`..`8` give a terrain a layout of its own:
  `TERRAIN_A: <mask 1>; <mask 2>; ... <mask 14>`, each mask one or more `tile,rotation` entries as in `TextureBlend`.
- `MixedCoastBlend=1` (the default, once `StandardBlendOnLand` is in use): on coast squares with two or more of those
  land terrains, each paints the whole land and is weighted by its share of the square's land corners, so they blend
  across the square instead of meeting in square blocks. Listing `TERRAIN_PEAK` blends the peak layer the same way. It
  needs GraphicsUpgrade's terrain shader (the lighting or the shadows on), and each paired terrain should have a blend
  sheet of its own.
- With more than one tile group, the coast tiles are picked by group but the blends aren't, so the pairing may not hold
  on every coast: test it if you combine them.

### Multi-plot heightmaps: `JigsawPieces1`..`8`

The flat land and water plots of chosen terrains are covered with heightmap pieces made for one or more plots, laid as
one field across the map and added to the base tiles, so dunes or swells run across plots.

```ini
[terrain tiles]
JigsawPieces1Terrains=TERRAIN_DESERT
JigsawPieces1Folder=Art\Terrain\Jigsaw\Dunes
JigsawPieces1Floor=water
```

- `Floor`: what those plots count as when the game picks their base tile: `keep` (default), `water` (a floor to build
  on) or `land`.
- `Height`: grid units per height level of the pieces (the game's heights are 0 to 255); default from the folder's
  `pieces.xml`, else 1.
- The folder holds `pieces.xml` and the pieces:

```xml
<JigsawPieces>
  <fHeightScale>1.5</fHeightScale>
  <Layout>
    <Plots>0,0 1,0 0,1</Plots>     <!-- x east, y north -->
    <Piece>3_L_01.dds</Piece>      <!-- more Pieces with the same Plots are variants -->
    <Piece>3_L_02.dds</Piece>
  </Layout>
  <Layout><Plots>0,0</Plots><Piece>one.dds</Piece></Layout>
</JigsawPieces>
```

- A one-plot layout is required: it fills whatever the bigger ones leave.
- Pieces are uncompressed greyscale DDS: 16-bit (L16) or 8-bit (L8, or the red channel of RGB). A piece runs from the
  centre of the plot beyond its layout on one side to the one beyond it on the other, so a one-plot piece covers 2×2
  plots, centred on its plot. Pixels per plot are the same both ways, at least 4 (64 is a good size). Row 0 is north,
  and the edges must fall to 0.
- Pieces are turned and mirrored to fit, so they must look right every way round. Where pieces overlap, the higher
  wins.

### One stamp per plot: `PlotStamps1`..`8`

Every plot of chosen terrains, or with chosen features, gets one one-plot heightmap stamp on top of what the game built
there, peak included: one mountain shape per mountain plot, say. The folder is laid out as for jigsaw pieces (only
one-plot layouts are used, and stamps aren't turned).

```ini
[terrain tiles]
PlotStamps1Terrains=TERRAIN_PEAK
PlotStamps1Folder=Art\Terrain\Stamps\Mountain
PlotStamps1HideModels=Art\Terrain\Features\Peaks\peak_single.dds
```

- `OnFeatures` instead of `Terrains`: every plot with those features, whatever its terrain. A plot takes one set, by
  feature first.
- `Features`: the first listed feature picks the first piece in `pieces.xml`, the second the second, and so on, so the
  stamp matches the feature's model. Other plots take a piece by place.
- `HideModels`: a texture list; models drawn with these textures aren't drawn (nor their shadows) while the stamps are
  on, for when the stamp replaces a model. Don't list a texture other models share.
- `Height`: as for jigsaw pieces. Stamps can reach the top of the game's height range; the log counts what's clipped.
- `Textures=1`: each piece can have a painting, `<piece>_colour.dds` beside it, drawn on the stamped terrain's own
  layer, so the lightmap, shadows, fog of war and day–night apply. Paintings are DXT5 or 32-bit ARGB, square, a power
  of two (16 to 2048) with all mip levels, all the same size and format, covering the same 2×2 plots as the stamp, with
  straight alpha that falls to 0 well inside the edge. The terrain needs a blend sheet of its own (its `<Path>` in the
  terrain art) and GraphicsUpgrade's terrain shader (the lighting or the shadows on). For `OnFeatures` sets the
  painting goes on the peak layer, so it shows on peak plots.
- `TextureDetail` (0 to 1, default 0): how much of the terrain's detail texture shows through a painting.
- `TextureMultiply=1`: the painting is multiplied by the light instead of added to it, so black stays black at any
  hour.
- `Flatten=1`: the pieces are weights rather than heights, to level a seat for a model laid on the ground by
  `ContourGeometry`, which otherwise shears on uneven ground. Use 16-bit pieces: weight 0 to 1 over 0 to 65535. Where
  the weight is 1 the ground goes to one level, the mean height there, and the ground around it is drawn towards that
  level by the weight. The weight must be 0 before it reaches the edge of a square (squares are bounded by lines through
  the plot centres), so a seat lies wholly inside one square; the log warns if not.

```ini
[terrain tiles]
PlotStamps2OnFeatures=FEATURE_CRATER
PlotStamps2Folder=Art\Terrain\Stamps\CraterSeats
PlotStamps2Flatten=1
```

### River layers: `RiverLayerTerrain`

Adds a terrain's layer to every square a river runs through, whatever the terrain there: sandy floors along rocky
river beds, for example.

```ini
[terrain tiles]
RiverLayerTerrain=TERRAIN_HILL
RiverLayerFirstTile=33
```

- Use a terrain type whose art your mod doesn't otherwise draw (its other blend lists transparent), with a high layer
  order. Re-using an existing type keeps saves as they are.
- Without shape tiles, a square takes blend tile `RiverLayerFirstTile` + arms − 1, where the river's arms on the square
  are North 1, East 2, South 4, West 8: the blend sheet needs 15 tiles from `RiverLayerFirstTile`.
- `RiverLayerCaps=<folder>` (or `RiverLayerDepths`, which wins if both are set) cuts the ground to fit, with 64×64
  8-bit greyscale TGA tiles: `cap<arms>_<n>.tga` for arms 1 to 15, and `mouth`, `mouthr`, `mouthl<arms>_<n>.tga` for a
  river's last square. Caps say the highest the ground may be (255 = no cap); depths say how far to lower it. Each tile
  then has its own blend tile, in the order the files are read; the log lists them. These need `TerrainVertsPerPlot=65`.
- Culture borders bridge shallow cuts rather than dipping into them.

### `HillStamps`

`HillStamps=1` builds hills from multi-plot hill shapes, so ridges run across squares. It's a prototype: it has only
built-in shapes, and a mod can't yet give it its own.

## Developer tools

- **`[debug] TerrainDebug`** (Ctrl+Shift+0 steps through them): 1, the terrain layers drawn opaque; 2, a flat colour per
  layer; 3, the flat colours with each blend sheet's alpha as BtS reads it; 4, the same as GraphicsUpgrade reads it.
  Modes 2 to 4 draw nothing else, on magenta, so gaps and seams in your terrain art show up.
- **`[debug] Traces=1`**: Ctrl+Shift+9 records a frame (every draw, its textures, shaders and state, and a picture of the
  frame) to `%LOCALAPPDATA%\GraphicsUpgrade\bts\traces`. Useful when reporting something GraphicsUpgrade draws wrongly
  in your mod.
- **`[debug] WaterTime`**: stops the water's animation at that many seconds, for screenshots that match.

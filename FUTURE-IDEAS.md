# Future ideas

Ways GraphicsUpgrade could improve Civ4's look further. Each builds on what the hook can already do:

- change a draw's shaders
- redraw scenes from other views into its own textures
- recognise textures by their content
- know the terrain heights, the sun and the time of day
- process the finished frame before it's shown

Roughly in order of how much they'd add for the work.

## The ten ideas

### 1. Sharper distant textures (anisotropic filtering): done

Anisotropic filtering keeps terrain, roads and water sharp towards the horizon at Civ4's tilted camera, instead of blurring them.

- **Setting:** `[textures] Anisotropy` (default 8)
- **Hotkey:** Ctrl+Shift+5
- **Status:** not released yet

### 2. City lights at night

When the day–night cycle reaches night:

- **Windows:** building windows glow, from an emissive term added to the fixed-function building draws (already recoloured by the lighting).
- **Ground:** a warm pool of light spreads around each city, with city positions collected from that frame's building draws and fed to the terrain shader.

Makes night look lived-in rather than just dim.

### 3. Drifting cloud shadows

A soft, scrolling noise texture multiplied into our terrain shader.

- **Movement:** it drifts with a wind direction and follows the sun's direction.
- **Night:** it fades out.

Cheap, and it gives the map life when nothing is moving.

### 4. Ambient occlusion

Soft contact shading where units stand, around buildings and trees, and in valleys.

- **How:** most cards let the game's depth buffer be read back through the `INTZ` depth format, which opens screen-space effects.
- **Catch:** `INTZ` doesn't combine with multisample anti-aliasing (MSAA), so it needs a fallback when MSAA is on.

### 5. Atmosphere: distance haze

Haze and colour shift towards the horizon, tinted by the time of day: golden at dusk, blue at night.

- **How:** the depth readback from idea 4, or the terrain's own distance in the terrain shader.
- **Effect:** a sense of scale when zoomed out.

### 6. Shoreline and shallow-water detail

The terrain heights give the distance to shore and the water depth. From those:

- **Foam:** bands that follow the actual coastline.
- **Caustics:** light patterns shimmering on the sea floor in shallow water.
- **Glint:** a sun glint that follows the cycle's sun, not the fixed spot in `water_env.dds`.

### 7. Animated fog of war

Slowly drifting cloud over unexplored and out-of-sight tiles, replacing the flat grey: the fog-of-war texture modulated by noise in the terrain shader.

### 8. Colour grading and better bloom

A final full-screen pass with a lookup-table colour grade before the frame is shown.

- **Presets:** natural, vivid (like Civ VI), aged parchment map, or a mod's own through its GraphicsUpgrade.ini.
- **Bloom:** BtS's bloom replaced with a softer modern one.

### 9. Seasons and weather

- **Snow:** by latitude, or on a winter cycle like the day–night one, blended into the terrain shader by height and slope.
- **Precipitation:** optional rain or snow particles.
- **Wet ground:** a darker, wet look during rain.

The biggest item here, but very distinctive.

### 10. Photo mode

- **Interface:** hide it reliably (BtS's own Hide Interface is hard to get out of).
- **Animation:** freeze it and the light cycle.
- **Capture:** two to four times screen resolution, by rendering the frame in tiles.

Builds on the before/after screenshots.

## Smaller ideas

- **Rim lighting on units,** so they read better at night.
- **Normal-mapped terrain,** per pixel from the height data, for crisper hill and mountain relief.
- **A sky backdrop** where the camera's tilt shows the horizon.

## Open threads

- **Colonization's shore waves:**
  - **Test files:** Colonization's 31 wave files are in `CustomAssets\art\terrain\waves`, with whitened wave textures.
  - **Tuning:** settle on how white the foam should be.
  - **Night:** the waves are drawn unlit, so they need the same night darkening as the roads.
  - **Release:** decide how to ship them (copied from the player's Colonization install, or with credit like the water textures).
- **Ship sails staying white at night:** some ships, not the galleon. Needs a trace with an affected ship in view at night.
- **Civilopedia previews keep BtS's water:** a known limitation.
- **The game hung on exit once,** stuck while shutting down, which locked the DLL. Watch for it happening again.

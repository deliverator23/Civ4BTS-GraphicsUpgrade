# Future ideas

Ways GraphicsUpgrade could improve Civ4's look further. Each builds on what the hook can already do:

- change a draw's shaders
- redraw scenes from other views into its own textures
- recognise textures by their content
- know the terrain heights, the sun and the time of day
- process the finished frame before it's shown

Roughly in order of how much they'd add for the work.

## Ideas

### 1. Sharper distant textures (anisotropic filtering): done

Anisotropic filtering keeps terrain, roads, water, units and buildings sharp towards the horizon at Civ4's tilted
camera, instead of blurring them.

- **Setting:** `[textures] Anisotropy` (default 8)
- **Hotkey:** Ctrl+Shift+5
- **Status:** released in 1.0.4

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

### 4. Atmosphere: distance haze

Haze and colour shift towards the horizon, tinted by the time of day: golden at dusk, blue at night.

- **How:** the game's depth buffer read back through the `INTZ` depth format (most cards have it, though not with
  multisample anti-aliasing), or the terrain's own distance in the terrain shader.
- **Effect:** a sense of scale when zoomed out.

### 5. Shoreline and shallow-water detail: done

The terrain heights give the distance to shore and the water depth. From those:

- **Caustics:** done. Light patterns shimmering on the sea floor in shallow water, from the shipped caustic
  textures (`[water] Caustics`).
- **Glint:** done. The sun glint follows the cycle's sun, and the moon at night, instead of staying at the fixed spot
  in `water_env.dds`, mirrored to the far side of the view so it stays visible (`[water] GlintFollowsSun`).
- **Waves:** done. The waves' faces are lit by the sun or moon, with sparkle around the glint (`[water] WaveShading`).
- **Coast waves:** done. Drawn with our own wave shader: the foam takes the light (`[water] FoamLight`) and runs up the
  beach over the terrain.
- **Rivers:** done. The glint off the rivers (`[water] RiverGlint`), and ripples that sway along the channel
  (`[water] RiverShimmer`).
- **Foam bands:** tried and dropped. Rolling bands of foam, then granular foam along the waterline: neither looked
  right at Civ4's scale.

Status: released in 1.0.4.

### 6. Animated fog of war

Slowly drifting cloud over unexplored and out-of-sight tiles, replacing the flat grey: the fog-of-war texture modulated by noise in the terrain shader.

### 7. Colour grading and better bloom

A final full-screen pass with a lookup-table colour grade before the frame is shown.

- **Presets:** natural, vivid, aged parchment map, or a mod's own through its GraphicsUpgrade.ini.
- **Bloom:** BtS's bloom replaced with a softer modern one.

### 8. Photo mode

- **Interface:** hide it reliably (BtS's own Hide Interface is hard to get out of).
- **Animation:** freeze it and the light cycle.
- **Capture:** two to four times screen resolution, by rendering the frame in tiles.

Builds on the before/after screenshots.

## Smaller ideas

- **Rim lighting on units,** so they read better at night.
- **Normal-mapped terrain:** done. The terrain is lit per pixel from its heights, and in 1.0.4 its small bumps catch
  the sun too (`[textures] DetailNormals`).
- **A sky backdrop** where the camera's tilt shows the horizon.

## Open threads

- **Ship sails staying white at night:** some ships, not the galleon. Needs a trace with an affected ship in view at night.
- **Civilopedia previews keep BtS's water:** a known limitation.
- **The game hung on exit once,** stuck while shutting down, which locked the DLL. Watch for it happening again.

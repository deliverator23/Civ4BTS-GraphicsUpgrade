// BtS terrain with sun shadows and re-lighting.
//
// Draws a BtS terrain layer exactly as BtS's own Terrain_SplatTile "TerrainShader" does (same inputs, same constant
// registers, same output), and darkens it by the sun-shadow texture built from the previous frame's casters. Because
// the registers match BtS's, the game's constants are used as they are; only mtxShadow (c20-c23), f4Shadow (PS c10)
// and the textures on s4-s6 are added.
//
// BtS's shading (from its compiled shader): colour = (base + detail + lightmap - 1) * fog of war, alpha = base alpha.
// With the lighting on (c15.x = 1), the lightmap is re-lit with the INI colours first. BtS bakes it at half strength,
// texel = (ambient + sun x amount) / 2, so amount = (2 x texel - baked ambient) / baked sun (averaged over the
// channels), and the texel becomes (ambient' + sun' x amount) / 2.
// Per pixel, when the sun's direction differs from the one BtS baked with: amount += N . L' - N . L_baked, with N from
// the terrain normal map (s5; built from the terrain's own vertex buffers). At BtS's direction that adds
// nothing, so the terrain stays exactly as baked; as the sun moves, the hill shading follows it.
// Detail normals ([terrain] DetailNormals): the ground's small bumps, tiled like the detail texture. They tilt that
// normal, and amount += N_detail . L' - N . L': the bumps are lit by the sun as it is now. They come from a normal
// map shipped with the detail texture (s7, tangent space: red along +u, green along -v), or else from the detail
// texture's own brightness, read as height (two more samples, one texel along u and v).
// Built a second time with BLENDNORMALS (Shader Model 3), for every terrain draw the card allows: the blend sheet
// (BtS's base texture: 4 tiles across, the engine's texture coordinates running to half a texel inside the square's
// tile) read inside its tile at every mip (c23): the coordinates clamped a texel of the next coarser mip in from the
// tile's edges, read with the unclamped coordinates' derivatives, so filtering never takes colour or alpha from the
// neighbouring tile in the sheet (thin lines along square edges otherwise, where neighbouring tiles differ). The tile
// and the clamped position come from the coordinates at the centroid (the part of the pixel the triangle covers): with
// multisampling, a pixel on a square's edge is shaded at its centre for each square's triangle, and the centre can lie
// just outside a triangle, so its coordinates run on into the next tile in the sheet (a faint line of that tile's colour
// and alpha along the square's edge). The derivatives still come from the centre's. And the
// blend sheet's own normal map too (s8, a terrain's
// <Path> with _normal before .dds; DetailNormals::ForBlend), read at the sheet's texture coordinates, so with the tile
// and the turn the engine gave this square. The turn is in the coordinates, so which way the sheet's u and v run on the
// ground is read from how they and the position change across the pixel. Added to the ground's normal as the detail
// normals are, x c22.x.

float4x4 mtxWorldViewProj : register(c0);
float4x3 mtxWorld : register(c4);
float4x2 mtxFOW : register(c7);
float4x2 mtxLightmap : register(c9);
float fDetailTexScaling : register(c11);
float4x4 mtxShadow : register(c20);  // world -> shadow texture (projective)

sampler2D TerrainBase : register(s0);
sampler2D TerrainFOWar : register(s1);
sampler2D TerrainLightmap : register(s2);
sampler2D TerrainDetail : register(s3);
sampler2D ShadowMap : register(s4);  // .r = light factor from objects' shadows (1 = lit), shadow_blur.hlsl
float4 f4Shadow : register(c10);     // x = darkness for hill shadows, z = 1: objects' shadows on (s4 bound)
float4 f4BakedAmbient : register(c11);
float4 f4BakedSunInv : register(c12);  // 1 / baked sun colour
float4 f4Ambient : register(c13);
float4 f4Sun : register(c14);
float4 f4Relight : register(c15);      // x = 1: re-light the lightmap with c13/c14
float4 f4LightNew : register(c16);     // xyz: towards the INI sun (unit)
float4 f4LightBaked : register(c17);   // xyz: towards the sun BtS baked with (unit)
float4 f4NormalMap : register(c18);    // x = 1 / (spacing x cells), y = 0.5 / cells, z = 1: use the normal map,
                                       // w = 1: hill shadows (s6)
float4 f4HillGrid : register(c19);     // world xy -> hill mask uv: x = scale, yz = offset
sampler2D TerrainNormals : register(s5);  // rgb = normal x 0.5 + 0.5, a = 1 where known
sampler2D HillShadows : register(s6);     // a = shaded by hills (0..1)
float4 f4DetailAxes : register(c20);      // xy: world direction of +u in the detail texture, zw: of +v (unit)
float4 f4DetailNormal : register(c21);    // x = strength of s7's normal map, y = strength of the normals made from
                                          // the detail texture (0 = none), zw = one detail texel in uv
sampler2D DetailNormals : register(s7);   // a normal map for the detail texture, or nothing (f4DetailNormal.x = 0)
#ifdef BLENDNORMALS
sampler2D BlendNormals : register(s8);    // the blend sheet's normal map (red along +u, green along -v)
float4 f4BlendNormal : register(c22);     // x = strength, y = 1: the map is DXT5nm (x in alpha, y in green)
float4 f4Sheet : register(c23);           // the blend sheet: xy = its size in texels; w = 1: read inside the tile
float4 f4Debug : register(c24);           // [debug] TerrainDebug 4: w = 1, a flat colour (rgb) with the sheet's alpha
// [terrain tiles] MixedCoastBlend: on a coast square of two or more land terrains with paired lists, each of those
// layers paints the whole land (terrain_sets.h, BlendSquare); here each is weighted by its share of the square's land
// corners. The plots (s9): r = terrain + 1, g = 1 where water, b = the paired land terrain's rank by layer order (0:
// none), a = 1 where the square with this plot at its south-west corner blends.
sampler2D MixPlots : register(s9);
float4 f4Mix : register(c25);        // x = this layer's terrain + 1, y = its rank, w = 1: weight it
float4 f4MixToPlot : register(c26);  // world xy -> plot coordinates (plot centres at whole numbers): x = scale, yz = offset
float4 f4MixSize : register(c27);    // the map: xy = its size in plots, zw = 1 where it wraps

// Plot i's data (wrapped onto the map); off the map, water (as the heightmap counts it).
float4 MixPlot(float2 i)
{
    float2 s = f4MixSize.xy;
    float2 j = lerp(i, i - s * floor(i / s), f4MixSize.zw);
    if (j.x < 0 || j.y < 0 || j.x > s.x - 1 || j.y > s.y - 1)
        return float4(0, 1, 0, 0);
    return tex2Dlod(MixPlots, float4((j + 0.5) / s, 0, 0));
}

// This layer's weight at world xy (at: the same point taken inside the triangle, which picks the square). Corners SW,
// SE, NW, NE, bilinear between them. Drawn one over another in layer order, the lowest paired terrain whole and each
// higher one at its share of the paired land among it and those below, every terrain ends up weighted by its share of
// the paired land corners. Along an edge only its two corners count, so the squares either side agree.
float MixWeight(float2 world, float2 at)
{
    float2 i0 = floor(at * f4MixToPlot.x + f4MixToPlot.yz);
    float2 f = saturate(world * f4MixToPlot.x + f4MixToPlot.yz - i0);
    float4 c0 = MixPlot(i0), c1 = MixPlot(i0 + float2(1, 0)), c2 = MixPlot(i0 + float2(0, 1)), c3 = MixPlot(i0 + 1);
    if (c0.a < 0.5)
        return 1;  // not a blended square
    float4 bw = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    float4 land = 1 - step(0.5, float4(c0.g, c1.g, c2.g, c3.g));
    float4 terrain = floor(float4(c0.r, c1.r, c2.r, c3.r) * 255 + 0.5);
    float4 rank = floor(float4(c0.b, c1.b, c2.b, c3.b) * 255 + 0.5) * land;
    float4 own = (1 - step(0.5, abs(terrain - f4Mix.x))) * land;
    float4 lower = step(0.5, rank) * (1 - step(f4Mix.y - 0.5, rank));
    if (dot(own, 1) == 0 || dot(lower, 1) == 0)
        return 1;  // not at this square's land corners (an overlay), or the lowest paired terrain: drawn whole
    return dot(bw, own) / max(dot(bw, own + lower), 1e-5);
}

// The sheet's texture coordinates kept inside this pixel's tile (at: the coordinates at the centroid, inside the
// triangle; uv: at the pixel's centre, for the derivatives): one texel of the next coarser mip than the widest
// footprint reads in from its edges (that covers bilinear, trilinear and anisotropic filtering's reach). The
// derivatives are per triangle, so they don't jump where squares (tiles) meet.
float2 SheetCoords(float2 uv, float2 at)
{
    float tilePx = max(f4Sheet.x * 0.25, 1);
    float2 tiles = float2(4, max(f4Sheet.y / tilePx, 1));
    float2 t = floor(at * tiles);
    float2 px = uv * f4Sheet.xy;
    float lod = max(0, log2(max(length(ddx(px)), length(ddy(px)))));
    float inset = min(0.5, exp2(ceil(lod)) / tilePx);
    return lerp(uv, (t + clamp(at * tiles - t, inset, 1 - inset)) / tiles, f4Sheet.w);
}
#endif

struct VSOut
{
    float4 pos : POSITION;
    float2 base : TEXCOORD0;
    float2 fow : TEXCOORD1;
    float2 lightmap : TEXCOORD2;
    float2 detail : TEXCOORD3;
    float4 shadow : TEXCOORD4;
    float2 worldXY : TEXCOORD5;
#ifdef BLENDNORMALS
    float2 baseAt : TEXCOORD6;   // the base coordinates again, read at the centroid
    float2 worldAt : TEXCOORD7;  // and the world position (MixWeight's square)
#endif
};

VSOut VSMain(float3 pos : POSITION, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1)
{
    VSOut o;
    float4 p = float4(pos, 1);
    o.pos = mul(p, mtxWorldViewProj);
    float4 world = float4(mul(p, mtxWorld), 1);
    o.fow = mul(world, mtxFOW);
    o.lightmap = mul(world, mtxLightmap);
    o.detail = uv1 * fDetailTexScaling;
    o.base = uv0;
#ifdef BLENDNORMALS
    o.baseAt = uv0;
    o.worldAt = world.xy;
#endif
    o.shadow = mul(world, mtxShadow);
    o.worldXY = world.xy;
    return o;
}

float4 PSMain(float2 base : TEXCOORD0, float2 fow : TEXCOORD1, float2 lightmap : TEXCOORD2, float2 detail : TEXCOORD3,
              float4 shadow : TEXCOORD4, float2 worldXY : TEXCOORD5
#ifdef BLENDNORMALS
              , float2 baseAt : TEXCOORD6_centroid, float2 worldAt : TEXCOORD7_centroid
#endif
              ) : COLOR
{
#ifdef BLENDNORMALS
    float2 sheetUV = SheetCoords(base, baseAt);
    float4 b = tex2Dgrad(TerrainBase, sheetUV, ddx(base), ddy(base));
    if (f4Mix.w > 0)
        b.a *= MixWeight(worldXY, worldAt);
#else
    float4 b = tex2D(TerrainBase, base);
#endif
    float3 lm = tex2D(TerrainLightmap, lightmap).rgb;
    float amount = dot((2 * lm - f4BakedAmbient.rgb) * f4BakedSunInv.rgb, 1.0 / 3);
    float2 grid = worldXY * f4NormalMap.x + f4NormalMap.y;
    float4 nm = tex2D(TerrainNormals, grid);
    float3 n = nm.rgb * 2 - 1;
    float known = f4NormalMap.z * nm.a;
    amount += known * (dot(n, f4LightNew.xyz) - dot(n, f4LightBaked.xyz));
    // Detail normals: a slope in the detail texture's tangent space (x along +u, y along -v), into the world.
    float3 d = tex2D(TerrainDetail, detail).rgb;
    const float3 kLuma = float3(0.299, 0.587, 0.114);
    float h = dot(d, kLuma);
    float2 made = float2(h - dot(tex2D(TerrainDetail, detail + float2(f4DetailNormal.z, 0)).rgb, kLuma),
                         dot(tex2D(TerrainDetail, detail + float2(0, f4DetailNormal.w)).rgb, kLuma) - h);
    float2 slope = f4DetailNormal.x * (tex2D(DetailNormals, detail).rg * 2 - 1) + f4DetailNormal.y * made;
    float3 flat = lerp(float3(0, 0, 1), n, known);
    float2 worldSlope = slope.x * f4DetailAxes.xy - slope.y * f4DetailAxes.zw;
#ifdef BLENDNORMALS
    // The sheet's u and v directions on the ground: d(world)/d(uv) from the screen derivatives of both. A tile is square
    // on the ground, so u and v come out as long as each other and at right angles, except where the pixel's
    // neighbours are on another square (the coordinates jump to another tile there): those pixels go without.
    float2 du = ddx(base), dv = ddy(base), px = ddx(worldXY), py = ddy(worldXY);
    float det = du.x * dv.y - dv.x * du.y;
    float safe = abs(det) > 1e-12 ? det : 1e-12;
    float2 pu = (px * dv.y - py * du.y) / safe, pv = (py * du.x - px * dv.x) / safe;
    float lu = max(length(pu), 1e-6), lv = max(length(pv), 1e-6);
    float square = abs(det) > 1e-12 && abs(lu - lv) < 0.2 * max(lu, lv) && abs(dot(pu, pv)) < 0.2 * lu * lv ? 1 : 0;
    float4 sheetTexel = tex2Dgrad(BlendNormals, sheetUV, ddx(base), ddy(base));
    float2 sheet = (lerp(sheetTexel.rg, sheetTexel.ag, f4BlendNormal.y) * 2 - 1) * f4BlendNormal.x * square;
    worldSlope += sheet.x * pu / lu - sheet.y * pv / lv;
#endif
    float3 bumped = normalize(flat + float3(worldSlope, 0));
    amount = max(0, amount + dot(bumped, f4LightNew.xyz) - dot(flat, f4LightNew.xyz));
    lm = lerp(lm, saturate(0.5 * (f4Ambient.rgb + f4Sun.rgb * amount)), f4Relight.x);
    float3 colour = b.rgb + d + lm - 1;
    // BtS's output is clamped to 1 before blending; clamp first so bright ground darkens by the same fraction.
    float3 lit = saturate(tex2D(TerrainFOWar, fow).rgb * colour);
    // Objects' shadows and hills' shadows: the darker of the two (no double darkening where they overlap).
    float objects = lerp(1, tex2Dproj(ShadowMap, shadow).r, f4Shadow.z);
    float hills = 1 - f4Shadow.x * f4NormalMap.w * tex2D(HillShadows, worldXY * f4HillGrid.x + f4HillGrid.yz).a;
#ifdef BLENDNORMALS
    if (f4Debug.w > 0)
        return float4(f4Debug.rgb, b.a);
#endif
    return float4(lit * min(objects, hills), b.a);
}

// BtS's coast wave shader (Civ4Wave.fx, Wave_Shader pass P0: vs_1_1, ps_1_1), with the same inputs, constant
// registers and samplers, so the game's own settings for each wave draw stand as they are. Two additions: an opacity
// over time, and the light: BtS draws the foam unlit, the same white at noon, at sunset and at night; PS c0 scales it
// by the change of light (Lighting::DecalLight, as the roads take it; 1 = as drawn).
//
// A coast wave is a strip of foam: the crest texture (DecalBase) slides across it, blended over the base texture
// (WaveBase, transparent in Colonization's and BtS's art) by the crest's alpha; the strip's vertex opacity and the fog
// of war apply. The base layer has its own animated offset, which nothing shows; a wave model can use it to carry an
// opacity instead: an offset of 10 + opacity (or its negative; the texture repeats every 1, so a whole number of
// repeats samples the same) multiplies the foam's opacity, so the wave can fade in and out over its cycle, whatever
// part of the strip the crest is on. Any other offset (BtS's own waves) leaves it as the game draws it.
//
// Built a second time with DRAPE defined (Shader Model 3, a vertex texture): the waves draped over the terrain, so
// the foam runs up the beach instead of vanishing under it. Each vertex reads the terrain's height where it is (the
// height grid, bilinear between its four nearest points) and, where the ground is above the wave, is lifted onto it
// (plus a little, so it isn't lost in the ground), then placed with the camera's view-projection.

float4 mtxWorldViewProj[4] : register(c0);
float4 mtxWorld[3] : register(c4);
float4 mtxFOW[2] : register(c7);
float4 mtxBaseTexture[2] : register(c9);
float4 mtxDecalTexture[2] : register(c11);
#ifdef DRAPE
float4 mtxViewProj[4] : register(c13);  // world -> clip (rows), for the draped positions
float4 f4Drape : register(c17);          // x = 1 / the height grid's spacing, y = 1 / its cells, z = lift above ground
sampler2D HeightMap : register(s0);      // a vertex texture: the terrain's heights (unknown = -10000)
#endif

float4 f4Light : register(c0);  // rgb: the change of light (1 = none)

sampler2D WaveBase : register(s0);
sampler2D DecalBase : register(s1);
sampler2D Fog : register(s2);

struct VSIn
{
    float4 pos : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : POSITION;
    float4 colour : COLOR0;
    float2 base : TEXCOORD0;
    float2 crest : TEXCOORD1;
    float2 fog : TEXCOORD2;
    float opacity : TEXCOORD3;  // the base layer's offset: 10 + opacity, or anything else for none
};

VSOut VSMain(VSIn i)
{
    VSOut o;
#ifndef DRAPE
    o.pos = float4(dot(i.pos, mtxWorldViewProj[0]), dot(i.pos, mtxWorldViewProj[1]), dot(i.pos, mtxWorldViewProj[2]),
                   dot(i.pos, mtxWorldViewProj[3]));
#endif
    float4 uv = float4(i.uv, 1, 1);  // as BtS's: (u, v, 1, 1) against each texture matrix row
    o.base = float2(dot(uv, mtxBaseTexture[0]), dot(uv, mtxBaseTexture[1]));
    o.crest = float2(dot(uv, mtxDecalTexture[0]), dot(uv, mtxDecalTexture[1]));
    float4 world = float4(dot(i.pos, mtxWorld[0]), dot(i.pos, mtxWorld[1]), dot(i.pos, mtxWorld[2]), 1);
    o.fog = float2(dot(world, mtxFOW[0]), dot(world, mtxFOW[1]));
#ifdef DRAPE
    // The ground here: grid points are at whole multiples of the spacing (texel centres), bilinear between the four.
    float2 cell = world.xy * f4Drape.x;
    float2 corner = floor(cell), t = cell - corner;
    // Wrapped here, not by the sampler: the grid wraps (the map is centred on 0, so half of it is at negative cells),
    // and a vertex texture read past 0 clamped to the edge on Iain's card (borders west and south of the map's
    // middle read the wrong ground and weren't lifted; trace 2026-11-11 00:06)
    float2 at = frac((corner + 0.5) * f4Drape.y), next = frac(at + f4Drape.y);
    float h00 = tex2Dlod(HeightMap, float4(at, 0, 0)).r;
    float h10 = tex2Dlod(HeightMap, float4(next.x, at.y, 0, 0)).r;
    float h01 = tex2Dlod(HeightMap, float4(at.x, next.y, 0, 0)).r;
    float h11 = tex2Dlod(HeightMap, float4(next, 0, 0)).r;
    float ground = lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y);  // unknown ground: far below, no change
    world.z = max(world.z, ground + f4Drape.z);
    o.pos = float4(dot(world, mtxViewProj[0]), dot(world, mtxViewProj[1]), dot(world, mtxViewProj[2]),
                   dot(world, mtxViewProj[3]));
#endif
    o.colour = i.colour;
    // The base offset: the constant part of its first row (the base texture isn't scaled across, so it's the offset).
    o.opacity = mtxBaseTexture[0].z + mtxBaseTexture[0].w;
    return o;
}

float4 PSMain(VSOut i) : COLOR
{
    float4 base = tex2D(WaveBase, i.base);
    float4 crest = tex2D(DecalBase, i.crest);
    float4 fog = tex2D(Fog, i.fog);
    float4 c = lerp(base, crest, crest.a);
    c.a *= i.colour.a;
    c.rgb *= fog.rgb * f4Light.rgb;
    float marker = abs(i.opacity);
    if (marker > 9.5)
        c.a *= saturate(marker - 10);
    return c;
}

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
                                       // w = 1: hill shadows (s6, same grid)
sampler2D TerrainNormals : register(s5);  // rgb = normal x 0.5 + 0.5, a = 1 where known
sampler2D HillShadows : register(s6);     // a = shaded by hills (0..1)

struct VSOut
{
    float4 pos : POSITION;
    float2 base : TEXCOORD0;
    float2 fow : TEXCOORD1;
    float2 lightmap : TEXCOORD2;
    float2 detail : TEXCOORD3;
    float4 shadow : TEXCOORD4;
    float2 worldXY : TEXCOORD5;
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
    o.shadow = mul(world, mtxShadow);
    o.worldXY = world.xy;
    return o;
}

float4 PSMain(float2 base : TEXCOORD0, float2 fow : TEXCOORD1, float2 lightmap : TEXCOORD2, float2 detail : TEXCOORD3,
              float4 shadow : TEXCOORD4, float2 worldXY : TEXCOORD5) : COLOR
{
    float4 b = tex2D(TerrainBase, base);
    float3 lm = tex2D(TerrainLightmap, lightmap).rgb;
    float amount = dot((2 * lm - f4BakedAmbient.rgb) * f4BakedSunInv.rgb, 1.0 / 3);
    float2 grid = worldXY * f4NormalMap.x + f4NormalMap.y;
    float4 nm = tex2D(TerrainNormals, grid);
    float3 n = nm.rgb * 2 - 1;
    amount += f4NormalMap.z * nm.a * (dot(n, f4LightNew.xyz) - dot(n, f4LightBaked.xyz));
    lm = lerp(lm, saturate(0.5 * (f4Ambient.rgb + f4Sun.rgb * amount)), f4Relight.x);
    float3 colour = b.rgb + tex2D(TerrainDetail, detail).rgb + lm - 1;
    // BtS's output is clamped to 1 before blending; clamp first so bright ground darkens by the same fraction.
    float3 lit = saturate(tex2D(TerrainFOWar, fow).rgb * colour);
    // Objects' shadows and hills' shadows: the darker of the two (no double darkening where they overlap).
    float objects = lerp(1, tex2Dproj(ShadowMap, shadow).r, f4Shadow.z);
    float hills = 1 - f4Shadow.x * f4NormalMap.w * tex2D(HillShadows, grid).a;
    return float4(lit * min(objects, hills), b.a);
}

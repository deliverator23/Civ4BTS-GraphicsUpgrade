// BtS-style terrain for the shadow tests: the vertex inputs, constant registers, sampler names and shading of BtS's
// terrain shader (mtxWorldViewProj c0, mtxWorld c4, mtxFOW c7, mtxLightmap c9, fDetailTexScaling c11; TerrainBase s0,
// TerrainFOWar s1, TerrainLightmap s2, TerrainDetail s3; colour = (base + detail + lightmap - 1) * fog of war).
// The proxy should swap in its shadowed copy, which must give the same colour where there's no shadow.
float4x4 mtxWorldViewProj : register(c0);
float4x3 mtxWorld : register(c4);
float4x2 mtxFOW : register(c7);
float4x2 mtxLightmap : register(c9);
float fDetailTexScaling : register(c11);

#ifdef OLD_NAMES
#define TerrainBase TerrainBase0  // older BtS builds' name
#endif
sampler2D TerrainBase : register(s0);
sampler2D TerrainFOWar : register(s1);
sampler2D TerrainLightmap : register(s2);
sampler2D TerrainDetail : register(s3);

struct VSOut
{
    float4 pos : POSITION;
    float2 base : TEXCOORD0;
    float2 fow : TEXCOORD1;
    float2 lightmap : TEXCOORD2;
    float2 detail : TEXCOORD3;
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
    return o;
}

float4 PSMain(float2 base : TEXCOORD0, float2 fow : TEXCOORD1, float2 lightmap : TEXCOORD2, float2 detail : TEXCOORD3)
    : COLOR
{
    float4 b = tex2D(TerrainBase, base);
    float3 colour = b.rgb + tex2D(TerrainDetail, detail).rgb + tex2D(TerrainLightmap, lightmap).rgb - 1;
    return float4(tex2D(TerrainFOWar, fow).rgb * colour, b.a);
}

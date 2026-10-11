// BtS-style water for the water tests: the vertex inputs and constant/sampler names of BtS's Water.fx
// (mtxWorldViewProj c0, mtxWorld c4, mtxWaterTextureMat; TEXCOORD1 coast fade, TEXCOORD2 fog of war; CoastFade s2,
// Fog s3). The pixel shader outputs magenta, so any other colour means the proxy drew its own water instead.
float4x4 mtxWorldViewProj : register(c0);
float4x3 mtxWorld : register(c4);
float3x3 mtxWaterTextureMat : register(c7);
sampler2D CoastFade : register(s2);
sampler2D Fog : register(s3);

struct VSOut { float4 pos : POSITION; float2 t0 : TEXCOORD0; float2 t2 : TEXCOORD2; float2 t3 : TEXCOORD3; };

VSOut VSMain(float3 pos : POSITION, float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2)
{
    VSOut o;
    o.pos = mul(float4(pos, 1), mtxWorldViewProj);
    o.t0 = mul(float4(pos, 1), mtxWorld).xy + mul(pos, mtxWaterTextureMat).xy;
    o.t2 = uv1;
    o.t3 = uv2;
    return o;
}

float4 PSMain(float2 t0 : TEXCOORD0, float2 t2 : TEXCOORD2, float2 t3 : TEXCOORD3) : COLOR
{
    return float4(1, 0, 1, 1) + 0.0001 * (tex2D(CoastFade, t2) + tex2D(Fog, t3));
}

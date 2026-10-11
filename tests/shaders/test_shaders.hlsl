// Minimal shaders for the proxy tests: a transformed triangle with one texture.
float4x4 mtxWorldViewProj : register(c0);
float4 f4Tint : register(c0);
sampler2D BaseMap : register(s0);

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOut o;
    o.pos = mul(float4(pos, 1), mtxWorldViewProj);
    o.uv = uv;
    return o;
}

float4 PSMain(float2 uv : TEXCOORD0) : COLOR
{
    return tex2D(BaseMap, uv) * f4Tint;
}

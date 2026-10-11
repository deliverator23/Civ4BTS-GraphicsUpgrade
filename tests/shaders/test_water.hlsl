// Water-style vertex shader for the reflection test: the constant names the proxy uses to spot the water draw
// (mtxWaterTextureMat, as in BtS Water.fx) and read the plane height (mtxWorld translation z).
float4x4 mtxWorldViewProj : register(c0);
float4x3 mtxWorld : register(c4);
float3x3 mtxWaterTextureMat : register(c7);

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float3 pos : POSITION)
{
    VSOut o;
    o.pos = mul(float4(pos, 1), mtxWorldViewProj);
    o.uv = mul(float4(pos, 1), mtxWorld).xy + mul(pos, mtxWaterTextureMat).xy;
    return o;
}

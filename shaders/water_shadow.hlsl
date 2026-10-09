// Sun shadows on BtS's own water (when the reflective, dynamic water is off; it takes them in its own shader).
//
// Drawn right after a water draw, with the same geometry and BtS's water constants still
// in place (Water_Shader: mtxWorldViewProj c0-c3, mtxWorld c4-c6). It multiplies what's on screen by the shadow's
// light factor at the surface, scaled by [shadows] WaterShadow, so shadows lie on the water as well as on the sea
// floor seen through it. The shadow lookup is the terrain's: VS c20-c23 = world -> shadow texture.
float4x4 mtxWorldViewProj : register(c0);
float4x4 mtxWorld : register(c4);  // only c4-c6 are BtS's (the three output rows)
float4x4 mtxShadow : register(c20);
float4 f4WaterShadow : register(c0);  // PS: x = how much of the shadow the surface takes (WaterShadow)
sampler2D ShadowMap : register(s0);   // .r = light factor from objects' shadows

struct VSOut
{
    float4 pos : POSITION;
    float4 shadow : TEXCOORD0;
};

VSOut VSMain(float3 pos : POSITION)
{
    VSOut o;
    float4 p = float4(pos, 1);
    o.pos = mul(p, mtxWorldViewProj);
    float3 world = mul(p, (float4x3)mtxWorld);
    o.shadow = mul(float4(world, 1), mtxShadow);
    return o;
}

float4 PSMain(float4 shadow : TEXCOORD0) : COLOR
{
    float light = tex2Dproj(ShadowMap, shadow).r;
    return float4(lerp(1, light, f4WaterShadow.x).xxx, 1);
}

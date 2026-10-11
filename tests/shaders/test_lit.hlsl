// BtS-style lit unit for the lighting tests: the constant names BtS's skinning shaders light units with
// (mtxWorldBones, mtxViewProj c60, f3UnitLightDir/Diffuse/AmbientColor c64-c66), computing
// colour = saturate(N . -direction) x diffuse + ambient per vertex, as Civ4's ComputeCiv4UnitLighting does.
float4x3 mtxWorldBones[1] : register(c0);
float4x4 mtxViewProj : register(c60);
float3 f3UnitLightDir : register(c64);
float3 f3UnitLightDiffuse : register(c65);
float3 f3UnitAmbientColor : register(c66);

struct VSOut
{
    float4 pos : POSITION;
    float4 colour : COLOR0;
};

VSOut VSMain(float3 pos : POSITION, float3 normal : NORMAL)
{
    VSOut o;
    float3 world = mul(float4(pos, 1), mtxWorldBones[0]);
    float3 n = mul(normal, (float3x3)mtxWorldBones[0]);
    o.pos = mul(float4(world, 1), mtxViewProj);
    o.colour = float4(saturate(dot(n, -f3UnitLightDir)) * f3UnitLightDiffuse + f3UnitAmbientColor, 1);
    return o;
}

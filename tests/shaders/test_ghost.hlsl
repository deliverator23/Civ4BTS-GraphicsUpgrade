// Unit-style vertex shader for the ghost and shadow tests: a bone palette, then the camera, named and placed like
// the games' TCiv4Mech/TCiv4Skinning shaders so the proxy's constant-table filter selects it. Built twice: as in
// Steam's BtS (world-space mtxWorldBones, mtxViewProj = the camera) and, with OLD_NAMES, as in older builds (the
// model's own mtxBones, SkinWorldViewProj = the camera times the model's placement).
#ifdef OLD_NAMES
#define mtxWorldBones mtxBones
#define mtxViewProj SkinWorldViewProj
#endif
float4x3 mtxWorldBones[20] : register(c0);
float4x4 mtxViewProj : register(c60);

float4 VSMain(float3 pos : POSITION) : POSITION
{
    float3 world = mul(float4(pos, 1), mtxWorldBones[0]);
    return mul(float4(world, 1), mtxViewProj);
}

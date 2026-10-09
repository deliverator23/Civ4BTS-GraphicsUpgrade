// BtS rivers with sun shadows.
//
// BtS draws rivers with River.fx's River_Shader in two passes: P0 the floodplain, P1 the river itself. These are the
// same two passes with the same inputs, registers and arithmetic as the game's compiled shaders (vs_1_1/ps_1_1
// hashes 140246271c1a123b/579d9dd87d7f3783 and 4cbc89303161e055/1c78680e17f6d324), plus the shadow lookup the
// terrain uses: VS c20-c23 = world -> shadow texture, PS c10.x = darkness, s4 = the shadow texture.
//
// Each pass's object-space light vector (c14 in P0, c12 in P1) is filled in by the effect's preshader, so it isn't
// in the shaders' constant tables; the registers come from the disassembly.

float4x4 mtxWorldViewProj : register(c0);
float4x4 mtxWorld : register(c4);
float4x2 mtxFOW : register(c8);
float3x2 mtxFloodPlainMaskMat : register(c10);  // P0 only
float3x2 mtxBaseTextureMatP0 : register(c12);
float3x2 mtxBaseTextureMatP1 : register(c10);
float3 f3LightP0 : register(c14);  // object-space light direction (preshader output)
float3 f3LightP1 : register(c12);
float4x4 mtxShadow : register(c20);

float3 f3SunLightDiffuse : register(c0);  // P1 pixel shader
float3 f3SunAmbientColor : register(c1);
float4 f4Shadow : register(c10);  // x = darkness for hill shadows, z = 1: objects' shadows on (s4)
float4 f4HillGrid : register(c11); // x = 1 / (spacing x cells), y = 0.5 / cells, w = 1: hill shadows (s6)

sampler2D RiverNormalSampler : register(s0);  // P1
sampler2D RiverFOWSampler : register(s1);
sampler2D RiverBase : register(s2);           // P1
sampler2D FloodPlainSampler : register(s2);   // P0
sampler2D FloodPlainMaskSampler : register(s3);
sampler2D ShadowMap : register(s4);  // .r = light factor from objects' shadows
sampler2D HillShadows : register(s6);  // .a = shaded by hills

struct VSIn
{
    float3 pos : POSITION;
    float2 uniformTex : TEXCOORD1;
    float3 normal : NORMAL;
    float3 binormal : BINORMAL;
    float3 tangent : TANGENT;
};

struct FloodOut
{
    float4 pos : POSITION;
    float3 lightVec : COLOR0;
    float2 normalTex : TEXCOORD0;
    float2 fow : TEXCOORD1;
    float2 flood : TEXCOORD2;
    float2 mask : TEXCOORD3;
    float4 shadow : TEXCOORD4;
    float2 worldXY : TEXCOORD5;
};

struct RiverOut
{
    float4 pos : POSITION;
    float3 lightVec : COLOR0;
    float2 normalTex : TEXCOORD0;
    float2 fow : TEXCOORD1;
    float2 base : TEXCOORD2;
    float4 shadow : TEXCOORD4;
    float2 worldXY : TEXCOORD5;
};

float Light(float4 shadow, float2 worldXY)
{
    float hill = f4HillGrid.w * tex2D(HillShadows, worldXY * f4HillGrid.x + f4HillGrid.y).a;
    return min(lerp(1, tex2Dproj(ShadowMap, shadow).r, f4Shadow.z), 1 - f4Shadow.x * hill);
}

FloodOut VSFlood(VSIn i)
{
    FloodOut o;
    float4 p = float4(i.pos, 1);
    o.pos = mul(p, mtxWorldViewProj);
    float4 world = mul(p, mtxWorld);
    o.lightVec = 0.5 * float3(dot(i.tangent, f3LightP0), dot(i.binormal, f3LightP0), dot(i.normal, f3LightP0)) + 0.5;
    float3 uv = float3(i.uniformTex, 1);
    o.mask = mul(uv, mtxFloodPlainMaskMat);
    o.fow = mul(world, mtxFOW);
    o.normalTex = mul(uv, mtxBaseTextureMatP0);
    o.flood = o.normalTex;
    o.shadow = mul(world, mtxShadow);
    o.worldXY = world.xy;
    return o;
}

float4 PSFlood(FloodOut i) : COLOR
{
    float4 flood = tex2D(FloodPlainSampler, i.flood);
    float3 colour = flood.rgb * tex2D(RiverFOWSampler, i.fow).rgb;
    return float4(saturate(colour) * Light(i.shadow, i.worldXY), flood.a * tex2D(FloodPlainMaskSampler, i.mask).a);
}

RiverOut VSRiver(VSIn i)
{
    RiverOut o;
    float4 p = float4(i.pos, 1);
    o.pos = mul(p, mtxWorldViewProj);
    float4 world = mul(p, mtxWorld);
    o.lightVec = 0.5 * float3(dot(i.tangent, f3LightP1), dot(i.binormal, f3LightP1), dot(i.normal, f3LightP1)) + 0.5;
    o.fow = mul(world, mtxFOW);
    o.base = mul(float3(i.uniformTex, 1), mtxBaseTextureMatP1);
    o.normalTex = o.base;
    o.shadow = mul(world, mtxShadow);
    o.worldXY = world.xy;
    return o;
}

float4 PSRiver(RiverOut i) : COLOR
{
    // ps_1_1: dp3 of _bx2 inputs, then mad (registers hold -1..1), mul_sat with the base, times fog of war.
    float3 n = tex2D(RiverNormalSampler, i.normalTex).rgb * 2 - 1;
    float3 l = saturate(i.lightVec) * 2 - 1;
    float3 diffuse = clamp(dot(n, l) * f3SunLightDiffuse + f3SunAmbientColor, -1, 1);
    float4 base = tex2D(RiverBase, i.base);
    float3 colour = saturate(base.rgb * diffuse) * tex2D(RiverFOWSampler, i.fow).rgb;
    return float4(saturate(colour) * Light(i.shadow, i.worldXY), base.a);
}

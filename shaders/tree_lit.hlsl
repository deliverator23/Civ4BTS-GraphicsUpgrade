// BtS trees with lighting.
//
// BtS's ContourShader vertex shader (vs_1_1 d550d4da5e297ff4) places each tree of a forest patch on the terrain and
// sways it in the wind, but leaves it unlit (colour 1). This is the same vertex shader with the same inputs and
// registers, lit the way Civ4 lights units: colour = saturate(N . -direction) x sun + ambient, with the normal the
// tree vertices carry. BtS's own pixel shader stays (base x fog of war x colour, texkill t2). Added constants:
// c100 direction, c101 sun, c102 ambient,
// c103.x = 1 to hide BtS's painted tree shadows: flat cards at each tree's foot that use the atlas's shadow images
// (art\shared\trees_1024.dds, u 0.5-1, v 0.25-0.5). With real sun shadows they'd point the wrong way, so their
// vertices get a negative t2 and the pixel shader's texkill drops them.

float4 fHeightOffset0[64] : register(c0);  // height of each tree's spot, by instance index
float4x4 WorldViewProj : register(c64);
float4x4 mtxFOW : register(c68);
float4x3 World : register(c72);
float fFrameTime : register(c75);
float3 windir : register(c76);
float3 f3LightDir : register(c100);  // direction the light travels
float3 f3LightSun : register(c101);
float3 f3LightAmbient : register(c102);
float4 f4TreeOptions : register(c103);  // x = 1: hide the painted tree shadows

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 instance : COLOR;
    float2 tex : TEXCOORD0;
};

struct VSOut
{
    float4 pos : POSITION;
    float4 diffuse : COLOR0;
    float2 base : TEXCOORD0;
    float4 fow : TEXCOORD1;
    float4 kill : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    VSOut o = (VSOut)0;
    // As BtS: lift to the tree's spot, sway by the original height above it.
    int index = i.instance.x * 256.0f;
    float3 pos = i.pos;
    pos.z += fHeightOffset0[index].x;
    float zdist = pos.z - fHeightOffset0[index].x;
    pos += sin(fFrameTime + index) * zdist * windir * 0.15;
    o.pos = mul(float4(pos, 1), WorldViewProj);
    float3 world = mul(float4(pos, 1), World);
    o.base = i.tex;
    o.fow = mul(float4(world, 1), mtxFOW);
    float painted = f4TreeOptions.x * step(0.5f, i.tex.x) * step(0.25f, i.tex.y) * step(i.tex.y, 0.5f);
    o.kill.xy = lerp(pos.z + 80.0f, -1.0f, painted);
    float3 n = normalize(mul(i.normal, (float3x3)World));
    o.diffuse = float4(saturate(dot(n, -f3LightDir)) * f3LightSun + f3LightAmbient, 1);
    return o;
}

// Reflective, dynamic water. Reverse engineered from Civilization IV: Colonization's compiled water shader (Water.fx,
// Water_Shader pass P0: vs_1_1 2d8d0013f830bfbb, ps_2_0 d03b0dfb6f9efa7c), with the same inputs, constant names and
// registers.
//
// What it does: two scrolling layers of the normal map make the surface normal; the view reflected off it looks up
// the sky (environment cube), with the ships' reflection over it; the refraction (what's under the water) shows
// through, more of the reflection at grazing angles (Fresnel); the sun glints where the sky's alpha says; all in
// roughly gamma-2 space (squared colours in, square root out).
//
// Colonization's effect sets these through its preshader: c1 = f3SunLightDiffuse squared.
//
// Additions (neutral values give Colonization's water exactly): sun shadows on the surface (the
// terrain's shadow texture and matrix; they take the sun's glint away and darken the water by WaterShadow), and a
// sky light that tints the reflected sky (the light cycle dims it at night).

float4x4 mtxWorldViewProj : register(c0);
float4x4 mtxInvView : register(c4);  // only the eye position (column 3) is used
float4x4 mtxWorld : register(c8);
float4x4 mtxReflection : register(c12);
float4x4 mtxRefraction : register(c16);
float3x3 mtxWaterTexture1 : register(c20);
float3x3 mtxWaterTexture2 : register(c23);
float3x3 mtxWaterGrid : register(c26);
float4x4 mtxShadow : register(c30);  // world -> the shadow texture (as the terrain's)

float4 f4WaterConstants : register(c0);  // x = Fresnel pull towards 0.5, y = reflection ripple, z = refraction ripple
float3 f3SunSquared : register(c1);      // the sun colour squared (Colonization's preshader output)
float4 f4WaterShadow : register(c2);     // x = how much of a shadow the water takes, y = 1: shadows on, z = darkness
float3 f3SkyLight : register(c3);        // the reflected sky's tint (squared space; 1 = Colonization's)

sampler2D NormalMap : register(s0);
sampler2D CoastFade : register(s1);
sampler2D FogSRGB : register(s2);
sampler2D GridMap : register(s3);
samplerCUBE EnvironmentMap : register(s4);
sampler2D ReflectionMap : register(s5);
sampler2D RefractionMap : register(s6);
sampler2D ShadowMap : register(s7);  // .r = light factor from objects' shadows

struct VSIn
{
    float3 pos : POSITION;
    float2 coast : TEXCOORD1;
    float2 fog : TEXCOORD2;
};

struct VSOut
{
    float4 pos : POSITION;
    float4 normalUV : TEXCOORD0;  // xy layer 1, zw layer 2
    float2 coast : TEXCOORD1;
    float2 fog : TEXCOORD2;
    float3 toEye : TEXCOORD3;     // unit, from the surface to the eye
    float4 reflection : TEXCOORD4;
    float4 refraction : TEXCOORD5;
    float2 grid : TEXCOORD6;
    float4 shadow : TEXCOORD7;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 p = float4(i.pos, 1);
    o.pos = mul(p, mtxWorldViewProj);
    float4 world = mul(p, mtxWorld);
    float3 eye = float3(mtxInvView._14, mtxInvView._24, mtxInvView._34);
    o.toEye = normalize(eye - world.xyz);
    float3 xy1 = float3(world.xy, 1);
    o.normalUV.xy = mul(mtxWaterTexture1, xy1).xy;
    o.normalUV.zw = mul(mtxWaterTexture2, xy1).xy;
    o.grid = mul(mtxWaterGrid, xy1).xy;
    o.reflection = mul(world, mtxReflection);
    o.refraction = mul(world, mtxRefraction);
    o.shadow = mul(world, mtxShadow);
    o.coast = i.coast;
    o.fog = i.fog;
    return o;
}

float4 PSMain(VSOut i) : COLOR
{
    // Surface normal: the two layers' normals added, flattened (z halved).
    float3 n = (2 * tex2D(NormalMap, i.normalUV.zw).xyz - 1) + (2 * tex2D(NormalMap, i.normalUV.xy).xyz - 1);
    float3 normal = normalize(float3(n.xy, n.z * 0.5));
    float coast = tex2D(CoastFade, i.coast).x;
    float3 toEye = i.toEye;

    float4 sky = texCUBE(EnvironmentMap, reflect(-toEye, normal));
    float4 reflection = tex2D(ReflectionMap, i.reflection.xy / i.reflection.w + normal.xy * f4WaterConstants.y);
    float4 refraction = tex2D(RefractionMap, i.refraction.xy / i.refraction.w + coast * normal.xy * f4WaterConstants.z);
    float4 fog = tex2D(FogSRGB, i.fog);
    float3 grid = tex2D(GridMap, i.grid).xyz;

    // Fresnel: 0.1 looking straight down, more at grazing angles, pulled towards 0.5.
    float facing = dot(toEye, normal);
    float fresnel = 0.6 * (1 - facing) * (1 - facing) + 0.1;
    fresnel = lerp(fresnel, 0.5, f4WaterConstants.x);

    // Sun shadows: the light factor here, and how much of a full shadow that is (no glint in full shadow).
    float light = lerp(1, tex2Dproj(ShadowMap, i.shadow).r, f4WaterShadow.y);
    float sunlit = saturate(1 - (1 - light) / max(f4WaterShadow.z, 0.01));

    float3 reflected = lerp(sky.rgb * f3SkyLight, reflection.rgb, reflection.a);  // ships over the sky
    float3 colour = lerp(refraction.rgb, reflected * fog.rgb, coast * fresnel);
    float glint = coast * fog.r * sky.a * sky.a * (1 - reflection.a);  // the sun, where the sky's alpha says, not under ships
    colour += f3SunSquared * glint * sunlit;
    colour += grid * fog.rgb;
    return float4(sqrt(colour) * lerp(1, light, f4WaterShadow.x), refraction.a * (4.0 / 3.0));
}

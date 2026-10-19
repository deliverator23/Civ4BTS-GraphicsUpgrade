// BtS rivers with sun shadows.
//
// BtS draws rivers with River.fx's River_Shader in two passes: P0 the floodplain, P1 the river itself. These are the
// same two passes with the same inputs, registers and arithmetic as the game's compiled shaders (vs_1_1/ps_1_1
// hashes 140246271c1a123b/579d9dd87d7f3783 and 4cbc89303161e055/1c78680e17f6d324), plus the shadow lookup the
// terrain uses: VS c20-c23 = world -> shadow texture, PS c10.x = darkness, s4 = the shadow texture.
//
// Each pass's object-space light vector (c14 in P0, c12 in P1) is filled in by the effect's preshader, so it isn't
// in the shaders' constant tables; the registers come from the disassembly. River.fx works it out as -f3SunLightDir
// times the inverse world matrix: GraphicsUpgrade can put its own sun there (Shadows::riverSun).
//
// P1 also has the glint (GraphicsUpgrade's addition): the sea's, off the river's ripples. The view reflected off them
// looks up the sky texture's painted sun (its alpha), turned as on the sea so it is where the sea's glint is now.
// VS c24 = the camera in world space (w = 1; for a view without perspective, w = 0 and xyz the way towards it);
// PS c12 = the glint's colour (0: none), c13-c15 = the turn (water.hlsl's mtxGlint), s5 = the sky texture.
//
// And the ripples sway: the normal map read twice, each copy swayed back and forth along the channel (the flow map,
// s3: which way the channel runs at each place in the atlas, and how much to sway there) by its own amount now (PS c16:
// xy the first copy's, zw the second's, in texture units), and the two added. c17.x: how much to steepen the swayed
// ripples (the two copies' ripples partly cancel). All 0: the normal map as BtS reads it.

float4x4 mtxWorldViewProj : register(c0);
float4x4 mtxWorld : register(c4);
float4x2 mtxFOW : register(c8);
float3x2 mtxFloodPlainMaskMat : register(c10);  // P0 only
float3x2 mtxBaseTextureMatP0 : register(c12);
float3x2 mtxBaseTextureMatP1 : register(c10);
float3 f3LightP0 : register(c14);  // object-space light direction (preshader output)
float3 f3LightP1 : register(c12);
float4x4 mtxShadow : register(c20);
float4 f4Eye : register(c24);  // P1

float3 f3SunLightDiffuse : register(c0);  // P1 pixel shader
float3 f3SunAmbientColor : register(c1);
float4 f4Shadow : register(c10);  // x = darkness for hill shadows, z = 1: objects' shadows on (s4)
float4 f4HillGrid : register(c11); // world xy -> hill mask uv: x = scale, yz = offset; w = 1: hill shadows (s6)
float3 f3RiverGlint : register(c12);  // P1: the glint's colour, times its strength
float3x3 mtxGlint : register(c13);    // P1
float4 f4Shimmer : register(c16);     // P1
float4 f4ShimmerBoost : register(c17);  // P1

sampler2D RiverNormalSampler : register(s0);  // P1
sampler2D RiverFOWSampler : register(s1);
sampler2D RiverBase : register(s2);           // P1
sampler2D FloodPlainSampler : register(s2);   // P0
sampler2D FloodPlainMaskSampler : register(s3);
sampler2D RiverFlow : register(s3);  // P1: rg = the channel's direction (texture space, x 0.5 + 0.5), b = how much to sway
sampler2D ShadowMap : register(s4);  // .r = light factor from objects' shadows
samplerCUBE GlintSky : register(s5);   // P1: the sky texture (water_env.dds); .a = the painted sun
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
    float3 normal : TEXCOORD0;  // the surface's normal, tangent and binormal in world space (the normal map's z, x, y)
    float2 fow : TEXCOORD1;
    float2 base : TEXCOORD2;    // also the normal map's (as River.fx)
    float3 toEye : TEXCOORD3;   // towards the camera, world space
    float4 shadow : TEXCOORD4;
    float2 worldXY : TEXCOORD5;
    float3 tangent : TEXCOORD6;
    float3 binormal : TEXCOORD7;
};

float Light(float4 shadow, float2 worldXY)
{
    float hill = f4HillGrid.w * tex2D(HillShadows, worldXY * f4HillGrid.x + f4HillGrid.yz).a;
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
    o.shadow = mul(world, mtxShadow);
    o.worldXY = world.xy;
    o.toEye = f4Eye.xyz - world.xyz * f4Eye.w;
    o.normal = mul(i.normal, (float3x3)mtxWorld);
    o.tangent = mul(i.tangent, (float3x3)mtxWorld);
    o.binormal = mul(i.binormal, (float3x3)mtxWorld);
    return o;
}

float4 PSRiver(RiverOut i) : COLOR
{
    // ps_1_1: dp3 of _bx2 inputs, then mad (registers hold -1..1), mul_sat with the base, times fog of war.
    float4 flow = tex2D(RiverFlow, i.base);
    float2 along = (flow.rg * 2 - 1) * flow.b;
    float3 n = tex2D(RiverNormalSampler, i.base + along * f4Shimmer.xy).rgb +
               tex2D(RiverNormalSampler, i.base + along * f4Shimmer.zw).rgb - 1;
    n.xy *= 1 + f4ShimmerBoost.x * flow.b;
    float3 l = saturate(i.lightVec) * 2 - 1;
    float3 diffuse = clamp(dot(n, l) * f3SunLightDiffuse + f3SunAmbientColor, -1, 1);
    float4 base = tex2D(RiverBase, i.base);
    float3 fow = tex2D(RiverFOWSampler, i.fow).rgb;
    float3 colour = saturate(base.rgb * diffuse) * fow;
    float light = Light(i.shadow, i.worldXY);
    // The glint: the sea's painted sun, where the ripples mirror it into the camera, added as water.hlsl adds it (in
    // squared colours, so the painted sun's faint edges barely show). None in shadow (light at the shadows' darkness)
    // or the fog of war.
    float3 normal = normalize(n.x * i.tangent + n.y * i.binormal + n.z * i.normal);
    float sun = texCUBE(GlintSky, mul(reflect(-normalize(i.toEye), normal), mtxGlint)).a;
    float sunlit = saturate(1 - (1 - light) / max(f4Shadow.x, 0.05));
    float3 glint = f3RiverGlint * sun;
    colour = sqrt(colour * colour + glint * glint * sunlit * fow);
    return float4(saturate(colour) * light, base.a);
}

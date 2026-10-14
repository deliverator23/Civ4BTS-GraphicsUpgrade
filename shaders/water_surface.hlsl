// Water on models: an oasis' pool, or any fixed-function mesh drawn with one of [water] SurfaceTextures. BtS draws
// the mesh first, as it always does; this shader then draws it again on top with what water.hlsl adds to the sea:
// two scrolling layers of the normal map, the sky (environment cube) reflected off them with Fresnel, and the sun's
// glint where the sky's alpha says (not in shadow). BtS's own picture of the water is what's below the surface.
//
// It blends premultiplied (ONE, INVSRCALPHA): colour = what's there x (1 - k) + sky x k + glint, k = the Fresnel
// amount where the mesh is drawn (its texture's alpha times its diffuse alpha, as fixed function draws it). No
// planar reflection: the reflection texture is made for the sea's height.

float4x4 mtxWorldViewProj : register(c0);
float4x4 mtxWorld : register(c4);
float3x3 mtxWaterTexture1 : register(c8);
float3x3 mtxWaterTexture2 : register(c11);
float3x3 mtxBase : register(c14);    // the mesh's texture coordinates as fixed function transforms them (stage 0)
float4x4 mtxShadow : register(c17);  // world -> the shadow texture (as the terrain's)
float4 f4Eye : register(c21);        // xyz = the camera's position
float4 f4Alpha : register(c22);      // x = 1: the vertex colour's alpha, y = otherwise this alpha

float4 f4WaterConstants : register(c0);  // as water.hlsl: x = Fresnel pull towards 0.5
float3 f3SunSquared : register(c1);      // the sun colour squared
float4 f4WaterShadow : register(c2);     // y = 1: shadows on, z = darkness
float3 f3SkyLight : register(c3);        // the reflected sky's tint (squared space)

sampler2D NormalMap : register(s0);
sampler2D BaseMap : register(s1);    // the mesh's own texture: its alpha is the water's extent
samplerCUBE EnvironmentMap : register(s2);
sampler2D ShadowMap : register(s3);  // .r = light factor from objects' shadows

struct VSIn
{
    float3 pos : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : POSITION;
    float4 normalUV : TEXCOORD0;  // xy layer 1, zw layer 2
    float2 base : TEXCOORD1;
    float3 toEye : TEXCOORD2;     // from the surface to the eye
    float4 shadow : TEXCOORD3;
    float alpha : TEXCOORD4;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 p = float4(i.pos, 1);
    o.pos = mul(p, mtxWorldViewProj);
    float4 world = mul(p, mtxWorld);
    o.toEye = f4Eye.xyz - world.xyz;
    float3 xy1 = float3(world.xy, 1);
    o.normalUV.xy = mul(mtxWaterTexture1, xy1).xy;
    o.normalUV.zw = mul(mtxWaterTexture2, xy1).xy;
    o.base = mul(mtxBase, float3(i.uv, 1)).xy;
    o.shadow = mul(world, mtxShadow);
    o.alpha = lerp(f4Alpha.y, i.colour.a, f4Alpha.x);
    return o;
}

float4 PSMain(VSOut i) : COLOR
{
    float3 n = (2 * tex2D(NormalMap, i.normalUV.zw).xyz - 1) + (2 * tex2D(NormalMap, i.normalUV.xy).xyz - 1);
    float3 normal = normalize(float3(n.xy, n.z * 0.5));
    float3 toEye = normalize(i.toEye);
    float4 sky = texCUBE(EnvironmentMap, reflect(-toEye, normal));
    float coverage = saturate(tex2D(BaseMap, i.base).a * i.alpha);

    float facing = dot(toEye, normal);
    float fresnel = 0.6 * (1 - facing) * (1 - facing) + 0.1;
    fresnel = lerp(fresnel, 0.5, f4WaterConstants.x);
    float light = lerp(1, tex2Dproj(ShadowMap, i.shadow).r, f4WaterShadow.y);
    float sunlit = saturate(1 - (1 - light) / max(f4WaterShadow.z, 0.01));

    float k = coverage * fresnel;
    float3 glint = f3SunSquared * coverage * sky.a * sky.a * sunlit;
    return float4(sqrt(sky.rgb * f3SkyLight) * k + sqrt(glint), k);
}

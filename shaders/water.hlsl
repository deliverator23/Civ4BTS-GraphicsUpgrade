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
//
// Built a second time with SHORE defined (Shader Model 3), for the shoreline and shallow water:
//   - the glint follows the sun: the sky's alpha (Colonization's painted sun) is looked up with the reflected view
//     turned so that today's sun lands where the painted one is (mtxGlint);
//   - caustics: sunlight shimmering on the sea floor near the coast, two scrolling layers of a caustic network
//     (Civilization V's) brightening the sea floor's colour, fading out a plot and a half from land;
//   - wave shading: the waves' faces lit by the glint's light, and sparkle on choppy facets around the glint.
// Where the coast is comes from the shore texture (distance to land and depth, from the terrain's heights).

float4x4 mtxWorldViewProj : register(c0);
float4x4 mtxInvView : register(c4);  // only the eye position (column 3) is used
float4x4 mtxWorld : register(c8);
float4x4 mtxReflection : register(c12);
float4x4 mtxRefraction : register(c16);
float3x3 mtxWaterTexture1 : register(c20);
float3x3 mtxWaterTexture2 : register(c23);
float3x3 mtxWaterGrid : register(c26);
float4x4 mtxShadow : register(c30);  // world -> the shadow texture (as the terrain's)
#ifdef SHORE
float4 f4ShoreGrid : register(c34);    // x: world -> shore texture uv scale, y: its offset; z: caustic scale
float4 f4ShoreScroll : register(c35);  // the two caustic layers' scroll (uv)
#endif

float4 f4WaterConstants : register(c0);  // x = Fresnel pull towards 0.5, y = reflection ripple, z = refraction ripple
float3 f3SunSquared : register(c1);      // the sun colour squared (Colonization's preshader output)
float4 f4WaterShadow : register(c2);     // x = how much of a shadow the water takes, y = 1: shadows on, z = darkness
float3 f3SkyLight : register(c3);        // the reflected sky's tint (squared space; 1 = Colonization's)
#ifdef SHORE
float4 f4Shore : register(c4);    // x = caustics strength, y = how faded the glint is (0 = not), z = time (seconds),
                                  // w = 1: glint follows the sun
float3x3 mtxGlint : register(c5); // rows: turns a direction so that the sun now lands on the sky texture's sun
#endif
float4 f4NormalFormat : register(c8);  // x = 1: the normal map is DXT5nm (NormalSum); 0 = rgb, as Colonization's
#ifdef SHORE
float4 f4WaveLight : register(c9);  // xyz = unit, towards the glint's light; w = wave shading strength (0 = off)
#endif

sampler2D NormalMap : register(s0);
sampler2D CoastFade : register(s1);
sampler2D FogSRGB : register(s2);
sampler2D GridMap : register(s3);
samplerCUBE EnvironmentMap : register(s4);
sampler2D ReflectionMap : register(s5);
sampler2D RefractionMap : register(s6);
sampler2D ShadowMap : register(s7);  // .r = light factor from objects' shadows
#ifdef SHORE
sampler2D ShoreMap : register(s8);     // r = distance to land / 32 cells, g = depth / 32, a = 1 where known
sampler2D CausticMap1 : register(s9);  // alpha: a caustic network (tiling)
sampler2D CausticMap2 : register(s10);
#endif

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
#ifdef SHORE
    float4 caustic : TEXCOORD8;   // xy layer 1, zw layer 2
    float2 shore : TEXCOORD9;     // the shore texture
#endif
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
#ifdef SHORE
    o.caustic.xy = world.xy * f4ShoreGrid.z + f4ShoreScroll.xy;
    o.caustic.zw = world.xy * (f4ShoreGrid.z * 1.37) + f4ShoreScroll.zw;  // the second layer a different size
    o.shore = world.xy * f4ShoreGrid.x + f4ShoreGrid.y;
#endif
    return o;
}

// Two normal-map texels added as normals: from rgb, or with DXT5nm (f4NormalFormat.x = 1) x from alpha and y from
// green, the two channels DXT5 keeps best. The sum's z is rebuilt once from the summed slopes, as if both had their
// average: within a hair for water's gentle slopes. The Shader Model 2 build has no room for that, so there each
// DXT5nm z counts as 1 (under half a percent off for slopes of 5 degrees).
float3 NormalSum(float4 a, float4 b)
{
    float4 s = a + b;
    float2 xy = lerp(s.rg, s.ag, f4NormalFormat.x) * 2 - 2;
#ifdef SHORE
    float z = 2 * sqrt(saturate(1 - 0.25 * dot(xy, xy)));
#else
    float z = 2;
#endif
    return float3(xy, lerp(s.b * 2 - 2, z, f4NormalFormat.x));
}

float4 PSMain(VSOut i) : COLOR
{
    // Surface normal: the two layers' normals added, flattened (z halved).
    float3 n = NormalSum(tex2D(NormalMap, i.normalUV.zw), tex2D(NormalMap, i.normalUV.xy));
    float3 normal = normalize(float3(n.xy, n.z * 0.5));
    float coast = tex2D(CoastFade, i.coast).x;
    float3 toEye = i.toEye;

    float3 reflectedView = reflect(-toEye, normal);
    float4 sky = texCUBE(EnvironmentMap, reflectedView);
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

    float sun = sky.a;
#ifdef SHORE
    // The glint where the sun is now: the painted sun looked up with the view turned onto it, faded by f4Shore.y as
    // it slips off one edge of the view and back in at the other (0 = not at all).
    if (f4Shore.w > 0)
        sun = texCUBE(EnvironmentMap, mul(reflectedView, mtxGlint)).a * (1 - f4Shore.y);

    // The shore here: cells from land, depth, and whether the terrain is known.
    float4 shore = tex2D(ShoreMap, i.shore);
    float cells = shore.r * 31.875, depth = shore.g * 31.875, known = shore.a * fog.r;

    // Caustics on the sea floor: two scrolling layers of a caustic network averaged (so the light lines cross and stay
    // visible from the game's distance), rippled by the surface; strongest at the shore, gone a plot and
    // a half out (12 cells) and in the last couple of units at the waterline. They brighten the sea floor's own colour
    // (up to 90% on the lines in full sun) with a little light of their own, so they show over a dark floor too.
    float2 ripple = normal.xy * 0.03;
    float layers = tex2D(CausticMap1, i.caustic.xy + ripple).a + tex2D(CausticMap2, i.caustic.zw - ripple).a;
    float caustic = layers * 0.5;
    float shallow = (1 - smoothstep(4, 12, cells)) * saturate(depth * 0.5) * known;
    float3 causticLight = f3SunSquared * (sunlit * caustic * shallow * f4Shore.x);
    refraction.rgb = refraction.rgb * (1 + 0.9 * causticLight) + 0.05 * causticLight;
#endif

    float3 reflected = lerp(sky.rgb * f3SkyLight, reflection.rgb, reflection.a);  // ships over the sky
    float3 colour = lerp(refraction.rgb, reflected * fog.rgb, coast * fresnel);
    float glint = coast * fog.r * sun * sun * (1 - reflection.a);  // the sun, where the sky's alpha says, not under ships
#ifdef SHORE
    // Wave shading, so the water moves where there's no glint to show it: the waves' faces turned towards the light
    // lighter, those turned away darker (flat water unchanged; less in shadow, where only the sky lights it). And
    // sparkle: the light caught by the steeper facets of choppy water, sharp glints scattered well around the sun's.
    float3 toLight = f4WaveLight.xyz;
    float tiltToLight = dot(normal, toLight) - toLight.z;
    colour *= clamp(1 + f4WaveLight.w * 2.5 * tiltToLight * lerp(0.4, 1, sunlit), 0.5, 1.5);
    float3 steep = normalize(float3(normal.xy * 3, normal.z));
    float sparkle = pow(saturate(dot(reflect(-toEye, steep), toLight)), 120);
    glint += f4WaveLight.w * 0.6 * sparkle * coast * fog.r * (1 - reflection.a) * (1 - f4Shore.y);
#endif
    colour += f3SunSquared * glint * sunlit;
    colour += grid * fog.rgb;
    float alpha = refraction.a * (4.0 / 3.0);
    return float4(sqrt(colour) * lerp(1, light, f4WaterShadow.x), alpha);
}

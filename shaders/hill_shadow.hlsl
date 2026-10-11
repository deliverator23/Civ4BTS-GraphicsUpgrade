// Hill shadows: the mask the terrain, rivers and ground decals are darkened by, worked out on the GPU every frame.
//
// Drawn as a full-target quad over the ground in view (the mask is much finer than the height grid; the heights are
// sampled bilinearly, as the terrain itself is). For each texel, step towards the sun through the heights, the sunbeam rising
// by the sun's slope per step; the texel is shaded by how far the terrain rises above the beam, softly: full shadow
// HillShadowSoftness above it at the first step, more further away (a penumbra that widens with distance), ignoring
// the first couple of units (small bumps). The
// beam's rise is divided by HillShadowLength, which lengthens the shadows: BtS's peaks are low for their tiles.
// Output: rgb = the light factor 1 - darkness x shadow, a = shadow. Unknown terrain (heights below the lowest known,
// as the edge of known terrain blends with the -10000 of unknown cells) is unshaded.

sampler2D Heights : register(s0);  // world z per cell
float4 f4Step : register(c0);  // xy = uv step towards the sun, z = the beam's rise per step (world units), w = steps
float4 f4Shade : register(c1);  // x = softness, y = darkness, z = highest terrain (the beam can stop above it),
                                // w = the softness added per step
float4 f4Known : register(c2);  // x = below this, unknown terrain; y = heights above the beam ignored (small bumps)

struct VSOut
{
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOut o;
    o.pos = pos;
    o.uv = uv;
    return o;
}

float4 PSMain(float2 uv : TEXCOORD0) : COLOR
{
    float h = tex2Dlod(Heights, float4(uv, 0, 0)).r;
    if (h < f4Known.x)
        return float4(1, 1, 1, 0);
    float shade = 0;
    float2 p = uv;
    float z = h;
    float soft = f4Shade.x;
    for (int i = 0; i < 192; ++i)
    {
        if (i >= f4Step.w || z > f4Shade.z)
            break;
        p += f4Step.xy;
        z += f4Step.z;
        soft += f4Shade.w;
        shade = max(shade, (tex2Dlod(Heights, float4(p, 0, 0)).r - z - f4Known.y) / soft);
    }
    shade = saturate(shade);
    float light = 1 - f4Shade.y * shade;
    return float4(light, light, light, shade);
}

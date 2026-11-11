// BtS's culture border shader (CultureBOrderShader.fx, TCultureBorder_Shader14 passes P0 and P1: vs_1_1, ps_1_1; the
// two passes are the same code, P0 with f4BorderColor1/2 and mtxBorderTextureMat1/2, P1 with 3/4 in the same
// registers), with the same inputs, constant registers and samplers, so the game's own settings for each border draw
// stand as they are. Used with [debug] TerrainVertsPerPlot 33 or 65 (Shader Model 3, a vertex texture), where the
// border, a 17 x 17 grid over each plot (a vertex every 11.25 world units), no longer matches the terrain under it:
// the terrain's finer relief rises between the border's vertices and the border breaks up under it.
//
// Two changes, both in the vertex shader:
//   - Each vertex reads the terrain's height where it is (the height grid, bilinear between its four nearest points)
//     and, where the ground is above it, is lifted onto it (plus a little).
//   - Each vertex's depth is taken from up to f4Drape.w world units nearer the camera along its own line of sight: it
//     stays at the same place on the screen, only nearer in depth, so ground that rises between the border's vertices,
//     less than that in front of it, no longer covers it. The border writes no depth, and everything after it (trees,
//     units, buildings) is drawn over it as before.
// The fog of war is read where the vertex was, as BtS reads it.
//
// The pixel shader is BtS's: two border textures, each times its colour, the one with the higher alpha drawn (the
// first where they're equal), times the fog of war.

float4 mtxWorldViewProj[4] : register(c0);
float4 mtxWorld[4] : register(c4);
float4 mtxFOW[2] : register(c8);
float4 mtxBorderTexture1[2] : register(c10);  // P0: mtxBorderTextureMat1, P1: mtxBorderTextureMat3
float4 mtxBorderTexture2[2] : register(c12);  // P0: mtxBorderTextureMat2, P1: mtxBorderTextureMat4
float4 mtxViewProj[4] : register(c14);        // world -> clip (rows), for the moved positions
float4 f4Drape : register(c18);  // x = 1 / the height grid's spacing, y = 1 / its cells, z = lift above ground, w = pull
float4 f4Eye : register(c19);    // xyz = the camera in the world
sampler2D HeightMap : register(s0);  // a vertex texture: the terrain's heights (unknown = -10000)

float4 f4BorderColor1 : register(c0);  // P1: f4BorderColor3
float4 f4BorderColor2 : register(c1);  // P1: f4BorderColor4

sampler2D CultureBorderBase1 : register(s0);
sampler2D CultureBorderBase2 : register(s1);
sampler2D TerrainFOWar : register(s2);

struct VSIn
{
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : POSITION;
    float2 border1 : TEXCOORD0;
    float2 border2 : TEXCOORD1;
    float2 fog : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 p = float4(i.pos.xyz, 1);
    float3 uv = float3(i.uv, 1);  // as BtS's: (u, v, 1) against each texture matrix row
    o.border1 = float2(dot(uv, mtxBorderTexture1[0].xyz), dot(uv, mtxBorderTexture1[1].xyz));
    o.border2 = float2(dot(uv, mtxBorderTexture2[0].xyz), dot(uv, mtxBorderTexture2[1].xyz));
    float4 world = float4(dot(p, mtxWorld[0]), dot(p, mtxWorld[1]), dot(p, mtxWorld[2]), dot(p, mtxWorld[3]));
    o.fog = float2(dot(world, mtxFOW[0]), dot(world, mtxFOW[1]));
    world /= world.w;
    // The ground here: grid points are at whole multiples of the spacing (texel centres), bilinear between the four.
    float2 cell = world.xy * f4Drape.x;
    float2 corner = floor(cell), t = cell - corner;
    // Wrapped here, not by the sampler: the grid wraps (the map is centred on 0, so half of it is at negative cells),
    // and a vertex texture read past 0 clamped to the edge on Iain's card (borders west and south of the map's
    // middle read the wrong ground and weren't lifted; trace 2026-11-11 00:06)
    float2 at = frac((corner + 0.5) * f4Drape.y), next = frac(at + f4Drape.y);
    float h00 = tex2Dlod(HeightMap, float4(at, 0, 0)).r;
    float h10 = tex2Dlod(HeightMap, float4(next.x, at.y, 0, 0)).r;
    float h01 = tex2Dlod(HeightMap, float4(at.x, next.y, 0, 0)).r;
    float h11 = tex2Dlod(HeightMap, float4(next, 0, 0)).r;
    float ground = lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y);  // unknown ground: far below, no change
    world.z = max(world.z, ground + f4Drape.z);
    o.pos = float4(dot(world, mtxViewProj[0]), dot(world, mtxViewProj[1]), dot(world, mtxViewProj[2]),
                   dot(world, mtxViewProj[3]));
    // Towards the camera along the line of sight (never more than half way there): only the depth is taken from
    // there, so x, y and w, and with them the textures across each triangle, stay as they are.
    float3 sight = world.xyz - f4Eye.xyz;
    float distance = length(sight);
    float4 nearer = float4(world.xyz - sight * (min(f4Drape.w, 0.5 * distance) / max(distance, 1e-3)), 1);
    float2 zw = float2(dot(nearer, mtxViewProj[2]), dot(nearer, mtxViewProj[3]));
    o.pos.z = zw.x / zw.y * o.pos.w;
    return o;
}

float4 PSMain(VSOut i) : COLOR
{
    float4 a = tex2D(CultureBorderBase1, i.border1) * f4BorderColor1;
    float4 b = tex2D(CultureBorderBase2, i.border2) * f4BorderColor2;
    float4 fog = tex2D(TerrainFOWar, i.fog);
    float4 c = b.a > a.a ? b : a;
    return float4(fog.rgb * c.rgb, c.a);
}

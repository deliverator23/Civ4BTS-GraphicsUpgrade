// Softens the sun-shadow coverage texture into the shadow texture.
//
// Input: the coverage texture, units' and trees' coverage in alpha, buildings' in red (each has its own darkness).
// Each is the average of 4 bilinear taps around the texel, faded to zero near the texture's edges so shadows don't
// end in a hard line where the sun camera's coverage stops.
// Output: colour = the light factor 1 - max(darkness x alpha, building darkness x red), which everything that receives
// shadows multiplies by; alpha = max(alpha, red) (the debug overlay).
// Drawn as a pretransformed full-target quad with fixed-function vertex processing.

sampler2D Coverage : register(s0);
float4 f4Texel : register(c0);  // x, y = tap offset in uv; z = edge fade width in uv; w = darkness
float4 f4Building : register(c1);  // x = building darkness

float4 PSMain(float2 uv : TEXCOORD0) : COLOR
{
    float4 c = tex2D(Coverage, uv + float2(-f4Texel.x, -f4Texel.y)) + tex2D(Coverage, uv + float2(f4Texel.x, -f4Texel.y)) +
               tex2D(Coverage, uv + float2(-f4Texel.x, f4Texel.y)) + tex2D(Coverage, uv + float2(f4Texel.x, f4Texel.y));
    float2 edge = min(uv, 1 - uv) / f4Texel.z;
    c *= 0.25 * saturate(min(edge.x, edge.y));
    float light = 1 - max(f4Texel.w * c.a, f4Building.x * c.r);
    return float4(light, light, light, max(c.a, c.r));
}

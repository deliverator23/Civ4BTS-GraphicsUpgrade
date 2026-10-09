#include "lighting.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

#include "tree_lit_vs.h"

namespace
{
// BtS's ContourShader tree vertex shader, whose copy tree_lit.hlsl is.
const uint64_t kTreeVs = 0xd550d4da5e297ff4ull;
}

void Lighting::Set(bool vs, int reg, const float* rgb)
{
    if (reg < 0 || m_count >= 8)
        return;
    Saved& s = m_saved[m_count];
    s.vs = vs;
    s.reg = static_cast<UINT>(reg);
    HRESULT hr = vs ? m_dev->GetVertexShaderConstantF(s.reg, s.value, 1) : m_dev->GetPixelShaderConstantF(s.reg, s.value, 1);
    if (FAILED(hr))
        return;
    ++m_count;
    const float v[4] = {rgb[0], rgb[1], rgb[2], s.value[3]};
    if (vs)
        m_dev->SetVertexShaderConstantF(s.reg, v, 1);
    else
        m_dev->SetPixelShaderConstantF(s.reg, v, 1);
}

// The keyframes either side of pos (0 .. count), wrapping round from the last to the first, and how far between them.
static void CycleSpan(const std::vector<Config::LightCycleKey>& keys, double pos, double count,
                      const Config::LightCycleKey*& a, const Config::LightCycleKey*& b, double& t)
{
    size_t next = 0;
    while (next < keys.size() && keys[next].step <= pos)
        ++next;
    a = &keys[(next + keys.size() - 1) % keys.size()];
    b = &keys[next % keys.size()];
    double from = a->step, to = b->step;
    if (to <= from)
        to += count;  // across the end of the cycle
    double at = pos < from ? pos + count : pos;
    t = to > from ? (at - from) / (to - from) : 0;
}

// The colour at position pos (0 .. count) of a cycle: a straight blend between the keyframes either side.
static void CycleColour(const std::vector<Config::LightCycleKey>& keys, double pos, double count, float* out)
{
    if (keys.empty())
        return;
    const Config::LightCycleKey *a, *b;
    double t;
    CycleSpan(keys, pos, count, a, b, t);
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<float>(a->value[i] + (b->value[i] - a->value[i]) * t);
}

// The sun's travel direction from angle keyframes: azimuth the shorter way round, elevation straight, so the sun moves
// round the sky (a circle, or an ellipse with the elevation changing) instead of swinging back across it.
static void CycleSunAngles(const std::vector<Config::LightCycleKey>& keys, double pos, double count, float* dir)
{
    const Config::LightCycleKey *a, *b;
    double t;
    CycleSpan(keys, pos, count, a, b, t);
    double turn = std::fmod(b->value[0] - a->value[0], 360.0);
    if (turn > 180)
        turn -= 360;
    else if (turn < -180)
        turn += 360;
    const double kDeg = 3.14159265358979 / 180;
    const double az = (a->value[0] + turn * t) * kDeg, el = (a->value[1] + (b->value[1] - a->value[1]) * t) * kDeg;
    // Light comes from (cos el cos az, cos el sin az, sin el); it travels the other way.
    dir[0] = static_cast<float>(-std::cos(el) * std::cos(az));
    dir[1] = static_cast<float>(-std::cos(el) * std::sin(az));
    dir[2] = static_cast<float>(-std::sin(el));
}

// Perceived brightness of a colour (Rec. 601 weights).
static float Brightness(const float* c)
{
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

void Lighting::NewFrame()
{
    // The cycle's own clock: it moves on only while the cycle runs and the world is on screen, so pausing it,
    // turning the lighting off, menus and loading screens leave the light where it is.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!m_frequency.QuadPart)
    {
        QueryPerformanceFrequency(&m_frequency);
        m_last = now;
    }
    m_cycleActive = enabled && g_config.lightCycleOn;
    if (m_cycleActive && !cyclePaused && m_worldThisFrame)
        m_cycleSeconds += static_cast<double>(now.QuadPart - m_last.QuadPart) / m_frequency.QuadPart;
    m_last = now;
    m_worldThisFrame = false;
    m_cycleDirActive = false;
    m_shadowScale = 1.0f;
    if (!m_cycleActive)
        return;
    const double seconds = m_cycleSeconds;
    const double count = g_config.lightCycleStepCount;
    double pos = std::fmod(g_config.lightCycleStart + seconds / g_config.lightCycleTime * count, count);
    CycleColour(g_config.lightCycle, pos, count, m_cycleSun);
    CycleColour(g_config.lightCycleAmbient, pos, count, m_cycleAmbient);
    if (g_config.sunPath)
    {
        // Midday at half the cycle, midnight at 0; sunrise and sunset a quarter either side of midday.
        const double kPi = 3.14159265358979, kDeg = kPi / 180;
        const double x = std::cos(kPi * (pos - count / 2) / (count / 2));  // 1 midday, 0 sunrise/sunset, -1 midnight
        const double low = g_config.sunriseHeight;
        const double el = x >= 0 ? low + (g_config.sunMiddayHeight - low) * x : low + (g_config.moonHeight - low) * -x;
        const double bearing = g_config.sunMiddayFrom + 360.0 * (pos - count / 2) / count;  // clockwise
        const double az = (90.0 - bearing) * kDeg;  // compass bearing -> anticlockwise from east (+x)
        m_cycleDir[0] = static_cast<float>(-std::cos(el * kDeg) * std::cos(az));
        m_cycleDir[1] = static_cast<float>(-std::cos(el * kDeg) * std::sin(az));
        m_cycleDir[2] = static_cast<float>(-std::sin(el * kDeg));
        m_cycleDirActive = true;
    }
    else if (!g_config.lightCycleAngles.empty())
    {
        CycleSunAngles(g_config.lightCycleAngles, pos, count, m_cycleDir);
        m_cycleDirActive = true;
    }
    else if (!g_config.lightCycleDirection.empty())
    {
        // Blend the directions either side, then back to unit length (two opposite ones would cancel: keep the last).
        float dir[3];
        CycleColour(g_config.lightCycleDirection, pos, count, dir);
        float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len > 1e-3f)
            for (int i = 0; i < 3; ++i)
                m_cycleDir[i] = dir[i] / len;
        m_cycleDirActive = len > 1e-3f || m_cycleDir[2] != 0;
    }
    if (g_config.shadowsFollowSun && !g_config.lightCycle.empty())
    {
        if (m_brightestSun < 0)
        {
            m_brightestSun = 0;
            for (const auto& k : g_config.lightCycle)
                m_brightestSun = (std::max)(m_brightestSun, Brightness(k.value));
        }
        m_shadowScale = m_brightestSun > 0 ? (std::min)(1.0f, Brightness(m_cycleSun) / m_brightestSun) : 1.0f;
    }
    m_cyclePos = pos;
    if (!m_loggedCycle)
    {
        m_loggedCycle = true;
        Log::Write("lighting: light cycle, %zu sun, %zu ambient, %zu direction and %zu sun angle keyframes over %g "
                   "steps, %g s per cycle, starting at step %g%s", g_config.lightCycle.size(),
                   g_config.lightCycleAmbient.size(), g_config.lightCycleDirection.size(),
                   g_config.lightCycleAngles.size(), count, g_config.lightCycleTime, g_config.lightCycleStart,
                   g_config.shadowsFollowSun && !g_config.lightCycle.empty() ? "; shadows follow the sun" : "");
        if (g_config.sunPath)
            Log::Write("lighting: sun path from %g degrees (compass) at midday; %g degrees up at midday, %g at sunrise "
                       "and sunset, the moon %g at midnight", g_config.sunMiddayFrom, g_config.sunMiddayHeight,
                       g_config.sunriseHeight, g_config.moonHeight);
    }
    // Each whole step as the cycle reaches it, so the log shows it moving.
    int whole = static_cast<int>(pos);
    if (whole != m_loggedStep)
    {
        m_loggedStep = whole;
        Log::Write("lighting: cycle step %d, %s", whole, Describe().c_str());
    }
}

const float* Lighting::WaterSun()
{
    if (!enabled || !SunSet(kLightWater))
        return nullptr;
    const float* sun = Sun(kLightWater);
    if (!g_config.moonGlintSet || !CyclesSun(kLightWater))
        return sun;
    // Night: nothing until a sixth of the cycle before midnight (20 h of 24), all of it at midnight, nothing again
    // a sixth after (4 h): 2 x -cos - 1, with cos 1 at midday, 0 at sunrise and sunset, -1 at midnight.
    const double count = g_config.lightCycleStepCount, kPi = 3.14159265358979;
    const double night = (std::max)(0.0, (std::min)(1.0, -2.0 * std::cos(kPi * (m_cyclePos - count / 2) / (count / 2)) - 1.0));
    for (int i = 0; i < 3; ++i)
        m_waterSun[i] = static_cast<float>(sun[i] + (g_config.moonGlint[i] - sun[i]) * night);
    return m_waterSun;
}

const float* Lighting::SkyLight()
{
    // Measured against the light the sky is as it is under: the light cycle's own midday ambient while the cycle sets
    // the ambient, else Colonization's own ambient (its SunLight), the light its sky texture goes with.
    static const float kColonizationAmbient[3] = {89 / 255.0f, 103 / 255.0f, 133 / 255.0f};
    float reference[3] = {kColonizationAmbient[0], kColonizationAmbient[1], kColonizationAmbient[2]};
    if (CyclesAmbient(kLightTerrain))
        CycleColour(g_config.lightCycleAmbient, g_config.lightCycleStepCount / 2.0, g_config.lightCycleStepCount,
                    reference);
    for (int i = 0; i < 3; ++i)
    {
        float r = enabled && AmbientSet(kLightTerrain) && reference[i] > 0 ? Ambient(kLightTerrain)[i] / reference[i]
                                                                           : 1.0f;
        m_skyLight[i] = (std::min)(4.0f, r * r);  // squared: the water works in squared colours
    }
    return m_skyLight;
}

std::string Lighting::Describe() const
{
    char buf[256] = {};
    int n = 0;
    if (!g_config.lightCycle.empty())
        n += snprintf(buf + n, sizeof(buf) - n, "sun %.0f,%.0f,%.0f", m_cycleSun[0] * 255, m_cycleSun[1] * 255,
                      m_cycleSun[2] * 255);
    if (!g_config.lightCycleAmbient.empty())
        n += snprintf(buf + n, sizeof(buf) - n, "%sambient %.0f,%.0f,%.0f", n ? ", " : "", m_cycleAmbient[0] * 255,
                      m_cycleAmbient[1] * 255, m_cycleAmbient[2] * 255);
    if (m_cycleDirActive)
        n += snprintf(buf + n, sizeof(buf) - n, "%sdirection %.3f,%.3f,%.3f", n ? ", " : "", m_cycleDir[0],
                      m_cycleDir[1], m_cycleDir[2]);
    if (m_shadowScale < 1.0f)
        snprintf(buf + n, sizeof(buf) - n, "%sshadows x%.2f", n ? ", " : "", m_shadowScale);
    return buf;
}

bool Lighting::Direction(int group, float* dir) const
{
    // LightDirection if set, else the shadows' SunDirection (MatchShadowDirection=1), else the game's own. Trees
    // have no light direction in BtS, so they take the shadows' sun then too. Rivers' and the terrain's light
    // directions aren't set directly (effect preshader, baked lightmap).
    if (group == kLightRiver || group == kLightTerrain)
        return false;
    const float* from = nullptr;
    if (g_config.haveLightDirection)
        from = g_config.lightDirection;
    else if (g_config.matchShadowDirection || group == kLightTree)
        from = SunDirection();
    else
        return false;
    float len = std::sqrt(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]);
    for (int i = 0; i < 3; ++i)
        dir[i] = from[i] / (len > 0 ? len : 1);
    return true;
}

bool Lighting::EnsureTreeShader()
{
    if (m_treeVs || m_treeFailed)
        return m_treeVs != nullptr;
    std::vector<DWORD> code(sizeof(g_treeLitVS) / 4);
    memcpy(code.data(), g_treeLitVS, sizeof(g_treeLitVS));
    ULONG before = DeviceRefs(m_dev);
    if (FAILED(m_dev->CreateVertexShader(code.data(), &m_treeVs)))
    {
        m_treeFailed = true;
        Log::Write("lighting: creating the lit tree shader failed; trees stay unlit");
        return false;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    return true;
}

void Lighting::ReleaseAll()
{
    if (m_treeVs)
        m_treeVs->Release();  // no reference counting afterwards: this can be the device's last object
    m_treeVs = nullptr;
    m_internalRefs = 0;
}

void Lighting::Apply(const ShaderInfo* vs, const ShaderInfo* ps, int ffGroup)
{
    m_count = 0;
    m_lightCount = 0;
    if (!enabled && !hidePaintedTreeShadows)
        return;
    float dir[3];
    for (int k = 0; k < 2 && enabled; ++k)
    {
        const ShaderInfo* info = k == 0 ? vs : ps;
        if (!info || info->lightGroup < 0)
            continue;
        int g = info->lightGroup;
        if (SunSet(g))
            Set(k == 0, info->lightDiffuse, Sun(g));
        if (AmbientSet(g))
            Set(k == 0, info->lightAmbient, Ambient(g));
        if (Direction(g, dir))
            Set(k == 0, info->lightDir, dir);
    }
    const bool treeColours = enabled && (SunSet(kLightTree) || AmbientSet(kLightTree));
    if (vs && vs->hash == kTreeVs && (treeColours || hidePaintedTreeShadows) && EnsureTreeShader())
    {
        // Unset tree colours (or the lighting off) as BtS: no sun, full ambient (= colour 1).
        static const float kNoSun[3] = {0, 0, 0}, kFull[3] = {1, 1, 1}, kHide[3] = {1, 0, 0}, kShow[3] = {0, 0, 0};
        m_dev->GetVertexShader(&m_gameVs);
        m_dev->SetVertexShader(m_treeVs);
        if (!Direction(kLightTree, dir))
            dir[0] = dir[1] = 0, dir[2] = -1;
        Set(true, 100, dir);
        Set(true, 101, treeColours && SunSet(kLightTree) ? Sun(kLightTree) : kNoSun);
        Set(true, 102, treeColours && AmbientSet(kLightTree) ? Ambient(kLightTree) : kFull);
        Set(true, 103, hidePaintedTreeShadows ? kHide : kShow);
    }
    if (!enabled)
        return;
    if (!vs && ffGroup >= 0)
        ApplyFixedFunction(ffGroup);
}

void Lighting::ApplyFixedFunction(int g)
{
    DWORD lighting = FALSE;
    m_dev->GetRenderState(D3DRS_LIGHTING, &lighting);
    if (!lighting || (!SunSet(g) && !AmbientSet(g)))
        return;
    float dir[3];
    const bool newDir = Direction(g, dir);
    for (DWORD i : m_slots)
    {
        BOOL on = FALSE;
        D3DLIGHT9 l;
        if (m_lightCount == 8 || FAILED(m_dev->GetLightEnable(i, &on)) || !on || FAILED(m_dev->GetLight(i, &l)))
            continue;
        m_lights[m_lightCount++] = {i, l};
        // BtS puts each light's shade in the light itself (its global D3DRS_AMBIENT is white), so the INI colours
        // go there too: diffuse (and specular, which BtS keeps equal to it) = sun, the light's ambient = ambient.
        if (l.Type != D3DLIGHT_DIRECTIONAL)
        {
            --m_lightCount;  // point lights (effects) stay as they are
            continue;
        }
        if (SunSet(g))
        {
            l.Diffuse.r = l.Specular.r = Sun(g)[0];
            l.Diffuse.g = l.Specular.g = Sun(g)[1];
            l.Diffuse.b = l.Specular.b = Sun(g)[2];
        }
        if (AmbientSet(g))
            l.Ambient.r = Ambient(g)[0], l.Ambient.g = Ambient(g)[1], l.Ambient.b = Ambient(g)[2];
        if (newDir)
            l.Direction.x = dir[0], l.Direction.y = dir[1], l.Direction.z = dir[2];
        m_dev->SetLight(i, &l);
    }
}

void Lighting::Restore()
{
    for (int i = m_count - 1; i >= 0; --i)
    {
        const Saved& s = m_saved[i];
        if (s.vs)
            m_dev->SetVertexShaderConstantF(s.reg, s.value, 1);
        else
            m_dev->SetPixelShaderConstantF(s.reg, s.value, 1);
    }
    m_count = 0;
    if (m_gameVs)
    {
        m_dev->SetVertexShader(m_gameVs);
        m_gameVs->Release();
        m_gameVs = nullptr;
    }
    for (int i = m_lightCount - 1; i >= 0; --i)
        m_dev->SetLight(m_lights[i].index, &m_lights[i].light);
    m_lightCount = 0;
}

void Lighting::TerrainConstants(float* c, float normalSpacing, UINT normalCells) const
{
    memset(c, 0, 32 * sizeof(float));
    const float* bakedAmbient = g_config.bakedTerrainAmbient;
    const float* bakedSun = g_config.bakedTerrainSun;
    for (int i = 0; i < 3; ++i)
    {
        c[0 + i] = bakedAmbient[i];
        c[4 + i] = bakedSun[i] > 0.001f ? 1.0f / bakedSun[i] : 0.0f;
        c[8 + i] = AmbientSet(kLightTerrain) ? Ambient(kLightTerrain)[i] : bakedAmbient[i];
        c[12 + i] = SunSet(kLightTerrain) ? Sun(kLightTerrain)[i] : bakedSun[i];
    }
    // Directions: towards the sun (the negated travel direction), unit length. The terrain's new sun: LightDirection,
    // else the shadows' SunDirection (MatchShadowDirection=1), else the baked one.
    auto towards = [](const float* travel, float* out) {
        float len = std::sqrt(travel[0] * travel[0] + travel[1] * travel[1] + travel[2] * travel[2]);
        for (int i = 0; i < 3; ++i)
            out[i] = -travel[i] / (len > 0 ? len : 1);
    };
    const float* newDir = g_config.haveLightDirection ? g_config.lightDirection
                          : g_config.matchShadowDirection ? SunDirection()
                                                          : g_config.bakedTerrainDirection;
    towards(newDir, c + 20);
    towards(g_config.bakedTerrainDirection, c + 24);
    bool turned = false;
    for (int i = 0; i < 3; ++i)
        turned = turned || std::fabs(c[20 + i] - c[24 + i]) > 1e-4f;
    const bool perPixel = turned && normalSpacing > 0 && normalCells > 0;
    if (perPixel)
    {
        c[28] = 1.0f / (normalSpacing * normalCells);
        c[29] = 0.5f / normalCells;
        c[30] = 1.0f;
    }
    // Colours equal to the baked ones and the sun where BtS baked it: exactly as baked.
    bool same = true;
    for (int i = 0; i < 3; ++i)
        same = same && std::fabs(c[8 + i] - bakedAmbient[i]) < 0.5f / 255 &&
               std::fabs(c[12 + i] - bakedSun[i]) < 0.5f / 255;
    c[16] = enabled && (!same || perPixel) ? 1.0f : 0.0f;
}

void Lighting::CheckLightmap(IDirect3DBaseTexture9* base)
{
    if (!base || !m_checked.insert(base).second || base->GetType() != D3DRTYPE_TEXTURE)
        return;
    auto* tex = static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC d = {};
    D3DLOCKED_RECT lr;
    if (FAILED(tex->GetLevelDesc(0, &d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) ||
        d.Pool == D3DPOOL_DEFAULT || FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
    {
        Log::Write("lighting: lightmap %ux%u format %u can't be read; not checked", d.Width, d.Height, d.Format);
        return;
    }
    // Undo the half-strength storage, remove the baked ambient, divide by the baked sun: if the bake is
    // (ambient + sun x amount) / 2, the three channels then agree. Their spread (max - min) per texel says how well
    // the re-light can recover the amount.
    std::vector<float> spread;
    float lo = 1e9f, hi = -1e9f;
    unsigned saturated = 0, total = 0;
    for (UINT y = 0; y < d.Height; y += 4)
    {
        const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
        for (UINT x = 0; x < d.Width; x += 4)
        {
            DWORD p = row[x];
            float c[3] = {((p >> 16) & 0xFF) / 255.0f, ((p >> 8) & 0xFF) / 255.0f, (p & 0xFF) / 255.0f};
            float k[3];
            for (int i = 0; i < 3; ++i)
                k[i] = (2 * c[i] - g_config.bakedTerrainAmbient[i]) /
                       (g_config.bakedTerrainSun[i] > 0.001f ? g_config.bakedTerrainSun[i] : 1.0f);
            ++total;
            if (((p >> 16) & 0xFF) == 255 || ((p >> 8) & 0xFF) == 255 || (p & 0xFF) == 255)
            {
                ++saturated;
                continue;
            }
            float mn = (std::min)({k[0], k[1], k[2]}), mx = (std::max)({k[0], k[1], k[2]});
            spread.push_back(mx - mn);
            lo = (std::min)(lo, mn);
            hi = (std::max)(hi, mx);
        }
    }
    tex->UnlockRect(0);
    if (spread.empty())
    {
        Log::Write("lighting: lightmap %ux%u: every sampled texel saturated", d.Width, d.Height);
        return;
    }
    std::sort(spread.begin(), spread.end());
    Log::Write("lighting: lightmap %ux%u check (%u texels sampled, %u saturated): amount of sun %.3f..%.3f; channel "
               "spread median %.3f, 95%% %.3f, max %.3f (near 0 = the re-light is exact)",
               d.Width, d.Height, total, saturated, lo, hi, spread[spread.size() / 2], spread[spread.size() * 95 / 100],
               spread.back());
}

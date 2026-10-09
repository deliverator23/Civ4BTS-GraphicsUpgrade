// Reflection pass: an offscreen texture holding the ships mirrored about the water plane, built the way Colonization
// builds its ReflectionTexture:
//   - cleared to alpha 0 once per frame, so alpha marks where something was reflected;
//   - each unit/ship draw made before the frame's first water draw is redrawn into it straight away, with the
//     camera mirrored about the water plane and flipped in x (which keeps the triangle winding, so culling is
//     unchanged), and a clip plane at the water surface so nothing below the waterline is reflected.
// BtS draws ships before water, so the texture is complete by the time the water is drawn.
#pragma once
#include "common.h"
#include "trace.h"

#include <functional>
#include <string>

class Reflection
{
public:
    explicit Reflection(IDirect3DDevice9* device) : m_dev(device), m_height(g_config.waterHeight) {}
    ~Reflection() { ReleaseResources(); }

    bool enabled = false;
    bool overlay = false;
    bool needed = true;  // set per frame: only run the pass if the water or the overlay uses it

    // Called after each Present.
    void NewFrame();
    // Called before every draw with its vertex shader (null for fixed function). Spots the first water draw.
    void BeforeDraw(const ShaderInfo* vs);
    // Called after a draw. If it was a unit/ship draw to the back buffer before the water, redraws it mirrored.
    // Returns true if it did.
    bool AfterDraw(const ShaderInfo* vs, bool fixedFunction, const std::function<void()>& redraw);
    // Debug overlay: the texture's colour and alpha side by side in the top-right corner. Call before Present.
    void DrawOverlay();
    // Saves the texture as a 32-bit BMP (BGRA, alpha kept). False if there's no texture yet.
    bool SaveBmp(const std::wstring& path);

    // Our D3DPOOL_DEFAULT objects must go before IDirect3DDevice9::Reset and before the device dies.
    void ReleaseResources();
    // Device references held by our objects, so the device wrapper knows when only we keep the device alive.
    ULONG InternalRefs() const { return m_internalRefs; }
    float WaterHeight() const { return m_height; }
    IDirect3DTexture9* Texture() const { return m_tex; }
    // True when render target 0 is the swap chain's back buffer (the main scene, not e.g. the HUD unit portrait).
    bool TargetIsBackBuffer();
    uint32_t FrameRedraws() const { return m_frameRedraws; }

private:
    bool EnsureResources();
    void EnsureCleared();

    IDirect3DDevice9* m_dev;
    IDirect3DTexture9* m_tex = nullptr;
    IDirect3DSurface9* m_surf = nullptr;
    IDirect3DSurface9* m_depth = nullptr;
    IDirect3DStateBlock9* m_overlayState = nullptr;
    ULONG m_internalRefs = 0;
    float m_height;
    bool m_waterSeen = false;
    bool m_cleared = false;
    bool m_loggedHeight = false;
    uint32_t m_frameRedraws = 0;
};

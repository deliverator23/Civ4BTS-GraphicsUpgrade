// Models under the culture borders' tint ([textures] ModelsUnderBorders): BtS draws the borders onto the ground first,
// then the models over them, so a model that is part of the landscape (Dune Wars' Caves) stands out of the owner's
// tint. Here each border draw is drawn a second time into a screen-sized layer of its own (the tint alone, as it fell
// on the ground), and a listed model's pixels, marked in the stencil as it is drawn, get that layer laid over them: the
// tint of the ground behind each pixel, so the model looks to be under it. Nothing moves; models and units drawn after
// it cover it as before.
#pragma once
#include "assets.h"
#include "common.h"

#include <functional>

class BorderHaze
{
public:
    explicit BorderHaze(IDirect3DDevice9* device) : m_dev(device) {}
    ~BorderHaze() { ReleaseAll(); }

    bool enabled = false;  // something is listed (and effects allowed in this game)
    TextureList models;    // [textures] ModelsUnderBorders

    // A draw whose stage 0 texture is one of the listed models'.
    bool Listed();
    // A border draw into the back buffer, just drawn as usual: draw it again (the same way) into the layer.
    void AddBorder(const std::function<void()>& draw);
    // A listed model's draw: drawn with its pixels marked, then the layer laid over them. Drawn as usual when there's
    // no tint this frame.
    HRESULT DrawUnder(const std::function<HRESULT()>& draw);

    void EndFrame() { m_tinted = false; }  // Present: the layer starts empty next frame
    void ForgetTexture(const void* texture) { models.Forget(texture); }
    void ReleaseDefault();  // before a Reset: the targets and the state block; made again on use
    void ReleaseAll() { ReleaseDefault(); }
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    bool EnsureTargets(IDirect3DSurface9* backBuffer);  // the layer, matching the back buffer (size, multisampling)
    bool EnsureResolved();  // the resolved copy up to date with every border drawn so far
    // A fixed-function, blended model drawn again, taking the layer at each pixel in proportion to its own alpha.
    void DrawWeighted(const std::function<HRESULT()>& draw);

    IDirect3DDevice9* m_dev;
    IDirect3DSurface9* m_layer = nullptr;     // render target: multisampled as the back buffer, to share its depth
    IDirect3DTexture9* m_resolved = nullptr;  // the layer, resolved to be read
    IDirect3DSurface9* m_resolvedSurf = nullptr;
    IDirect3DStateBlock9* m_state = nullptr;
    D3DSURFACE_DESC m_desc = {};  // the back buffer's the targets were made for
    bool m_tinted = false;        // a border drawn into the layer this frame
    bool m_resolvedFresh = false; // the resolved copy has every border drawn so far
    bool m_failed = false, m_logged = false;
    ULONG m_internalRefs = 0;
};

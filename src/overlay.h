// Developer overlay: textures drawn as 256-pixel squares along the top right of the screen, on top of the finished
// frame (call just before Present). Used for the reflection texture and the sun-shadow textures.
#pragma once
#include "common.h"

struct OverlayItem
{
    IDirect3DBaseTexture9* texture;
    bool alpha;  // show the alpha channel as grey instead of the colour
};

// row 0 is the top row, row 1 the one below it. state is a state block owned by the caller (created on first use,
// counted in internalRefs; release it before Reset).
void DrawOverlay(IDirect3DDevice9* dev, IDirect3DStateBlock9*& state, ULONG& internalRefs, const OverlayItem* items,
                 int count, int row);

// Saves a render-target surface (A8R8G8B8 or X8R8G8B8, not multisampled) as a 32-bit top-down BMP, alpha kept.
bool SaveSurfaceBmp(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const std::wstring& path);
// Same for a lockable (managed or system-memory) A8R8G8B8/X8R8G8B8 texture's top level, e.g. the terrain lightmap.
bool SaveTextureBmp(IDirect3DBaseTexture9* texture, const std::wstring& path);

// The main view's camera this frame (world -> clip, register form), and what units' and ships' camera register
// (ShaderInfo::viewProj) must hold to draw them through another camera: the sun's, the mirrored one, a shifted one.
//
// Steam's BtS shaders take world-space bones and the camera alone (mtxViewProj), so that's simply the new camera.
// Older builds take the model's own bones and the camera times the model's placement (SkinWorldViewProj): the
// placement is recovered from the register's current value and this frame's camera, then put under the new camera.
#pragma once
#include "common.h"
#include "trace.h"

namespace Camera
{
void NewFrame();            // after Present: last frame's camera no longer counts
void Set(const float* vp);  // the main view's camera, from a terrain or tree draw
const float* Get();         // this frame's, or null before any such draw

// What the unit draw's camera register must hold to draw it through the world -> clip camera vp.
bool UnitViewProj(IDirect3DDevice9* dev, const ShaderInfo& vs, const float* vp, float* out);
// The unit draw's own world -> clip camera (shaders take clip planes in clip space).
bool UnitWorldToClip(IDirect3DDevice9* dev, const ShaderInfo& vs, float* out);
}

/*
 * Descent 3
 * Copyright (C) 2024 Descent Developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// TODO: This is missing a good way of overriding base behavior (like, you know, method overrides...)

#include <algorithm>
#include <cstring>

#include "pserror.h"
#include "log.h"
#include "mono.h"
#include "3d.h"
#include "renderer.h"
#include "bitmap.h"
#include "grdefs.h"

#include "HardwareInternal.h"
#include "lightmap.h"

// FIXME: Unused
// The font characteristics
static float rend_FontRed[4], rend_FontBlue[4], rend_FontGreen[4], rend_FontAlpha[4];
char Renderer_error_message[256] = "Generic renderer error";

// The following values are referenced by the game code, but only
// UseMultitexture is actually referenced and used in this code - and
// even that can probably go away and we just assume there is more than
// one texture unit (which, since I believe OpenGL 1.2 is a requirement)
bool UseHardware = true;
bool NoLightmaps = false;
bool StateLimited = false;
bool UseWBuffer = false;

// General renderer states
int gpu_Overlay_map = -1;
uint8_t gpu_Overlay_type = OT_NONE;
uint8_t Renderer_initted = 0;

// Generic GPU data
rendering_state gpu_state;
renderer_preferred_state gpu_preferred_state = {0, 1, 1.5};

int gpu_last_frame_polys_drawn = 0;
int gpu_last_frame_verts_processed = 0;
int gpu_last_uploaded = 0;

float gpu_Alpha_factor = 1.0f;
float gpu_Alpha_multiplier = 1.0f;

PosColorUVVertex vArray[100];
PosColorUV2Vertex vArray2[100];

// returns the alpha that we should use
float rend_GetAlphaMultiplier() {
  switch (gpu_state.cur_alpha_type) {
  case AT_ALWAYS:
  case AT_TEXTURE:
  case AT_VERTEX:
  case AT_TEXTURE_VERTEX:
  case AT_SATURATE_VERTEX:
  case AT_SATURATE_TEXTURE_VERTEX:
  case AT_SPECULAR:
    return 1.0f;
  case AT_CONSTANT:
  case AT_CONSTANT_TEXTURE:
  case AT_CONSTANT_TEXTURE_VERTEX:
  case AT_CONSTANT_VERTEX:
  case AT_LIGHTMAP_BLEND:
  case AT_LIGHTMAP_BLEND_SATURATE:
  case AT_SATURATE_TEXTURE:
  case AT_SATURATE_CONSTANT_VERTEX:
    return gpu_state.cur_alpha / 255.0f;
  default:
    // Int3();		// no type defined,get jason
    return 0.0f;
  }
}

// Retrieves an error message
const char *rend_GetErrorMessage() { return Renderer_error_message; }

// Sets an error message
void rend_SetErrorMessage(const char *str) {
  ASSERT(strlen(str) < 256);
  strcpy(Renderer_error_message, str);
}

// FIXME: Unused
// Sets the argb characteristics of the font characters.  color1 is the upper left and proceeds clockwise
void rend_SetCharacterParameters(ddgr_color color1, ddgr_color color2, ddgr_color color3, ddgr_color color4) {
  rend_FontRed[0] = (float)(GR_COLOR_RED(color1) / 255.0f);
  rend_FontRed[1] = (float)(GR_COLOR_RED(color2) / 255.0f);
  rend_FontRed[2] = (float)(GR_COLOR_RED(color3) / 255.0f);
  rend_FontRed[3] = (float)(GR_COLOR_RED(color4) / 255.0f);
  rend_FontGreen[0] = (float)(GR_COLOR_GREEN(color1) / 255.0f);
  rend_FontGreen[1] = (float)(GR_COLOR_GREEN(color2) / 255.0f);
  rend_FontGreen[2] = (float)(GR_COLOR_GREEN(color3) / 255.0f);
  rend_FontGreen[3] = (float)(GR_COLOR_GREEN(color4) / 255.0f);
  rend_FontBlue[0] = (float)(GR_COLOR_BLUE(color1) / 255.0f);
  rend_FontBlue[1] = (float)(GR_COLOR_BLUE(color2) / 255.0f);
  rend_FontBlue[2] = (float)(GR_COLOR_BLUE(color3) / 255.0f);
  rend_FontBlue[3] = (float)(GR_COLOR_BLUE(color4) / 255.0f);
  rend_FontAlpha[0] = (color1 >> 24) / 255.0f;
  rend_FontAlpha[1] = (color2 >> 24) / 255.0f;
  rend_FontAlpha[2] = (color3 >> 24) / 255.0f;
  rend_FontAlpha[3] = (color4 >> 24) / 255.0f;
}

// Sets where the software renderer should write to
void rend_SetSoftwareParameters(float aspect, int width, int height, int pitch, uint8_t *framebuffer) {}

// Sets the state of bilinear filtering for our textures
void rend_SetFiltering(int8_t state) { gpu_state.cur_bilinear_state = state; }

// Sets a bitmap as a overlay map to rendered on top of the next texture map
// a -1 value indicates no overlay map
void rend_SetOverlayMap(int handle) { gpu_Overlay_map = handle; }

void rend_SetOverlayType(uint8_t type) { gpu_Overlay_type = type; }

void rend_FillCircle(ddgr_color col, int x, int y, int rad) {}

void rend_DrawCircle(int x, int y, int rad) {}

// Sets up a font character to draw.  We draw our fonts as pieces of textures
void rend_DrawFontCharacter(int bm_handle, int x1, int y1, int x2, int y2, float u, float v, float w, float h) {
  g3Point *ptr_pnts[4];
  g3Point pnts[4];
  for (int i = 0; i < 4; i++) {
    pnts[i].p3_z = 1; // Make REALLY close!
    pnts[i].p3_flags = PF_PROJECTED;
    ptr_pnts[i] = &pnts[i];
  }
  pnts[0].p3_sx = (float)x1;
  pnts[0].p3_sy = (float)y1;
  pnts[0].p3_u = u;
  pnts[0].p3_v = v;
  pnts[1].p3_sx = (float)x2;
  pnts[1].p3_sy = (float)y1;
  pnts[1].p3_u = u + w;
  pnts[1].p3_v = v;
  pnts[2].p3_sx = (float)x2;
  pnts[2].p3_sy = (float)y2;
  pnts[2].p3_u = u + w;
  pnts[2].p3_v = v + h;
  pnts[3].p3_sx = (float)x1;
  pnts[3].p3_sy = (float)y2;
  pnts[3].p3_u = u;
  pnts[3].p3_v = v + h;
  rend_DrawPolygon2D(bm_handle, ptr_pnts, 4);
}

// Sets the alpha value for constant alpha
void rend_SetAlphaValue(uint8_t val) {
  gpu_state.cur_alpha = val;
  gpu_Alpha_multiplier = rend_GetAlphaMultiplier();
}

// Sets the texture wrapping type
void rend_SetWrapType(wrap_type val) { gpu_state.cur_wrap_type = val; }

void rend_SetZBias(float z_bias) {
  if (Z_bias != z_bias) {
    Z_bias = z_bias;

    // Force refresh our transforms to take the Zbias into account
    g3_GetModelViewMatrix(&View_position, &Unscaled_matrix, (float *)gTransformModelView);
    g3_UpdateFullTransform();
    g3_ForceTransformRefresh();
  }
}

// Sets the overall alpha scale factor (all alpha values are scaled by this value)
// useful for motion blur effect
void rend_SetAlphaFactor(float val) { gpu_Alpha_factor = std::clamp(val, 0.0f, 1.0f); }

// Returns the current Alpha factor
float rend_GetAlphaFactor() { return gpu_Alpha_factor; }

// Gets a pointer to a linear frame buffer
void rend_GetLFBLock(renderer_lfb *lfb) {}

// Releases an lfb lock
void rend_ReleaseLFBLock(renderer_lfb *lfb) {}

// Returns the aspect ratio of the physical screen
void rend_GetProjectionParameters(int *width, int *height) {
  *width = gpu_state.clip_x2 - gpu_state.clip_x1;
  *height = gpu_state.clip_y2 - gpu_state.clip_y1;
}

void rend_GetProjectionScreenParameters(int &screenLX, int &screenTY, int &screenW, int &screenH) {
  screenLX = gpu_state.clip_x1;
  screenTY = gpu_state.clip_y1;
  screenW = gpu_state.clip_x2 - gpu_state.clip_x1 + 1;
  screenH = gpu_state.clip_y2 - gpu_state.clip_y1 + 1;
}

// Returns the aspect ratio of the physical screen
float rend_GetAspectRatio() {
  float aspect_ratio = (float)((3.0f * gpu_state.screen_width) / (4.0f * gpu_state.screen_height));
  return aspect_ratio;
}

// Given a source x,y and width,height, draws any sized bitmap into the renderer lfb
void rend_DrawLFBBitmap(int sx, int sy, int w, int h, int dx, int dy, uint16_t *data, int rowsize) {}

// draws a scaled 2d bitmap to our buffer
void rend_DrawScaledBitmap(int x1, int y1, int x2, int y2, int bm, float u0, float v0, float u1, float v1, int color,
                           const float *alphas) {
  g3Point *ptr_pnts[4];
  g3Point pnts[4];
  float r, g, b;
  if (color != -1) {
    r = GR_COLOR_RED(color) / 255.0f;
    g = GR_COLOR_GREEN(color) / 255.0f;
    b = GR_COLOR_BLUE(color) / 255.0f;
  }
  for (int i = 0; i < 4; i++) {
    if (color == -1)
      pnts[i].p3_l = 1.0;
    else {
      pnts[i].p3_r = r;
      pnts[i].p3_g = g;
      pnts[i].p3_b = b;
    }
    if (alphas) {
      pnts[i].p3_a = alphas[i];
    }

    pnts[i].p3_z = 1.0f;
    pnts[i].p3_flags = PF_PROJECTED;
  }

  pnts[0].p3_sx = (float)x1;
  pnts[0].p3_sy = (float)y1;
  pnts[0].p3_u = u0;
  pnts[0].p3_v = v0;
  pnts[1].p3_sx = (float)x2;
  pnts[1].p3_sy = (float)y1;
  pnts[1].p3_u = u1;
  pnts[1].p3_v = v0;
  pnts[2].p3_sx = (float)x2;
  pnts[2].p3_sy = (float)y2;
  pnts[2].p3_u = u1;
  pnts[2].p3_v = v1;
  pnts[3].p3_sx = (float)x1;
  pnts[3].p3_sy = (float)y2;
  pnts[3].p3_u = u0;
  pnts[3].p3_v = v1;
  ptr_pnts[0] = &pnts[0];
  ptr_pnts[1] = &pnts[1];
  ptr_pnts[2] = &pnts[2];
  ptr_pnts[3] = &pnts[3];
  rend_SetTextureType(TT_LINEAR);
  rend_DrawPolygon2D(bm, ptr_pnts, 4);
}

//	given a chunked bitmap, renders it.
void rend_DrawChunkedBitmap(chunked_bitmap *chunk, int x, int y, uint8_t alpha) {
  int *bm_array = chunk->bm_array;
  int w = chunk->w;
  int h = chunk->h;
  int piece_w = bm_w(bm_array[0], 0);
  int piece_h = bm_h(bm_array[0], 0);
  int screen_w, screen_h;
  rend_SetZBufferState(0);
  rend_GetProjectionParameters(&screen_w, &screen_h);
  for (int i = 0; i < h; i++) {
    for (int t = 0; t < w; t++) {
      int dx = x + (piece_w * t);
      int dy = y + (piece_h * i);
      rend_DrawSimpleBitmap(bm_array[i * w + t], dx, dy);
    }
  }
  rend_SetZBufferState(1);
}

//	given a chunked bitmap, renders it.scaled
void rend_DrawScaledChunkedBitmap(chunked_bitmap *chunk, int x, int y, int neww, int newh, uint8_t alpha) {
  int *bm_array = chunk->bm_array;
  int w = chunk->w;
  int h = chunk->h;
  int piece_w;
  int piece_h;
  int screen_w, screen_h;
  int i, t;

  float scalew, scaleh;

  scalew = ((float)neww) / ((float)chunk->pw);
  scaleh = ((float)newh) / ((float)chunk->ph);
  piece_w = scalew * ((float)bm_w(bm_array[0], 0));
  piece_h = scaleh * ((float)bm_h(bm_array[0], 0));
  rend_GetProjectionParameters(&screen_w, &screen_h);
  rend_SetOverlayType(OT_NONE);
  rend_SetLighting(LS_NONE);
  rend_SetColorModel(CM_MONO);
  rend_SetZBufferState(0);
  rend_SetAlphaType(AT_CONSTANT_TEXTURE);
  rend_SetAlphaValue(alpha);
  rend_SetWrapType(WT_WRAP);
  for (i = 0; i < h; i++) {
    for (t = 0; t < w; t++) {
      int dx = x + (piece_w * t);
      int dy = y + (piece_h * i);
      int dw, dh;
      if ((dx + piece_w) > screen_w)
        dw = piece_w - ((dx + piece_w) - screen_w);
      else
        dw = piece_w;
      if ((dy + piece_h) > screen_h)
        dh = piece_h - ((dy + piece_h) - screen_h);
      else
        dh = piece_h;

      float u2 = (float)dw / (float)piece_w;
      float v2 = (float)dh / (float)piece_h;
      rend_DrawScaledBitmap(dx, dy, dx + dw, dy + dh, bm_array[i * w + t], 0, 0, u2, v2);
    }
  }
  rend_SetZBufferState(1);
}

// Sets some global preferences for the renderer
int rend_SetPreferredState(renderer_preferred_state *pref_state, bool reinit) {
  int retval = 1;

  renderer_preferred_state old_state = gpu_preferred_state;

  gpu_preferred_state = *pref_state;
  if (gpu_state.initted) {
    LOG_DEBUG << "Inside pref state!";

    if (old_state.fullscreen != pref_state->fullscreen) {
      rend_SetFullScreen(pref_state->fullscreen);
    }

    if (pref_state->width != gpu_state.screen_width || pref_state->height != gpu_state.screen_height ||
        old_state.bit_depth != pref_state->bit_depth || reinit) {
      rend_ReInit();
    }

    if (old_state.gamma != pref_state->gamma) {
      rend_SetGammaValue(pref_state->gamma);
    }
  } else {
    gpu_preferred_state = *pref_state;
  }

  return retval;
}

// Draws a simple bitmap at the specified x,y location
void rend_DrawSimpleBitmap(int bm_handle, int x, int y) {
  rend_SetAlphaType(AT_CONSTANT_TEXTURE);
  rend_SetAlphaValue(255);
  rend_SetLighting(LS_NONE);
  rend_SetColorModel(CM_MONO);
  rend_SetOverlayType(OT_NONE);
  rend_SetFiltering(0);
  rend_DrawScaledBitmap(x, y, x + bm_w(bm_handle, 0), y + bm_h(bm_handle, 0), bm_handle, 0, 0, 1, 1);
  rend_SetFiltering(1);
}

// Fills in the passed in pointer with the current rendering state
void rend_GetRenderState(rendering_state *rstate) { memcpy(rstate, &gpu_state, sizeof(rendering_state)); }

// Preuploads a texture to the video card
void rend_PreUploadTextureToCard(int handle, int map_type) {}

// Frees an uploaded texture from the video card
void rend_FreePreUploadedTexture(int handle, int map_type) {}

// Returns 1 if there is mid video memory, 2 if there is low vid memory, or 0 if there is large vid memory
int rend_LowVidMem() { return 0; }

// Returns 1 if the renderer supports bumpmapping
int rend_SupportsBumpmapping() { return 0; }

// Sets a bumpmap to be rendered, or turns off bumpmapping altogether
void rend_SetBumpmapReadyState(int state, int map) {}

// returns rendering statistics for the frame
void rend_GetStatistics(tRendererStats *stats) {
  if (Renderer_initted) {
    stats->poly_count = gpu_last_frame_polys_drawn;
    stats->vert_count = gpu_last_frame_verts_processed;
    stats->texture_uploads = gpu_last_uploaded;
  } else {
    memset(stats, 0, sizeof(tRendererStats));
  }
}

#ifdef DEDICATED_ONLY

void rend_DrawPolygon2D(int, g3Point **, int, int) {}
void rend_DrawPolygon3D(int, g3Point **, int, int) {}

#else

// Takes nv vertices and draws the 2D polygon defined by those vertices.
// Uses bitmap "handle" as a texture
void rend_DrawPolygon2D(int handle, g3Point **p, int nv) {
  ASSERT(nv < 100);
  ASSERT(gpu_Overlay_type == OT_NONE);

  g3_RefreshTransforms(true);

  gpu_SetMultitextureBlendMode(false);

  int xAdd = gpu_state.clip_x1;
  int yAdd = gpu_state.clip_y1;

  // make sure our bitmap is ready to be drawn
  gpu_BindTexture(handle, MAP_TYPE_BITMAP, 0);

  PosColorUVVertex *vData = &vArray[0];

  // Specify our coordinates
  for (int i = 0; i < nv; ++i, ++vData) {
    g3Point *pnt = p[i];

    vData->color = DeterminePointColor(pnt, false, true);

    vData->uv.s = pnt->p3_u;
    vData->uv.t = pnt->p3_v;
    vData->uv.r = 0.0f;
    vData->uv.w = 1.0f;

    // Finally, specify a vertex
    vData->pos.x() = pnt->p3_sx + xAdd;
    vData->pos.y() = pnt->p3_sy + yAdd;
    vData->pos.z() = 0.0f;
  }

  gpu_RenderPolygon(&vArray[0], nv);
}

color_array DeterminePointColor(g3Point const *pnt, bool disableGouraud, bool checkTextureQuality,
                                bool flatColorForNoLight) {
  auto alpha = gpu_Alpha_multiplier * gpu_Alpha_factor;
  if (gpu_state.cur_alpha_type & ATF_VERTEX) {
    alpha *= pnt->p3_a;
  }

  // If we have a lighting model, apply the correct lighting!
  if ((gpu_state.cur_light_state == LS_FLAT_GOURAUD && !disableGouraud) ||
      (gpu_state.cur_texture_quality == 0 && checkTextureQuality) ||
      (gpu_state.cur_light_state == LS_NONE && flatColorForNoLight)) {
    return {GR_COLOR_RED(gpu_state.cur_color) / 255.0f, GR_COLOR_GREEN(gpu_state.cur_color) / 255.0f,
            GR_COLOR_BLUE(gpu_state.cur_color) / 255.0f, alpha};
  } else if (gpu_state.cur_light_state == LS_NONE) {
    return {1, 1, 1, alpha};
  } else if (gpu_state.cur_color_model == CM_MONO) {
    return {pnt->p3_l, pnt->p3_l, pnt->p3_l, alpha};
  } else {
    return {pnt->p3_r, pnt->p3_g, pnt->p3_b, alpha};
  }
}

// Takes nv vertices and draws the 3D polygon defined by those vertices.
// Uses bitmap "handle" as a texture
void rend_DrawPolygon3D(int handle, g3Point **p, int nv, int map_type) {
  g3Point *pnt;

  ASSERT(nv < 100);

  g3_RefreshTransforms(false);

  if (gpu_state.cur_texture_quality == 0) {
    gpu_DrawFlatPolygon3D(p, nv);
    return;
  }

  if (gpu_Overlay_type != OT_NONE) {
    rend_DrawMultitexturePolygon3D(handle, p, nv, map_type);
    return;
  }

  gpu_SetMultitextureBlendMode(false);

  gpu_BindTexture(handle, map_type, 0);

  PosColorUVVertex *vData = &vArray[0];

  // Specify our coordinates
  for (int i = 0; i < nv; i++, vData++) {
    pnt = p[i];

    // all points should be original
    ASSERT(pnt->p3_flags & PF_ORIGPOINT);

    vData->color = DeterminePointColor(pnt);

    vData->uv.s = pnt->p3_u;
    vData->uv.t = pnt->p3_v;
    vData->uv.r = 0.0f;
    vData->uv.w = 1.0f;

    // Finally, specify a vertex
    vData->pos = pnt->p3_vecPreRot;
  }

  // And draw!
  gpu_RenderPolygon(&vArray[0], nv);

  // If there is a lightmap to draw, draw it as well
  if (gpu_Overlay_type != OT_NONE) {
    return; // Temp fix until I figure out whats going on
    Int3(); // Shouldn't reach here
  }
}

#endif // DEDICATED_ONLY

// Takes nv vertices and draws the polygon defined by those vertices.  Uses bitmap "handle"
// as a texture
void rend_DrawMultitexturePolygon3D(int handle, g3Point **p, int nv, int map_type) {
  g3Point *pnt;

  float one_over_square_res = 1.0f / GameLightmaps[gpu_Overlay_map].square_res;
  float xscalar = (float)GameLightmaps[gpu_Overlay_map].width * one_over_square_res;
  float yscalar = (float)GameLightmaps[gpu_Overlay_map].height * one_over_square_res;

  ASSERT(nv < 100);

  PosColorUV2Vertex *vData = &vArray2[0];

  // Specify our coordinates
  for (int i = 0; i < nv; i++, vData++) {
    pnt = p[i];
    ASSERT(pnt->p3_flags & PF_ORIGPOINT);

    vData->color = DeterminePointColor(pnt, true);

    vData->uv0.s = pnt->p3_u;
    vData->uv0.t = pnt->p3_v;
    vData->uv0.r = 0.0f;
    vData->uv0.w = 1.0f;

    vData->uv1.s = pnt->p3_u2 * xscalar;
    vData->uv1.t = pnt->p3_v2 * yscalar;
    vData->uv1.r = 0.0f;
    vData->uv1.w = 1.0f;

    // Finally, specify a vertex
    vData->pos = pnt->p3_vecPreRot;
  }

  // make sure our bitmap is ready to be drawn
  gpu_BindTexture(handle, map_type, 0);

  // make sure our bitmap is ready to be drawn
  gpu_BindTexture(gpu_Overlay_map, MAP_TYPE_LIGHTMAP, 1);

  gpu_SetMultitextureBlendMode(true);

  gpu_RenderPolygonUV2(&vArray2[0], nv);
}

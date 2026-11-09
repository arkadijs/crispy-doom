//
// Copyright(C) 2024 Crispy Doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//    Optional OpenGL 2.1 / GLSL 1.20 presentation path with a CRT shader.
//    This bypasses SDL_Renderer entirely; i_video.c falls back to the
//    SDL_Renderer path if anything in here fails.
//

#ifndef __I_CRT__
#define __I_CRT__

#include "SDL.h"
#include "doomtype.h"

// crt_shader values
#define CRT_OFF  0
#define CRT_LITE 1 // single pass: scanlines, mask, curvature, vignette
#define CRT_FULL 2 // lite + bloom/halation (needs framebuffer objects)

// crt_mask values
#define CRT_MASK_NONE     0
#define CRT_MASK_GRILLE   1 // aperture grille
#define CRT_MASK_SHADOW   2 // shadow mask (staggered dots)
#define CRT_MASK_SLOT     3 // slot mask

// User configuration (percent values are 0..100)
extern int crt_shader;
extern int crt_curvature;
extern int crt_scanlines;
extern int crt_mask;
extern int crt_mask_strength;
extern int crt_vignette;
extern int crt_softness;
extern int crt_bloom;
extern char *crt_shader_file;

typedef struct
{
    int src_w, src_h;     // size of the game framebuffer
    int lines;            // number of logical scanlines
    int aspect_h;         // height used for aspect ratio (e.g. 6/5 stretched)
    boolean integer_scale;
    boolean aspect_correct;
    boolean vsync;
    float tint[4];        // screen flash, rgb + alpha
} crt_frame_t;

// Must be called before the window is created.
void I_CRT_SetGLAttributes(void);

// Create context, load GL functions, compile shaders. Returns false (after
// cleaning up after itself) on any failure.
boolean I_CRT_Init(SDL_Window *window);
void I_CRT_Shutdown(void);

// (Re-)create the source texture for a new framebuffer size.
boolean I_CRT_ResizeSource(int w, int h, int lines);

void I_CRT_SetVsync(boolean vsync);
void I_CRT_SetBorderColor(int r, int g, int b);

// Upload "pixels" (ARGB8888, w*4 pitch) and draw to the window.
void I_CRT_Present(const void *pixels, const crt_frame_t *frame);

// Re-draw the last frame without re-uploading (used for screenshots).
// Returns a malloc()ed RGB24 buffer of the visible game area.
byte *I_CRT_ReadPixels(const crt_frame_t *frame, int *w, int *h, int *pitch);

// Name of the active tier, for logging.
const char *I_CRT_TierName(void);

#endif

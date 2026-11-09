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
//    OpenGL 2.1 / GLSL 1.20 CRT presentation path.
//
//    Design notes (see also the shader sources below):
//      * Only desktop OpenGL 2.1 + GLSL 1.20 features are used, so this runs
//        on old hardware (e.g. r300g/R500 under Mesa).
//      * All GL entry points, including 1.x ones, are loaded through
//        SDL_GL_GetProcAddress, so we neither link against nor depend on
//        libGL/GLEW.
//      * Lite tier: one pass, one texture fetch.
//      * Full tier: two half-res blur passes into FBOs (halation/bloom), then
//        the same composite pass. Requires framebuffer objects; falls back
//        to the lite tier if they are not available.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "SDL_opengl.h"

#include "i_crt.h"
#include "m_misc.h"

// Configuration (bound in i_video.c)

int crt_shader = CRT_OFF;
int crt_curvature = 20;
int crt_scanlines = 70;
int crt_mask = CRT_MASK_GRILLE;
int crt_mask_strength = 30;
int crt_vignette = 30;
int crt_softness = 0;
int crt_bloom = 25;
char *crt_shader_file = "";

// ---------------------------------------------------------------------------
// GL function table

#ifndef APIENTRY
#define APIENTRY
#endif

#define GL_CORE_FUNCS(X) \
    X(void,   Clear,              (GLbitfield)) \
    X(void,   ClearColor,         (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   Viewport,           (GLint, GLint, GLsizei, GLsizei)) \
    X(void,   GenTextures,        (GLsizei, GLuint *)) \
    X(void,   DeleteTextures,     (GLsizei, const GLuint *)) \
    X(void,   BindTexture,        (GLenum, GLuint)) \
    X(void,   TexParameteri,      (GLenum, GLenum, GLint)) \
    X(void,   TexImage2D,         (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void,   TexSubImage2D,      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void,   PixelStorei,        (GLenum, GLint)) \
    X(void,   ReadPixels,         (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(GLenum, GetError,           (void)) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(void,   ActiveTexture,      (GLenum)) \
    X(GLuint, CreateShader,       (GLenum)) \
    X(void,   ShaderSource,       (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,   CompileShader,      (GLuint)) \
    X(void,   GetShaderiv,        (GLuint, GLenum, GLint *)) \
    X(void,   GetShaderInfoLog,   (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   DeleteShader,       (GLuint)) \
    X(GLuint, CreateProgram,      (void)) \
    X(void,   AttachShader,       (GLuint, GLuint)) \
    X(void,   BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void,   LinkProgram,        (GLuint)) \
    X(void,   GetProgramiv,       (GLuint, GLenum, GLint *)) \
    X(void,   GetProgramInfoLog,  (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   DeleteProgram,      (GLuint)) \
    X(void,   UseProgram,         (GLuint)) \
    X(GLint,  GetUniformLocation, (GLuint, const GLchar *)) \
    X(void,   Uniform1i,          (GLint, GLint)) \
    X(void,   Uniform1f,          (GLint, GLfloat)) \
    X(void,   Uniform2f,          (GLint, GLfloat, GLfloat)) \
    X(void,   Uniform4f,          (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   EnableVertexAttribArray, (GLuint)) \
    X(void,   DisableVertexAttribArray, (GLuint)) \
    X(void,   VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,   DrawArrays,         (GLenum, GLint, GLsizei)) \
    X(void,   GenBuffers,         (GLsizei, GLuint *)) \
    X(void,   BindBuffer,         (GLenum, GLuint)) \
    X(void,   BufferData,         (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void,   DeleteBuffers,      (GLsizei, const GLuint *))

// Framebuffer objects are optional (EXT_framebuffer_object, or core/ARB names)
#define GL_FBO_FUNCS(X) \
    X(void,   GenFramebuffers,    (GLsizei, GLuint *)) \
    X(void,   DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void,   BindFramebuffer,    (GLenum, GLuint)) \
    X(void,   FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum))

#define DECLARE_FUNC(ret, name, args) ret (APIENTRY *name) args;

static struct
{
    GL_CORE_FUNCS(DECLARE_FUNC)
    GL_FBO_FUNCS(DECLARE_FUNC)
} gl;

// Fallback values for tokens that older headers might lack
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

#define LOAD_CORE(ret, name, args) \
    if ((gl.name = (ret (APIENTRY *) args) SDL_GL_GetProcAddress("gl" #name)) == NULL) \
    { \
        fprintf(stderr, "I_CRT: missing GL function gl%s\n", #name); \
        ok = false; \
    }

static boolean LoadCoreFunctions(void)
{
    boolean ok = true;
    GL_CORE_FUNCS(LOAD_CORE)
    return ok;
}

static boolean LoadFBOFunctions(void)
{
    boolean ok = true;

#define LOAD_FBO(ret, name, args) \
    if ((gl.name = (ret (APIENTRY *) args) SDL_GL_GetProcAddress("gl" #name)) == NULL \
     && (gl.name = (ret (APIENTRY *) args) SDL_GL_GetProcAddress("gl" #name "EXT")) == NULL) \
    { \
        ok = false; \
    }

    GL_FBO_FUNCS(LOAD_FBO)

#undef LOAD_FBO

    return ok;
}

// ---------------------------------------------------------------------------
// Shaders (GLSL 1.20)

static const char vertex_src[] =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main()\n"
    "{\n"
    "    // a_pos is 0..1 with y up; texture row 0 is the top of the screen\n"
    "    v_uv = vec2(a_pos.x, 1.0 - a_pos.y);\n"
    "    gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// 9-tap gaussian using the linear-filtering trick (5 fetches).
static const char blur_src[] =
    "uniform sampler2D u_src;\n"
    "uniform vec2 u_dir;\n"
    "varying vec2 v_uv;\n"
    "void main()\n"
    "{\n"
    "    vec3 c = texture2D(u_src, v_uv).rgb * 0.2270270270;\n"
    "    c += (texture2D(u_src, v_uv + u_dir * 1.3846153846).rgb\n"
    "        + texture2D(u_src, v_uv - u_dir * 1.3846153846).rgb) * 0.3162162162;\n"
    "    c += (texture2D(u_src, v_uv + u_dir * 3.2307692308).rgb\n"
    "        + texture2D(u_src, v_uv - u_dir * 3.2307692308).rgb) * 0.0702702703;\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

// The composite pass. Deliberately free of loops, dynamic branches, integer
// ops and derivatives so it stays within the limits of R300/R500 class
// hardware (fp24, no flow control on R3xx/R4xx).
static const char crt_src[] =
    "uniform sampler2D u_src;\n"
    "#ifdef CRT_BLOOM\n"
    "uniform sampler2D u_bloom;\n"
    "#endif\n"
    "uniform vec2  u_srcsize;   // source texture size in texels\n"
    "uniform vec2  u_outsize;   // output viewport size in pixels\n"
    "uniform vec4  u_p1;        // curvature, scanline strength, mask strength, vignette\n"
    "uniform vec4  u_p2;        // softness, scanline count, mask type, bloom\n"
    "uniform vec4  u_tint;      // screen flash color + alpha\n"
    "uniform float u_maskscale; // mask cell scale in output pixels\n"
    "varying vec2 v_uv;\n"
    "\n"
    "// one-hot RGB triad for an integer x\n"
    "vec3 triad(float x)\n"
    "{\n"
    "    float m = mod(x, 3.0);\n"
    "    return vec3(step(m, 0.5), step(abs(m - 1.0), 0.5), step(1.5, m));\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "    // --- screen curvature -------------------------------------------\n"
    "    vec2 c = v_uv * 2.0 - 1.0;\n"
    "    float vig = 1.0 - u_p1.w * 0.4 * dot(c, c);\n"
    "    float k = u_p1.x * 0.15;\n"
    "    c *= vec2(1.0 + c.y * c.y * k, 1.0 + c.x * c.x * k);\n"
    "    vec2 uv = c * 0.5 + 0.5;\n"
    "    vec2 aa = vec2(0.0001) + (1.5 / u_outsize) * min(k * 1000.0, 1.0);\n"
    "    vec2 inside = smoothstep(vec2(0.0), aa, uv)\n"
    "                * (1.0 - smoothstep(vec2(1.0) - aa, vec2(1.0), uv));\n"
    "    float edge = inside.x * inside.y;\n"
    "\n"
    "    // --- source sampling: sharp-bilinear in x, snapped to the scanline\n"
    "    //     center in y, so scanlines are tied to the source and don't moire\n"
    "    float lines = u_p2.y;\n"
    "    float ly = uv.y * lines;\n"
    "    float dy = fract(ly) - 0.5;\n"
    "    float sy = (floor(ly) + 0.5) / lines;\n"
    "\n"
    "    float px = uv.x * u_srcsize.x;\n"
    "    float scale = mix(max(u_outsize.x / u_srcsize.x, 1.0), 1.0, u_p2.x);\n"
    "    float cd = fract(px) - 0.5;\n"
    "    float rr = 0.5 - 0.5 / scale;\n"
    "    float fx = (cd - clamp(cd, -rr, rr)) * scale + 0.5;\n"
    "    vec2 suv = vec2((floor(px) + fx) / u_srcsize.x, sy);\n"
    "\n"
    "    vec3 col = texture2D(u_src, suv).rgb;\n"
    "    col *= col; // approximate linear light\n"
    "\n"
    "#ifdef CRT_BLOOM\n"
    "    vec3 b = texture2D(u_bloom, uv).rgb;\n"
    "    col += b * b * u_p2.w;\n"
    "#endif\n"
    "\n"
    "    // --- scanlines: bright pixels get a wider beam -------------------\n"
    "    float luma = clamp(dot(col, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);\n"
    "    float beam = exp2(-dy * dy * mix(14.0, 4.0, luma));\n"
    "    col *= mix(1.0, beam * 1.3, u_p1.y);\n"
    "\n"
    "    // --- phosphor mask (computed from the output pixel position) -----\n"
    "    vec2 fc = floor(gl_FragCoord.xy / u_maskscale);\n"
    "    float type = u_p2.z;\n"
    "    float s = u_p1.z;\n"
    "    float is_any = float(type > 0.5);\n"
    "    float is_shadow = float(type > 1.5 && type < 2.5);\n"
    "    float is_slot = float(type > 2.5);\n"
    "    vec3 t = triad(fc.x + is_shadow * mod(fc.y, 2.0));\n"
    "    float yy = mod(fc.y + 2.0 * mod(floor(fc.x / 3.0), 2.0), 4.0);\n"
    "    float gap = mix(1.0, step(yy, 2.5), is_slot * s);\n"
    "    vec3 mk = mix(vec3(1.0), vec3(1.0 - s) + (1.5 * s) * t, is_any) * gap;\n"
    "    col *= mk;\n"
    "\n"
    "    col *= (vig * edge) * 1.3; // compensate mask/scanline darkening\n"
    "    col = sqrt(clamp(col, 0.0, 1.0));\n"
    "    col = mix(col, u_tint.rgb, u_tint.a);\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

// ---------------------------------------------------------------------------
// State

typedef struct
{
    GLuint id;
    GLint src, bloom, srcsize, outsize, p1, p2, tint, maskscale, dir;
} program_t;

static SDL_Window *win;
static SDL_GLContext glctx;
static int tier;                 // CRT_LITE or CRT_FULL

static GLuint vbo;
static GLuint src_tex;
static int src_w, src_h;

static program_t prog_crt, prog_blur;
static GLuint bloom_tex[2], bloom_fbo[2];
static int bloom_w, bloom_h;

static float border[3];
static int vsync_state = -1;
static boolean have_frame;       // something was uploaded to src_tex

const char *I_CRT_TierName(void)
{
    return tier == CRT_FULL ? "full" : "lite";
}

void I_CRT_SetGLAttributes(void)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

static GLuint CompileShader(GLenum type, int nsrc, const char **src,
                            const char *name)
{
    GLuint sh = gl.CreateShader(type);
    GLint ok = 0;

    gl.ShaderSource(sh, nsrc, src, NULL);
    gl.CompileShader(sh);
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);

    if (!ok)
    {
        char log[2048] = "";
        gl.GetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "I_CRT: failed to compile %s shader:\n%s\n", name, log);
        gl.DeleteShader(sh);
        return 0;
    }

    return sh;
}

static char *ReadTextFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;

    if (f == NULL)
    {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024)
    {
        fclose(f);
        return NULL;
    }

    buf = malloc(len + 1);
    if (buf == NULL || fread(buf, 1, len, f) != (size_t) len)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static boolean LinkProgram(program_t *p, const char *vs_name, const char *fs_name,
                           const char **fs, int nfs)
{
    const char *vs[2] = { "#version 120\n", vertex_src };
    GLuint v, f;
    GLint ok = 0;

    memset(p, 0, sizeof(*p));

    v = CompileShader(GL_VERTEX_SHADER, 2, vs, vs_name);
    f = CompileShader(GL_FRAGMENT_SHADER, nfs, fs, fs_name);

    if (v == 0 || f == 0)
    {
        if (v) gl.DeleteShader(v);
        if (f) gl.DeleteShader(f);
        return false;
    }

    p->id = gl.CreateProgram();
    gl.AttachShader(p->id, v);
    gl.AttachShader(p->id, f);
    gl.BindAttribLocation(p->id, 0, "a_pos");
    gl.LinkProgram(p->id);
    gl.DeleteShader(v);
    gl.DeleteShader(f);

    gl.GetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048] = "";
        gl.GetProgramInfoLog(p->id, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "I_CRT: failed to link %s program:\n%s\n", fs_name, log);
        gl.DeleteProgram(p->id);
        p->id = 0;
        return false;
    }

    p->src       = gl.GetUniformLocation(p->id, "u_src");
    p->bloom     = gl.GetUniformLocation(p->id, "u_bloom");
    p->srcsize   = gl.GetUniformLocation(p->id, "u_srcsize");
    p->outsize   = gl.GetUniformLocation(p->id, "u_outsize");
    p->p1        = gl.GetUniformLocation(p->id, "u_p1");
    p->p2        = gl.GetUniformLocation(p->id, "u_p2");
    p->tint      = gl.GetUniformLocation(p->id, "u_tint");
    p->maskscale = gl.GetUniformLocation(p->id, "u_maskscale");
    p->dir       = gl.GetUniformLocation(p->id, "u_dir");
    return true;
}

static boolean BuildCompositeProgram(boolean bloom)
{
    const char *fs[3];
    char *external = NULL;
    boolean ok;

    if (crt_shader_file != NULL && crt_shader_file[0] != '\0')
    {
        external = ReadTextFile(crt_shader_file);

        if (external == NULL)
        {
            fprintf(stderr, "I_CRT: can't read crt_shader_file '%s', "
                            "using the built-in shader\n", crt_shader_file);
        }
    }

    if (external != NULL)
    {
        // The file replaces the whole fragment shader. If it has no #version
        // line we add one (and the CRT_BLOOM define) for convenience.
        if (strstr(external, "#version") != NULL)
        {
            fs[0] = external;
            ok = LinkProgram(&prog_crt, "vertex", "external CRT", fs, 1);
        }
        else
        {
            fs[0] = bloom ? "#version 120\n#define CRT_BLOOM 1\n"
                          : "#version 120\n";
            fs[1] = external;
            ok = LinkProgram(&prog_crt, "vertex", "external CRT", fs, 2);
        }

        free(external);

        if (ok)
        {
            return true;
        }

        fprintf(stderr, "I_CRT: external shader failed, "
                        "using the built-in shader\n");
    }

    fs[0] = "#version 120\n";
    fs[1] = bloom ? "#define CRT_BLOOM 1\n" : "\n";
    fs[2] = crt_src;
    return LinkProgram(&prog_crt, "vertex", "CRT", fs, 3);
}

static void SetTexParams(GLuint tex, GLint filter)
{
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    // Non-power-of-two textures (e.g. 320x200) must use clamp-to-edge on
    // some old hardware.
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void DestroyBloomTargets(void)
{
    int i;

    for (i = 0; i < 2; ++i)
    {
        if (bloom_fbo[i] != 0 && gl.DeleteFramebuffers != NULL)
        {
            gl.DeleteFramebuffers(1, &bloom_fbo[i]);
        }
        if (bloom_tex[i] != 0)
        {
            gl.DeleteTextures(1, &bloom_tex[i]);
        }
        bloom_fbo[i] = 0;
        bloom_tex[i] = 0;
    }
}

static boolean CreateBloomTargets(int w, int h)
{
    int i;

    for (i = 0; i < 2; ++i)
    {
        gl.GenTextures(1, &bloom_tex[i]);
        SetTexParams(bloom_tex[i], GL_LINEAR);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        gl.GenFramebuffers(1, &bloom_fbo[i]);
        gl.BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i]);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, bloom_tex[i], 0);

        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
    }

    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    bloom_w = w;
    bloom_h = h;
    return true;
}

static boolean HasExtension(const char *name)
{
    const char *ext = (const char *) gl.GetString(GL_EXTENSIONS);
    size_t len = strlen(name);
    const char *p = ext;

    while (p != NULL && (p = strstr(p, name)) != NULL)
    {
        if ((p == ext || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0'))
        {
            return true;
        }
        p += len;
    }
    return false;
}

boolean I_CRT_Init(SDL_Window *window)
{
    static const GLfloat quad[] = { 0, 0,  1, 0,  0, 1,  1, 1 };
    boolean want_bloom;

    win = window;
    tier = crt_shader >= CRT_FULL ? CRT_FULL : CRT_LITE;
    vsync_state = -1;
    have_frame = false;

    glctx = SDL_GL_CreateContext(window);
    if (glctx == NULL)
    {
        fprintf(stderr, "I_CRT: can't create an OpenGL 2.1 context: %s\n",
                SDL_GetError());
        return false;
    }

    if (SDL_GL_MakeCurrent(window, glctx) != 0 || !LoadCoreFunctions())
    {
        fprintf(stderr, "I_CRT: can't make the OpenGL context current: %s\n",
                SDL_GetError());
        SDL_GL_DeleteContext(glctx);
        glctx = NULL;
        return false;
    }

    printf("I_CRT: OpenGL %s on %s (GLSL %s)\n",
           (const char *) gl.GetString(GL_VERSION),
           (const char *) gl.GetString(GL_RENDERER),
           (const char *) gl.GetString(GL_SHADING_LANGUAGE_VERSION));

    if (tier == CRT_FULL)
    {
        if (!(HasExtension("GL_EXT_framebuffer_object")
           || HasExtension("GL_ARB_framebuffer_object"))
         || !LoadFBOFunctions())
        {
            printf("I_CRT: framebuffer objects not available, "
                   "using the lite shader\n");
            tier = CRT_LITE;
        }
    }

    want_bloom = (tier == CRT_FULL);

    if (!BuildCompositeProgram(want_bloom))
    {
        goto fail;
    }

    if (want_bloom)
    {
        const char *fs[2] = { "#version 120\n", blur_src };

        if (!LinkProgram(&prog_blur, "vertex", "blur", fs, 2))
        {
            printf("I_CRT: falling back to the lite shader\n");
            tier = CRT_LITE;
            gl.DeleteProgram(prog_crt.id);
            if (!BuildCompositeProgram(false))
            {
                goto fail;
            }
        }
    }

    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    gl.ClearColor(0, 0, 0, 1);

    printf("I_CRT: using the %s CRT shader\n", I_CRT_TierName());
    return true;

fail:
    I_CRT_Shutdown();
    return false;
}

void I_CRT_Shutdown(void)
{
    if (glctx == NULL)
    {
        return;
    }

    if (gl.DeleteTextures != NULL)
    {
        DestroyBloomTargets();
        if (src_tex) gl.DeleteTextures(1, &src_tex);
        if (vbo) gl.DeleteBuffers(1, &vbo);
        if (prog_crt.id) gl.DeleteProgram(prog_crt.id);
        if (prog_blur.id) gl.DeleteProgram(prog_blur.id);
    }

    src_tex = 0;
    vbo = 0;
    memset(&prog_crt, 0, sizeof(prog_crt));
    memset(&prog_blur, 0, sizeof(prog_blur));

    SDL_GL_DeleteContext(glctx);
    glctx = NULL;
}

boolean I_CRT_ResizeSource(int w, int h, int lines)
{
    if (glctx == NULL)
    {
        return false;
    }

    SDL_GL_MakeCurrent(win, glctx);

    if (src_tex != 0)
    {
        gl.DeleteTextures(1, &src_tex);
        src_tex = 0;
    }

    gl.GenTextures(1, &src_tex);
    SetTexParams(src_tex, GL_LINEAR);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

    src_w = w;
    src_h = h;
    have_frame = false;

    DestroyBloomTargets();

    if (tier == CRT_FULL)
    {
        int bh = lines > 0 ? lines : h;
        int bw = (int) ((long) w * bh / h);

        if (bw < 1) bw = 1;

        if (!CreateBloomTargets(bw, bh))
        {
            fprintf(stderr, "I_CRT: can't create bloom render targets, "
                            "using the lite shader\n");
            DestroyBloomTargets();
            tier = CRT_LITE;

            // The composite program was built with CRT_BLOOM; rebuild it.
            gl.DeleteProgram(prog_crt.id);
            if (!BuildCompositeProgram(false))
            {
                return false;
            }
        }
    }

    return gl.GetError() == GL_NO_ERROR;
}

void I_CRT_SetVsync(boolean vsync)
{
    if (glctx == NULL || vsync_state == (int) vsync)
    {
        return;
    }

    SDL_GL_MakeCurrent(win, glctx);

    if (SDL_GL_SetSwapInterval(vsync ? 1 : 0) != 0)
    {
        fprintf(stderr, "I_CRT: can't set swap interval: %s\n", SDL_GetError());
    }

    vsync_state = vsync;
}

void I_CRT_SetBorderColor(int r, int g, int b)
{
    border[0] = r / 255.0f;
    border[1] = g / 255.0f;
    border[2] = b / 255.0f;
}

static float Pct(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v / 100.0f;
}

// Compute the destination rectangle (in drawable pixels, GL origin).
static void GetDestRect(const crt_frame_t *f, int dw, int dh,
                        int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = 0;
    *w = dw;
    *h = dh;

    if (f->integer_scale)
    {
        int sx = dw / f->src_w;
        int sy = dh / f->aspect_h;
        int scale = sx < sy ? sx : sy;

        if (scale >= 1)
        {
            *w = f->src_w * scale;
            *h = f->aspect_h * scale;
            *x = (dw - *w) / 2;
            *y = (dh - *h) / 2;
            return;
        }
    }

    if (f->aspect_correct || f->integer_scale)
    {
        if (dw * f->aspect_h > dh * f->src_w)
        {
            *w = dh * f->src_w / f->aspect_h;
            *x = (dw - *w) / 2;
        }
        else
        {
            *h = dw * f->aspect_h / f->src_w;
            *y = (dh - *h) / 2;
        }
    }
}

static void BindQuad(void)
{
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
}

static void BloomPasses(void)
{
    // Pass 1: source -> horizontal blur, at bloom resolution.
    gl.BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[0]);
    gl.Viewport(0, 0, bloom_w, bloom_h);
    gl.UseProgram(prog_blur.id);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src_tex);
    gl.Uniform1i(prog_blur.src, 0);
    gl.Uniform2f(prog_blur.dir, 1.0f / bloom_w, 0.0f);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Pass 2: vertical blur.
    gl.BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[1]);
    gl.BindTexture(GL_TEXTURE_2D, bloom_tex[0]);
    gl.Uniform2f(prog_blur.dir, 0.0f, 1.0f / bloom_h);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Draw the last uploaded frame into the back buffer. Returns the dest rect.
static void Draw(const crt_frame_t *f, int *rx, int *ry, int *rw, int *rh)
{
    int dw, dh, x, y, w, h;
    boolean bloom = (tier == CRT_FULL && crt_bloom > 0);
    int mask_scale;

    SDL_GL_GetDrawableSize(win, &dw, &dh);
    GetDestRect(f, dw, dh, &x, &y, &w, &h);

    BindQuad();

    if (bloom)
    {
        BloomPasses();
    }

    gl.Viewport(0, 0, dw, dh);
    gl.ClearColor(border[0], border[1], border[2], 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport(x, y, w, h);

    gl.UseProgram(prog_crt.id);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src_tex);
    gl.Uniform1i(prog_crt.src, 0);

    if (tier == CRT_FULL)
    {
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, bloom ? bloom_tex[1] : src_tex);
        gl.Uniform1i(prog_crt.bloom, 1);
        gl.ActiveTexture(GL_TEXTURE0);
    }

    // Keep the phosphor mask cells at a sensible size on large outputs.
    mask_scale = (h + 540) / 1080;
    if (mask_scale < 1) mask_scale = 1;

    gl.Uniform2f(prog_crt.srcsize, (float) f->src_w, (float) f->src_h);
    gl.Uniform2f(prog_crt.outsize, (float) w, (float) h);
    gl.Uniform4f(prog_crt.p1, Pct(crt_curvature), Pct(crt_scanlines),
                 Pct(crt_mask_strength), Pct(crt_vignette));
    gl.Uniform4f(prog_crt.p2, Pct(crt_softness), (float) f->lines,
                 (float) crt_mask, bloom ? Pct(crt_bloom) : 0.0f);
    gl.Uniform4f(prog_crt.tint, f->tint[0], f->tint[1], f->tint[2], f->tint[3]);
    gl.Uniform1f(prog_crt.maskscale, (float) mask_scale);

    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    gl.DisableVertexAttribArray(0);
    gl.UseProgram(0);

    *rx = x; *ry = y; *rw = w; *rh = h;
}

void I_CRT_Present(const void *pixels, const crt_frame_t *f)
{
    int x, y, w, h;

    if (glctx == NULL)
    {
        return;
    }

    SDL_GL_MakeCurrent(win, glctx);
    I_CRT_SetVsync(f->vsync);

    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src_tex);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->src_w, f->src_h,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
    have_frame = true;

    Draw(f, &x, &y, &w, &h);

    SDL_GL_SwapWindow(win);
}

byte *I_CRT_ReadPixels(const crt_frame_t *f, int *rw, int *rh, int *pitch)
{
    int x, y, w, h, row;
    byte *buf, *tmp;

    if (glctx == NULL || !have_frame)
    {
        return NULL;
    }

    SDL_GL_MakeCurrent(win, glctx);

    // The back buffer content is undefined after a swap; redraw it.
    Draw(f, &x, &y, &w, &h);

    buf = malloc((size_t) w * h * 3);
    tmp = malloc((size_t) w * 3);
    if (buf == NULL || tmp == NULL)
    {
        free(buf);
        free(tmp);
        return NULL;
    }

    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);

    // GL reads bottom-up; flip.
    for (row = 0; row < h / 2; ++row)
    {
        byte *a = buf + (size_t) row * w * 3;
        byte *b = buf + (size_t) (h - 1 - row) * w * 3;
        memcpy(tmp, a, (size_t) w * 3);
        memcpy(a, b, (size_t) w * 3);
        memcpy(b, tmp, (size_t) w * 3);
    }

    free(tmp);

    *rw = w;
    *rh = h;
    *pitch = w * 3;
    return buf;
}

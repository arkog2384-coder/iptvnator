/*
 * Headless GL context for the frame-copy helper, one implementation per
 * platform behind the same tiny GlContext surface.
 *
 * macOS: accelerated CGL 3.2 core context; GL entry points for mpv come
 * from the OpenGL framework via dlsym.
 *
 * Linux: EGL display picked in order — Mesa's surfaceless platform (no
 * display server needed), the session's default display, then a GBM render
 * node — with a desktop-GL 3.2 core context bound surfaceless (1x1 pbuffer
 * fallback for drivers without EGL_KHR_surfaceless_context). The helper's
 * own GL calls resolve at link time through libOpenGL (glvnd); mpv resolves
 * its entry points via eglGetProcAddress.
 *
 * Windows: WGL against a hidden 1x1 window (never shown, never pumped —
 * offscreen FBO rendering only, no SwapBuffers). A legacy context
 * bootstraps wglCreateContextAttribsARB for a 3.2 core context. opengl32
 * only exports GL 1.1, so the post-1.1 entry points the render pipeline
 * needs are declared here as same-named function pointers and resolved via
 * wglGetProcAddress; mpv resolves its own through the same loader.
 *
 * Threading contract: create() runs on the main thread and must leave the
 * context unbound; the render thread calls makeCurrent() once and owns the
 * context until destroy().
 */
#pragma once

#include <string>

#if defined(__APPLE__)

#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <dlfcn.h>

#elif defined(__linux__)

/* Keep eglplatform.h away from Xlib types: the helper never uses native
 * window/display handles, so X11 headers are an unnecessary build dep. */
#define EGL_NO_X11 1
#define MESA_EGL_NO_X11_HEADERS 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GL/gl.h>

#include <cstdint>
#include <cstdio>

/* The Windows SDK ships GL 1.1 headers only; everything newer that the
 * render pipeline touches is declared below and resolved at runtime. */
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif

typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

#endif

namespace frame_helper {

/* Matches mpv_opengl_init_params.get_proc_address. */
using GlGetProcAddressFn = void* (*)(void* ctx, const char* name);

#if defined(__APPLE__)

inline void* glDlsymGetProcAddress(void* ctx, const char* name) {
    return dlsym(ctx, name);
}

class GlContext {
public:
    bool create(std::string& errorOut) {
        glDylib_ = dlopen(
            "/System/Library/Frameworks/OpenGL.framework/Versions/Current/"
            "OpenGL",
            RTLD_LAZY | RTLD_LOCAL);
        if (!glDylib_) {
            errorOut = "failed to open the OpenGL framework";
            return false;
        }

        CGLPixelFormatAttribute attrs[] = {
            kCGLPFAOpenGLProfile,
            (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
            kCGLPFAAccelerated,
            kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
            kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
            (CGLPixelFormatAttribute)0,
        };
        CGLPixelFormatObj pixelFormat = nullptr;
        GLint matched = 0;
        if (CGLChoosePixelFormat(attrs, &pixelFormat, &matched) !=
                kCGLNoError ||
            !pixelFormat) {
            errorOut = "no accelerated CGL pixel format";
            return false;
        }
        const CGLError contextError =
            CGLCreateContext(pixelFormat, nullptr, &cgl_);
        CGLDestroyPixelFormat(pixelFormat);
        if (contextError != kCGLNoError || !cgl_) {
            errorOut = "failed to create a headless CGL context";
            return false;
        }
        return true;
    }

    void makeCurrent() { CGLSetCurrentContext(cgl_); }

    void destroy() {
        CGLSetCurrentContext(nullptr);
        if (cgl_) CGLDestroyContext(cgl_);
        cgl_ = nullptr;
        /* glDylib_ stays mapped: mpv may still resolve symbols during its
         * own teardown, and the process exits right after anyway. */
    }

    GlGetProcAddressFn procLoader() const { return glDlsymGetProcAddress; }
    void* procLoaderCtx() const { return glDylib_; }

private:
    CGLContextObj cgl_ = nullptr;
    void* glDylib_ = nullptr;
};

#elif defined(__linux__)

inline void* eglWrapGetProcAddress(void* /*ctx*/, const char* name) {
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

class GlContext {
public:
    bool create(std::string& errorOut) {
        if (!acquireDisplay()) {
            errorOut = "no usable EGL display (surfaceless/default/GBM)";
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
            errorOut = "EGL implementation lacks desktop OpenGL support";
            return false;
        }
        EGLConfig config = nullptr;
        if (!chooseConfig(EGL_PBUFFER_BIT, &config) &&
            !chooseConfig(0, &config)) {
            errorOut = "no usable EGLConfig for desktop OpenGL";
            return false;
        }
        const EGLint contextAttrs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 2,
            EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE,
        };
        context_ =
            eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttrs);
        if (context_ == EGL_NO_CONTEXT) {
            errorOut = "failed to create a 3.2 core EGL context";
            return false;
        }
        /* Probe surfaceless binding here (main thread) so the render thread
         * can just makeCurrent(); fall back to a 1x1 pbuffer. Unbind before
         * returning — see the threading contract above. */
        if (eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           context_) != EGL_TRUE) {
            const EGLint pbufferAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1,
                                           EGL_NONE};
            surface_ = eglCreatePbufferSurface(display_, config, pbufferAttrs);
            if (surface_ == EGL_NO_SURFACE ||
                eglMakeCurrent(display_, surface_, surface_, context_) !=
                    EGL_TRUE) {
                errorOut =
                    "eglMakeCurrent failed (no surfaceless context or "
                    "pbuffer)";
                return false;
            }
        }
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return true;
    }

    void makeCurrent() {
        eglMakeCurrent(display_, surface_, surface_, context_);
    }

    void destroy() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
            if (surface_ != EGL_NO_SURFACE)
                eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT)
                eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
        surface_ = EGL_NO_SURFACE;
        context_ = EGL_NO_CONTEXT;
        display_ = EGL_NO_DISPLAY;
        if (gbmDevice_) {
            gbm_device_destroy(gbmDevice_);
            gbmDevice_ = nullptr;
        }
        if (gbmFd_ >= 0) {
            close(gbmFd_);
            gbmFd_ = -1;
        }
    }

    GlGetProcAddressFn procLoader() const { return eglWrapGetProcAddress; }
    void* procLoaderCtx() const { return nullptr; }

private:
    bool acquireDisplay() {
        /* Tier log goes to stderr (the adapter mirrors it) — stdout is
         * reserved for the JSON event protocol. */
        display_ = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                         nullptr, nullptr);
        if (initDisplay(display_)) {
            std::fprintf(stderr, "egl display: surfaceless-mesa\n");
            return true;
        }

        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (initDisplay(display_)) {
            std::fprintf(stderr, "egl display: default\n");
            return true;
        }

        display_ = EGL_NO_DISPLAY;
        if (openGbmDevice()) {
            display_ = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbmDevice_,
                                             nullptr);
            if (initDisplay(display_)) {
                std::fprintf(stderr, "egl display: gbm render node\n");
                return true;
            }
        }
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    static bool initDisplay(EGLDisplay display) {
        if (display == EGL_NO_DISPLAY) return false;
        EGLint major = 0;
        EGLint minor = 0;
        return eglInitialize(display, &major, &minor) == EGL_TRUE;
    }

    bool openGbmDevice() {
        for (int node = 128; node <= 131; node++) {
            char devicePath[32];
            std::snprintf(devicePath, sizeof(devicePath),
                          "/dev/dri/renderD%d", node);
            const int fd = open(devicePath, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            struct gbm_device* device = gbm_create_device(fd);
            if (!device) {
                close(fd);
                continue;
            }
            gbmFd_ = fd;
            gbmDevice_ = device;
            return true;
        }
        return false;
    }

    bool chooseConfig(EGLint surfaceType, EGLConfig* out) {
        const EGLint attrs[] = {
            EGL_SURFACE_TYPE, surfaceType,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLint matched = 0;
        return eglChooseConfig(display_, attrs, out, 1, &matched) ==
                   EGL_TRUE &&
               matched > 0;
    }

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    struct gbm_device* gbmDevice_ = nullptr;
    int gbmFd_ = -1;
};

#elif defined(_WIN32)

/* Post-1.1 GL entry points used by frame_helper_render.h, with their real
 * names so the shared render code compiles unchanged. APIENTRY (stdcall)
 * must match the driver exports. Single-TU helper; inline for safety. */
#define FRAME_HELPER_WGL_ENTRY_POINTS(X)                                      \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* out))                      \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* ids))             \
    X(void, glBindFramebuffer, (GLenum target, GLuint fbo))                   \
    X(void, glFramebufferTexture2D,                                           \
      (GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    \
       GLint level))                                                          \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                      \
    X(void, glGenBuffers, (GLsizei n, GLuint* out))                           \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* ids))                  \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                     \
    X(void, glBufferData,                                                     \
      (GLenum target, GLsizeiptr size, const void* data, GLenum usage))       \
    X(void*, glMapBufferRange,                                                \
      (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)) \
    X(GLboolean, glUnmapBuffer, (GLenum target))

#define FRAME_HELPER_WGL_DECLARE(ret, name, args)                             \
    inline ret(APIENTRY* name) args = nullptr;
FRAME_HELPER_WGL_ENTRY_POINTS(FRAME_HELPER_WGL_DECLARE)
#undef FRAME_HELPER_WGL_DECLARE

typedef HGLRC(WINAPI* FrameHelperWglCreateContextAttribsFn)(HDC, HGLRC,
                                                            const int*);

/* wglGetProcAddress covers post-1.1 symbols but returns sentinel values for
 * the 1.1 core set, which lives in opengl32.dll instead. */
inline void* wglLoaderGetProcAddress(void* ctx, const char* name) {
    PROC proc = wglGetProcAddress(name);
    const intptr_t sentinel = reinterpret_cast<intptr_t>(proc);
    if (proc && sentinel != 1 && sentinel != 2 && sentinel != 3 &&
        sentinel != -1) {
        return reinterpret_cast<void*>(proc);
    }
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(ctx), name));
}

class GlContext {
public:
    bool create(std::string& errorOut) {
        opengl32_ = GetModuleHandleA("opengl32.dll");
        if (!opengl32_) {
            errorOut = "opengl32.dll is not loaded";
            return false;
        }

        WNDCLASSA windowClass = {};
        windowClass.style = CS_OWNDC;
        windowClass.lpfnWndProc = DefWindowProcA;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = "iptvnator_mpv_helper_gl";
        /* Re-registration fails harmlessly if a previous context leaked the
         * class; CreateWindow below is the real gate. */
        RegisterClassA(&windowClass);
        window_ = CreateWindowExA(0, windowClass.lpszClassName, "", WS_POPUP,
                                  0, 0, 1, 1, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
        if (!window_) {
            errorOut = "failed to create the hidden GL window";
            return false;
        }
        dc_ = GetDC(window_);
        if (!dc_) {
            errorOut = "failed to get a DC for the hidden GL window";
            return false;
        }

        PIXELFORMATDESCRIPTOR descriptor = {};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        descriptor.cAlphaBits = 8;
        descriptor.iLayerType = PFD_MAIN_PLANE;
        const int pixelFormat = ChoosePixelFormat(dc_, &descriptor);
        if (pixelFormat == 0 ||
            SetPixelFormat(dc_, pixelFormat, &descriptor) != TRUE) {
            errorOut = "no usable pixel format for the hidden GL window";
            return false;
        }

        HGLRC legacy = wglCreateContext(dc_);
        if (!legacy) {
            errorOut = "failed to create a legacy WGL context";
            return false;
        }
        if (wglMakeCurrent(dc_, legacy) != TRUE) {
            wglDeleteContext(legacy);
            errorOut = "failed to bind the legacy WGL context";
            return false;
        }

        const auto createContextAttribs =
            reinterpret_cast<FrameHelperWglCreateContextAttribsFn>(
                wglGetProcAddress("wglCreateContextAttribsARB"));
        if (createContextAttribs) {
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 2,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0,
            };
            HGLRC core = createContextAttribs(dc_, nullptr, attribs);
            if (core) {
                wglMakeCurrent(dc_, core);
                wglDeleteContext(legacy);
                context_ = core;
            } else {
                /* Pre-3.2 driver: keep the legacy (compatibility) context;
                 * the entry-point loading below decides adequacy. */
                context_ = legacy;
            }
        } else {
            context_ = legacy;
        }

        bool loaded = true;
        const char* missing = nullptr;
#define FRAME_HELPER_WGL_LOAD(ret, name, args)                                \
    name = reinterpret_cast<ret(APIENTRY*) args>(                             \
        wglLoaderGetProcAddress(opengl32_, #name));                           \
    if (!name && loaded) {                                                    \
        loaded = false;                                                       \
        missing = #name;                                                      \
    }
        FRAME_HELPER_WGL_ENTRY_POINTS(FRAME_HELPER_WGL_LOAD)
#undef FRAME_HELPER_WGL_LOAD
        if (!loaded) {
            errorOut = std::string("missing GL entry point: ") + missing;
            wglMakeCurrent(nullptr, nullptr);
            return false;
        }

        /* Unbind before returning — see the threading contract above. */
        wglMakeCurrent(nullptr, nullptr);
        return true;
    }

    void makeCurrent() { wglMakeCurrent(dc_, context_); }

    void destroy() {
        wglMakeCurrent(nullptr, nullptr);
        if (context_) wglDeleteContext(context_);
        context_ = nullptr;
        /* The hidden window/class belong to the main thread (DestroyWindow
         * is thread-affine and this runs on the render thread); the process
         * exits right after, so they are left to OS teardown. */
        dc_ = nullptr;
        window_ = nullptr;
    }

    GlGetProcAddressFn procLoader() const { return wglLoaderGetProcAddress; }
    void* procLoaderCtx() const { return opengl32_; }

private:
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC context_ = nullptr;
    HMODULE opengl32_ = nullptr;
};

#else
#error "frame_helper_gl.h has no GL context implementation for this platform"
#endif

} // namespace frame_helper

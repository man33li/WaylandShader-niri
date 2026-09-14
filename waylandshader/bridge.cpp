#include "bridge.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#ifndef LIBRA_RUNTIME_OPENGL
#define LIBRA_RUNTIME_OPENGL
#endif
#include <librashader.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message)
{
    throw std::runtime_error(message);
}

[[noreturn]] void eglFailure(const char* operation)
{
    char message[256];
    std::snprintf(message, sizeof(message), "%s failed (EGL error 0x%04x)", operation, eglGetError());
    fail(message);
}

void checkGl(const char* operation)
{
    const GLenum code = glGetError();
    if (code != GL_NO_ERROR) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s failed (OpenGL error 0x%04x)", operation, code);
        fail(message);
    }
}

void report(char* error, size_t size, const char* message) noexcept
{
    if (error && size) {
        std::snprintf(error, size, "%s", message);
    }
}

void discardLibra(libra_error_t error) noexcept
{
    if (error) {
        libra_error_print(error);
        libra_error_free(&error);
    }
}

void checkLibra(libra_error_t error)
{
    if (!error) {
        return;
    }
    char* message = nullptr;
    libra_error_write(error, &message);
    // Release the C API allocations even if copying the diagnostic throws.
    try {
        std::string text = message ? message : "librashader returned an error without a diagnostic";
        if (message) {
            libra_error_free_string(&message);
        }
        libra_error_free(&error);
        throw std::runtime_error(text);
    } catch (...) {
        if (message) {
            libra_error_free_string(&message);
        }
        if (error) {
            libra_error_free(&error);
        }
        throw;
    }
}

// EGL binding is thread-local, including the selected client API. A loading
// worker commonly has no context, and must return in precisely that state.
class CurrentContext {
public:
    explicit CurrentContext(EGLDisplay target) noexcept
        : target_(target)
        , display_(eglGetCurrentDisplay())
        , context_(eglGetCurrentContext())
        , draw_(eglGetCurrentSurface(EGL_DRAW))
        , read_(eglGetCurrentSurface(EGL_READ))
        , api_(eglQueryAPI())
    {
    }

    ~CurrentContext()
    {
        if (!restore()) {
            std::fprintf(stderr, "WaylandShader: could not restore caller EGL context (0x%04x)\n", eglGetError());
        }
    }

    void selectDesktop()
    {
        changed_ = true;
        if (!eglBindAPI(EGL_OPENGL_API)) {
            eglFailure("eglBindAPI(OpenGL)");
        }
    }

    void enter(EGLContext context, EGLSurface surface)
    {
        selectDesktop();
        if (!eglMakeCurrent(target_, surface, surface, context)) {
            eglFailure("eglMakeCurrent(private desktop-GL context)");
        }
    }

    bool restore() noexcept
    {
        if (!changed_) {
            return true;
        }
        // Try all restoration operations even when a lost context cannot be
        // released. In particular, never leave the caller's API changed.
        bool ok = eglBindAPI(EGL_OPENGL_API);
        if (!eglMakeCurrent(target_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
            ok = false;
        }
        if (!eglBindAPI(api_)) {
            ok = false;
        }
        if (context_ != EGL_NO_CONTEXT && !eglMakeCurrent(display_, draw_, read_, context_)) {
            ok = false;
        }
        if (ok) {
            changed_ = false;
        }
        return ok;
    }

private:
    EGLDisplay target_;
    EGLDisplay display_;
    EGLContext context_;
    EGLSurface draw_;
    EGLSurface read_;
    EGLenum api_;
    bool changed_ = false;
};

struct Preset {
    libra_shader_preset_t handle = nullptr;
    ~Preset()
    {
        if (handle)
            discardLibra(libra_preset_free(&handle));
    }
};

struct ParameterList {
    libra_preset_param_list_t list { };
    bool owned = false;
    ~ParameterList()
    {
        if (owned)
            discardLibra(libra_preset_free_runtime_params(list));
    }
};

struct Parameter {
    std::string name;
    std::string description;
    float minimum;
    float maximum;
    float step;
    float initial;
    float value;
};

void validateValue(const Parameter& parameter, float value)
{
    if (!std::isfinite(value) || value < parameter.minimum || value > parameter.maximum) {
        throw std::runtime_error("Parameter '" + parameter.name + "' must be finite and within its declared range");
    }
}

void appendJsonString(std::string& out, const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

void appendNumber(std::string& out, float number)
{
    char text[64];
    const auto result = std::to_chars(text, text + sizeof(text), number);
    if (result.ec != std::errc()) {
        fail("Could not serialize shader parameter");
    }
    out.append(text, result.ptr);
}

std::string parameterJson(const std::vector<Parameter>& parameters,
    const Parameter* changed = nullptr, float value = 0)
{
    std::string json = "[";
    for (const auto& p : parameters) {
        if (json.size() != 1)
            json += ',';
        json += "{\"name\":";
        appendJsonString(json, p.name);
        json += ",\"description\":";
        appendJsonString(json, p.description);
        json += ",\"minimum\":";
        appendNumber(json, p.minimum);
        json += ",\"maximum\":";
        appendNumber(json, p.maximum);
        json += ",\"step\":";
        appendNumber(json, p.step);
        json += ",\"default\":";
        appendNumber(json, p.initial);
        json += ",\"value\":";
        appendNumber(json, &p == changed ? value : p.value);
        json += '}';
    }
    json += ']';
    return json;
}

const void* loadGl(const char* name)
{
    return reinterpret_cast<const void*>(eglGetProcAddress(name));
}

GLuint compileShader(GLenum kind, const char* source)
{
    const GLuint shader = glCreateShader(kind);
    if (!shader)
        fail("Could not allocate the SDR color shader");
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] { };
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("SDR color shader compilation failed: ") + log);
    }
    return shader;
}

struct Images {
    GLuint input = 0;
    GLuint intermediate = 0;
    GLuint output = 0;
    GLuint framebuffer = 0;
    EGLImage inputImage = EGL_NO_IMAGE;
    EGLImage outputImage = EGL_NO_IMAGE;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace

struct WsRuntime {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext hostContext = EGL_NO_CONTEXT;
    bool coreEgl = false;
    bool poisoned = false;
    bool resetHistory = true;
    GLint maximumTextureSize = 0;
    libra_gl_filter_chain_t chain = nullptr;
    GLuint colorProgram = 0;
    GLuint colorVao = 0;
    GLuint colorSampler = 0;
    GLint gammaUniform = -1;
    GLint saturationUniform = -1;
    Images images;
    GLuint chainDestination = 0;
    GLuint retiredIntermediate = 0;
    std::vector<Parameter> parameters;
    std::string json = "[]";

    ~WsRuntime()
    {
        // A GL name is meaningful only in this exact, unshared context. On
        // display/context loss, do not run librashader destructors in GLES.
        if (context != EGL_NO_CONTEXT) {
            CurrentContext current(display);
            try {
                current.enter(context, surface);
                if (chain)
                    discardLibra(libra_gl_filter_chain_free(&chain));
                destroyImages(images, true);
                if (retiredIntermediate)
                    glDeleteTextures(1, &retiredIntermediate);
                if (colorProgram)
                    glDeleteProgram(colorProgram);
                if (colorVao)
                    glDeleteVertexArrays(1, &colorVao);
                if (colorSampler)
                    glDeleteSamplers(1, &colorSampler);
                glFlush();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "WaylandShader GPU cleanup: %s; abandoning context-owned handles\n", error.what());
                destroyImages(images, false);
            } catch (...) {
                std::fprintf(stderr, "WaylandShader GPU cleanup: unexpected failure\n");
                destroyImages(images, false);
            }
            current.restore();
            // Destroying the owner context releases its remaining GL objects;
            // imported GLES siblings retain their image storage independently.
            eglDestroyContext(display, context);
        } else {
            destroyImages(images, false);
        }
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
    }

    EGLSync fence()
    {
        EGLSync sync = coreEgl ? eglCreateSync(display, EGL_SYNC_FENCE, nullptr)
                               : reinterpret_cast<EGLSync>(eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, nullptr));
        if (sync == EGL_NO_SYNC)
            eglFailure("eglCreateSync(GPU fence)");
        glFlush();
        return sync;
    }

    void destroyFence(EGLSync sync) noexcept
    {
        if (sync == EGL_NO_SYNC)
            return;
        if (coreEgl)
            eglDestroySync(display, sync);
        else
            eglDestroySyncKHR(display, reinterpret_cast<EGLSyncKHR>(sync));
    }

    void waitFence(EGLSync sync)
    {
        const EGLBoolean ok = coreEgl ? eglWaitSync(display, sync, 0)
                                      : eglWaitSyncKHR(display, reinterpret_cast<EGLSyncKHR>(sync), 0);
        if (!ok)
            eglFailure("eglWaitSync(GPU server wait)");
    }

    EGLImage exportTexture(GLuint texture)
    {
        const EGLClientBuffer buffer = reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture));
        EGLImage image;
        if (coreEgl) {
            const EGLAttrib attributes[] = { EGL_GL_TEXTURE_LEVEL, 0, EGL_IMAGE_PRESERVED, EGL_TRUE, EGL_NONE };
            image = eglCreateImage(display, context, EGL_GL_TEXTURE_2D, buffer, attributes);
        } else {
            const EGLint attributes[] = { EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
            image = reinterpret_cast<EGLImage>(eglCreateImageKHR(display, context, EGL_GL_TEXTURE_2D_KHR, buffer, attributes));
        }
        if (image == EGL_NO_IMAGE)
            eglFailure("eglCreateImage(RGBA8 desktop texture)");
        return image;
    }

    void destroyImage(EGLImage image) noexcept
    {
        if (image == EGL_NO_IMAGE)
            return;
        if (coreEgl)
            eglDestroyImage(display, image);
        else
            eglDestroyImageKHR(display, reinterpret_cast<EGLImageKHR>(image));
    }

    void destroyImages(Images& old, bool haveContext) noexcept
    {
        destroyImage(old.inputImage);
        destroyImage(old.outputImage);
        if (haveContext) {
            if (old.framebuffer)
                glDeleteFramebuffers(1, &old.framebuffer);
            const GLuint textures[] = { old.input, old.intermediate, old.output };
            glDeleteTextures(3, textures);
        }
        old = { };
    }

    void requireHost()
    {
        if (poisoned)
            fail("Shader runtime was retired after a GPU/context failure; recreate it");
        const EGLContext current = eglGetCurrentContext();
        if (current == EGL_NO_CONTEXT || eglGetCurrentDisplay() != display || eglQueryAPI() != EGL_OPENGL_ES_API) {
            fail("Shader image exchange requires the compositor GLES context current on the original EGLDisplay");
        }
        if (hostContext != EGL_NO_CONTEXT && hostContext != current) {
            fail("Compositor EGL context changed; recreate the output shader runtime and image siblings");
        }
        if (hostContext == EGL_NO_CONTEXT) {
            EGLint api = 0;
            if (!eglQueryContext(display, current, EGL_CONTEXT_CLIENT_TYPE, &api))
                eglFailure("eglQueryContext(compositor API)");
            if (api != EGL_OPENGL_ES_API)
                fail("Shader image exchange requires an OpenGL ES context");
            if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
                fail("Compositor GLES context lacks GL_OES_EGL_image for native texture siblings");
            }
        }
    }

    void createContext(CurrentContext& current)
    {
        coreEgl = epoxy_egl_version(display) >= 15;
        if (!coreEgl && (!epoxy_has_egl_extension(display, "EGL_KHR_create_context") || !epoxy_has_egl_extension(display, "EGL_KHR_image_base") || !epoxy_has_egl_extension(display, "EGL_KHR_gl_texture_2D_image") || !epoxy_has_egl_extension(display, "EGL_KHR_fence_sync") || !epoxy_has_egl_extension(display, "EGL_KHR_wait_sync"))) {
            fail("Native shader rendering requires EGL 1.5 or KHR create_context, image_base, gl_texture_2D_image, fence_sync and wait_sync");
        }
        current.selectDesktop();
        const bool surfaceless = coreEgl || epoxy_has_egl_extension(display, "EGL_KHR_surfaceless_context");
        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, surfaceless ? 0 : EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE
        };
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (!eglChooseConfig(display, configAttributes, &config, 1, &count))
            eglFailure("eglChooseConfig(desktop OpenGL)");
        if (!count)
            fail("The compositor EGLDisplay has no desktop-OpenGL config; software fallback is not supported");
        for (const auto version : { std::pair { 4, 6 }, std::pair { 4, 5 }, std::pair { 3, 3 } }) {
            const EGLint attributes[] = {
                EGL_CONTEXT_MAJOR_VERSION_KHR, version.first,
                EGL_CONTEXT_MINOR_VERSION_KHR, version.second,
                EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                EGL_NONE
            };
            context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
            if (context != EGL_NO_CONTEXT)
                break;
        }
        if (context == EGL_NO_CONTEXT)
            eglFailure("eglCreateContext(private desktop OpenGL 3.3+)");
        if (!surfaceless) {
            const EGLint attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            surface = eglCreatePbufferSurface(display, config, attributes);
            if (surface == EGL_NO_SURFACE)
                eglFailure("eglCreatePbufferSurface(private desktop OpenGL)");
        }
        current.enter(context, surface);
        const int version = epoxy_gl_version();
        if (!epoxy_is_desktop_gl() || version < 33
            || (version < 42 && !epoxy_has_gl_extension("GL_ARB_texture_storage"))) {
            fail("Native shader rendering requires desktop OpenGL 3.3 and immutable texture storage (OpenGL 4.2 or GL_ARB_texture_storage)");
        }
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (!renderer || std::strstr(renderer, "llvmpipe") || std::strstr(renderer, "softpipe")
            || std::strstr(renderer, "Software") || std::strstr(renderer, "SWR") || std::strstr(renderer, "swrast")) {
            fail("Native shader rendering requires a hardware GPU; software OpenGL renderers are not supported");
        }
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
        checkGl("Querying desktop-GL capabilities");
        if (maximumTextureSize <= 0)
            fail("Desktop OpenGL reported no usable texture dimensions");
    }

    void createColorProgram()
    {
        static constexpr char vertex[] = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 position = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)";
        static constexpr char fragment[] = R"(#version 330 core
uniform sampler2D sourceTexture;
uniform float inverseGamma;
uniform float outputSaturation;
in vec2 uv;
out vec4 fragmentColor;
vec3 decodeSrgb(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec3 encodeSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}
void main() {
    vec3 color = max(texture(sourceTexture, uv).rgb, vec3(0.0));
    if (inverseGamma != 1.0) color = pow(color, vec3(inverseGamma));
    if (outputSaturation != 1.0) {
        color = decodeSrgb(color);
        float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = encodeSrgb(max(mix(vec3(luminance), color, outputSaturation), vec3(0.0)));
    }
    fragmentColor = vec4(color, 1.0);
}
)";
        const GLuint vs = compileShader(GL_VERTEX_SHADER, vertex);
        GLuint fs = 0;
        try {
            fs = compileShader(GL_FRAGMENT_SHADER, fragment);
            colorProgram = glCreateProgram();
            if (!colorProgram)
                fail("Could not allocate the SDR color program");
            glAttachShader(colorProgram, vs);
            glAttachShader(colorProgram, fs);
            glLinkProgram(colorProgram);
            glDetachShader(colorProgram, vs);
            glDetachShader(colorProgram, fs);
            GLint ok = GL_FALSE;
            glGetProgramiv(colorProgram, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[2048] { };
                glGetProgramInfoLog(colorProgram, sizeof(log), nullptr, log);
                throw std::runtime_error(std::string("SDR color program link failed: ") + log);
            }
        } catch (...) {
            glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
            throw;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
        glGenVertexArrays(1, &colorVao);
        glGenSamplers(1, &colorSampler);
        glSamplerParameteri(colorSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(colorSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(colorSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(colorSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUseProgram(colorProgram);
        glUniform1i(glGetUniformLocation(colorProgram, "sourceTexture"), 0);
        gammaUniform = glGetUniformLocation(colorProgram, "inverseGamma");
        saturationUniform = glGetUniformLocation(colorProgram, "outputSaturation");
        checkGl("Creating SDR color pipeline");
        if (!colorVao || !colorSampler || gammaUniform < 0 || saturationUniform < 0)
            fail("Incomplete SDR color pipeline");
    }

    void loadPreset(const char* path, const WsParameter* overrides, size_t count)
    {
        if (!path || !*path) {
            if (count)
                fail("Color-only runtime has no shader parameters");
            return;
        }
        if (*path != '/')
            fail("Shader preset must be an absolute path");
        Preset preset;
        libra_preset_opt_t presetOptions { };
        presetOptions.version = LIBRASHADER_CURRENT_VERSION;
        presetOptions.original_aspect_uniforms = true;
        presetOptions.frametime_uniforms = true;
        checkLibra(libra_preset_create_with_options(path, nullptr, &presetOptions, &preset.handle));
        ParameterList list;
        checkLibra(libra_preset_get_runtime_params(&preset.handle, &list.list));
        list.owned = true;
        for (uint64_t i = 0; i < list.list.length; ++i) {
            const auto& meta = list.list.parameters[i];
            if (!meta.name || !*meta.name || !std::isfinite(meta.minimum) || !std::isfinite(meta.maximum)
                || meta.minimum > meta.maximum || !std::isfinite(meta.step) || meta.step < 0) {
                fail("Shader preset contains invalid parameter metadata");
            }
            Parameter p { meta.name, meta.description ? meta.description : "", meta.minimum, meta.maximum,
                meta.step, meta.initial, meta.initial };
            validateValue(p, p.initial);
            const auto existing = std::find_if(parameters.begin(), parameters.end(), [&](const auto& other) { return other.name == p.name; });
            // Runtime parameter maps use the last pass's declaration.
            if (existing == parameters.end())
                parameters.push_back(std::move(p));
            else
                *existing = std::move(p);
        }
        std::sort(parameters.begin(), parameters.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (size_t i = 0; i < count; ++i) {
            if (!overrides[i].name)
                fail("Shader parameter name must not be null");
            const auto p = std::find_if(parameters.begin(), parameters.end(), [&](const auto& entry) { return entry.name == overrides[i].name; });
            if (p == parameters.end())
                throw std::runtime_error(std::string("Unknown shader parameter '") + overrides[i].name + "'");
            validateValue(*p, overrides[i].value);
        }
        const int version = epoxy_gl_version();
        filter_chain_gl_opt_t options { };
        options.version = LIBRASHADER_CURRENT_VERSION;
        options.glsl_version = static_cast<uint16_t>(version * 10);
        options.use_dsa = version >= 45;
        options.force_no_mipmaps = false;
        options.disable_cache = false;
        checkLibra(libra_gl_filter_chain_create(&preset.handle, loadGl, &options, &chain));
        uint32_t passes = 0;
        checkLibra(libra_gl_filter_chain_get_active_pass_count(&chain, &passes));
        if (!passes)
            fail("Shader preset has no active passes");
        for (auto& p : parameters) {
            checkLibra(libra_gl_filter_chain_get_param(&chain, p.name.c_str(), &p.value));
            validateValue(p, p.value);
            for (size_t i = 0; i < count; ++i) {
                if (p.name == overrides[i].name)
                    p.value = overrides[i].value;
            }
            checkLibra(libra_gl_filter_chain_set_param(&chain, p.name.c_str(), p.value));
        }
        json = parameterJson(parameters);
        checkGl("Compiling shader preset");
    }

    void allocateTexture(GLuint& texture, uint32_t width, uint32_t height, GLint levels)
    {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        // Allocated mip levels are not initialized yet. Keep them unsampleable;
        // librashader exposes them only when the preset requests generation.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        checkGl("Allocating immutable RGBA8 shader texture");
        if (!texture)
            fail("Desktop OpenGL returned an invalid texture name");
    }

    void allocateImages(Images& next, uint32_t width, uint32_t height)
    {
        next.width = width;
        next.height = height;
        GLint levels = 1;
        for (uint32_t extent = std::max(width, height); extent > 1; extent >>= 1)
            ++levels;
        allocateTexture(next.input, width, height, levels);
        if (chain)
            allocateTexture(next.intermediate, width, height, 1);
        allocateTexture(next.output, width, height, 1);
        glGenFramebuffers(1, &next.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, next.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, next.output, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fail("GPU cannot render the required RGBA8 EGLImage output; format downgrade is not supported");
        }
        checkGl("Creating shader output framebuffer");
        next.inputImage = exportTexture(next.input);
        next.outputImage = exportTexture(next.output);
    }

    void draw(uint64_t frame, float fps, uint32_t elapsed, bool clear, bool shader, float gamma, float saturation)
    {
        GLuint source = images.input;
        resetHistory = resetHistory || clear || !shader;
        if (shader && chain) {
            frame_gl_opt_t options { };
            options.version = LIBRASHADER_CURRENT_VERSION;
            options.clear_history = resetHistory;
            options.frame_direction = 1;
            options.total_subframes = 1;
            options.current_subframe = 1;
            options.aspect_ratio = static_cast<float>(images.width) / static_cast<float>(images.height);
            options.frames_per_second = fps;
            options.frametime_delta = elapsed;
            const libra_image_gl_t input { images.input, GL_RGBA8, images.width, images.height };
            const libra_image_gl_t output { images.intermediate, GL_RGBA8, images.width, images.height };
            // librashader's final quad is [0,1]; this keeps bottom-left texels
            // at bottom-left and leaves output transform handling to Smithay.
            static constexpr float mvp[] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1 };
            // The library generates input mips when requested by the preset;
            // the exported desktop source owns every required mip level.
            checkLibra(libra_gl_filter_chain_frame(&chain, static_cast<size_t>(frame), input, output, nullptr, mvp, &options));
            chainDestination = images.intermediate;
            if (retiredIntermediate) {
                glDeleteTextures(1, &retiredIntermediate);
                retiredIntermediate = 0;
            }
            source = images.intermediate;
        }
        // librashader owns the private context state between frames. Reassert
        // the complete color-pass state rather than assuming its final pass.
        glBindFramebuffer(GL_FRAMEBUFFER, images.framebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, static_cast<GLsizei>(images.width), static_cast<GLsizei>(images.height));
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(colorProgram);
        glUniform1f(gammaUniform, 1.0f / gamma);
        glUniform1f(saturationUniform, saturation);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        glBindSampler(0, colorSampler);
        glBindVertexArray(colorVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        checkGl("Rendering native shader and SDR color output");
        if (shader && chain)
            resetHistory = false;
    }
};

namespace {

// Every command touching shared image storage is ordered by GPU server waits,
// never glFinish/eglClientWaitSync. Even an unsuccessful draw must fence its
// queued reads/writes before restoring GLES so the next scene can reuse input.
class Exchange {
public:
    explicit Exchange(WsRuntime& runtime)
        : runtime_(runtime)
        , current_(runtime.display)
    {
        EGLSync incoming = EGL_NO_SYNC;
        try {
            incoming = runtime_.fence();
            current_.enter(runtime_.context, runtime_.surface);
            runtime_.waitFence(incoming);
            runtime_.destroyFence(incoming);
            entered_ = true;
        } catch (...) {
            runtime_.destroyFence(incoming);
            runtime_.poisoned = true;
            throw;
        }
    }

    ~Exchange()
    {
        if (!finished_) {
            try {
                finish();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "WaylandShader GPU exchange cleanup: %s\n", error.what());
            } catch (...) {
                std::fprintf(stderr, "WaylandShader GPU exchange cleanup failed\n");
            }
        }
    }

    void finish()
    {
        if (finished_)
            return;
        finished_ = true;
        EGLSync outgoing = EGL_NO_SYNC;
        try {
            if (entered_)
                outgoing = runtime_.fence();
            if (!current_.restore())
                eglFailure("Restoring compositor EGL context");
            if (outgoing != EGL_NO_SYNC)
                runtime_.waitFence(outgoing);
            runtime_.destroyFence(outgoing);
        } catch (...) {
            runtime_.destroyFence(outgoing);
            runtime_.poisoned = true;
            throw;
        }
    }

private:
    WsRuntime& runtime_;
    CurrentContext current_;
    bool entered_ = false;
    bool finished_ = false;
};

} // namespace

extern "C" WsRuntime* ws_create(void* display, const char* preset,
    const WsParameter* parameters, size_t parameter_count, char* error, size_t error_size)
{
    report(error, error_size, "");
    try {
        if (!display)
            fail("No initialized compositor EGLDisplay was supplied");
        if (parameter_count && !parameters)
            fail("Shader parameter array must not be null");
        auto runtime = std::make_unique<WsRuntime>();
        runtime->display = static_cast<EGLDisplay>(display);
        CurrentContext current(runtime->display);
        runtime->createContext(current);
        runtime->createColorProgram();
        runtime->loadPreset(preset, parameters, parameter_count);
        glFlush();
        if (!current.restore())
            eglFailure("Restoring caller EGL context after shader creation");
        return runtime.release();
    } catch (const std::exception& failure) {
        report(error, error_size, failure.what());
    } catch (...) {
        report(error, error_size, "Unexpected native shader creation failure");
    }
    return nullptr;
}

extern "C" void ws_destroy(WsRuntime* runtime)
{
    delete runtime;
}

extern "C" bool ws_resize(WsRuntime* runtime, uint32_t width, uint32_t height, char* error, size_t error_size)
{
    report(error, error_size, "");
    try {
        if (!runtime)
            fail("No native shader runtime");
        runtime->requireHost();
        if (!width || !height || width > static_cast<uint32_t>(runtime->maximumTextureSize)
            || height > static_cast<uint32_t>(runtime->maximumTextureSize)
            || width > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())
            || height > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())) {
            fail("Shader image dimensions must be nonzero and fit the GPU texture limit");
        }
        if (runtime->images.width == width && runtime->images.height == height)
            return true;
        const EGLContext host = eglGetCurrentContext();
        Exchange exchange(*runtime);
        Images next;
        try {
            runtime->allocateImages(next, width, height);
        } catch (...) {
            runtime->destroyImages(next, true);
            throw;
        }
        // EGLImage siblings retain storage after deleting the desktop names.
        // The incoming fence additionally orders the last GLES read of old
        // output before retirement; allocation failure leaves old images intact.
        std::swap(runtime->images, next);
        // The library caches its external FBO by texture name and dimensions.
        // Keep its last attached name alive until a successful frame replaces it,
        // including across several resizes before the next shader frame.
        if (next.intermediate && next.intermediate == runtime->chainDestination) {
            runtime->retiredIntermediate = next.intermediate;
            next.intermediate = 0;
        }
        runtime->destroyImages(next, true);
        runtime->hostContext = host;
        runtime->resetHistory = true;
        exchange.finish();
        return true;
    } catch (const std::exception& failure) {
        report(error, error_size, failure.what());
    } catch (...) {
        report(error, error_size, "Unexpected native shader resize failure");
    }
    return false;
}

extern "C" void* ws_input_image(const WsRuntime* runtime)
{
    return runtime && !runtime->poisoned ? runtime->images.inputImage : nullptr;
}

extern "C" void* ws_output_image(const WsRuntime* runtime)
{
    return runtime && !runtime->poisoned ? runtime->images.outputImage : nullptr;
}

extern "C" bool ws_render(WsRuntime* runtime, uint64_t frame_number, float frames_per_second,
    uint32_t elapsed_milliseconds, bool clear_history, bool shader_enabled,
    float gamma, float saturation, char* error, size_t error_size)
{
    report(error, error_size, "");
    try {
        if (!runtime)
            fail("No native shader runtime");
        runtime->requireHost();
        if (!runtime->images.input || !runtime->images.output)
            fail("Shader images must be resized and imported before rendering");
        if (!std::isfinite(frames_per_second) || frames_per_second <= 0
            || frame_number > std::numeric_limits<size_t>::max()
            || !std::isfinite(gamma) || gamma < 0.1f || gamma > 5.0f
            || !std::isfinite(saturation) || saturation < 0.0f || saturation > 2.0f) {
            fail("Invalid shader frame timing or SDR color controls (gamma 0.1..5, saturation 0..2)");
        }
        Exchange exchange(*runtime);
        try {
            runtime->draw(frame_number, frames_per_second, elapsed_milliseconds,
                clear_history, shader_enabled, gamma, saturation);
        } catch (...) {
            runtime->poisoned = true;
            runtime->resetHistory = true;
            throw;
        }
        exchange.finish();
        return true;
    } catch (const std::exception& failure) {
        report(error, error_size, failure.what());
    } catch (...) {
        report(error, error_size, "Unexpected native shader render failure");
    }
    return false;
}

extern "C" bool ws_set_parameter(WsRuntime* runtime, const char* name, float value,
    char* error, size_t error_size)
{
    report(error, error_size, "");
    try {
        if (!runtime || runtime->poisoned)
            fail("No usable native shader runtime");
        if (!name)
            fail("Shader parameter name must not be null");
        const auto parameter = std::find_if(runtime->parameters.begin(), runtime->parameters.end(),
            [&](const auto& p) { return p.name == name; });
        if (parameter == runtime->parameters.end())
            throw std::runtime_error(std::string("Unknown shader parameter '") + name + "'");
        validateValue(*parameter, value);
        if (parameter->value == value)
            return true;
        // Allocate metadata before mutating the chain, so allocation failure
        // cannot leave the controller's committed value out of sync.
        std::string json = parameterJson(runtime->parameters, &*parameter, value);
        CurrentContext current(runtime->display);
        current.enter(runtime->context, runtime->surface);
        checkLibra(libra_gl_filter_chain_set_param(&runtime->chain, parameter->name.c_str(), value));
        parameter->value = value;
        runtime->json.swap(json);
        if (!current.restore()) {
            runtime->poisoned = true;
            eglFailure("Restoring caller EGL context after parameter update");
        }
        return true;
    } catch (const std::exception& failure) {
        report(error, error_size, failure.what());
    } catch (...) {
        report(error, error_size, "Unexpected native shader parameter failure");
    }
    return false;
}

extern "C" const char* ws_parameters_json(const WsRuntime* runtime)
{
    return runtime ? runtime->json.c_str() : "[]";
}

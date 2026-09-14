#include "bridge.h"

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

static void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using Runtime = std::unique_ptr<WsRuntime, decltype(&ws_destroy)>;

static Runtime load(EGLDisplay display, const QString& preset)
{
    return std::async(std::launch::async, [display, preset] {
        std::array<char, 2048> error { };
        auto* runtime = ws_create(display, preset.toUtf8().constData(), nullptr, 0, error.data(), error.size());
        require(runtime, error.data());
        require(eglGetCurrentContext() == EGL_NO_CONTEXT, "Worker retained its private EGL context");
        return Runtime(runtime, ws_destroy);
    }).get();
}

struct Image {
    GLuint texture = 0;
    GLuint framebuffer = 0;

    explicit Image(void* image)
    {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "EGLImage sibling is not renderable");
        require(glGetError() == GL_NO_ERROR, "EGLImage import failed");
    }

    ~Image()
    {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
    }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    void fill(float red, float green = 0.5f, float blue = 0.75f)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(red, green, blue, 0.25f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void expect(int x, int y, const std::array<float, 3>& expected) const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        std::array<unsigned char, 4> bytes { };
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
        const std::array<float, 4> pixel { bytes[0] / 255.0f, bytes[1] / 255.0f,
            bytes[2] / 255.0f, bytes[3] / 255.0f };
        require(glGetError() == GL_NO_ERROR, "GLES verification readback failed");
        for (size_t channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(pixel[channel]) || std::abs(pixel[channel] - expected[channel]) > 0.006f) {
                std::cerr << "channel " << channel << ": got " << pixel[channel] << ", expected " << expected[channel] << '\n';
                throw std::runtime_error("Cross-API shader pixels differ");
            }
        }
        require(std::abs(pixel[3] - 1.0f) < 0.001f, "Shader presentation must be opaque");
    }
};

static void resize(WsRuntime* runtime, unsigned width, unsigned height)
{
    std::array<char, 2048> error { };
    require(ws_resize(runtime, width, height, error.data(), error.size()), error.data());
}

static void render(WsRuntime* runtime, unsigned frame, bool reset, bool shader = true,
    float gamma = 1.0f, float saturation = 1.0f)
{
    const EGLContext context = eglGetCurrentContext();
    const EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface read = eglGetCurrentSurface(EGL_READ);
    glViewport(7, 9, 13, 15);
    std::array<char, 2048> error { };
    require(ws_render(runtime, frame, 60.0f, 17, reset, shader, gamma, saturation, error.data(), error.size()), error.data());
    require(eglQueryAPI() == EGL_OPENGL_ES_API && eglGetCurrentContext() == context
            && eglGetCurrentSurface(EGL_DRAW) == draw && eglGetCurrentSurface(EGL_READ) == read,
        "Compositor EGL API, context, or surfaces were not restored");
    GLint viewport[4] { };
    glGetIntegerv(GL_VIEWPORT, viewport);
    require(viewport[0] == 7 && viewport[1] == 9 && viewport[2] == 13 && viewport[3] == 15,
        "Private shader rendering changed the compositor viewport");
}

static void exercise(EGLDisplay display, const QString& preset, const QString& borderPreset,
    const QString& baseLevelPreset)
{
    auto first = load(display, preset);
    auto second = load(display, preset);
    resize(first.get(), 64, 32);
    resize(second.get(), 64, 32);
    Image firstInput(ws_input_image(first.get()));
    Image firstOutput(ws_output_image(first.get()));
    Image secondInput(ws_input_image(second.get()));
    Image secondOutput(ws_output_image(second.get()));

    firstInput.fill(0.25f);
    render(first.get(), 0, true);
    firstOutput.expect(32, 16, { 0.12549f, 0.25f, 0.125f });
    // Reusing the same EGLImage after mip generation must not orphan its base level.
    firstInput.fill(0.75f);
    render(first.get(), 1, false);
    firstOutput.expect(32, 16, { 0.37647f, 0.75f, 0.25f });
    secondInput.fill(0.5f);
    render(second.get(), 0, true);
    secondOutput.expect(32, 16, { 0.25098f, 0.25f, 0.125f });
    render(first.get(), 2, true);
    firstOutput.expect(32, 16, { 0.37647f, 0.25f, 0.125f });
    std::array<char, 2048> error { };
    require(ws_set_parameter(first.get(), "GAIN", 0.5f, error.data(), error.size()), error.data());
    render(first.get(), 3, false);
    firstOutput.expect(32, 16, { 0.188235f, 0.375f, 0.125f });

    // Repeated resize before a frame must not alias librashader's cached FBO
    // when the GL allocator reuses a recently deleted texture name.
    resize(first.get(), 65, 33);
    resize(first.get(), 64, 32);
    Image returnedInput(ws_input_image(first.get()));
    Image returnedOutput(ws_output_image(first.get()));
    returnedInput.fill(0.25f);
    render(first.get(), 4, true);
    returnedOutput.expect(32, 16, { 0.062745f, 0.125f, 0.0625f });

    // Keep old GLES siblings alive across resizing and a new frame: storage ownership
    // must be correct even while the old destination is attached in the filter chain.
    resize(second.get(), 65, 33);
    Image resizedInput(ws_input_image(second.get()));
    Image resizedOutput(ws_output_image(second.get()));
    resizedInput.fill(0.5f);
    render(second.get(), 1, true);
    resizedOutput.expect(32, 16, { 0.25098f, 0.25f, 0.125f });
    resizedInput.fill(0.125f);
    render(second.get(), 2, false);
    resizedOutput.expect(32, 16, { 0.062745f, 0.75f, 0.25f });

    auto color = load(display, { });
    resize(color.get(), 65, 33);
    Image colorInput(ws_input_image(color.get()));
    Image colorOutput(ws_output_image(color.get()));
    colorInput.fill(0.25f, 0.5f, 0.75f);
    // An asymmetric source catches an accidental vertical flip across the APIs.
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 16, 8);
    glClearColor(1, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    render(color.get(), 0, true, false);
    colorOutput.expect(4, 4, { 1, 0, 0 });
    colorOutput.expect(32, 16, { 0.25f, 0.5f, 0.75f });
    render(color.get(), 1, false, false, 2.0f, 0.0f);
    const auto decode = [](float c) { return std::pow((c + 0.055f) / 1.055f, 2.4f); };
    const float luminance = decode(std::sqrt(0.25f)) * 0.2126f
        + decode(std::sqrt(0.5f)) * 0.7152f + decode(std::sqrt(0.75f)) * 0.0722f;
    const float gray = 1.055f * std::pow(luminance, 1.0f / 2.4f) - 0.055f;
    colorOutput.expect(32, 16, { gray, gray, gray });

    auto border = load(display, borderPreset);
    resize(border.get(), 64, 32);
    Image borderInput(ws_input_image(border.get()));
    Image borderOutput(ws_output_image(border.get()));
    borderInput.fill(0.75f);
    render(border.get(), 0, true);
    borderOutput.expect(32, 16, { 0, 0, 0 });

    // Without mipmap_input, even a high requested LOD must sample the live
    // base level, not allocated but uninitialized mipmap storage.
    auto baseLevel = load(display, baseLevelPreset);
    resize(baseLevel.get(), 64, 32);
    Image baseInput(ws_input_image(baseLevel.get()));
    Image baseOutput(ws_output_image(baseLevel.get()));
    baseInput.fill(0.75f);
    render(baseLevel.get(), 0, true);
    baseOutput.expect(32, 16, { 0.75f, 0.5f, 0.75f });
    baseInput.fill(0.125f);
    render(baseLevel.get(), 1, false);
    baseOutput.expect(32, 16, { 0.125f, 0.5f, 0.75f });
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(argc == 2, "Pass the shader fixture directory");
        if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
            return 77;
        QTemporaryDir directory;
        require(directory.isValid(), "Cannot create isolated shader fixtures");
        for (const char* name : { "temporal.slangp", "temporal.slang", "finish.slang" }) {
            require(QFile::copy(QString::fromLocal8Bit(argv[1]) + '/' + QLatin1String(name), directory.filePath(QLatin1String(name))), "Cannot copy fixture");
        }
        QImage lut(2, 2, QImage::Format_RGBA8888);
        lut.fill(QColor(128, 255, 255));
        require(lut.save(directory.filePath("lookup.png")), "Cannot create lookup texture");
        QFile borderPreset(directory.filePath("border.slangp"));
        require(borderPreset.open(QIODevice::WriteOnly), "Cannot create border preset");
        borderPreset.write("shaders = 1\nshader0 = border.slang\nwrap_mode0 = clamp_to_border\n");
        borderPreset.close();
        QFile borderShader(directory.filePath("border.slang"));
        require(borderShader.open(QIODevice::WriteOnly), "Cannot create border shader");
        borderShader.write(R"(#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = global.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() { FragColor = texture(Source, uv + vec2(2.0)); }
)");
        borderShader.close();
        QFile basePreset(directory.filePath("base-level.slangp"));
        require(basePreset.open(QIODevice::WriteOnly), "Cannot create base-level preset");
        basePreset.write("shaders = 1\nshader0 = base-level.slang\nfilter_linear0 = true\nmipmap_input0 = false\n");
        basePreset.close();
        QFile baseShader(directory.filePath("base-level.slang"));
        require(baseShader.open(QIODevice::WriteOnly), "Cannot create base-level shader");
        baseShader.write(R"(#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = global.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() { FragColor = textureLod(Source, uv, 3.0); }
)");
        baseShader.close();
        const EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        require(eglInitialize(display, nullptr, nullptr), "Cannot initialize surfaceless EGL");
        require(eglBindAPI(EGL_OPENGL_ES_API), "GLES unavailable");
        const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
        EGLConfig config = nullptr;
        EGLint count = 0;
        require(eglChooseConfig(display, configAttributes, &config, 1, &count) && count, "No GLES3 EGLConfig");
        const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        const EGLint dimensions[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        const EGLSurface surface = eglCreatePbufferSurface(display, config, dimensions);
        require(context && surface && eglMakeCurrent(display, surface, surface, context), "Cannot make GLES current");
        std::cout << glGetString(GL_RENDERER) << "; " << glGetString(GL_VERSION) << '\n';
        exercise(display, directory.filePath("temporal.slangp"), directory.filePath("border.slangp"),
            directory.filePath("base-level.slangp"));
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        std::cout << "EGLImage live mipmaps, base-level sampling, independent history, resets, resize, parameters, orientation, color, and border clamp passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

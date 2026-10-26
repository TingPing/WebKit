/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GraphicsContextGLVideoFrameCopierGStreamer.h"

#if ENABLE(WEBGL) && ENABLE(VIDEO) && USE(GSTREAMER) && USE(COORDINATED_GRAPHICS)

#include "ANGLEHeaders.h"
#include "GLContext.h"
#include "GStreamerCommon.h"
#include "Logging.h"
#include "VideoFrameGStreamer.h"
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/ASCIILiteral.h>

#if USE(GBM) && USE(GSTREAMER_GL) && GST_CHECK_VERSION(1, 24, 0) && !USE(TEXTURE_MAPPER)
#include "DMABufBuffer.h"
#include <drm_fourcc.h>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GraphicsContextGLVideoFrameCopierGStreamer);

static constexpr auto s_vertexShaderSource {
    "attribute vec2 a_position;"
    "uniform int u_flipY;"
    "varying vec2 v_textureCoordinate;"
    "void main()"
    "{"
    "    gl_Position = vec4(a_position, 0.0, 1.0);"
    "    vec2 textureCoordinate = a_position * 0.5 + 0.5;"
    "    if (u_flipY == 1)"
    "        textureCoordinate.y = 1.0 - textureCoordinate.y;"
    "    v_textureCoordinate = textureCoordinate;"
    "}"_s
};

// Uploaded planes are luminance or luminance alpha textures, imported ones red or red green textures.
static constexpr auto s_fragmentShaderSource {
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_texture0;"
    "uniform sampler2D u_texture1;"
    "uniform sampler2D u_texture2;"
    "uniform sampler2D u_texture3;"
    "uniform int u_planeCount;"
    "uniform int u_swapUV;"
    "uniform int u_chromaInRedGreen;"
    "uniform int u_premultiplyAlpha;"
    "uniform mat4 u_colorMatrix;"
    "varying vec2 v_textureCoordinate;"
    "void main()"
    "{"
    "    vec4 yuv;"
    "    yuv.r = texture2D(u_texture0, v_textureCoordinate).r;"
    "    if (u_planeCount == 2) {"
    "        vec4 chroma = texture2D(u_texture1, v_textureCoordinate);"
    "        yuv.gb = u_chromaInRedGreen == 1 ? chroma.rg : chroma.ra;"
    "    } else {"
    "        yuv.g = texture2D(u_texture1, v_textureCoordinate).r;"
    "        yuv.b = texture2D(u_texture2, v_textureCoordinate).r;"
    "    }"
    "    if (u_swapUV == 1)"
    "        yuv.gb = yuv.bg;"
    "    yuv.a = 1.0;"
    "    vec4 color = yuv * u_colorMatrix;"
    "    color.a = u_planeCount == 4 ? texture2D(u_texture3, v_textureCoordinate).r : 1.0;"
    "    if (u_premultiplyAlpha == 1)"
    "        color.rgb *= color.a;"
    "    gl_FragColor = color;"
    "}"_s
};

static GLuint compileShader(GLenum type, ASCIILiteral source)
{
    auto shader = GL_CreateShader(type);
    const char* sourceString = source.characters();
    GL_ShaderSource(shader, 1, &sourceString, nullptr);
    GL_CompileShader(shader);

    GLint status = 0;
    GL_GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GL_DeleteShader(shader);
        return 0;
    }
    return shader;
}

static void setPlaneTextureParameters()
{
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// Columns of the matrix that converts (Y, U, V, 1), each normalized to [0, 1], to RGBA.
static std::array<float, 16> yuvToRGBMatrix(const GstVideoColorimetry& colorimetry)
{
    gdouble kr, kb;
    if (!gst_video_color_matrix_get_Kr_Kb(colorimetry.matrix, &kr, &kb)) {
        kr = 0.299;
        kb = 0.114;
    }
    double kg = 1 - kr - kb;

    bool isFullRange = colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    double yOffset = isFullRange ? 0 : 16. / 255;
    double yScale = isFullRange ? 1 : 255. / 219;
    double uvOffset = 128. / 255;
    double uvScale = isFullRange ? 1 : 255. / 224;

    double vToR = 2 * (1 - kr) * uvScale;
    double uToG = 2 * kb * (1 - kb) / kg * uvScale;
    double vToG = 2 * kr * (1 - kr) / kg * uvScale;
    double uToB = 2 * (1 - kb) * uvScale;
    double yOffsetScaled = yScale * yOffset;

    return {
        static_cast<float>(yScale), 0, static_cast<float>(vToR), static_cast<float>(-yOffsetScaled - vToR * uvOffset),
        static_cast<float>(yScale), static_cast<float>(-uToG), static_cast<float>(-vToG), static_cast<float>(-yOffsetScaled + (uToG + vToG) * uvOffset),
        static_cast<float>(yScale), static_cast<float>(uToB), 0, static_cast<float>(-yOffsetScaled - uToB * uvOffset),
        0, 0, 0, 1
    };
}

std::unique_ptr<GraphicsContextGLVideoFrameCopierGStreamer> GraphicsContextGLVideoFrameCopierGStreamer::create(GCGLDisplay display, GCGLConfig config, GCGLContext ownerContext, GCEGLSurface ownerSurface, bool isForWebGL2)
{
    std::unique_ptr<GraphicsContextGLVideoFrameCopierGStreamer> copier(new GraphicsContextGLVideoFrameCopierGStreamer(display, ownerContext, ownerSurface));
    if (!copier->initialize(config, isForWebGL2))
        return nullptr;
    return copier;
}

GraphicsContextGLVideoFrameCopierGStreamer::GraphicsContextGLVideoFrameCopierGStreamer(GCGLDisplay display, GCGLContext ownerContext, GCEGLSurface ownerSurface)
    : m_display(display)
    , m_ownerContext(ownerContext)
    , m_ownerSurface(ownerSurface)
{
}

GraphicsContextGLVideoFrameCopierGStreamer::~GraphicsContextGLVideoFrameCopierGStreamer()
{
    if (!m_context)
        return;

    if (makeContextCurrent()) {
        if (m_program)
            GL_DeleteProgram(m_program);
        if (m_vertexBuffer)
            GL_DeleteBuffers(1, &m_vertexBuffer);
        if (m_framebuffer)
            GL_DeleteFramebuffers(1, &m_framebuffer);
        GL_DeleteTextures(m_planeTextures.size(), m_planeTextures.data());
        restoreOwnerContext();
    }
    EGL_DestroyContext(m_display, m_context);
}

bool GraphicsContextGLVideoFrameCopierGStreamer::makeContextCurrent()
{
    return !!EGL_MakeCurrent(m_display, m_ownerSurface, m_ownerSurface, m_context);
}

void GraphicsContextGLVideoFrameCopierGStreamer::restoreOwnerContext()
{
    EGL_MakeCurrent(m_display, m_ownerSurface, m_ownerSurface, m_ownerContext);
}

bool GraphicsContextGLVideoFrameCopierGStreamer::initialize(GCGLConfig config, bool isForWebGL2)
{
    // A context sharing the textures of the owner, without its robustness or WebGL compatibility.
    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, isForWebGL2 ? 3 : 2,
        EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE, EGL_FALSE,
        EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE, EGL_FALSE,
        EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM, EGL_FALSE,
        EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE, 0,
        EGL_NONE
    };
    m_context = EGL_CreateContext(m_display, config, m_ownerContext, contextAttributes);
    if (m_context == EGL_NO_CONTEXT) {
        LOG(WebGL, "GraphicsContextGLVideoFrameCopierGStreamer: EGLContext creation failed: %d", EGL_GetError());
        return false;
    }

    if (!makeContextCurrent())
        return false;
    auto restoreOwner = makeScopeExit([this] {
        restoreOwnerContext();
    });

    const char* glExtensions = byteCast<char>(GL_GetString(GL_EXTENSIONS));
    m_canUnpackRowLength = isForWebGL2 || GLContext::isExtensionSupported(glExtensions, "GL_EXT_unpack_subimage");
    const char* displayExtensions = EGL_QueryString(m_display, EGL_EXTENSIONS);
    m_canImportDMABuf = GLContext::isExtensionSupported(displayExtensions, "EGL_EXT_image_dma_buf_import") && GLContext::isExtensionSupported(glExtensions, "GL_OES_EGL_image");
    m_canImportDMABufModifiers = GLContext::isExtensionSupported(displayExtensions, "EGL_EXT_image_dma_buf_import_modifiers");

    auto vertexShader = compileShader(GL_VERTEX_SHADER, s_vertexShaderSource);
    auto fragmentShader = compileShader(GL_FRAGMENT_SHADER, s_fragmentShaderSource);
    auto deleteShaders = makeScopeExit([&] {
        if (vertexShader)
            GL_DeleteShader(vertexShader);
        if (fragmentShader)
            GL_DeleteShader(fragmentShader);
    });
    if (!vertexShader || !fragmentShader)
        return false;

    m_program = GL_CreateProgram();
    GL_AttachShader(m_program, vertexShader);
    GL_AttachShader(m_program, fragmentShader);
    GL_LinkProgram(m_program);
    GLint status = 0;
    GL_GetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (!status) {
        LOG(WebGL, "GraphicsContextGLVideoFrameCopierGStreamer: program link failed");
        return false;
    }

    auto positionLocation = GL_GetAttribLocation(m_program, "a_position");
    auto texture0Location = GL_GetUniformLocation(m_program, "u_texture0");
    auto texture1Location = GL_GetUniformLocation(m_program, "u_texture1");
    auto texture2Location = GL_GetUniformLocation(m_program, "u_texture2");
    auto texture3Location = GL_GetUniformLocation(m_program, "u_texture3");
    m_planeCountLocation = GL_GetUniformLocation(m_program, "u_planeCount");
    m_swapUVLocation = GL_GetUniformLocation(m_program, "u_swapUV");
    m_chromaInRedGreenLocation = GL_GetUniformLocation(m_program, "u_chromaInRedGreen");
    m_flipYLocation = GL_GetUniformLocation(m_program, "u_flipY");
    m_premultiplyAlphaLocation = GL_GetUniformLocation(m_program, "u_premultiplyAlpha");
    m_colorMatrixLocation = GL_GetUniformLocation(m_program, "u_colorMatrix");
    if (positionLocation < 0 || texture0Location < 0 || texture1Location < 0 || texture2Location < 0 || texture3Location < 0 || m_planeCountLocation < 0
        || m_swapUVLocation < 0 || m_chromaInRedGreenLocation < 0 || m_flipYLocation < 0 || m_premultiplyAlphaLocation < 0 || m_colorMatrixLocation < 0)
        return false;

    GL_UseProgram(m_program);
    GL_Uniform1i(texture0Location, 0);
    GL_Uniform1i(texture1Location, 1);
    GL_Uniform1i(texture2Location, 2);
    GL_Uniform1i(texture3Location, 3);

    static constexpr std::array<GLfloat, 12> vertices { -1, -1, 1, -1, 1, 1, 1, 1, -1, 1, -1, -1 };
    GL_GenBuffers(1, &m_vertexBuffer);
    GL_BindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
    GL_EnableVertexAttribArray(positionLocation);
    GL_VertexAttribPointer(positionLocation, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    GL_GenTextures(m_planeTextures.size(), m_planeTextures.data());
    for (unsigned i = 0; i < m_planeTextures.size(); ++i) {
        GL_ActiveTexture(GL_TEXTURE0 + i);
        GL_BindTexture(GL_TEXTURE_2D, m_planeTextures[i]);
        setPlaneTextureParameters();
    }
    GL_PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GL_GenFramebuffers(1, &m_framebuffer);
    GL_BindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    GL_Disable(GL_DITHER);
    return true;
}

auto GraphicsContextGLVideoFrameCopierGStreamer::uploadPlanes(const VideoFrameGStreamer& frame) -> std::optional<Planes>
{
    GstMappedFrame mappedFrame(frame.sample(), GST_MAP_READ);
    if (!mappedFrame)
        return std::nullopt;

    auto* info = mappedFrame.info();
    Planes planes;
    switch (GST_VIDEO_INFO_FORMAT(info)) {
    case GST_VIDEO_FORMAT_YV12:
        planes.swapUV = true;
        [[fallthrough]];
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_Y41B:
        planes.count = 3;
        break;
    case GST_VIDEO_FORMAT_NV21:
        planes.swapUV = true;
        [[fallthrough]];
    case GST_VIDEO_FORMAT_NV12:
        planes.count = 2;
        break;
    case GST_VIDEO_FORMAT_A420:
        planes.count = 4;
        break;
    default:
        return std::nullopt;
    }

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN; // GLib port
    for (unsigned i = 0; i < planes.count; ++i) {
        std::array<gint, GST_VIDEO_MAX_COMPONENTS> components;
        gst_video_format_info_component(info->finfo, i, components.data());
        int width = GST_VIDEO_INFO_COMP_WIDTH(info, components[0]);
        int height = GST_VIDEO_INFO_COMP_HEIGHT(info, components[0]);
        int pixelStride = GST_VIDEO_INFO_COMP_PSTRIDE(info, components[0]);
        int stride = mappedFrame.planeStride(i);
        if (pixelStride <= 0 || stride % pixelStride)
            return std::nullopt;
        int rowLength = stride / pixelStride;
        if (rowLength != width && !m_canUnpackRowLength)
            return std::nullopt;
        if (m_canUnpackRowLength)
            GL_PixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);

        auto planeFormat = pixelStride == 2 ? GL_LUMINANCE_ALPHA : GL_LUMINANCE;
        GL_ActiveTexture(GL_TEXTURE0 + i);
        GL_BindTexture(GL_TEXTURE_2D, m_planeTextures[i]);
        GL_TexImage2D(GL_TEXTURE_2D, 0, planeFormat, width, height, 0, planeFormat, GL_UNSIGNED_BYTE, mappedFrame.planeData(i).data());
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    return planes;
}

#if USE(GBM) && USE(GSTREAMER_GL) && GST_CHECK_VERSION(1, 24, 0) && !USE(TEXTURE_MAPPER)
auto GraphicsContextGLVideoFrameCopierGStreamer::importPlanes(VideoFrameGStreamer& frame, std::array<GCGLuint, 3>& textures) -> std::optional<Planes>
{
    if (!m_canImportDMABuf)
        return std::nullopt;

    RefPtr dmabuf = frame.getDMABuf();
    if (!dmabuf)
        return std::nullopt;

    Planes planes;
    switch (dmabuf->attributes().fourcc.value) {
    case DRM_FORMAT_NV21:
        planes.swapUV = true;
        [[fallthrough]];
    case DRM_FORMAT_NV12:
        planes.count = 2;
        planes.chromaInRedGreen = true;
        break;
    case DRM_FORMAT_YVU420:
        planes.swapUV = true;
        [[fallthrough]];
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YUV444:
    case DRM_FORMAT_YUV411:
        planes.count = 3;
        break;
    default:
        return std::nullopt;
    }

    auto yuvPlanes = DMABufBuffer::yuvPlanes(dmabuf->attributes());
    if (yuvPlanes.size() != planes.count)
        return std::nullopt;

    auto enableModifiers = m_canImportDMABufModifiers ? DMABufBuffer::Attributes::EnableModifiers::Yes : DMABufBuffer::Attributes::EnableModifiers::No;
    GL_GenTextures(planes.count, textures.data());
    for (unsigned i = 0; i < planes.count; ++i) {
        auto attributes = DMABufBuffer::buildEGLImageAttributes(yuvPlanes[i].attributes, enableModifiers);
        if (!attributes)
            return std::nullopt;

        auto image = EGL_CreateImageKHR(m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes->span().data());
        if (!image) {
            LOG(WebGL, "GraphicsContextGLVideoFrameCopierGStreamer: EGLImage import of plane %u failed: %d", i, EGL_GetError());
            return std::nullopt;
        }

        GL_ActiveTexture(GL_TEXTURE0 + i);
        GL_BindTexture(GL_TEXTURE_2D, textures[i]);
        setPlaneTextureParameters();
        GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        EGL_DestroyImageKHR(m_display, image);
    }
    return planes;
}
#endif

bool GraphicsContextGLVideoFrameCopierGStreamer::copy(VideoFrameGStreamer& frame, GCGLuint outputTexture, GCGLint level, GCGLenum internalFormat, GCGLenum format, GCGLenum type, bool premultiplyAlpha, bool flipY)
{
    if (!frame.hasNoTransformation())
        return false;

    if (!makeContextCurrent())
        return false;
    auto restoreOwner = makeScopeExit([this] {
        restoreOwnerContext();
    });

    // The textures of imported planes keep the DMA-BUF of the frame alive, so they don't outlive the copy.
    std::array<GCGLuint, 3> importedTextures { };
    auto deleteImportedTextures = makeScopeExit([&] {
        GL_DeleteTextures(importedTextures.size(), importedTextures.data());
    });

    std::optional<Planes> planes;
    switch (frame.memoryType()) {
    case VideoFrameGStreamer::MemoryType::System:
        planes = uploadPlanes(frame);
        break;
#if USE(GBM) && USE(GSTREAMER_GL) && GST_CHECK_VERSION(1, 24, 0) && !USE(TEXTURE_MAPPER)
    case VideoFrameGStreamer::MemoryType::DMABuf:
        planes = importPlanes(frame, importedTextures);
        break;
#endif
    default:
        break;
    }
    if (!planes)
        return false;

    const auto& info = frame.info();
    auto size = frame.presentationSize();
    if (size.isEmpty())
        size = { GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info) };

    auto clearOutputTextureOnError = makeScopeExit([&] {
        GL_BindTexture(GL_TEXTURE_2D, outputTexture);
        GL_TexImage2D(GL_TEXTURE_2D, level, internalFormat, 0, 0, 0, format, type, nullptr);
    });
    GL_ActiveTexture(GL_TEXTURE0 + m_planeTextures.size());
    GL_BindTexture(GL_TEXTURE_2D, outputTexture);
    GL_TexImage2D(GL_TEXTURE_2D, level, internalFormat, size.width(), size.height(), 0, format, type, nullptr);
    GL_BindTexture(GL_TEXTURE_2D, 0);

    GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, level);
    auto detachOutputTexture = makeScopeExit([] {
        GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    });
    if (GL_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG(WebGL, "GraphicsContextGLVideoFrameCopierGStreamer: incomplete framebuffer for internal format %#x", internalFormat);
        return false;
    }

    GL_Viewport(0, 0, size.width(), size.height());
    GL_Uniform1i(m_planeCountLocation, planes->count);
    GL_Uniform1i(m_swapUVLocation, planes->swapUV);
    GL_Uniform1i(m_chromaInRedGreenLocation, planes->chromaInRedGreen);
    GL_Uniform1i(m_flipYLocation, flipY);
    GL_Uniform1i(m_premultiplyAlphaLocation, premultiplyAlpha);
    auto colorMatrix = yuvToRGBMatrix(GST_VIDEO_INFO_COLORIMETRY(&info));
    GL_UniformMatrix4fv(m_colorMatrixLocation, 1, GL_FALSE, colorMatrix.data());
    GL_DrawArrays(GL_TRIANGLES, 0, 6);
    GL_Flush();

    clearOutputTextureOnError.release();
    return true;
}

} // namespace WebCore

#endif // ENABLE(WEBGL) && ENABLE(VIDEO) && USE(GSTREAMER) && USE(COORDINATED_GRAPHICS)

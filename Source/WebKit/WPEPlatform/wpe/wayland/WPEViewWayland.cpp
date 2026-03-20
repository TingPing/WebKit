/*
 * Copyright (C) 2023, 2025 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WPEViewWayland.h"

#include "WPEBufferFormats.h"
#include "WPEDisplayWaylandPrivate.h"
#include <wpe/WPEPopupMenu.h>
#include "WPEToplevelWaylandPrivate.h"
#include "WPEWaylandSHMPool.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#if USE(SYSPROF_CAPTURE)
#include "presentation-time-client-protocol.h"
#endif
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <cairo/cairo.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <wtf/Deque.h>
#include <wtf/FastMalloc.h>
#include <wtf/MonotonicTime.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/GSpanExtras.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/GWeakPtr.h>
#include <wtf/glib/WTFGType.h>

// These includes need to be in this order because wayland-egl.h defines WL_EGL_PLATFORM
// and egl.h checks that to decide whether it's Wayland platform.
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
#include <epoxy/egl.h>

#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer *(EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)(EGLDisplay dpy, EGLImageKHR image);
#endif

#if USE(SYSPROF_CAPTURE)
static constexpr unsigned kFrameHistorySize = 30;

class PresentationFeedbackStatistics {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PresentationFeedbackStatistics);
    WTF_MAKE_NONCOPYABLE(PresentationFeedbackStatistics);
public:
    explicit PresentationFeedbackStatistics(unsigned historySize);
    ~PresentationFeedbackStatistics();

    void beginFrame(struct wp_presentation*, WPEView*);
    void presentedFrame(struct wp_presentation_feedback*, Seconds presentationTime, Seconds refreshInterval, uint64_t sequence, uint32_t flags);
    void discardedFrame(struct wp_presentation_feedback*);

    Seconds calculateAverageLatency() const;

private:
    unsigned m_maxHistorySize { 0 };
    Vector<std::tuple<struct wp_presentation_feedback*, MonotonicTime>, 3> m_pendingFeedbacks;
    Deque<Seconds> m_latencyHistory;
};
#endif


class WPEWaylandPopupMenu;
static void destroyPopupMenu(WPEWaylandPopupMenu*);

/**
 * WPEViewWayland:
 *
 */
struct _WPEViewWaylandPrivate {
    GRefPtr<WPEBuffer> buffer;
    struct wl_callback* frameCallback;

#if USE(SYSPROF_CAPTURE)
    std::unique_ptr<PresentationFeedbackStatistics> presentationFeedbackStatistics;
#endif

    Vector<WPERectangle, 1> opaqueRegion;
    unsigned long resizedID;

    struct zwp_relative_pointer_v1* relativePointer;
    struct zwp_locked_pointer_v1* lockedPointer;
    uint32_t savedPointerModifiers { 0 };
    std::pair<double, double> savedPointerCoords { 0, 0 };

    WPEWaylandPopupMenu* popupMenu { nullptr };
};
WEBKIT_DEFINE_FINAL_TYPE(WPEViewWayland, wpe_view_wayland, WPE_TYPE_VIEW, WPEView)

static void wpeViewWaylandConstructed(GObject* object)
{
    G_OBJECT_CLASS(wpe_view_wayland_parent_class)->constructed(object);

    auto* view = WPE_VIEW(object);
    auto* priv = WPE_VIEW_WAYLAND(view)->priv;
    // The web view default background color is opaque white, so set the whole view region as opaque initially.
    priv->opaqueRegion.append({ 0, 0, wpe_view_get_width(view), wpe_view_get_height(view) });

    priv->resizedID = g_signal_connect(view, "resized", G_CALLBACK(+[](WPEView* view, gpointer) {
        auto* priv = WPE_VIEW_WAYLAND(view)->priv;
        priv->opaqueRegion.clear();
        priv->opaqueRegion.append({ 0, 0, wpe_view_get_width(view), wpe_view_get_height(view) });
        if (auto* toplevel = wpe_view_get_toplevel(view))
            wpeToplevelWaylandSetOpaqueRectangles(WPE_TOPLEVEL_WAYLAND(toplevel), priv->opaqueRegion.mutableSpan().data(), priv->opaqueRegion.size());
    }), nullptr);

    g_signal_connect(view, "notify::toplevel", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer) {
        auto* toplevel = wpe_view_get_toplevel(view);
        if (!toplevel) {
            wpe_view_unmap(view);
            return;
        }

        int width;
        int height;
        wpe_toplevel_get_size(toplevel, &width, &height);
        if (width && height)
            wpe_view_resized(view, width, height);

        auto* priv = WPE_VIEW_WAYLAND(view)->priv;
        wpeToplevelWaylandSetOpaqueRectangles(WPE_TOPLEVEL_WAYLAND(toplevel), !priv->opaqueRegion.isEmpty() ? priv->opaqueRegion.mutableSpan().data() : nullptr, priv->opaqueRegion.size());

        wpe_view_map(view);
    }), nullptr);

    g_signal_connect(view, "notify::screen", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer) {
        if (wpe_view_get_screen(view))
            wpe_view_map(view);
        else
            wpe_view_unmap(view);
    }), nullptr);

    g_signal_connect(view, "notify::visible", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer) {
        auto* toplevel = wpe_view_get_toplevel(view);
        if (!toplevel)
            return;

        wpeToplevelWaylandViewVisibilityChanged(WPE_TOPLEVEL_WAYLAND(toplevel), view);
    }), nullptr);
}

static void wpeViewWaylandDispose(GObject* object)
{
    auto* priv = WPE_VIEW_WAYLAND(object)->priv;
    g_clear_pointer(&priv->popupMenu, destroyPopupMenu);
    g_clear_pointer(&priv->frameCallback, wl_callback_destroy);
    g_clear_pointer(&priv->lockedPointer, zwp_locked_pointer_v1_destroy);
    g_clear_pointer(&priv->relativePointer, zwp_relative_pointer_v1_destroy);

#if USE(SYSPROF_CAPTURE)
    priv->presentationFeedbackStatistics = nullptr;
#endif

    G_OBJECT_CLASS(wpe_view_wayland_parent_class)->dispose(object);
}

class WaylandBuffer {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WaylandBuffer);
public:
    virtual ~WaylandBuffer()
    {
        if (m_wlBuffer)
            wl_buffer_destroy(m_wlBuffer);
    }

    WPEView* view() const  { return m_view.get(); }
    struct wl_buffer* wlBuffer() const { return m_wlBuffer; }

protected:
    WaylandBuffer(WPEView* view, wl_buffer* buffer)
        : m_view(view)
        , m_wlBuffer(buffer)
    {
    }

    GWeakPtr<WPEView> m_view;
    struct wl_buffer* m_wlBuffer { nullptr };
};

static void waylandBufferDestroy(WaylandBuffer* buffer)
{
    delete buffer;
}

class DMABufBuffer final : public WaylandBuffer {
public:
    DMABufBuffer(WPEView* view, wl_buffer* buffer)
        : WaylandBuffer(view, buffer)
    {
    }

    virtual ~DMABufBuffer()
    {
        if (m_release)
            zwp_linux_buffer_release_v1_destroy(m_release);
    }

    void setRelease(struct zwp_linux_buffer_release_v1* release)
    {
        released();
        m_release = release;
    }

    void released()
    {
        g_clear_pointer(&m_release, zwp_linux_buffer_release_v1_destroy);
    }

private:
    struct zwp_linux_buffer_release_v1* m_release { nullptr };
};

class SharedMemoryBuffer final : public WaylandBuffer {
public:
    SharedMemoryBuffer(WPEView* view, std::unique_ptr<WPE::WaylandSHMPool>&& pool, uint32_t offset, uint32_t width, uint32_t height, uint32_t stride)
        : WaylandBuffer(view, pool->createBuffer(offset, width, height, stride))
        , m_wlPool(WTF::move(pool))
    {
    }

    virtual ~SharedMemoryBuffer() = default;

    WPE::WaylandSHMPool* wlPool() const { return m_wlPool.get(); }

private:
    std::unique_ptr<WPE::WaylandSHMPool> m_wlPool;
};

static const struct wl_buffer_listener bufferListener = {
    // release
    [](void* userData, struct wl_buffer*)
    {
        auto* buffer = WPE_BUFFER(userData);
        auto* waylandBuffer = static_cast<WaylandBuffer*>(wpe_buffer_get_user_data(buffer));
        if (!waylandBuffer)
            return;

        if (auto* view = waylandBuffer->view())
            wpe_view_buffer_released(view, buffer);
    }
};

static DMABufBuffer* createWaylandBufferFromEGLImage(WPEView* view, WPEBuffer* buffer, GError** error)
{
    GUniqueOutPtr<GError> bufferError;
    auto* eglDisplay = wpe_display_get_egl_display(wpe_view_get_display(view), &bufferError.outPtr());
    if (!eglDisplay) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: can't create Wayland buffer because failed to get EGL display: %s", bufferError->message);
        return nullptr;
    }

    auto eglImage = wpe_buffer_import_to_egl_image(buffer, &bufferError.outPtr());
    if (!eglImage) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to import buffer to EGL image: %s", bufferError->message);
        return nullptr;
    }

    static PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL s_eglCreateWaylandBufferFromImageWL;
    if (!s_eglCreateWaylandBufferFromImageWL) {
        if (epoxy_has_egl_extension(eglDisplay, "EGL_WL_create_wayland_buffer_from_image"))
            s_eglCreateWaylandBufferFromImageWL = reinterpret_cast<PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL>(epoxy_eglGetProcAddress("eglCreateWaylandBufferFromImageWL"));
    }
    if (!s_eglCreateWaylandBufferFromImageWL) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: eglCreateWaylandBufferFromImageWL is not available");
        return nullptr;
    }

    if (auto* wlBuffer = s_eglCreateWaylandBufferFromImageWL(eglDisplay, eglImage))
        return new DMABufBuffer(view, wlBuffer);

    g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: eglCreateWaylandBufferFromImageWL failed with error %#04x", eglGetError());
    return nullptr;
}

static struct wl_buffer* createWaylandBufferFromDMABuf(WPEView* view, WPEBuffer* buffer, GError** error)
{
    if (auto* dmaBufBuffer = static_cast<DMABufBuffer*>(wpe_buffer_get_user_data(buffer)))
        return dmaBufBuffer->wlBuffer();

    auto* bufferDMABuf = WPE_BUFFER_DMA_BUF(buffer);
    DMABufBuffer* dmaBufBuffer = nullptr;
    if (auto* dmabuf = wpeDisplayWaylandGetLinuxDMABuf(WPE_DISPLAY_WAYLAND(wpe_view_get_display(view)))) {
        auto modifier = wpe_buffer_dma_buf_get_modifier(bufferDMABuf);
        auto* params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        auto planeCount = wpe_buffer_dma_buf_get_n_planes(bufferDMABuf);
        for (guint32 i = 0; i < planeCount; ++i) {
            zwp_linux_buffer_params_v1_add(params, wpe_buffer_dma_buf_get_fd(bufferDMABuf, i), i, wpe_buffer_dma_buf_get_offset(bufferDMABuf, i),
                wpe_buffer_dma_buf_get_stride(bufferDMABuf, i), modifier >> 32, modifier & 0xffffffff);
        }
        auto* wlBuffer = zwp_linux_buffer_params_v1_create_immed(params, wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer),
            wpe_buffer_dma_buf_get_format(bufferDMABuf), 0);
        zwp_linux_buffer_params_v1_destroy(params);

        if (!wlBuffer) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to create Wayland buffer from DMABuf");
            return nullptr;
        }

        dmaBufBuffer = new DMABufBuffer(view, wlBuffer);
    } else {
        dmaBufBuffer = createWaylandBufferFromEGLImage(view, buffer, error);
        if (!dmaBufBuffer)
            return nullptr;
    }

    auto* toplevel = wpe_view_get_toplevel(WPE_VIEW(view));
    if (!wpeToplevelWaylandGetSurfaceSync(WPE_TOPLEVEL_WAYLAND(toplevel)) || wpe_buffer_get_rendering_fence(buffer) == -1)
        wl_buffer_add_listener(dmaBufBuffer->wlBuffer(), &bufferListener, buffer);

    wpe_buffer_set_user_data(buffer, dmaBufBuffer, reinterpret_cast<GDestroyNotify>(waylandBufferDestroy));
    return dmaBufBuffer->wlBuffer();
}

static SharedMemoryBuffer* sharedMemoryBufferCreate(WPEView* view, GBytes* bytes, int width, int height, unsigned stride)
{
    auto* display = WPE_DISPLAY_WAYLAND(wpe_view_get_display(view));
    auto size = g_bytes_get_size(bytes);
    auto wlPool = WPE::WaylandSHMPool::create(wpe_display_wayland_get_wl_shm(display), size);
    if (!wlPool)
        return nullptr;

    auto offset = wlPool->allocate(size);
    if (offset < 0)
        return nullptr;

    wlPool->write(WTF::span(bytes), offset);
    return new SharedMemoryBuffer(view, WTF::move(wlPool), offset, width, height, stride);
}

static struct wl_buffer* createWaylandBufferSHM(WPEView* view, WPEBuffer* buffer, GError** error)
{
    if (auto* sharedMemoryBuffer = static_cast<SharedMemoryBuffer*>(wpe_buffer_get_user_data(buffer))) {
        GBytes* bytes = wpe_buffer_shm_get_data(WPE_BUFFER_SHM(buffer));
        sharedMemoryBuffer->wlPool()->write(WTF::span(bytes));
        return sharedMemoryBuffer->wlBuffer();
    }

    auto* bufferSHM = WPE_BUFFER_SHM(buffer);
    if (wpe_buffer_shm_get_format(bufferSHM) != WPE_PIXEL_FORMAT_ARGB8888) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: can't create Wayland buffer because format is unsupported");
        return nullptr;
    }

    auto* sharedMemoryBuffer = sharedMemoryBufferCreate(view, wpe_buffer_shm_get_data(bufferSHM),
        wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer), wpe_buffer_shm_get_stride(bufferSHM));
    if (!sharedMemoryBuffer) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: can't create Wayland buffer because failed to create shared memory");
        return nullptr;
    }

    wl_buffer_add_listener(sharedMemoryBuffer->wlBuffer(), &bufferListener, buffer);

    wpe_buffer_set_user_data(buffer, sharedMemoryBuffer, reinterpret_cast<GDestroyNotify>(waylandBufferDestroy));
    return sharedMemoryBuffer->wlBuffer();
}

static struct wl_buffer* createWaylandBuffer(WPEView* view, WPEBuffer* buffer, GError** error)
{
    struct wl_buffer* wlBuffer = nullptr;
    if (WPE_IS_BUFFER_DMA_BUF(buffer))
        wlBuffer = createWaylandBufferFromDMABuf(view, buffer, error);
    else if (WPE_IS_BUFFER_SHM(buffer))
        wlBuffer = createWaylandBufferSHM(view, buffer, error);
    else
        RELEASE_ASSERT_NOT_REACHED();

    return wlBuffer;
}

const struct wl_callback_listener frameListener = {
    // frame
    [](void* data, struct wl_callback* callback, uint32_t)
    {
        auto* view = WPE_VIEW(data);
        auto* priv = WPE_VIEW_WAYLAND(view)->priv;
        RELEASE_ASSERT(!priv->frameCallback || priv->frameCallback == callback);
        g_clear_pointer(&priv->frameCallback, wl_callback_destroy);

        wpe_view_buffer_rendered(view, priv->buffer.get());
        priv->buffer = nullptr;
    }
};

#if USE(SYSPROF_CAPTURE)
static const struct wp_presentation_feedback_listener presentationFeedbackListener = {
    // sync_output
    [](void*, struct wp_presentation_feedback*, struct wl_output*) {
        // Quoting https://wayland.app/protocols/presentation-time#wp_presentation_feedback:event:presented:
        // As presentation can be synchronized to only one output at a time, this event tells which output it was.
        // This event is only sent prior to the presented event.
    },
    // presented
    [](void* data, struct wp_presentation_feedback* feedback, uint32_t tvSecHi, uint32_t tvSecLo, uint32_t tvNsec, uint32_t refreshNsec, uint32_t seqHi, uint32_t seqLo, uint32_t flags) {
        auto* priv = WPE_VIEW_WAYLAND(data)->priv;

        auto combineToUInt64 = [](uint32_t lo, uint32_t hi) -> uint64_t {
            return (static_cast<uint64_t>(hi) << 32) | lo;
        };

        auto parseTimeValue = [&](uint32_t secLo, uint32_t secHi, uint32_t nsec) -> Seconds {
            return Seconds { static_cast<double>(combineToUInt64(secLo, secHi)) } + Seconds::fromNanoseconds(nsec);
        };

        // Quoting https://wayland.app/protocols/presentation-time#wp_presentation_feedback:event:presented:
        // The associated content update was displayed to the user at the indicated time (tvSecHi/Lo, tvNsec).
        // For the interpretation of the timestamp, see presentation.clock_id event.
        auto presentationTime = parseTimeValue(tvSecLo, tvSecHi, tvNsec);
        auto refreshInterval = Seconds::fromNanoseconds(refreshNsec);
        auto sequence = combineToUInt64(seqLo, seqHi);
        priv->presentationFeedbackStatistics->presentedFrame(feedback, presentationTime, refreshInterval, sequence, flags);
    },
    // discarded
    [](void* data, struct wp_presentation_feedback* feedback) {
        auto* priv = WPE_VIEW_WAYLAND(data)->priv;

        // Quoting https://wayland.app/protocols/presentation-time#wp_presentation_feedback:event:discarded:
        // The content update was never displayed to the user.
        priv->presentationFeedbackStatistics->discardedFrame(feedback);
    },
};
// END

PresentationFeedbackStatistics::PresentationFeedbackStatistics(unsigned historySize)
    : m_maxHistorySize(historySize)
{
}

PresentationFeedbackStatistics::~PresentationFeedbackStatistics()
{
    for (auto [presentationFeedback, frameStartTime] : m_pendingFeedbacks)
        wp_presentation_feedback_destroy(presentationFeedback);
}

void PresentationFeedbackStatistics::beginFrame(struct wp_presentation* presentation, WPEView* view)
{
    auto* wlSurface = wpe_view_wayland_get_wl_surface(WPE_VIEW_WAYLAND(view));

    auto* presentationFeedback = wp_presentation_feedback(presentation, wlSurface);
    wp_presentation_feedback_add_listener(presentationFeedback, &presentationFeedbackListener, view);
    m_pendingFeedbacks.append({ presentationFeedback, MonotonicTime::now() });

    WTFEmitSignpost(this, WaylandFrameBegin, "pending feedbacks? %lu", m_pendingFeedbacks.size());
}

void PresentationFeedbackStatistics::presentedFrame(struct wp_presentation_feedback* feedback, Seconds presentationTime, Seconds refreshInterval, uint64_t sequence, uint32_t flags)
{
    Seconds frameStartTime;
    bool found = m_pendingFeedbacks.removeFirstMatching([&](const auto& entry) {
        auto [presentationFeedback, frameStartTimeForFeedback] = entry;
        if (presentationFeedback == feedback) {
            frameStartTime = frameStartTimeForFeedback.secondsSinceEpoch();
            wp_presentation_feedback_destroy(feedback);
            return true;
        }
        return false;
    });

    RELEASE_ASSERT(found);

    auto latency = presentationTime - frameStartTime;
    m_latencyHistory.append(latency);
    if (m_latencyHistory.size() > m_maxHistorySize)
        m_latencyHistory.removeFirst();

    bool hasHardwareClockFlag = flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK;
    bool hasHardwareCompletionFlag = flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;
    bool hasVSyncFlag = flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC;
    bool hasZeroCopyFlag = flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;

    auto averageLatency = calculateAverageLatency();
    WTFSetCounter(FrameLatency, static_cast<int>(std::round(averageLatency.milliseconds())));
    WTFEmitSignpost(this, WaylandFramePresented,
        "sequence=%lu, latency=%.3f ms, refreshInterval=%.3f ms, flags =%s%s%s%s, pending feedbacks? %lu (avg. latency %.3f ms)",
        sequence, latency.milliseconds(), refreshInterval.milliseconds(),
        hasHardwareClockFlag ? " [hw-clock]" : "",
        hasHardwareCompletionFlag ? " [hw-completion]" : "",
        hasVSyncFlag ? " [vsync]" : "",
        hasZeroCopyFlag ? "  [zero-copy]" : "",
        m_pendingFeedbacks.size(),
        averageLatency.milliseconds());
}

void PresentationFeedbackStatistics::discardedFrame(struct wp_presentation_feedback* feedback)
{
    m_pendingFeedbacks.removeFirstMatching([&](const auto& entry) {
        auto [presentationFeedback, frameStartTimeForFeedback] = entry;
        if (presentationFeedback == feedback) {
            wp_presentation_feedback_destroy(feedback);
            return true;
        }
        return false;
    });

    WTFEmitSignpost(this, WaylandFrameDiscarded, "pending feedbacks? %lu", m_pendingFeedbacks.size());
}

Seconds PresentationFeedbackStatistics::calculateAverageLatency() const
{
    if (m_latencyHistory.isEmpty())
        return Seconds { 1.0 / 60.0 };

    auto total = std::accumulate(m_latencyHistory.begin(), m_latencyHistory.end(), Seconds { });
    return total / m_latencyHistory.size();
}
#endif

static void dmaBufBufferReleased(WPEBuffer* buffer)
{
    auto* dmaBufBuffer = static_cast<DMABufBuffer*>(wpe_buffer_get_user_data(buffer));
    if (!dmaBufBuffer)
        return;

    if (auto* view = dmaBufBuffer->view())
        wpe_view_buffer_released(view, buffer);
    dmaBufBuffer->released();
}

const struct zwp_linux_buffer_release_v1_listener bufferReleaseListener = {
    // fenced_release
    [](void* userData, struct zwp_linux_buffer_release_v1*, int32_t fence)
    {
        auto* buffer = WPE_BUFFER(userData);
        wpe_buffer_set_release_fence(buffer, fence);
        dmaBufBufferReleased(buffer);
    },
    // immediate_release
    [](void* userData, struct zwp_linux_buffer_release_v1*)
    {
        dmaBufBufferReleased(WPE_BUFFER(userData));
    }
};

static gboolean wpeViewWaylandRenderBuffer(WPEView* view, WPEBuffer* buffer, const WPERectangle* damageRects, guint nDamageRects, GError** error)
{
    auto* wlBuffer = createWaylandBuffer(view, buffer, error);
    if (!wlBuffer)
        return FALSE;

    auto* toplevel = wpe_view_get_toplevel(view);
    if (wpe_toplevel_get_state(toplevel) & WPE_TOPLEVEL_STATE_MAXIMIZED) {
        // The surface is maximized. The window geometry specified in the configure
        // event must be obeyed by the client, or the xdg_wm_base.invalid_surface_state
        // error is raised.
        auto scale = wpe_view_get_scale(view);
        if (wpe_view_get_width(view) * scale != wpe_buffer_get_width(buffer) || wpe_view_get_height(view) * scale != wpe_buffer_get_height(buffer)) {
            wpe_view_buffer_rendered(view, buffer);
            return TRUE;
        }
    }

    auto* priv = WPE_VIEW_WAYLAND(view)->priv;
    priv->buffer = buffer;

    wpeToplevelWaylandUpdateOpaqueRegion(WPE_TOPLEVEL_WAYLAND(toplevel));

    auto* wlSurface = wpe_view_wayland_get_wl_surface(WPE_VIEW_WAYLAND(view));
    wl_surface_attach(wlSurface, wlBuffer, 0, 0);

    auto renderingFence = UnixFileDescriptor { wpe_buffer_take_rendering_fence(buffer), UnixFileDescriptor::Adopt };
    if (renderingFence) {
        auto* surfaceSync = wpeToplevelWaylandGetSurfaceSync(WPE_TOPLEVEL_WAYLAND(wpe_view_get_toplevel(view)));
        zwp_linux_surface_synchronization_v1_set_acquire_fence(surfaceSync, renderingFence.value());

        auto* release = zwp_linux_surface_synchronization_v1_get_release(surfaceSync);
        zwp_linux_buffer_release_v1_add_listener(release, &bufferReleaseListener, buffer);
        auto* dmaBufBuffer = static_cast<DMABufBuffer*>(wpe_buffer_get_user_data(buffer));
        dmaBufBuffer->setRelease(release);
    }

    auto* display = WPE_DISPLAY_WAYLAND(wpe_view_get_display(view));
    auto* wlCompositor = wpe_display_wayland_get_wl_compositor(display);
    if (nDamageRects && wl_compositor_get_version(wlCompositor) >= 4) [[likely]] {
        ASSERT(damageRects);
        for (unsigned i = 0; i < nDamageRects; ++i)
            wl_surface_damage_buffer(wlSurface, damageRects[i].x, damageRects[i].y, damageRects[i].width, damageRects[i].height);
    } else
        wl_surface_damage(wlSurface, 0, 0, INT32_MAX, INT32_MAX);

    RELEASE_ASSERT(!priv->frameCallback);
    priv->frameCallback = wl_surface_frame(wlSurface);
    wl_callback_add_listener(priv->frameCallback, &frameListener, view);

#if USE(SYSPROF_CAPTURE)
    if (SysprofAnnotator::singletonIfCreated()) {
        if (auto* presentation = wpeDisplayWaylandGetPresentation(display)) {
            if (!priv->presentationFeedbackStatistics)
                priv->presentationFeedbackStatistics = makeUnique<PresentationFeedbackStatistics>(kFrameHistorySize);
            priv->presentationFeedbackStatistics->beginFrame(presentation, view);
        }
    }
#endif

    wl_surface_commit(wlSurface);
    return TRUE;
}

static void wpeViewWaylandSetCursorFromName(WPEView* view, const char* name)
{
    if (auto* cursor = wpeDisplayWaylandGetCursor(WPE_DISPLAY_WAYLAND(wpe_view_get_display(view))))
        cursor->setFromName(name, wpe_view_get_scale(view));
}

static const struct wl_buffer_listener cursorBufferListener = {
    // release
    [](void* userData, struct wl_buffer*)
    {
        waylandBufferDestroy(static_cast<WaylandBuffer*>(userData));
    }
};

static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    // relative_motion
    [](void* data, struct zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t, wl_fixed_t deltaX, wl_fixed_t deltaY) {
        auto* view = reinterpret_cast<WPEView*>(data);
        auto pointerModifiers = WPE_VIEW_WAYLAND(view)->priv->savedPointerModifiers;
        auto pointerCoords = WPE_VIEW_WAYLAND(view)->priv->savedPointerCoords;
        double x = pointerCoords.first;
        double y = pointerCoords.second;
        double dX = wl_fixed_to_double(deltaX);
        double dY = wl_fixed_to_double(deltaY);

        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE, 0, static_cast<WPEModifiers>(pointerModifiers), x, y, dX, dY));
        wpe_view_event(view, event.get());
    }
};

static gboolean wpeViewWaylandLockPointer(WPEView* view)
{
    auto* priv = WPE_VIEW_WAYLAND(view)->priv;
    if (priv->relativePointer || priv->lockedPointer)
        return FALSE;

    auto* display = WPE_DISPLAY_WAYLAND(wpe_view_get_display(view));
    auto* pointerConstraints = wpeDisplayWaylandGetPointerConstraints(display);
    auto* relativePointerManager = wpeDisplayWaylandGetRelativePointerManager(display);
    if (!pointerConstraints || !relativePointerManager)
        return FALSE;

    WPE::WaylandSeat* seat = wpeDisplayWaylandGetSeat(display);
    priv->savedPointerModifiers = seat->pointerModifiers();
    priv->savedPointerCoords = seat->pointerCoords();
    struct wl_pointer* wlPointer = wl_seat_get_pointer(seat->seat());

    priv->relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relativePointerManager, wlPointer);
    zwp_relative_pointer_v1_add_listener(priv->relativePointer, &relativePointerListener, view);
    auto* wlSurface = wpe_view_wayland_get_wl_surface(WPE_VIEW_WAYLAND(view));
    priv->lockedPointer = zwp_pointer_constraints_v1_lock_pointer(pointerConstraints, wlSurface, wlPointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);

    return TRUE;
}

static gboolean wpeViewWaylandUnlockPointer(WPEView* view)
{
    auto* priv = WPE_VIEW_WAYLAND(view)->priv;
    g_clear_pointer(&priv->lockedPointer, zwp_locked_pointer_v1_destroy);
    g_clear_pointer(&priv->relativePointer, zwp_relative_pointer_v1_destroy);

    return TRUE;
}

static void wpeViewWaylandSetCursorFromBytes(WPEView* view, GBytes* bytes, guint width, guint height, guint stride, guint hotspotX, guint hotspotY)
{
    auto* display = WPE_DISPLAY_WAYLAND(wpe_view_get_display(view));
    auto* cursor = wpeDisplayWaylandGetCursor(display);
    if (!cursor)
        return;

    auto* sharedMemoryBuffer = sharedMemoryBufferCreate(view, bytes, width, height, stride);
    if (!sharedMemoryBuffer)
        return;

    wl_buffer_add_listener(sharedMemoryBuffer->wlBuffer(), &cursorBufferListener, sharedMemoryBuffer);
    cursor->setFromBuffer(sharedMemoryBuffer->wlBuffer(), width, height, hotspotX, hotspotY);
}

static void wpeViewWaylandSetOpaqueRectangles(WPEView* view, WPERectangle* rects, guint rectsCount)
{
    auto* priv = WPE_VIEW_WAYLAND(view)->priv;
    if (priv->resizedID) {
        g_signal_handler_disconnect(view, priv->resizedID);
        priv->resizedID = 0;
    }

    priv->opaqueRegion.clear();
    if (rects) {
        priv->opaqueRegion.reserveInitialCapacity(rectsCount);
        for (unsigned i = 0; i < rectsCount; ++i)
            priv->opaqueRegion.append(rects[i]);
    }
    if (auto* toplevel = wpe_view_get_toplevel(view))
        wpeToplevelWaylandSetOpaqueRectangles(WPE_TOPLEVEL_WAYLAND(toplevel), !priv->opaqueRegion.isEmpty() ? priv->opaqueRegion.mutableSpan().data() : nullptr, priv->opaqueRegion.size());
}

static gboolean wpeViewWaylandCanBeMapped(WPEView* view)
{
    if (auto* toplevel = wpe_view_get_toplevel(view))
        return !!wpe_toplevel_get_screen(toplevel);
    return FALSE;
}

static constexpr int kPopupItemHeight = 32;
static constexpr int kPopupVerticalPadding = 8;
static constexpr int kPopupHorizontalPadding = 8;
static constexpr double kPopupItemFontSize = 14.0;
static constexpr int kPopupDefaultWidth = 400;

// This is a very basic implementation of a popup menu.
// It supports: Simple text in fixed height rows, hover states, and activation.
// It does not support: Tooltips, RTL text, accessibility, or scaling.
// See wpe-platform-gtk if those are needed as it supports everything.
class WPEWaylandPopupMenu {
    WTF_MAKE_TZONE_ALLOCATED(WPEWaylandPopupMenu);
public:
    WPEWaylandPopupMenu(WPEView* view, WPEPopupMenu* menu)
        : m_view(view)
        , m_menu(menu)
        , m_itemCount(static_cast<int>(wpe_popup_menu_get_n_items(menu)))
    {
        m_width = kPopupDefaultWidth;
        m_height = 2 * kPopupVerticalPadding + m_itemCount * kPopupItemHeight;
        m_stride = m_width * 4; // ARGB32 = 4 bytes per pixel

        auto* display = WPE_DISPLAY_WAYLAND(wpe_view_get_display(view));
        auto* compositor = wpe_display_wayland_get_wl_compositor(display);
        auto* xdgWmBase = wpeDisplayWaylandGetXDGWMBase(display);
        auto* seat = wpeDisplayWaylandGetSeat(display);
        if (!compositor || !xdgWmBase || !seat)
            return;

        size_t bufferSize = static_cast<size_t>(m_stride) * m_height;
        m_shmPool = WPE::WaylandSHMPool::create(wpe_display_wayland_get_wl_shm(display), bufferSize);
        if (!m_shmPool)
            return;

        assert(m_shmPool->allocate(bufferSize) == 0);
        m_wlBuffer = m_shmPool->createBuffer(0, m_width, m_height, m_stride);
        if (!m_wlBuffer)
            return;

        paint();

        m_surface = wl_compositor_create_surface(compositor);
        if (!m_surface)
            return;

        auto* toplevel = WPE_TOPLEVEL_WAYLAND(wpe_view_get_toplevel(view));
        auto* parentXdgSurface = toplevel ? wpeToplevelWaylandGetXDGSurface(toplevel) : nullptr;

        WPERectangle rect;
        wpe_popup_menu_get_rect(menu, &rect);

        auto* positioner = xdg_wm_base_create_positioner(xdgWmBase);
        xdg_positioner_set_size(positioner, m_width, m_height);
        xdg_positioner_set_anchor_rect(positioner, rect.x, rect.y, std::max(1, rect.width), std::max(1, rect.height));
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(positioner,
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);

        m_xdgSurface = xdg_wm_base_get_xdg_surface(xdgWmBase, m_surface);
        xdg_surface_add_listener(m_xdgSurface, &m_xdgSurfaceListener, this);

        m_xdgPopup = xdg_surface_get_popup(m_xdgSurface, parentXdgSurface, positioner);
        xdg_popup_add_listener(m_xdgPopup, &m_xdgPopupListener, this);
        xdg_positioner_destroy(positioner);

        xdg_popup_grab(m_xdgPopup, seat->seat(), seat->pointerButtonSerial());

        // Route pointer events from the seat to our surface
        seat->setExternalSurface(m_surface, [this](uint32_t time, double x, double y, uint32_t button, uint32_t state) {
            if (!button)
                handleMotion(time, x, y);
            else
                handleButton(time, x, y, button, state);
        });

        wl_surface_commit(m_surface);
    }

    ~WPEWaylandPopupMenu()
    {
        auto* display = WPE_DISPLAY_WAYLAND(wpe_view_get_display(m_view));
        if (auto* seat = wpeDisplayWaylandGetSeat(display))
            seat->clearExternalSurface();

        g_clear_pointer(&m_xdgPopup, xdg_popup_destroy);
        g_clear_pointer(&m_xdgSurface, xdg_surface_destroy);
        g_clear_pointer(&m_wlBuffer, wl_buffer_destroy);
        g_clear_pointer(&m_surface, wl_surface_destroy);
    }

private:
    void paint()
    {
        assert(m_wlBuffer); // Buffer for surface was allocated on construction.
        auto* cairoSurface = m_shmPool->createCairoSurface(m_width, m_height, m_stride);
        auto* cr = cairo_create(cairoSurface);

        // White background
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);

        // Gray border
        cairo_set_source_rgb(cr, 0.70, 0.70, 0.70);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, 0.5, 0.5, m_width - 1, m_height - 1);
        cairo_stroke(cr);

        // FIXME: WPE_SETTING_FONT_NAME
        cairo_set_font_size(cr, kPopupItemFontSize);

        for (int i = 0; i < m_itemCount; i++) {
            auto* item = wpe_popup_menu_get_item(m_menu.get(), i);
            double itemY = kPopupVerticalPadding + i * kPopupItemHeight;

            bool isHovered = (i == m_hoveredIndex);
            bool isEnabled = wpe_popup_menu_item_is_enabled(item);
            WPEPopupMenuItemType itemType = wpe_popup_menu_item_get_item_type(item);
            bool isSelected = (i == wpe_popup_menu_get_selected_index(m_menu.get()));
            bool isInteractive = isEnabled && itemType != WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP && itemType != WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR;

            // Item background
            if (isHovered && isInteractive) // Bright blue
                cairo_set_source_rgb(cr, 0.22, 0.56, 0.85);
            else if (isSelected) // Light blue
                cairo_set_source_rgb(cr, 0.76, 0.90, 1.0);
            else // White
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

            cairo_rectangle(cr, 1, itemY, m_width - 2, kPopupItemHeight);
            cairo_fill(cr);

            if (itemType == WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR) {
                // Draw a gray horizontal separator centred in the item row.
                double sepY = itemY + kPopupItemHeight / 2.0;
                cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, kPopupHorizontalPadding, sepY);
                cairo_line_to(cr, m_width - kPopupHorizontalPadding, sepY);
                cairo_stroke(cr);
                continue;
            }

            if (!isEnabled)
                cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
            else if (isHovered && isInteractive)
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            else
                cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);

            double textX = kPopupHorizontalPadding;
            if (itemType == WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP_CHILD)
                textX += kPopupHorizontalPadding * 2; // Indent children
            else if (itemType == WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP)
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            else
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

            cairo_move_to(cr, textX, itemY + kPopupItemHeight * 0.65);
            cairo_show_text(cr, wpe_popup_menu_item_get_label(item));
        }

        cairo_destroy(cr);
        cairo_surface_destroy(cairoSurface);
    }

    int itemAtY(double y) const
    {
        double adjustedY = y - kPopupVerticalPadding;
        if (adjustedY < 0 || adjustedY >= m_itemCount * kPopupItemHeight)
            return -1;
        return static_cast<int>(adjustedY / kPopupItemHeight);
    }

    void handleMotion(uint32_t /*time*/, double /*x*/, double y)
    {
        int newHovered = itemAtY(y);
        if (newHovered == m_hoveredIndex)
            return;

        m_hoveredIndex = newHovered;
        paint();

        if (m_surface) {
            wl_surface_attach(m_surface, m_wlBuffer, 0, 0);
            wl_surface_damage(m_surface, 0, 0, m_width, m_height);
            wl_surface_commit(m_surface);
        }

        if (m_hoveredIndex >= 0) {
            auto* item = wpe_popup_menu_get_item(m_menu.get(), m_hoveredIndex);
            WPEPopupMenuItemType itemType = wpe_popup_menu_item_get_item_type(item);
            if (wpe_popup_menu_item_is_enabled(item) && itemType != WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP && itemType != WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR)
                wpe_popup_menu_select_item(m_menu.get(), m_hoveredIndex);
        }
    }

    void handleButton(uint32_t /*time*/, double /*x*/, double y, uint32_t button, uint32_t state)
    {
        if (button != BTN_LEFT)
            return;

        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            m_pressedIndex = itemAtY(y);
            return;
        }
        // else button released
        int pressed = m_pressedIndex;
        m_pressedIndex = -1;

        int released = itemAtY(y);
        if (released >= 0 && released == pressed) {
            auto* item = wpe_popup_menu_get_item(m_menu.get(), released);
            WPEPopupMenuItemType itemType = wpe_popup_menu_item_get_item_type(item);
            if (wpe_popup_menu_item_is_enabled(item) && itemType != WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP && itemType != WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR) {
                GRefPtr<WPEPopupMenu> menu = m_menu;
                // NOTE: activate_item will fire item-activated, which destroys 'this' via our
                // signal handler in wpeViewWaylandShowPopupMenu. Do NOT access any members after.
                wpe_popup_menu_activate_item(menu.get(), released);
                // 'this' is now destroyed — return immediately
                return;
            }
        }
    }

    static const struct xdg_surface_listener m_xdgSurfaceListener;
    static const struct xdg_popup_listener m_xdgPopupListener;

    WPEView* m_view;
    GRefPtr<WPEPopupMenu> m_menu;
    std::unique_ptr<WPE::WaylandSHMPool> m_shmPool;
    struct wl_buffer* m_wlBuffer { nullptr };
    struct wl_surface* m_surface { nullptr };
    struct xdg_surface* m_xdgSurface { nullptr };
    struct xdg_popup* m_xdgPopup { nullptr };

    int m_width { 0 };
    int m_height { 0 };
    int m_stride { 0 };
    int m_itemCount { 0 };
    int m_hoveredIndex { -1 };
    int m_pressedIndex { -1 };
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(WPEWaylandPopupMenu);

static void destroyPopupMenu(WPEWaylandPopupMenu* popup) { delete popup; }

const struct xdg_surface_listener WPEWaylandPopupMenu::m_xdgSurfaceListener = {
    // configure
    [](void* data, struct xdg_surface* xdgSurface, uint32_t serial)
    {
        xdg_surface_ack_configure(xdgSurface, serial);
        auto* self = static_cast<WPEWaylandPopupMenu*>(data);
        if (!self->m_surface || !self->m_wlBuffer) {
            return;
        }
        wl_surface_attach(self->m_surface, self->m_wlBuffer, 0, 0);
        wl_surface_damage(self->m_surface, 0, 0, self->m_width, self->m_height);
        wl_surface_commit(self->m_surface);
    }
};

const struct xdg_popup_listener WPEWaylandPopupMenu::m_xdgPopupListener = {
    // configure
    [](void*, struct xdg_popup*, int32_t, int32_t, int32_t, int32_t) { },
    // popup_done
    [](void* data, struct xdg_popup*)
    {
        auto* self = static_cast<WPEWaylandPopupMenu*>(data);
        GRefPtr<WPEPopupMenu> menu = self->m_menu;
        // Calling close fires the signal which frees the menu.
        wpe_popup_menu_close(menu.get());
    },
    // repositioned
    [](void*, struct xdg_popup*, uint32_t) { }
};

static gboolean wpeViewWaylandShowPopupMenu(WPEView* view, WPEPopupMenu* menu)
{
    auto priv = WPE_VIEW_WAYLAND(view)->priv;
    g_clear_pointer(&priv->popupMenu, destroyPopupMenu);
    priv->popupMenu = new WPEWaylandPopupMenu(view, menu);

    g_signal_connect(menu, "item-activated", G_CALLBACK(+[](WPEPopupMenu*, gint, WPEViewWayland* v) {
        g_clear_pointer(&v->priv->popupMenu, destroyPopupMenu);
    }), view);

    g_signal_connect(menu, "close", G_CALLBACK(+[](WPEPopupMenu*, WPEViewWayland* v) {
    g_clear_pointer(&v->priv->popupMenu, destroyPopupMenu);
    }), view);

    return TRUE;
}

static void wpe_view_wayland_class_init(WPEViewWaylandClass* viewWaylandClass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(viewWaylandClass);
    objectClass->constructed = wpeViewWaylandConstructed;
    objectClass->dispose = wpeViewWaylandDispose;

    WPEViewClass* viewClass = WPE_VIEW_CLASS(viewWaylandClass);
    viewClass->render_buffer = wpeViewWaylandRenderBuffer;
    viewClass->lock_pointer = wpeViewWaylandLockPointer;
    viewClass->unlock_pointer = wpeViewWaylandUnlockPointer;
    viewClass->set_cursor_from_name = wpeViewWaylandSetCursorFromName;
    viewClass->set_cursor_from_bytes = wpeViewWaylandSetCursorFromBytes;
    viewClass->set_opaque_rectangles = wpeViewWaylandSetOpaqueRectangles;
    viewClass->can_be_mapped = wpeViewWaylandCanBeMapped;
    viewClass->show_popup_menu = wpeViewWaylandShowPopupMenu;
}

/**
 * wpe_view_wayland_get_wl_surface: (skip)
 * @view: a #WPEViewWayland
 *
 * Get the native Wayland view of @view
 *
 * Returns: (transfer none) (nullable): a Wayland `wl_surface`
 */
struct wl_surface* wpe_view_wayland_get_wl_surface(WPEViewWayland* view)
{
    g_return_val_if_fail(WPE_IS_VIEW_WAYLAND(view), nullptr);

    if (auto* toplevel = wpe_view_get_toplevel(WPE_VIEW(view)))
        return wpe_toplevel_wayland_get_wl_surface(WPE_TOPLEVEL_WAYLAND(toplevel));
    return nullptr;
}

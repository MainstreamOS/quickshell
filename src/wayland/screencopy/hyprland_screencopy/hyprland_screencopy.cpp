#include "hyprland_screencopy.hpp"
#include <cstdint>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qtmetamacros.h>
#include <qwaylandclientextension.h>
#include <wayland-hyprland-toplevel-export-v1-client-protocol.h>

#include "../../../core/logcat.hpp"
#include "../../toplevel/wlr_toplevel.hpp"
#include "../manager.hpp"
#include "hyprland_screencopy_p.hpp"

namespace qs::wayland::screencopy::hyprland {

namespace {
QS_LOGGING_CATEGORY(logScreencopy, "quickshell.wayland.screencopy.hyprland", QtWarningMsg);
}

HyprlandScreencopyManager::HyprlandScreencopyManager(): QWaylandClientExtensionTemplate(2) {
	this->initialize();
}

HyprlandScreencopyManager* HyprlandScreencopyManager::instance() {
	static auto* instance = new HyprlandScreencopyManager();
	return instance;
}

ScreencopyContext* HyprlandScreencopyManager::captureToplevel(
    toplevel::wlr::ToplevelHandle* handle,
    bool paintCursors
) {
	return new HyprlandScreencopyContext(this, handle, paintCursors);
}

HyprlandScreencopyContext::HyprlandScreencopyContext(
    HyprlandScreencopyManager* manager,
    toplevel::wlr::ToplevelHandle* handle,
    bool paintCursors
)
    : manager(manager)
    , handle(handle)
    , paintCursors(paintCursors) {
	this->retryTimer.setSingleShot(true);
	QObject::connect(&this->retryTimer, &QTimer::timeout, this, [this]() { this->captureFrame(); });
	QObject::connect(
	    handle,
	    &QObject::destroyed,
	    this,
	    &HyprlandScreencopyContext::onToplevelDestroyed
	);
	// A closed handle has already been destroyed on the wire, long before the object goes away.
	// A frame requested with it passes null for a non-nullable argument, which is fatal to the
	// whole connection.
	QObject::connect(
	    handle,
	    &toplevel::wlr::ToplevelHandle::closed,
	    this,
	    &HyprlandScreencopyContext::onToplevelDestroyed
	);
}

HyprlandScreencopyContext::~HyprlandScreencopyContext() { this->releaseFrame(); }

// A view can drop this context while the closed event is still being handled, before
// onToplevelDestroyed runs, so both paths go through here.
void HyprlandScreencopyContext::releaseFrame() {
	if (!this->frame()) return;

	if (this->frameAnswered || (this->handle && this->handle->object())) {
		this->destroy();
	} else {
		// Dropped on this side only: no request names an object the compositor never made.
		wl_proxy_destroy(reinterpret_cast<wl_proxy*>(this->object())); // NOLINT
		this->frameAbandoned = true;
	}
}

struct ::hyprland_toplevel_export_frame_v1* HyprlandScreencopyContext::frame() {
	return this->frameAbandoned ? nullptr : this->object();
}

void HyprlandScreencopyContext::onToplevelDestroyed() {
	qCDebug(logScreencopy) << "Toplevel closed while recording. Stopping" << this;
	this->retryTimer.stop();
	this->releaseFrame();
	this->handle = nullptr;
	emit this->stopped();
}

void HyprlandScreencopyContext::captureFrame() {
	if (this->frame() || this->frameAbandoned || !this->handle || !this->handle->object()
	    || this->retryTimer.isActive())
		return;

	this->request.reset();
	this->frameAnswered = false;

	this->init(this->manager->capture_toplevel_with_wlr_toplevel_handle(
	    this->paintCursors ? 1 : 0,
	    this->handle->object()
	));
}

void HyprlandScreencopyContext::hyprland_toplevel_export_frame_v1_buffer(
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t /*stride*/
) {
	this->frameAnswered = true;
	// While different sizes can technically be requested, that would be insane.
	this->request.width = width;
	this->request.height = height;
	this->request.shm.formats.push(format);
}

void HyprlandScreencopyContext::hyprland_toplevel_export_frame_v1_linux_dmabuf(
    uint32_t format,
    uint32_t width,
    uint32_t height
) {
	this->frameAnswered = true;
	// While different sizes can technically be requested, that would be insane.
	this->request.width = width;
	this->request.height = height;
	this->request.dmabuf.formats.push(format);
}

void HyprlandScreencopyContext::hyprland_toplevel_export_frame_v1_flags(uint32_t flags) {
	this->frameAnswered = true;
	if (flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT) {
		this->mSwapchain.backbuffer()->transform = buffer::WlBufferTransform::Flipped180;
	}
}

void HyprlandScreencopyContext::hyprland_toplevel_export_frame_v1_buffer_done() {
	auto* backbuffer = this->mSwapchain.createBackbuffer(this->request);

	if (!backbuffer || !backbuffer->buffer()) {
		qCWarning(logScreencopy) << "Backbuffer creation failed for screencopy. Skipping frame.";

		this->retryFrame();
		return;
	}

	this->copy(backbuffer->buffer(), this->copiedFirstFrame ? 0 : 1);
}

void HyprlandScreencopyContext::hyprland_toplevel_export_frame_v1_ready(
    uint32_t /*tvSecHi*/,
    uint32_t /*tvSecLo*/,
    uint32_t /*tvNsec*/
) {
	this->destroy();
	this->failedFrames = 0;
	this->copiedFirstFrame = true;
	this->mSwapchain.swapBuffers();
	emit this->frameCaptured();
}

void HyprlandScreencopyContext::hyprland_toplevel_export_frame_v1_failed() {
	this->frameAnswered = true;
	this->retryFrame();
}

void HyprlandScreencopyContext::retryFrame() {
	// A resize can invalidate the size offered for an in-flight frame. Keep
	// the last good buffer and request a fresh offer instead of ending the view.
	if (this->frame()) this->destroy();
	if (++this->failedFrames > 10) {
		qCWarning(logScreencopy) << "Ending recording after repeated screencopy failures for" << this;
		emit this->stopped();
		return;
	}
	// Force damage on the replacement, even if the resized window is idle.
	this->copiedFirstFrame = false;
	this->retryTimer.start(qMin(this->failedFrames * 50U, 250U));
}

} // namespace qs::wayland::screencopy::hyprland

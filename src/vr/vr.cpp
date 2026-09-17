/* --------------------------------------------------------------------
EXTREME TUXRACER -- VR façade, implementation
---------------------------------------------------------------------*/

#include "vr.h"

#ifdef ENABLE_OPENXR

#include "xr_manager.hpp"
#include "xr_input.h"
#include "../gles/gl_compat.h"
#include "../platform/platform.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ETRVR", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRVR", __VA_ARGS__)

namespace vr {

// Starting point only; expect to tune this on-device. Larger values make
// the world feel bigger and the player smaller.
double world_scale = 1.0;

namespace {

int  g_current_eye = -1;
bool g_frame_live  = false;
bool g_in_screen   = false;

}  // namespace

// --------------------------------------------------------------------
//	Lifecycle
// --------------------------------------------------------------------

bool Init() {
	XRManager::create();
	XRManager* xr = XRManager::get();
	if (!xr || !xr->init()) {
		LOGE("no usable OpenXR runtime -- running flat");
		XRManager::destroy();
		return false;
	}
	if (!xr->createEyeSwapchains()) {
		LOGE("could not create the per-eye swapchains -- running flat");
		XRManager::destroy();
		return false;
	}

	// The 2D screens are composited onto a quad layer. Matching its size to
	// the game's 2D canvas keeps text crisp: the glyph atlas is rasterised
	// at layout size, so scaling up here would only blur it.
	unsigned int canvas_w = 0, canvas_h = 0;
	etr_platform::GetDrawableSize(canvas_w, canvas_h);
	if (!xr->createScreenSwapchain(canvas_w, canvas_h)) {
		LOGE("could not create the 2D screen swapchain");
		XRManager::destroy();
		return false;
	}
	InitInput(xr);
	LOGI("VR active, world scale %.2f units/m", world_scale);
	return true;
}

void Shutdown() {
	ShutdownInput();
	XRManager::destroy();
}

bool IsActive() { return XRManager::isVRActive(); }

bool IsSessionRunning() {
	XRManager* xr = XRManager::get();
	return xr && xr->isSessionRunning();
}

bool IsExitRequested() {
	XRManager* xr = XRManager::get();
	return xr && xr->isExitRequested();
}

// --------------------------------------------------------------------
//	Frame driver
// --------------------------------------------------------------------

bool BeginFrame() {
	XRManager* xr = XRManager::get();
	if (!xr) return false;

	xr->pollEvents();
	if (!xr->isSessionRunning()) return false;

	g_frame_live = xr->beginFrame();
	if (g_frame_live) UpdateInput();
	return g_frame_live && xr->shouldRender();
}

void EndFrame() {
	XRManager* xr = XRManager::get();
	if (!xr || !g_frame_live) return;
	xr->endFrame();
	g_frame_live = false;
}

int NumEyes() { return 2; }
int CurrentEye() { return g_current_eye; }

void BeginEye(int eye) {
	XRManager* xr = XRManager::get();
	if (!xr) return;

	const GLuint fbo = xr->acquireEyeImage(eye);
	if (fbo == 0) return;

	g_current_eye = eye;

	const XRSwapchain& sc = xr->getEyeSwapchain(eye);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, static_cast<GLsizei>(sc.m_width),
	           static_cast<GLsizei>(sc.m_height));
}

void EndEye(int eye) {
	XRManager* xr = XRManager::get();
	if (!xr) return;

	xr->releaseEyeImage(eye);
	g_current_eye = -1;

	// Queue the projection layer once both eyes are in.
	if (eye == NumEyes() - 1) xr->queueProjectionLayer();
}

// --------------------------------------------------------------------
//	Flat 2D path
// --------------------------------------------------------------------

bool BeginScreen() {
	XRManager* xr = XRManager::get();
	if (!xr) return false;

	const GLuint fbo = xr->acquireScreenImage();
	if (fbo == 0) return false;

	g_in_screen = true;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, static_cast<GLsizei>(xr->getScreenWidth()),
	           static_cast<GLsizei>(xr->getScreenHeight()));
	return true;
}

void EndScreen() {
	XRManager* xr = XRManager::get();
	if (!xr || !g_in_screen) return;
	xr->releaseScreenImage();
	g_in_screen = false;
}

void GetScreenSize(unsigned int& width, unsigned int& height) {
	XRManager* xr = XRManager::get();
	width  = xr ? xr->getScreenWidth() : 0;
	height = xr ? xr->getScreenHeight() : 0;
}

// --------------------------------------------------------------------
//	Camera injection
// --------------------------------------------------------------------

bool GetEyeProjection(double near_dist, double far_dist,
                      TMatrix<4, 4>& out) {
	XRManager* xr = XRManager::get();
	if (!xr || g_current_eye < 0) return false;

	const XrView* views = xr->getViews();
	if (!views) return false;

	const XrFovf& fov = views[g_current_eye].fov;

	// The runtime's field of view is asymmetric per eye, so this cannot be
	// expressed as a gluPerspective and has to be built from the four
	// tangents directly.
	const double tan_l = std::tan(fov.angleLeft);
	const double tan_r = std::tan(fov.angleRight);
	const double tan_u = std::tan(fov.angleUp);
	const double tan_d = std::tan(fov.angleDown);

	const double tan_w = tan_r - tan_l;
	const double tan_h = tan_u - tan_d;
	if (std::fabs(tan_w) < 1e-9 || std::fabs(tan_h) < 1e-9) return false;

	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			out[c][r] = 0.0;

	out[0][0] = 2.0 / tan_w;
	out[1][1] = 2.0 / tan_h;
	out[2][0] = (tan_r + tan_l) / tan_w;
	out[2][1] = (tan_u + tan_d) / tan_h;
	out[2][2] = -(far_dist + near_dist) / (far_dist - near_dist);
	out[2][3] = -1.0;
	out[3][2] = -(2.0 * far_dist * near_dist) / (far_dist - near_dist);
	return true;
}

bool GetEyeViewOffset(TMatrix<4, 4>& out) {
	XRManager* xr = XRManager::get();
	if (!xr || g_current_eye < 0) return false;

	const XrView* views = xr->getViews();
	if (!views) return false;

	const XrPosef& pose = views[g_current_eye].pose;
	const XrQuaternionf& q = pose.orientation;

	// Rotation matrix for the eye's orientation in the reference space.
	const double x = q.x, y = q.y, z = q.z, w = q.w;
	const double r00 = 1 - 2 * (y * y + z * z);
	const double r01 = 2 * (x * y - z * w);
	const double r02 = 2 * (x * z + y * w);
	const double r10 = 2 * (x * y + z * w);
	const double r11 = 1 - 2 * (x * x + z * z);
	const double r12 = 2 * (y * z - x * w);
	const double r20 = 2 * (x * z - y * w);
	const double r21 = 2 * (y * z + x * w);
	const double r22 = 1 - 2 * (x * x + y * y);

	// Head translation arrives in metres; the game world has its own unit.
	const double px = pose.position.x * world_scale;
	const double py = pose.position.y * world_scale;
	const double pz = pose.position.z * world_scale;

	// out = inverse(eye pose) = [R^T | -R^T * p], column-major.
	out[0][0] = r00; out[0][1] = r01; out[0][2] = r02; out[0][3] = 0.0;
	out[1][0] = r10; out[1][1] = r11; out[1][2] = r12; out[1][3] = 0.0;
	out[2][0] = r20; out[2][1] = r21; out[2][2] = r22; out[2][3] = 0.0;

	out[3][0] = -(r00 * px + r10 * py + r20 * pz);
	out[3][1] = -(r01 * px + r11 * py + r21 * pz);
	out[3][2] = -(r02 * px + r12 * py + r22 * pz);
	out[3][3] = 1.0;
	return true;
}

void GetCullHalfFov(double& half_vertical, double& half_horizontal) {
	half_vertical = half_horizontal = 0.0;

	XRManager* xr = XRManager::get();
	if (!xr) return;
	const XrView* views = xr->getViews();
	if (!views) return;

	// Take the widest angle across both eyes in each direction: the two
	// frusta are asymmetric and offset, so their union is what is actually
	// visible.
	for (int eye = 0; eye < 2; eye++) {
		const XrFovf& f = views[eye].fov;
		half_horizontal = std::max(half_horizontal,
		                           std::max(std::fabs((double)f.angleLeft),
		                                    std::fabs((double)f.angleRight)));
		half_vertical = std::max(half_vertical,
		                         std::max(std::fabs((double)f.angleUp),
		                                  std::fabs((double)f.angleDown)));
	}
}

}  // namespace vr

#else  // !ENABLE_OPENXR

// Flat build: everything compiles away to nothing.
namespace vr {
double world_scale = 1.0;
bool Init() { return false; }
void Shutdown() {}
bool IsActive() { return false; }
bool IsSessionRunning() { return false; }
bool IsExitRequested() { return false; }
bool BeginFrame() { return false; }
void EndFrame() {}
int  NumEyes() { return 1; }
void BeginEye(int) {}
void EndEye(int) {}
int  CurrentEye() { return -1; }
bool BeginScreen() { return false; }
void EndScreen() {}
void GetScreenSize(unsigned int& w, unsigned int& h) { w = h = 0; }
bool GetEyeProjection(double, double, TMatrix<4, 4>&) { return false; }
bool GetEyeViewOffset(TMatrix<4, 4>&) { return false; }
void GetCullHalfFov(double& v, double& h) { v = h = 0.0; }
void UpdateInput() {}
void SetUiNavigation(bool) {}
}  // namespace vr

#endif  // ENABLE_OPENXR

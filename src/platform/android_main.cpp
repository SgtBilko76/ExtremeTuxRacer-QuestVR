/* --------------------------------------------------------------------
EXTREME TUXRACER -- Android entry point

Quest immersive apps are NativeActivity-based and never get a 2D window
surface: Horizon OS tears it down as soon as it recognises the VR intent
categories. That rules out SDL's Android backend, which will not start its
main thread until a surface exists, so the platform layer owns the EGL
context and the activity lifecycle directly.

Rendering targets OpenXR swapchain framebuffers exclusively; the tiny
pbuffer below exists only to give the context something to be current on.
---------------------------------------------------------------------*/

#include "platform.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ETRPlat", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRPlat", __VA_ARGS__)

/** The game's entry point, renamed on Android so that android_main() can
 *  drive it (see the bottom of src/main.cpp). */
extern int etr_main(int argc, char** argv);

namespace etr_platform {
namespace {

struct AndroidState {
	android_app* app = nullptr;

	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig  config  = nullptr;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;

	bool resumed = false;
	bool destroy_requested = false;
	bool egl_ready = false;
};

AndroidState g;

void onAppCmd(android_app* /*app*/, int32_t cmd) {
	switch (cmd) {
		case APP_CMD_RESUME:  g.resumed = true;  break;
		case APP_CMD_PAUSE:   g.resumed = false; break;
		case APP_CMD_DESTROY: g.destroy_requested = true; break;
		default: break;
	}
}

bool createEGL() {
	g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g.display == EGL_NO_DISPLAY) {
		LOGE("eglGetDisplay failed");
		return false;
	}

	EGLint major = 0, minor = 0;
	if (!eglInitialize(g.display, &major, &minor)) {
		LOGE("eglInitialize failed");
		return false;
	}

	const EGLint cfg_attr[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
		EGL_RED_SIZE,        8,
		EGL_GREEN_SIZE,      8,
		EGL_BLUE_SIZE,       8,
		EGL_ALPHA_SIZE,      8,
		EGL_DEPTH_SIZE,      0,
		EGL_STENCIL_SIZE,    0,
		EGL_NONE
	};
	EGLint num_config = 0;
	if (!eglChooseConfig(g.display, cfg_attr, &g.config, 1, &num_config) ||
	    num_config < 1) {
		LOGE("no GLES3 pbuffer config available");
		return false;
	}

	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	g.context = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctx_attr);
	if (g.context == EGL_NO_CONTEXT) {
		LOGE("eglCreateContext failed");
		return false;
	}

	const EGLint surf_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	g.surface = eglCreatePbufferSurface(g.display, g.config, surf_attr);
	if (g.surface == EGL_NO_SURFACE) {
		LOGE("eglCreatePbufferSurface failed");
		return false;
	}

	if (!eglMakeCurrent(g.display, g.surface, g.surface, g.context)) {
		LOGE("eglMakeCurrent failed");
		return false;
	}

	LOGI("EGL %d.%d, %s / %s", major, minor, glGetString(GL_RENDERER),
	     glGetString(GL_VERSION));
	g.egl_ready = true;
	return true;
}

void destroyEGL() {
	if (g.display == EGL_NO_DISPLAY) return;
	eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g.surface != EGL_NO_SURFACE) eglDestroySurface(g.display, g.surface);
	if (g.context != EGL_NO_CONTEXT) eglDestroyContext(g.display, g.context);
	eglTerminate(g.display);
	g.display = EGL_NO_DISPLAY;
	g.egl_ready = false;
}

}  // namespace

// --------------------------------------------------------------------
//	Interface used by the rest of the platform layer
// --------------------------------------------------------------------

android_app* GetAndroidApp() { return g.app; }

bool IsResumed() { return g.resumed; }
bool IsDestroyRequested() {
	return g.destroy_requested || (g.app && g.app->destroyRequested);
}

/** Drains pending Android events without blocking. Called from the event
 *  pump every frame; blocking here would stall the XR frame loop. */
void PumpAndroidEvents() {
	if (!g.app) return;
	for (;;) {
		int events = 0;
		android_poll_source* source = nullptr;
		if (ALooper_pollOnce(0, nullptr, &events,
		                     reinterpret_cast<void**>(&source)) < 0)
			break;
		if (source) source->process(g.app, source);
	}
}

}  // namespace etr_platform

// --------------------------------------------------------------------
//	NativeActivity entry point
// --------------------------------------------------------------------

void android_main(android_app* app) {
	etr_platform::g.app = app;
	app->onAppCmd = etr_platform::onAppCmd;

	// The OpenXR loader calls back into the JVM, so this thread has to stay
	// attached for the lifetime of the app.
	JNIEnv* env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);

	// Wait until the activity is actually running before touching GL. The
	// runtime will not hand out an XR session before this either.
	while (!app->destroyRequested && !etr_platform::g.resumed) {
		etr_platform::PumpAndroidEvents();
		usleep(10000);
	}

	if (!app->destroyRequested) {
		if (etr_platform::createEGL()) {
			etr_main(0, nullptr);
		} else {
			LOGE("could not create a GLES 3 context -- exiting");
		}
	}

	etr_platform::destroyEGL();
	app->activity->vm->DetachCurrentThread();
	ANativeActivity_finish(app->activity);
}

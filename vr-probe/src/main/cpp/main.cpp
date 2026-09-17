// ETR VR probe -- minimal standalone OpenXR application for Meta Quest.
//
// Purpose: prove the whole toolchain end to end (NDK, vendored Khronos loader,
// manifest, signing, adb deploy) *and* the stereo submission path, before any
// Extreme Tux Racer code is involved. It creates an OpenXR session backed by
// GLES 3, allocates one swapchain per eye, and clears the left eye red and the
// right eye blue. Seeing two different colours in the headset means everything
// below the game is working.

#include <android/log.h>
#include <android_native_app_glue.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstring>
#include <string>
#include <vector>

#define LOG_TAG "ETRVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

const int NUM_EYES = 2;

// ---------------------------------------------------------------------------
//  Error helpers
// ---------------------------------------------------------------------------

XrInstance g_instance = XR_NULL_HANDLE;

std::string xrResultString(XrResult res) {
	char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
	if (g_instance != XR_NULL_HANDLE &&
	    xrResultToString(g_instance, res, buf) == XR_SUCCESS)
		return buf;
	return std::to_string(static_cast<int>(res));
}

// Logs and returns false on failure so callers can bail out cleanly. The probe
// deliberately never continues past a broken step -- a half-initialised XR
// session produces far more confusing symptoms than an early exit.
bool xrCheck(XrResult res, const char* what) {
	if (XR_SUCCEEDED(res)) return true;
	LOGE("%s failed: %s", what, xrResultString(res).c_str());
	return false;
}

// ---------------------------------------------------------------------------
//  EGL
// ---------------------------------------------------------------------------

// OpenXR owns the actual display surfaces, so the only thing EGL has to provide
// is a current GLES 3 context to bind the session to. A 16x16 pbuffer is enough
// to make the context current; nothing is ever drawn to it.
struct EglContext {
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig  config  = nullptr;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;

	bool create() {
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (display == EGL_NO_DISPLAY) {
			LOGE("eglGetDisplay failed");
			return false;
		}
		EGLint major = 0, minor = 0;
		if (!eglInitialize(display, &major, &minor)) {
			LOGE("eglInitialize failed");
			return false;
		}
		LOGI("EGL %d.%d", major, minor);

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
		if (!eglChooseConfig(display, cfg_attr, &config, 1, &num_config) ||
		    num_config < 1) {
			LOGE("eglChooseConfig found no GLES3 pbuffer config");
			return false;
		}

		const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attr);
		if (context == EGL_NO_CONTEXT) {
			LOGE("eglCreateContext failed");
			return false;
		}

		const EGLint surf_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
		surface = eglCreatePbufferSurface(display, config, surf_attr);
		if (surface == EGL_NO_SURFACE) {
			LOGE("eglCreatePbufferSurface failed");
			return false;
		}
		if (!eglMakeCurrent(display, surface, surface, context)) {
			LOGE("eglMakeCurrent failed");
			return false;
		}

		LOGI("GL_VERSION  : %s", glGetString(GL_VERSION));
		LOGI("GL_RENDERER : %s", glGetString(GL_RENDERER));
		return true;
	}

	void destroy() {
		if (display == EGL_NO_DISPLAY) return;
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
		if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
		eglTerminate(display);
		display = EGL_NO_DISPLAY;
	}
};

// ---------------------------------------------------------------------------
//  Swapchain + framebuffers
// ---------------------------------------------------------------------------

// One OpenXR swapchain plus a GL framebuffer object per image, so rendering an
// eye is just glBindFramebuffer() on the acquired index.
struct Swapchain {
	XrSwapchain handle = XR_NULL_HANDLE;
	uint32_t width = 0, height = 0;
	std::vector<XrSwapchainImageOpenGLESKHR> images;
	std::vector<GLuint> fbos;
	GLuint depth_rb = 0;

	bool create(XrSession session, int64_t format, uint32_t w, uint32_t h) {
		width = w;
		height = h;

		XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		ci.format      = format;
		ci.sampleCount = 1;
		ci.width       = w;
		ci.height      = h;
		ci.faceCount   = 1;
		ci.arraySize   = 1;
		ci.mipCount    = 1;
		if (!xrCheck(xrCreateSwapchain(session, &ci, &handle), "xrCreateSwapchain"))
			return false;

		uint32_t count = 0;
		if (!xrCheck(xrEnumerateSwapchainImages(handle, 0, &count, nullptr),
		             "xrEnumerateSwapchainImages(count)"))
			return false;

		images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
		if (!xrCheck(xrEnumerateSwapchainImages(
		                 handle, count, &count,
		                 reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
		             "xrEnumerateSwapchainImages"))
			return false;

		// A single depth buffer is shared by every image in the swapchain: only
		// one image is ever being rendered to at a time.
		glGenRenderbuffers(1, &depth_rb);
		glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		fbos.resize(count);
		glGenFramebuffers(count, fbos.data());
		for (uint32_t i = 0; i < count; i++) {
			glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       GL_TEXTURE_2D, images[i].image, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			                          GL_RENDERBUFFER, depth_rb);
			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				LOGE("FBO %u incomplete: 0x%x", i, status);
				return false;
			}
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		LOGI("swapchain %ux%u, %u images", w, h, count);
		return true;
	}

	void destroy() {
		if (!fbos.empty()) glDeleteFramebuffers(fbos.size(), fbos.data());
		if (depth_rb) glDeleteRenderbuffers(1, &depth_rb);
		if (handle != XR_NULL_HANDLE) xrDestroySwapchain(handle);
		fbos.clear();
		images.clear();
		handle = XR_NULL_HANDLE;
		depth_rb = 0;
	}
};

// ---------------------------------------------------------------------------
//  The probe
// ---------------------------------------------------------------------------

class VrProbe {
public:
	bool init(android_app* app);
	void shutdown();

	void pollXrEvents();
	void renderFrame();

	bool sessionRunning() const { return m_session_running; }
	bool exitRequested()  const { return m_exit_requested; }

private:
	bool initLoader(android_app* app);
	bool createInstance(android_app* app);
	bool createSession();
	int64_t chooseSwapchainFormat();
	void handleStateChange(const XrEventDataSessionStateChanged& ev);

	EglContext m_egl;

	XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
	XrSession  m_session   = XR_NULL_HANDLE;
	XrSpace    m_space     = XR_NULL_HANDLE;

	XrViewConfigurationView m_view_config[NUM_EYES];
	Swapchain m_swapchain[NUM_EYES];

	XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
	bool m_session_running = false;
	bool m_exit_requested  = false;
	int  m_frame_count     = 0;
};

bool VrProbe::initLoader(android_app* app) {
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	if (!xrCheck(xrGetInstanceProcAddr(
	                 XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	                 reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR)),
	             "xrGetInstanceProcAddr(xrInitializeLoaderKHR)"))
		return false;

	XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	info.applicationVM      = app->activity->vm;
	info.applicationContext = app->activity->clazz;
	return xrCheck(xrInitializeLoaderKHR(
	                   reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)),
	               "xrInitializeLoaderKHR");
}

bool VrProbe::createInstance(android_app* app) {
	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
	std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());

	bool has_gles = false, has_android = false;
	for (const auto& p : props) {
		if (!std::strcmp(p.extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME))
			has_gles = true;
		if (!std::strcmp(p.extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
			has_android = true;
	}
	if (!has_gles || !has_android) {
		LOGE("runtime lacks required extensions (gles=%d android=%d)",
		     has_gles, has_android);
		return false;
	}

	const char* enabled[] = {
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME
	};

	XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_info.applicationVM       = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
	ci.next = &android_info;
	std::strcpy(ci.applicationInfo.applicationName, "ETR VR Probe");
	std::strcpy(ci.applicationInfo.engineName, "etr");
	ci.applicationInfo.applicationVersion = 1;
	ci.applicationInfo.engineVersion      = 1;
	ci.applicationInfo.apiVersion         = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = 2;
	ci.enabledExtensionNames = enabled;

	if (!xrCheck(xrCreateInstance(&ci, &g_instance), "xrCreateInstance"))
		return false;

	XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &ip)))
		LOGI("runtime: %s", ip.runtimeName);
	return true;
}

bool VrProbe::createSession() {
	// The GLES graphics requirements query is mandatory before session creation
	// even though the returned versions are not otherwise used here.
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
	if (!xrCheck(xrGetInstanceProcAddr(
	                 g_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
	                 reinterpret_cast<PFN_xrVoidFunction*>(&getReq)),
	             "xrGetInstanceProcAddr(GraphicsRequirements)"))
		return false;

	XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	if (!xrCheck(getReq(g_instance, m_system_id, &req),
	             "xrGetOpenGLESGraphicsRequirementsKHR"))
		return false;

	XrGraphicsBindingOpenGLESAndroidKHR binding{
		XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	binding.display = m_egl.display;
	binding.config  = m_egl.config;
	binding.context = m_egl.context;

	XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
	ci.next     = &binding;
	ci.systemId = m_system_id;
	return xrCheck(xrCreateSession(g_instance, &ci, &m_session), "xrCreateSession");
}

int64_t VrProbe::chooseSwapchainFormat() {
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());

	// Prefer an sRGB target so the runtime's colour management matches what the
	// compositor expects; fall back to linear RGBA8 otherwise.
	for (int64_t f : formats)
		if (f == GL_SRGB8_ALPHA8) return f;
	for (int64_t f : formats)
		if (f == GL_RGBA8) return f;

	return count > 0 ? formats[0] : GL_RGBA8;
}

bool VrProbe::init(android_app* app) {
	if (!initLoader(app)) return false;
	if (!createInstance(app)) return false;

	XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
	sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!xrCheck(xrGetSystem(g_instance, &sys, &m_system_id), "xrGetSystem"))
		return false;

	if (!m_egl.create()) return false;
	if (!createSession()) return false;

	XrReferenceSpaceCreateInfo space_ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_ci.poseInReferenceSpace.orientation.w = 1.0f;
	if (!xrCheck(xrCreateReferenceSpace(m_session, &space_ci, &m_space),
	             "xrCreateReferenceSpace"))
		return false;

	uint32_t view_count = 0;
	if (!xrCheck(xrEnumerateViewConfigurationViews(
	                 g_instance, m_system_id,
	                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                 0, &view_count, nullptr),
	             "xrEnumerateViewConfigurationViews(count)"))
		return false;
	if (view_count != NUM_EYES) {
		LOGE("expected %d views, runtime reports %u", NUM_EYES, view_count);
		return false;
	}

	for (int i = 0; i < NUM_EYES; i++)
		m_view_config[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	if (!xrCheck(xrEnumerateViewConfigurationViews(
	                 g_instance, m_system_id,
	                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                 view_count, &view_count, m_view_config),
	             "xrEnumerateViewConfigurationViews"))
		return false;

	const int64_t format = chooseSwapchainFormat();
	for (int i = 0; i < NUM_EYES; i++) {
		if (!m_swapchain[i].create(m_session, format,
		                           m_view_config[i].recommendedImageRectWidth,
		                           m_view_config[i].recommendedImageRectHeight))
			return false;
	}

	LOGI("OpenXR initialised, %ux%u per eye",
	     m_view_config[0].recommendedImageRectWidth,
	     m_view_config[0].recommendedImageRectHeight);
	return true;
}

void VrProbe::handleStateChange(const XrEventDataSessionStateChanged& ev) {
	m_state = ev.state;
	LOGI("session state -> %d", static_cast<int>(m_state));

	switch (m_state) {
		case XR_SESSION_STATE_READY: {
			XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
			bi.primaryViewConfigurationType =
			    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if (xrCheck(xrBeginSession(m_session, &bi), "xrBeginSession"))
				m_session_running = true;
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			m_session_running = false;
			xrEndSession(m_session);
			break;
		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			m_exit_requested = true;
			break;
		default:
			break;
	}
}

void VrProbe::pollXrEvents() {
	XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
	while (true) {
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res != XR_SUCCESS) break;

		switch (ev.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				handleStateChange(
				    *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev));
				break;
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				m_exit_requested = true;
				break;
			default:
				break;
		}
	}
}

void VrProbe::renderFrame() {
	if (!m_session_running) return;

	XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState fs{XR_TYPE_FRAME_STATE};
	if (!xrCheck(xrWaitFrame(m_session, &wait, &fs), "xrWaitFrame")) return;

	XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
	if (!xrCheck(xrBeginFrame(m_session, &begin), "xrBeginFrame")) return;

	XrCompositionLayerProjectionView proj_views[NUM_EYES]{};
	XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	bool submit_layer = false;

	if (fs.shouldRender) {
		XrView views[NUM_EYES];
		for (int i = 0; i < NUM_EYES; i++) views[i] = {XR_TYPE_VIEW};

		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime           = fs.predictedDisplayTime;
		locate.space                 = m_space;

		XrViewState vs{XR_TYPE_VIEW_STATE};
		uint32_t out_count = 0;
		XrResult res = xrLocateViews(m_session, &locate, &vs, NUM_EYES,
		                             &out_count, views);

		const bool poses_valid =
		    XR_SUCCEEDED(res) &&
		    (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
		    (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

		if (poses_valid) {
			for (int eye = 0; eye < NUM_EYES; eye++) {
				Swapchain& sc = m_swapchain[eye];

				uint32_t index = 0;
				XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				if (!xrCheck(xrAcquireSwapchainImage(sc.handle, &ai, &index),
				             "xrAcquireSwapchainImage"))
					break;

				XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = XR_INFINITE_DURATION;
				if (!xrCheck(xrWaitSwapchainImage(sc.handle, &wi),
				             "xrWaitSwapchainImage")) {
					XrSwapchainImageReleaseInfo ri{
						XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					xrReleaseSwapchainImage(sc.handle, &ri);
					break;
				}

				// The actual test: left eye red, right eye blue. If both eyes
				// show the same colour the views are not being submitted
				// independently.
				glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[index]);
				glViewport(0, 0, sc.width, sc.height);
				glClearColor(eye == 0 ? 0.8f : 0.0f, 0.05f,
				             eye == 0 ? 0.0f : 0.8f, 1.0f);
				glClearDepthf(1.0f);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				glBindFramebuffer(GL_FRAMEBUFFER, 0);

				XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(sc.handle, &ri);

				proj_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				proj_views[eye].pose = views[eye].pose;
				proj_views[eye].fov  = views[eye].fov;
				proj_views[eye].subImage.swapchain = sc.handle;
				proj_views[eye].subImage.imageRect.offset = {0, 0};
				proj_views[eye].subImage.imageRect.extent = {
					static_cast<int32_t>(sc.width),
					static_cast<int32_t>(sc.height)
				};
				proj_views[eye].subImage.imageArrayIndex = 0;

				if (eye == NUM_EYES - 1) submit_layer = true;
			}

			if (submit_layer) {
				proj_layer.space     = m_space;
				proj_layer.viewCount = NUM_EYES;
				proj_layer.views     = proj_views;
			}
		}

		if (++m_frame_count % 300 == 1) {
			LOGI("frame %d: shouldRender=%d posesValid=%d headPos=(%.2f %.2f %.2f)",
			     m_frame_count, fs.shouldRender ? 1 : 0, poses_valid ? 1 : 0,
			     views[0].pose.position.x, views[0].pose.position.y,
			     views[0].pose.position.z);
		}
	}

	const XrCompositionLayerBaseHeader* layers[1] = {
		reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj_layer)
	};

	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
	end.displayTime          = fs.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount           = submit_layer ? 1 : 0;
	end.layers               = submit_layer ? layers : nullptr;
	xrCheck(xrEndFrame(m_session, &end), "xrEndFrame");
}

void VrProbe::shutdown() {
	for (int i = 0; i < NUM_EYES; i++) m_swapchain[i].destroy();
	if (m_space != XR_NULL_HANDLE) xrDestroySpace(m_space);
	if (m_session != XR_NULL_HANDLE) xrDestroySession(m_session);
	if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
	m_space = XR_NULL_HANDLE;
	m_session = XR_NULL_HANDLE;
	g_instance = XR_NULL_HANDLE;
	m_egl.destroy();
}

// ---------------------------------------------------------------------------
//  Android entry point
// ---------------------------------------------------------------------------

bool g_resumed = false;

void onAppCmd(android_app* /*app*/, int32_t cmd) {
	switch (cmd) {
		case APP_CMD_RESUME: g_resumed = true;  break;
		case APP_CMD_PAUSE:  g_resumed = false; break;
		default: break;
	}
}

}  // namespace

void android_main(android_app* app) {
	app->onAppCmd = onAppCmd;

	// The OpenXR loader reaches back into the JVM, so the calling thread has to
	// be attached for the lifetime of the app.
	JNIEnv* env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);

	VrProbe probe;
	if (!probe.init(app)) {
		LOGE("initialisation failed -- exiting");
		probe.shutdown();
		ANativeActivity_finish(app->activity);
	}

	while (!app->destroyRequested) {
		// Drain every pending Android event without blocking. A blocking
		// timeout here would never return once the queue went quiet, so the
		// XR event pump below would never run and the session would sit in
		// IDLE forever.
		for (;;) {
			int events = 0;
			android_poll_source* source = nullptr;
			if (ALooper_pollOnce(0, nullptr, &events,
			                     reinterpret_cast<void**>(&source)) < 0)
				break;
			if (source) source->process(app, source);
		}
		if (app->destroyRequested) break;

		probe.pollXrEvents();
		if (probe.exitRequested()) {
			ANativeActivity_finish(app->activity);
			usleep(20000);
			continue;
		}

		if (probe.sessionRunning()) {
			// xrWaitFrame paces us to the display refresh.
			probe.renderFrame();
		} else {
			// Not visible yet: idle politely instead of spinning.
			usleep(20000);
		}
	}

	probe.shutdown();
	app->activity->vm->DetachCurrentThread();
}

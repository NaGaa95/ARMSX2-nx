// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextEGLHorizon.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/Horizon/Horizon.h"
#include "common/Path.h"

#include "Config.h"

#include "nx2_platform.h"

#include <cstdlib>
#include <string>

GLContextEGLHorizon::GLContextEGLHorizon(const WindowInfo& wi)
	: GLContextEGL(wi)
{
}

GLContextEGLHorizon::~GLContextEGLHorizon()
{
	if (!m_owns_display || m_display == EGL_NO_DISPLAY)
		return;
	DestroySurface();
	DestroyContext();
	eglTerminate(m_display);
	m_display = EGL_NO_DISPLAY;
}

std::unique_ptr<GLContext> GLContextEGLHorizon::Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
	Error* error)
{
	std::unique_ptr<GLContextEGLHorizon> context = std::make_unique<GLContextEGLHorizon>(wi);
	// Set first: a failure after eglInitialize must still terminate the display (and free Mesa's device).
	context->m_owns_display = true;
	if (!context->Initialize(versions_to_try, error))
		return nullptr;
	return context;
}

std::unique_ptr<GLContext> GLContextEGLHorizon::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	// A second window surface cannot own the NWindow's buffers: shared contexts are surfaceless.
	WindowInfo surfaceless = wi;
	surfaceless.type = WindowInfo::Type::Surfaceless;
	surfaceless.window_handle = nullptr;
	std::unique_ptr<GLContextEGLHorizon> context = std::make_unique<GLContextEGLHorizon>(surfaceless);
	context->m_display = m_display;
	if (!context->CreateContextAndSurface(m_version, m_context, false))
	{
		Error::SetStringView(error, "Failed to create a shared context");
		return nullptr;
	}
	return context;
}

bool GLContextEGLHorizon::SetSwapInterval(s32 interval)
{
	const bool result = GLContextEGL::SetSwapInterval(interval);
	// Mesa sets its own interval on the window: nx2 puts the console's pacing back (120 Hz with VRR handheld).
	nx2DisplayApply(interval);
	return result;
}

EGLDisplay GLContextEGLHorizon::GetPlatformDisplay(Error* error)
{
	// The console's display is EGL's native platform only (Mesa's surfaceless platform refuses it).
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY)
	{
		const EGLint err = eglGetError();
		Error::SetStringFmt(error, "eglGetDisplay() failed: {} (0x{:X})", err, err);
	}
	return display;
}

EGLNativeWindowType GLContextEGLHorizon::GetNativeWindow(EGLConfig config)
{
	return static_cast<EGLNativeWindowType>(m_wi.window_handle ? m_wi.window_handle : nwindowGetDefault());
}

void GLContextEGLHorizon::FillWindowInfo(WindowInfo* wi)
{
	u32 width = 1920, height = 1080;
	nx2DisplaySize(&width, &height);
	wi->type = WindowInfo::Type::VI;
	wi->window_handle = nwindowGetDefault();
	wi->surface_width = width;
	wi->surface_height = height;
	wi->surface_refresh_rate = static_cast<float>(nx2DisplayRefreshRate());
	wi->surface_scale = 1.0f;
}

void GLContextEGLHorizon::PrepareDriver(bool zink)
{
	// Read at eglInitialize, once per display.
	setenv("MESA_SWITCH_GL_DRIVER", zink ? "zink" : "nvc0", 1);
	setenv("MESA_LOADER_DRIVER_OVERRIDE", zink ? "zink" : "nouveau", 1);
	const std::string cache = Path::Combine(EmuFolders::Cache, zink ? "mesa-zink" : "mesa-nvc0");
	setenv("MESA_SHADER_CACHE_DIR", cache.c_str(), 1);
	// The launcher ran Mesa earlier in this process: get a usable GPU device back.
	nx2MesaGpuRecover();
}

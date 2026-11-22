// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/OpenGL/GLContextEGL.h"

// mesa-nx's EGL on the Switch 2, linked in: the default display (its native platform) on the console's NWindow, with
// OpenGL on nvc0 or on Zink over NVK, chosen before eglInitialize (PrepareDriver).
class GLContextEGLHorizon final : public GLContextEGL
{
public:
	GLContextEGLHorizon(const WindowInfo& wi);
	~GLContextEGLHorizon() override;

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);
	std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;
	bool SetSwapInterval(s32 interval) override;

	// The window nx2DisplayInit set up (1920x1080 handheld, the TV's resolution docked).
	static void FillWindowInfo(WindowInfo* wi);
	// Before eglInitialize: picks nvc0 or Zink and its shader cache directory, and gets the GPU back from the launcher.
	static void PrepareDriver(bool zink);

protected:
	EGLDisplay GetPlatformDisplay(Error* error) override;
	EGLNativeWindowType GetNativeWindow(EGLConfig config) override;

private:
	// The context that initialized the display terminates it (the GPU is retired with the last user).
	bool m_owns_display = false;
};

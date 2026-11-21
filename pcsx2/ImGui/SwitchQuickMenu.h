// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

namespace SwitchQuickMenu
{
	bool IsOpen();
	void Toggle();
	bool CaptureInput(u64 held, u64 pressed);
	void OnVMStopped();
	void OnGSShutdown();
	void Render();
}

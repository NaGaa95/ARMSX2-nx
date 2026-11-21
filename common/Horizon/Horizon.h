// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#define u128 nx_u128
#define s128 nx_s128
#include <switch.h>
#undef u128
#undef s128

namespace Horizon
{
	u64 GetProcessCoreMask();
	// The core kept for explicit assignment (input polling, the GS back thread): core 3 on the Switch, where the
	// system shares it, and the last core of the process on the Switch 2 (six cores, mask 0x3f).
	u64 GetSpareCoreMask();
	// The cores general threads run on: every core of the process but the spare one.
	u64 GetApplicationCoreMask();
	void ReserveCore3ForCallingThread();
	bool PinCallingThreadToCore3();

	// Lazily brings up the BSD socket
	bool EnsureNetworkInitialized();

	// Init logging
	void BreadcrumbInit(const char* report_dir);
	void Breadcrumb(const char* message);
	void BreadcrumbShutdown();
	void ExitTrace(const char* message);

	void AuditHeapToExitTrace(const char* when);
} // namespace Horizon

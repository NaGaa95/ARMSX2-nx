// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The Switch 2 build's deko3d.h (put first on the include path by switch/app/CMakeLists.txt): drako3d, deko3d's API
// on the Switch 2's Ampere GPU, with the same dk names, so the DK3D renderer builds unchanged against it.
#pragma once

#if !defined(__SWITCH2__)
#error "switch/drako3d is only for the Switch 2 build"
#endif

#include <drako3d.h>

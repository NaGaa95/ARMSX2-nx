// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Host.h"
#include "common/SmallString.h"

#define TR_CONTEXT "FullscreenUI"

template <size_t L>
class IconStackString : public SmallStackString<L>
{
public:
	__fi IconStackString(const char* icon, const char* str)
	{
		SmallStackString<L>::format("{} {}", icon, Host::TranslateToStringView(TR_CONTEXT, str));
	}
	__fi IconStackString(const char8_t* icon, const char* str)
	{
		SmallStackString<L>::format("{} {}", reinterpret_cast<const char*>(icon), Host::TranslateToStringView(TR_CONTEXT, str));
	}
	__fi IconStackString(const char* icon, const char* str, const char* suffix)
	{
		SmallStackString<L>::format("{} {}##{}", icon, Host::TranslateToStringView(TR_CONTEXT, str), suffix);
	}
	__fi IconStackString(const char8_t* icon, const char* str, const char* suffix)
	{
		SmallStackString<L>::format("{} {}##{}", reinterpret_cast<const char*>(icon), Host::TranslateToStringView(TR_CONTEXT, str), suffix);
	}
};

#define FSUI_ICONSTR(icon, str) IconStackString<256>(icon, str).c_str()
#define FSUI_ICONSTR_S(icon, str, suffix) IconStackString<256>(icon, str, suffix).c_str()
#define FSUI_STR(str) Host::TranslateToString(TR_CONTEXT, str)
#define FSUI_CSTR(str) Host::TranslateToCString(TR_CONTEXT, str)
#define FSUI_VSTR(str) Host::TranslateToStringView(TR_CONTEXT, str)
#define FSUI_FSTR(str) fmt::runtime(Host::TranslateToStringView(TR_CONTEXT, str))
#define FSUI_NSTR(str) str

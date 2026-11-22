// SPDX-License-Identifier: GPL-3.0+

#include "switch/lsfg/SwitchLSFG.h"

#include "common/Console.h"
#include "pcsx2/Host.h"

#include "common/Horizon/Horizon.h"
#include "nx2_platform.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	std::atomic_bool s_prepared{false};
	std::atomic_bool s_installed{false};
	std::atomic_bool s_available{false};
	std::atomic_bool s_preparing{false};
	std::atomic_bool s_deko3d{false};
	// -1: the session's default (off), 0/1: the quick menu's choice.
	std::atomic_int s_session_override{-1};
	std::atomic_bool s_high_rate_passthrough{false};
	std::atomic_bool s_reset_session{false};
	std::atomic_bool s_in_game{false};
	// Bit 0 is fast forward; the other bits count its transitions.
	std::atomic_uint32_t s_speed_control_state{0};
	std::atomic<float> s_flow_scale{0.5f};
	std::atomic_bool s_performance{true};
	std::atomic_bool s_high_refresh{false};

	float FlowScale(float configured)
	{
		return configured >= 1.0f ? 1.0f : configured >= 0.5f ? 0.5f : 0.25f;
	}

	bool FileReadable(const char* path)
	{
		std::FILE* file = std::fopen(path, "rb");
		if (!file)
			return false;
		std::fclose(file);
		return true;
	}

	// lsfg-vk needs NVK_DEBUG=no_cbuf. NVK reads it at vkCreateInstance and applies it to every pipeline.
	bool EnableNvkNoCbuf()
	{
		const char* current = std::getenv("NVK_DEBUG");
		if (current && std::strstr(current, "no_cbuf"))
			return true;
		const std::string value = current && *current ? std::string(current) + ",no_cbuf" : "no_cbuf";
		return setenv("NVK_DEBUG", value.c_str(), 1) == 0;
	}

	bool Enabled()
	{
		return s_prepared.load(std::memory_order_acquire) && s_session_override.load(std::memory_order_acquire) == 1;
	}
} // namespace

void SwitchLSFG_Configure(bool deko3d)
{
	const bool requested = Host::GetBoolSettingValue("Switch", "FrameGeneration", false);
	s_flow_scale.store(FlowScale(Host::GetFloatSettingValue("Switch", "FrameGenerationFlowScale", 0.5f)), std::memory_order_release);
	s_performance.store(Host::GetBoolSettingValue("Switch", "FrameGenerationPerformance", true), std::memory_order_release);
	s_high_refresh.store(Host::GetBoolSettingValue("Switch", "FrameGeneration120Hz", false), std::memory_order_release);
	const bool installed = FileReadable(SWITCH_LSFG_DLL_PATH);
	s_installed.store(installed, std::memory_order_release);
	s_deko3d.store(deko3d, std::memory_order_release);
	const bool prepared = requested && installed && (deko3d || EnableNvkNoCbuf());
	s_prepared.store(prepared, std::memory_order_release);
	s_session_override.store(-1, std::memory_order_release);
	// The Vulkan core knows once its swapchain is attached; the Drako3D core once its shaders are prepared.
	s_available.store(deko3d && prepared, std::memory_order_release);
	s_preparing.store(false, std::memory_order_release);
	s_high_rate_passthrough.store(false, std::memory_order_release);
	s_reset_session.store(false, std::memory_order_release);
	s_speed_control_state.store(0, std::memory_order_release);
	if (requested && !installed)
		WARNING_LOG("LSFG: {} is missing, frame generation stays off", SWITCH_LSFG_DLL_PATH);
	else if (prepared)
		INFO_LOG("LSFG: prepared ({}, flow {}, {})", deko3d ? "Drako3D" : "Vulkan", s_flow_scale.load(),
			s_performance.load() ? "performance" : "quality");
}

void SwitchLSFG_SetGameState(bool in_game, bool fast_forward)
{
	s_in_game.store(in_game, std::memory_order_release);
	uint32_t state = s_speed_control_state.load(std::memory_order_relaxed);
	while (((state & 1) != 0) != fast_forward)
	{
		const uint32_t next = ((state + 2) & ~1u) | (fast_forward ? 1u : 0u);
		if (s_speed_control_state.compare_exchange_weak(state, next, std::memory_order_release, std::memory_order_relaxed))
			break;
	}
}

SwitchLSFGState SwitchLSFG_PollState()
{
	SwitchLSFGState state;
	state.enabled = Enabled();
	state.speed_control_state = s_speed_control_state.load(std::memory_order_acquire);
	state.active = state.enabled && s_in_game.load(std::memory_order_acquire) && !(state.speed_control_state & 1);
	state.reset_session = s_reset_session.exchange(false, std::memory_order_acq_rel);
	state.flow_scale = s_flow_scale.load(std::memory_order_acquire);
	state.performance = s_performance.load(std::memory_order_acquire);
	state.high_refresh = s_high_refresh.load(std::memory_order_acquire);
	return state;
}

void SwitchLSFG_SetStatus(bool available, bool high_rate, bool preparing)
{
	s_available.store(available && s_prepared.load(std::memory_order_acquire), std::memory_order_release);
	s_high_rate_passthrough.store(available && high_rate, std::memory_order_release);
	s_preparing.store(preparing, std::memory_order_release);
	// Frame generation that cannot run is off again, so the quick menu does not show it on.
	if (!available && s_session_override.load(std::memory_order_acquire) == 1)
		s_session_override.store(0, std::memory_order_release);
}

bool SwitchLSFG_IsPrepared()
{
	return s_prepared.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsInstalled()
{
	return s_installed.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsAvailable()
{
	return s_available.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsPreparing()
{
	return s_preparing.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsEnabled()
{
	return Enabled();
}

bool SwitchLSFG_IsHighRatePassthrough()
{
	return Enabled() && s_high_rate_passthrough.load(std::memory_order_acquire);
}

bool SwitchLSFG_DoublesSourceRate(float source_fps)
{
	if (source_fps >= 27.0f && source_fps <= 33.5f)
		return true;
	// A 50/60 FPS game at 120 Hz: handheld, or docked with the 120 Hz Docked Mode option.
	if (source_fps >= 45.0f && source_fps <= 62.0f)
		return nx2DisplayRefreshRate() >= 120 || s_high_refresh.load(std::memory_order_acquire);
	return false;
}

bool SwitchLSFG_RequestEnabled(bool enabled, float source_fps)
{
	if (enabled && (!s_available.load(std::memory_order_acquire) || !SwitchLSFG_DoublesSourceRate(source_fps)))
		return false;
	s_session_override.store(enabled ? 1 : 0, std::memory_order_release);
	s_reset_session.store(true, std::memory_order_release);
	return true;
}

void SwitchLSFG_ResetSession()
{
	s_session_override.store(-1, std::memory_order_release);
	s_reset_session.store(true, std::memory_order_release);
}

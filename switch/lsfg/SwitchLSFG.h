// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>

// LSFG 2x frame generation on the Switch 2 cores: lsfg-vk behind lsfg_bridge on the Vulkan core (SwitchLSFGVulkan.h),
// lsfg-deko3d on the Drako3D core (deko3d/). The shaders come from the player's Lossless.dll at run time. The launcher
// prepares it ([Switch] FrameGeneration...); each game starts with it off and the quick menu turns it on.

// The DLL and the caches, on the SD card (newlib's default device).
inline constexpr const char* SWITCH_LSFG_DLL_PATH = "/switch/armsx2-nx/lsfg/Lossless.dll";
inline constexpr const char* SWITCH_LSFG_VK_PIPELINE_CACHE = "/switch/armsx2-nx/cache/lsfg-vk-pipeline-cache.bin";
inline constexpr const char* SWITCH_LSFG_DEKO_CACHE_ROOT = "/switch/armsx2-nx/cache/drako3d/lsfg-1";

// Reads the launcher's settings and checks for the DLL. The Vulkan core calls it before it creates its Vulkan instance.
void SwitchLSFG_Configure(bool deko3d);

// The game's state at each present (GS thread): running with the quick menu closed, and in fast forward.
void SwitchLSFG_SetGameState(bool in_game, bool fast_forward);

// What the renderer reads at each present.
struct SwitchLSFGState
{
	bool enabled = false; // prepared and on for this session
	bool active = false; // enabled, in game and not in fast forward
	bool reset_session = false; // a new session started: drop the history and the source measurement
	uint32_t speed_control_state = 0; // changes with every fast-forward transition
	float flow_scale = 0.5f; // the optical-flow resolution: 0.25, 0.5 or 1
	bool performance = true;
	bool high_refresh = false; // the 120 Hz Docked Mode option
};
SwitchLSFGState SwitchLSFG_PollState();

// The renderer's verdict: whether it can generate (setup, shaders), whether the game runs at the display's rate on its
// own (native-rate passthrough), and whether the Drako3D shaders are still being prepared.
void SwitchLSFG_SetStatus(bool available, bool high_rate, bool preparing = false);

bool SwitchLSFG_IsPrepared();
bool SwitchLSFG_IsInstalled();
bool SwitchLSFG_IsAvailable();
bool SwitchLSFG_IsPreparing();
bool SwitchLSFG_IsEnabled();
bool SwitchLSFG_IsHighRatePassthrough();
// Whether frame generation doubles a game at source_fps: 30 FPS, and 50/60 FPS at 120 Hz.
bool SwitchLSFG_DoublesSourceRate(float source_fps);
// The quick menu's switch. False when it cannot be turned on (unavailable, or a rate it does not double).
bool SwitchLSFG_RequestEnabled(bool enabled, float source_fps);
// A game started or ended: frame generation is off again.
void SwitchLSFG_ResetSession();

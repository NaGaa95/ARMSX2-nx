// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Horizon/Horizon.h"
#include "common/Horizon/HorizonFastmem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/SwitchQuickMenu.h"
#include "switch/SwitchSettings.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"

#include "HorizonException.h"
#include "HorizonHost.h"
#include "HorizonThreadWrap.h"
#ifdef YADOKARI_USB_STORAGE
#include "HorizonUsbStorage.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

#ifdef __SWITCH2__
#include "nx2_gameshare.h"
#include "nx2_platform.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"

// The core ends with _Exit: no static destructors or atexit handlers beside threads nobody joined. drako3d releases its
// GPU client only in an atexit handler, which the launcher's Mesa needs: -Wl,--wrap=atexit records the one nvLibInit
// (the branch target of dk::detail::nvdev::initialize) registers, and the exit runs it.
extern "C" int __real_atexit(void (*function)(void));
extern "C" int __wrap_atexit(void (*function)(void));
extern "C" void ArmsxDrako3dNvInitialize(void) __asm__("_ZN2dk6detail5nvdev10initializeEv") __attribute__((weak));

namespace
{
	void (*s_drako3d_atexit)(void);

	bool IsDrako3dGpuRegistration(uintptr_t caller)
	{
		const uintptr_t entry = reinterpret_cast<uintptr_t>(&ArmsxDrako3dNvInitialize);
		if (!entry)
			return false;
		const uint32_t instruction = *reinterpret_cast<const uint32_t*>(entry);
		if ((instruction & 0xFC000000u) != 0x14000000u) // b <nvLibInit>
			return false;
		int32_t offset = static_cast<int32_t>(instruction & 0x03FFFFFFu);
		if (offset & 0x02000000)
			offset -= 0x04000000;
		const uintptr_t nv_lib_init = entry + static_cast<intptr_t>(offset) * 4;
		return caller >= nv_lib_init && caller - nv_lib_init < 0x800; // (nvLibInit is 0x660 bytes, its atexit at +0x4ac)
	}
} // namespace

extern "C" int __wrap_atexit(void (*function)(void))
{
	if (IsDrako3dGpuRegistration(reinterpret_cast<uintptr_t>(__builtin_return_address(0))))
		s_drako3d_atexit = function;
	return __real_atexit(function);
}
#endif

namespace
{
	// ARMSX2-nx: the launcher (an SDL program of its own) picks the game and writes the settings, then chainloads the
	// core of the renderer it was asked for with a handoff file naming the game and the launcher to return to.
	constexpr const char* YADOKARI_ROOT = "sdmc:/switch/armsx2-nx";
	constexpr const char* LOGS_DIR = "sdmc:/switch/armsx2-nx/logs";
	constexpr const char* SETTINGS_PATH = "sdmc:/switch/armsx2-nx/inis/PCSX2.ini";
	constexpr const char* LAUNCH_HANDOFF_PATH = "sdmc:/switch/armsx2-nx/launch.ini";
	// Why the game did not start, for the launcher to show (this core has no game list or landing page of its own).
	constexpr const char* LAUNCH_ERROR_PATH = "sdmc:/switch/armsx2-nx/launch_error.txt";
	constexpr u64 IDLE_POLL_NS = 8'000'000ULL;

	constexpr u64 INPUT_POLL_NS = 16'000'000ULL;
	constexpr float STICK_DEADZONE = 0.15f;
	constexpr u32 NUM_LOCAL_PLAYERS = 2;
	std::string s_launch_image;
	std::unique_ptr<INISettingsInterface> s_launcher_game_settings;
	std::string s_return_nro;

	std::unique_ptr<INISettingsInterface> s_settings_interface;
	// Credentials (RetroAchievements' token, which PCSX2 moves out of the base settings into this layer).
	std::unique_ptr<INISettingsInterface> s_secrets_settings_interface;
	std::atomic_bool s_applet_backgrounded{false};
	std::atomic_bool s_applet_resumed{false};
	bool s_paused_for_applet = false;

	void AppletHookCallback(AppletHookType hook, void*)
	{
		switch (hook)
		{
			case AppletHookType_OnExitRequest:
				HorizonHost::RequestExit();
				break;

			case AppletHookType_OnFocusState:
				if (appletGetFocusState() == AppletFocusState_Background)
					s_applet_backgrounded.store(true, std::memory_order_release);
				break;

			case AppletHookType_OnResume:
				s_applet_resumed.store(true, std::memory_order_release);
				break;

			default:
				break;
		}
	}

	void ProcessAppletLifecycle()
	{
		if (s_applet_backgrounded.exchange(false, std::memory_order_acq_rel))
		{
			if (VMManager::GetState() == VMState::Running)
			{
				VMManager::SetPaused(true);
				s_paused_for_applet = true;
			}
		}

		if (s_applet_resumed.exchange(false, std::memory_order_acq_rel) && s_paused_for_applet)
		{
			if (VMManager::GetState() == VMState::Paused)
				VMManager::SetPaused(false);
			s_paused_for_applet = false;
		}
	}

	struct IniEntry
	{
		std::string section;
		std::string key;
		std::string value;
	};

	// Every "key = value" of an INI file with its section, in file order.
	std::vector<IniEntry> ReadIniEntries(const char* path)
	{
		std::vector<IniEntry> entries;
		const std::optional<std::string> contents = FileSystem::ReadFileToString(path);
		if (!contents.has_value())
			return entries;

		std::string section;
		std::string_view rest = contents.value();
		while (!rest.empty())
		{
			const size_t eol = rest.find('\n');
			std::string_view line = rest.substr(0, eol);
			rest = (eol == std::string_view::npos) ? std::string_view() : rest.substr(eol + 1);
			line = StringUtil::StripWhitespace(line);
			if (line.empty() || line.front() == ';' || line.front() == '#')
				continue;
			if (line.front() == '[' && line.back() == ']')
			{
				section = std::string(StringUtil::StripWhitespace(line.substr(1, line.size() - 2)));
				continue;
			}
			const size_t equals = line.find('=');
			if (equals == std::string_view::npos || section.empty())
				continue;
			entries.push_back({section, std::string(StringUtil::StripWhitespace(line.substr(0, equals))),
				std::string(StringUtil::StripWhitespace(line.substr(equals + 1)))});
		}
		return entries;
	}

	bool InitializeSettings()
	{
		s_settings_interface = std::make_unique<INISettingsInterface>(SETTINGS_PATH);
		Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());

		if (!s_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
		{
			INFO_LOG("Initializing Switch settings at {}", SETTINGS_PATH);
			// The launcher may have written its settings before this core ever set the defaults: they win over them.
			const std::vector<IniEntry> launcher_entries = ReadIniEntries(SETTINGS_PATH);
			VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);
			s_settings_interface->SetStringValue("Pad2", "Type", "DualShock2");
			for (const IniEntry& entry : launcher_entries)
			{
				if (entry.section != "UI" || entry.key != "SettingsVersion")
					s_settings_interface->SetStringValue(entry.section.c_str(), entry.key.c_str(), entry.value.c_str());
			}
			if (!launcher_entries.empty())
				INFO_LOG("Kept {} settings the launcher wrote", launcher_entries.size());
		}

		// Each core has one GPU driver: the other core's renderer falls back to this core's (Software and Auto stay).
		{
#if defined(ARMSX2_DK3D)
			constexpr int native_renderer = static_cast<int>(GSRendererType::DK3D);
			constexpr int other_native_renderer = native_renderer;
#elif defined(ENABLE_VULKAN)
			constexpr int native_renderer = static_cast<int>(GSRendererType::VK);
			constexpr int other_native_renderer = native_renderer;
#else
			constexpr int native_renderer = static_cast<int>(GSRendererType::OGL);
			constexpr int other_native_renderer = static_cast<int>(GSRendererType::Zink);
#endif
			const int renderer = Host::GetIntSettingValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::Auto));
			if (renderer != native_renderer && renderer != other_native_renderer &&
				renderer != static_cast<int>(GSRendererType::Auto) && renderer != static_cast<int>(GSRendererType::SW))
			{
				WARNING_LOG("Renderer {} is not in this core: using {}", renderer, native_renderer);
				s_settings_interface->SetIntValue("EmuCore/GS", "Renderer", native_renderer);
			}
		}

		Error error;
		if (!s_settings_interface->Save(&error))
		{
			ERROR_LOG("Failed to save settings: {}", error.GetDescription());
			return false;
		}

		const std::string secrets_path = Path::Combine(EmuFolders::Settings, "secrets.ini");
		s_secrets_settings_interface = std::make_unique<INISettingsInterface>(secrets_path);
		Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface.get());
		if (FileSystem::FileExists(secrets_path.c_str()))
			s_secrets_settings_interface->Load();

		VMManager::Internal::LoadStartupSettings();
		return true;
	}

#ifdef YADOKARI_USB_STORAGE
	void LogUsbVolumes()
	{
		const std::vector<HorizonUsbStorage::Volume> volumes = HorizonUsbStorage::GetVolumes();
		if (volumes.empty())
		{
			INFO_LOG("USB storage disconnected");
			return;
		}

		for (const HorizonUsbStorage::Volume& volume : volumes)
			INFO_LOG("USB volume '{}' mounted at {} ({})", volume.label, volume.root, volume.filesystem);
	}
#endif

	// The launcher's handoff: [Launch] Path (the game) and Launcher (the NRO to chainload when it ends). Read once,
	// then removed, so that starting the core again on its own does not boot the same game.
	void ReadLaunchHandoff()
	{
		if (!FileSystem::FileExists(LAUNCH_HANDOFF_PATH))
			return;

		auto handoff = std::make_unique<INISettingsInterface>(LAUNCH_HANDOFF_PATH);
		if (handoff->Load())
		{
			s_launch_image = handoff->GetStringValue("Launch", "Path", "");
			s_return_nro = handoff->GetStringValue("Launch", "Launcher", "");
#ifdef __SWITCH2__
			// Minus on the game in the launcher: it starts with a GameShare session
			nx2GameShareConfigure(handoff->GetBoolValue("Launch", "GameShare", false), false);
#endif
			s_launcher_game_settings = std::move(handoff);
			auto lock = Host::GetSettingsLock();
			Host::Internal::SetSwitchLaunchSettingsLayer(s_launcher_game_settings.get(), lock);
		}
		FileSystem::DeleteFilePath(LAUNCH_HANDOFF_PATH);
		INFO_LOG("Launcher handoff: game '{}', return to '{}'", s_launch_image, s_return_nro);
	}

	// Chainloads the launcher when this program ends (the homebrew loader runs it next in this process).
	void ReturnToLauncher()
	{
		struct stat info = {};
		if (s_return_nro.empty() || !envHasNextLoad() || stat(s_return_nro.c_str(), &info) != 0)
			return;
		INFO_LOG("Returning to the launcher: {}", s_return_nro);
		envSetNextLoad(s_return_nro.c_str(), s_return_nro.c_str());
	}

	// The launcher runs next in this process. On the Switch 2 the exit cleanup stops the threads nobody joined and
	// releases the HOME lock, then drako3d's GPU client goes, and _Exit skips the static destructors (see
	// __wrap_atexit); the Switch releases the HOME lock and ends as main returning would.
	[[noreturn]] void ExitCore(int status)
	{
		Horizon::ShutdownNetwork();
		// (_Exit skips the atexit handler that closes crash.txt and boot.txt)
		HorizonException::Shutdown();
#ifdef __SWITCH2__
		nx2ExitReleaseLockLast();
		nx2ExitCleanupNow();
		if (void (*release)(void) = std::exchange(s_drako3d_atexit, nullptr))
			release();
		std::_Exit(status);
#else
		appletUnlockExit();
		std::exit(status);
#endif
	}

	// A core that cannot start: back to the launcher, which shows the reason.
	[[noreturn]] void ExitCoreWithError(const char* reason)
	{
		ERROR_LOG("{}", reason);
		FileSystem::WriteStringToFile(LAUNCH_ERROR_PATH, reason);
		ReturnToLauncher();
		ExitCore(1);
	}

	// The game cannot start: back to the launcher, which shows the reason.
	void ReportLaunchError(const std::string& message)
	{
		ERROR_LOG("Launch failed: {}", message);
		if (!FileSystem::WriteStringToFile(LAUNCH_ERROR_PATH, message))
			ERROR_LOG("Could not write {}", LAUNCH_ERROR_PATH);
		HorizonHost::RequestExit();
	}

#ifdef __SWITCH2__
	// GameShare's guest screen, made and destroyed on the GS thread (GSDevice::OpenGuestScreen)
	bool OpenGameShareScreen(void* window, int, int, void*)
	{
		bool opened = false;
		MTGS::RunOnGSThread([window, &opened]() { opened = g_gs_device && g_gs_device->OpenGuestScreen(window); });
		MTGS::WaitGS(false);
		return opened;
	}

	void CloseGameShareScreen(void*)
	{
		// (a GS device that already went closed it)
		if (!MTGS::IsOpen())
			return;
		MTGS::RunOnGSThread([]() {
			if (g_gs_device)
				g_gs_device->CloseGuestScreen();
		});
		MTGS::WaitGS(false);
	}

	// The VM is up and paused: the GameShare session the launcher asked for starts before the game runs. The guest
	// gets a screen of its own when the renderer can present to its window (Drako3D, Vulkan), else it mirrors this
	// console (OpenGL). False when the game must not run: matchmaking was cancelled (back to the launcher) or the
	// session failed (the launcher shows why).
	bool StartGameShare()
	{
		if (!nx2GameShareRequested() || nx2GameShareActive())
			return true;
		nx2GameShareSetSwapRoles(Host::GetBoolSettingValue("Switch", "GameShareSwap", false));
		static const Nx2GameShareScreen screen = {OpenGameShareScreen, CloseGameShareScreen, nullptr};
		const bool separate = g_gs_device && g_gs_device->SupportsGuestScreen();
		char error[256] = {};
		switch (nx2GameShareBegin(separate ? &screen : nullptr, error, sizeof(error)))
		{
			case Nx2GameShare_Started:
				INFO_LOG("GameShare: started ({}), the guest is player {}", separate ? "guest screen" : "mirrored",
					nx2GameShareSwapRoles() ? 1 : 2);
				return true;
			case Nx2GameShare_Cancelled:
				INFO_LOG("GameShare: matchmaking cancelled");
				return false;
			default:
				ReportLaunchError(fmt::format("GameShare could not start:\n{}", error));
				return false;
		}
	}
#endif

	void ReportInitialBootFailure(const VMBootResult result, const Error& error)
	{
		const std::string message = error.IsValid() ? error.GetDescription() : "The virtual machine could not be started.";
		ERROR_LOG("Failed to boot launch image: {} (result {})", message, static_cast<int>(result));

		ReportLaunchError(message);
	}

	bool BootInitialImage(const char* image_path)
	{
		if (!image_path || image_path[0] == '\0')
			return true;

		if (!FileSystem::FileExists(image_path))
		{
			ERROR_LOG("Launch image not found: {}", image_path);
			ReportLaunchError(fmt::format("The game file was not found:\n{}", image_path));
			return false;
		}

		VMBootParameters boot_params;
		boot_params.filename = image_path;
		auto hardcore_disable_callback = [](std::string reason, [[maybe_unused]] VMBootRestartCallback restart_callback) {
			ReportLaunchError(fmt::format("This game cannot start in RetroAchievements hardcore mode:\n{}\n"
										  "Turn hardcore mode off in the launcher's RetroAchievements settings.", reason));
		};
		auto done_callback = [](VMBootResult result, const Error& error) {
#ifdef __SWITCH2__
			if (result == VMBootResult::StartupSuccess)
				VMManager::SetState(StartGameShare() ? VMState::Running : VMState::Stopping);
#else
			if (result == VMBootResult::StartupSuccess)
				VMManager::SetState(VMState::Running);
#endif
			else
				ReportInitialBootFailure(result, error);
		};
		VMManager::InitializeAsync(boot_params, std::move(hardcore_disable_callback), std::move(done_callback));
		return true;
	}

	void RunMainLoop()
	{
		while (appletMainLoop())
		{
			Host::PumpMessagesOnCPUThread();
			ProcessAppletLifecycle();
#ifdef YADOKARI_USB_STORAGE
			if (HorizonUsbStorage::ConsumeChange())
				LogUsbVolumes();
#endif

			switch (VMManager::GetState())
			{
				case VMState::Running:
					VMManager::Execute();
					break;

				case VMState::Resetting:
					VMManager::Reset();
					break;

				case VMState::Stopping:
					VMManager::Shutdown(HorizonHost::TakeResumeSaveRequest());
					break;

				case VMState::Paused:
				case VMState::Shutdown:
					VMManager::IdlePollUpdate();
					svcSleepThread(IDLE_POLL_NS);
					break;

				case VMState::Initializing:
					break;
			}

			if (HorizonHost::IsExitRequested() && VMManager::GetState() == VMState::Shutdown)
				break;
		}

		HorizonHost::RequestExit();
	}

	constexpr PadDualshock2::Inputs PS2_BUTTONS[] = {
		PadDualshock2::Inputs::PAD_PRESSURE,
		PadDualshock2::Inputs::PAD_UP, PadDualshock2::Inputs::PAD_DOWN,
		PadDualshock2::Inputs::PAD_LEFT, PadDualshock2::Inputs::PAD_RIGHT,
		PadDualshock2::Inputs::PAD_TRIANGLE, PadDualshock2::Inputs::PAD_CIRCLE,
		PadDualshock2::Inputs::PAD_CROSS, PadDualshock2::Inputs::PAD_SQUARE,
		PadDualshock2::Inputs::PAD_SELECT, PadDualshock2::Inputs::PAD_START,
		PadDualshock2::Inputs::PAD_L1, PadDualshock2::Inputs::PAD_R1,
		PadDualshock2::Inputs::PAD_L2, PadDualshock2::Inputs::PAD_R2,
		PadDualshock2::Inputs::PAD_L3, PadDualshock2::Inputs::PAD_R3,
	};
	constexpr u64 SWITCH_BUTTONS[] = {
		0, HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y, HidNpadButton_L, HidNpadButton_R,
		HidNpadButton_ZL, HidNpadButton_ZR, HidNpadButton_StickL, HidNpadButton_StickR, HidNpadButton_Plus,
		HidNpadButton_Minus, HidNpadButton_Up, HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right,
	};
	static_assert(std::size(PS2_BUTTONS) == std::size(SwitchSettings::PAD_BINDINGS));
	static_assert(std::size(SWITCH_BUTTONS) == std::size(SwitchSettings::C_npad));
	std::array<std::array<std::atomic<u64>, std::size(PS2_BUTTONS)>, NUM_LOCAL_PLAYERS> s_pad_buttons{};
	std::array<std::atomic<u32>, NUM_LOCAL_PLAYERS> s_pad_vibration{};

	struct RumbleDevice
	{
		HidNpadIdType id = HidNpadIdType_No1;
		u32 style = 0;
		std::array<HidVibrationDeviceHandle, 2> handles{};
		s32 count = 0;
		u32 last = 0;
	};

	void SendRumble(RumbleDevice& device, u32 strength)
	{
		if (device.count == 0 || device.last == strength)
			return;
		const HidVibrationValue value{(strength & 0xFFFF) / 65535.0f, 160.0f, (strength >> 16) / 65535.0f, 320.0f};
		for (s32 i = 0; i < device.count; ++i)
			hidSendVibrationValue(device.handles[i], &value);
		device.last = strength;
	}

	// The console's controller slot (pad) rumbles at strength (low motor in the low 16 bits, high in the high ones)
	void PollPadRumble(RumbleDevice& device, const PadState& pad, u32 slot, u32 strength)
	{
		const HidNpadIdType id = padIsNpadActive(&pad, HidNpadIdType_Handheld) ?
			HidNpadIdType_Handheld : static_cast<HidNpadIdType>(HidNpadIdType_No1 + slot);
		const u32 style = padIsNpadActive(&pad, id) ? hidGetNpadStyleSet(id) : 0;
		if (device.id != id || device.style != style)
		{
			SendRumble(device, 0);
			device = {.id = id, .style = style};
			if (style != 0)
			{
				const s32 count = (style & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual)) ? 2 : 1;
				if (R_SUCCEEDED(hidInitializeVibrationDevices(device.handles.data(), count, id, static_cast<HidNpadStyleTag>(style))))
					device.count = count;
			}
		}
		SendRumble(device, strength);
	}

	// GameShare: the guest's controller drives its player (controller 2, or controller 1 when the roles are swapped)
	// and the console's controllers the other one in order
	struct PlayerSource
	{
		bool guest;
		u32 slot;
	};

	PlayerSource GetPlayerSource(u32 player)
	{
#ifdef __SWITCH2__
		if (!nx2GameShareActive())
			return {false, player};
		const u32 guest_player = nx2GameShareSwapRoles() ? 0 : 1;
		if (player == guest_player)
			return {true, 0};
		return {false, player < guest_player ? player : player - 1};
#else
		return {false, player};
#endif
	}

	std::array<PadState, NUM_LOCAL_PLAYERS> s_pads;
	std::thread s_input_thread;
	std::atomic_bool s_input_stop{false};

	float ApplyDeadzone(s32 raw)
	{
		const float v = std::clamp(static_cast<float>(raw) / static_cast<float>(JOYSTICK_MAX), -1.0f, 1.0f);
		const float mag = std::fabs(v);
		if (mag < STICK_DEADZONE)
			return 0.0f;
		const float scaled = (mag - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
		return (v < 0.0f) ? -scaled : scaled;
	}

	void ApplyStick(u32 player, const HidAnalogStickState& s, PadDualshock2::Inputs left, PadDualshock2::Inputs right,
		PadDualshock2::Inputs up, PadDualshock2::Inputs down)
	{
		const float x = ApplyDeadzone(s.x);
		const float y = ApplyDeadzone(s.y);
		Pad::SetControllerState(player, static_cast<u32>(right), x > 0.0f ? x : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(left), x < 0.0f ? -x : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(up), y > 0.0f ? y : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(down), y < 0.0f ? -y : 0.0f);
	}

	void FeedGamePad(u32 player, u64 held, HidAnalogStickState left, HidAnalogStickState right, bool captured = false)
	{
		if (captured)
		{
			held = 0;
			left = right = {};
		}
		for (size_t i = 0; i < std::size(PS2_BUTTONS); ++i)
		{
			const u64 button = s_pad_buttons[player][i].load(std::memory_order_relaxed);
			Pad::SetControllerState(player, static_cast<u32>(PS2_BUTTONS[i]), (held & button) ? 1.0f : 0.0f);
		}

		ApplyStick(player, left, PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_RIGHT,
			PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN);
		ApplyStick(player, right, PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_RIGHT,
			PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_DOWN);
	}

	void FeedGamePad(u32 player, PadState& pad, u64 held, bool captured = false)
	{
		FeedGamePad(player, held, padGetStickPos(&pad, 0), padGetStickPos(&pad, 1), captured);
	}

	void InputPollLoop()
	{
		Threading::SetNameOfCurrentThread("Input Poll");
		if (!Horizon::PinCallingThreadToCore3())
			Horizon::ReserveCore3ForCallingThread();
		bool was_captured = false;
		std::array<RumbleDevice, NUM_LOCAL_PLAYERS> rumble{};
#ifdef __SWITCH2__
		u64 guest_held_before = 0;
		u32 guest_rumble = 0;
#endif
		while (!s_input_stop.load(std::memory_order_relaxed))
		{
			if (!hidGetSharedmemAddr())
				return;

			std::array<u64, NUM_LOCAL_PLAYERS> held{};
			for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
			{
				padUpdate(&s_pads[player]);
				held[player] = padGetButtons(&s_pads[player]);
			}

#ifdef __SWITCH2__
			// (GameShare's guest opens and uses the quick menu too)
			Nx2GameShareInput guest{};
			const u64 guest_held = nx2GameShareGetGuestInput(&guest) ? guest.buttons : 0;
			const u64 guest_down = guest_held & ~guest_held_before;
			guest_held_before = guest_held;
#else
			constexpr u64 guest_held = 0, guest_down = 0;
#endif
			const bool captured =
				SwitchQuickMenu::CaptureInput(held[0] | guest_held, padGetButtonsDown(&s_pads[0]) | guest_down);
			const VMState state = VMManager::GetState();
			if ((!captured && state == VMState::Running) || (captured && !was_captured && VMManager::HasValidVM()))
			{
				for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
				{
					const PlayerSource source = GetPlayerSource(player);
#ifdef __SWITCH2__
					if (source.guest)
						FeedGamePad(player, guest_held, {guest.stick[0], guest.stick[1]}, {guest.stick[2], guest.stick[3]},
							captured);
					else
#endif
						FeedGamePad(player, s_pads[source.slot], held[source.slot], captured);
				}
			}
			std::array<u32, NUM_LOCAL_PLAYERS> slot_rumble{};
			for (u32 player = 0; player < NUM_LOCAL_PLAYERS; ++player)
			{
				const u32 strength =
					captured || state != VMState::Running ? 0 : s_pad_vibration[player].load(std::memory_order_relaxed);
				const PlayerSource source = GetPlayerSource(player);
				if (!source.guest)
					slot_rumble[source.slot] = strength;
#ifdef __SWITCH2__
				else if (strength != guest_rumble)
				{
					nx2GameShareSetGuestRumble((strength & 0xFFFF) / 65535.0f, (strength >> 16) / 65535.0f);
					guest_rumble = strength;
				}
#endif
			}
			for (u32 slot = 0; slot < NUM_LOCAL_PLAYERS; ++slot)
				PollPadRumble(rumble[slot], s_pads[slot], slot, slot_rumble[slot]);
			was_captured = captured;

			svcSleepThread(INPUT_POLL_NS);
		}
		for (auto& device : rumble)
			SendRumble(device, 0);
#ifdef __SWITCH2__
		if (guest_rumble)
			nx2GameShareSetGuestRumble(0.0f, 0.0f);
#endif
	}

	void StartInputPolling()
	{
		padConfigureInput(NUM_LOCAL_PLAYERS, HidNpadStyleSet_NpadStandard);
		padInitializeDefault(&s_pads[0]);
		padInitialize(&s_pads[1], HidNpadIdType_No2);

		s_input_stop.store(false, std::memory_order_relaxed);
		s_input_thread = std::thread(InputPollLoop);
	}

	void StopInputPolling()
	{
		if (!s_input_thread.joinable())
			return;
		s_input_stop.store(true, std::memory_order_relaxed);
		s_input_thread.join();
	}
} // namespace

void HorizonHost::LoadPadBindings(const SettingsInterface& settings)
{
	for (u32 player = 0; player < NUM_LOCAL_PLAYERS; ++player)
	{
		const std::string section = Pad::GetConfigSection(player);
		for (size_t i = 0; i < std::size(SwitchSettings::PAD_BINDINGS); ++i)
		{
			const auto& binding = SwitchSettings::PAD_BINDINGS[i];
			const std::string_view path(binding.keys[player]);
			const std::string key(path.substr(path.rfind('/') + 1));
			const std::string name = settings.GetStringValue(section.c_str(), key.c_str(), binding.default_button);
			auto button = std::find_if(std::begin(SwitchSettings::C_npad), std::end(SwitchSettings::C_npad),
				[&](const auto& choice) { return name == choice.val; });
			if (button == std::end(SwitchSettings::C_npad))
				button = std::find_if(std::begin(SwitchSettings::C_npad), std::end(SwitchSettings::C_npad),
					[&](const auto& choice) { return std::string_view(binding.default_button) == choice.val; });
			s_pad_buttons[player][i].store(SWITCH_BUTTONS[button - std::begin(SwitchSettings::C_npad)], std::memory_order_relaxed);
		}
	}
}

void HorizonHost::SetPadVibration(unsigned player, float low, float high)
{
	if (player >= s_pad_vibration.size())
		return;
	const auto strength = [](float value) { return static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f)); };
	s_pad_vibration[player].store(strength(low) | (strength(high) << 16), std::memory_order_relaxed);
}

void Host::CommitBaseSettingChanges()
{
	if (!s_settings_interface)
		return;

	Error error;
	if (!s_settings_interface->Save(&error))
		ERROR_LOG("Failed to save settings: {}", error.GetDescription());
}

int main(int argc, char* argv[])
{
#ifdef __SWITCH2__
	// 1920x1080 at 120 Hz handheld, the TV's resolution docked (switch/nx2/nx2_platform.h).
	nx2DisplayInit();
#endif
	Horizon::ReserveCore3ForCallingThread();
	appletLockExit();

	mkdir("sdmc:/switch", 0777);
	mkdir(YADOKARI_ROOT, 0777);
	mkdir(LOGS_DIR, 0777);
	mkdir("sdmc:/switch/armsx2-nx/inis", 0777);

	HorizonException::Initialize(LOGS_DIR);
	Horizon::ExitTrace("=== main entered");
	Horizon::AuditHeapToExitTrace("startup");

	Log::SetTimestampsEnabled(true);
	// The previous run's log stays as emulog.prev.txt (on the Switch 2 a crash leaves no report, only its log).
	{
		const std::string log_path = Path::Combine(LOGS_DIR, "emulog.txt");
		const std::string prev_path = Path::Combine(LOGS_DIR, "emulog.prev.txt");
		std::remove(prev_path.c_str());
		std::rename(log_path.c_str(), prev_path.c_str());
	}
	// INFO, as the launcher's EnableVerbose=false asks (SetFileLogPath opens it at DEBUG).
	Log::SetFileOutputLevel(LOGLEVEL_INFO, Path::Combine(LOGS_DIR, "emulog.txt"));
	ReadLaunchHandoff();
	HorizonHost::SetExitWhenVMEnds(!s_launch_image.empty());

	if (HorizonFastmem::IsSupported())
		INFO_LOG("Horizon fastmem available: {}", HorizonFastmem::GetSupportReason());
	else
		INFO_LOG("Horizon fastmem unavailable: {}", HorizonFastmem::GetSupportReason());
	HorizonException::Breadcrumb("fastmem checked");

	const bool romfs_available = R_SUCCEEDED(romfsInit());
	HorizonException::Breadcrumb(romfs_available ? "romfsInit ok" : "romfsInit failed");
	EmuFolders::AppRoot = YADOKARI_ROOT;
	EmuFolders::DataRoot = YADOKARI_ROOT;
	EmuFolders::Settings = Path::Combine(YADOKARI_ROOT, "inis");
	if (romfs_available)
		EmuFolders::Resources = "romfs:/resources";
	else if (!EmuFolders::SetResourcesDirectory())
	{
		ExitCoreWithError("The emulator's resources could not be read (romfs).");
	}

	HorizonException::Breadcrumb("initializing settings");
	if (!InitializeSettings())
	{
		ExitCoreWithError("The emulator's settings could not be loaded (inis/PCSX2.ini).");
	}

	HorizonException::Breadcrumb("CPUThreadInitialize");
	if (!VMManager::Internal::CPUThreadInitialize())
	{
		VMManager::Internal::CPUThreadShutdown();
		ExitCoreWithError("The emulator could not start (CPU thread initialization).");
	}

	HorizonHost::SetCPUThread();
	VMManager::ApplySettings();
	HorizonException::Breadcrumb("settings applied");

#ifdef YADOKARI_USB_STORAGE
	// (before the boot: the launcher's game may be on a USB drive, which libusbhsfs mounts in the background)
	if (HorizonUsbStorage::Initialize())
	{
		if (s_launch_image.starts_with("ums"))
		{
			for (int attempt = 0; attempt < 100 && !FileSystem::FileExists(s_launch_image.c_str()); attempt++)
				svcSleepThread(100'000'000ULL);
		}
		LogUsbVolumes();
		HorizonUsbStorage::ConsumeChange();
	}
	else
		WARNING_LOG("{}", HorizonUsbStorage::GetError());
#endif

	AppletHookCookie applet_hook{};
	appletHook(&applet_hook, AppletHookCallback, nullptr);
	HorizonException::Breadcrumb("USB + applet hook done");

	if (s_launch_image.empty())
	{
		ERROR_LOG("No launcher handoff: ARMSX2-nx's games are started from its launcher");
		HorizonHost::RequestExit();
	}
	else
	{
		HorizonException::Breadcrumb("booting the launcher's game");
		if (!BootInitialImage(s_launch_image.c_str()))
			HorizonHost::RequestExit();
	}

	HorizonException::Breadcrumb("starting input polling");
	StartInputPolling();

	HorizonException::Breadcrumb("entering main loop");
	RunMainLoop();
	HorizonException::Breadcrumb("main loop exited");
	Horizon::ExitTrace("shutdown begin");

	StopInputPolling();
	// (before the log closes)
	Host::SetSwitchCpuBoostEnabled(false);
	ReturnToLauncher();

	appletUnhook(&applet_hook);
#ifdef YADOKARI_USB_STORAGE
	HorizonUsbStorage::Shutdown();
#endif
	if (VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(HorizonHost::TakeResumeSaveRequest());
	Horizon::ExitTrace("VMManager::Shutdown done");
	if (MTGS::IsOpen())
		MTGS::WaitForClose();
	Horizon::ExitTrace("MTGS closed");
	VMManager::Internal::CPUThreadShutdown();
	Horizon::ExitTrace("CPUThreadShutdown done (SysMemory released, JIT freed)");
	Horizon::AuditHeapToExitTrace("after shutdown");
	HorizonThreadWrap::AuditUnjoinedThreads("after shutdown");

	if (romfs_available)
		romfsExit();
	Horizon::ExitTrace("clean exit");
	HorizonException::Breadcrumb("main returning");
	ExitCore(0);
}

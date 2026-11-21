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
#ifdef __SWITCH2__
#include "pcsx2/ImGui/SwitchQuickMenu.h"
#include "switch/SwitchSettings.h"
#else
#include "pcsx2/Catalog/CoverAutoDownload.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#endif
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"

#include "HorizonException.h"
#include "HorizonHost.h"
#include "HorizonThreadWrap.h"
#ifndef __SWITCH2__
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
#include "nx2_platform.h"

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
#ifdef __SWITCH2__
	// ARMSX2-nx: the launcher (an SDL program of its own) picks the game and writes the settings, then chainloads the
	// core of the renderer it was asked for with a handoff file naming the game and the launcher to return to.
	constexpr const char* YADOKARI_ROOT = "sdmc:/switch/armsx2-nx";
	constexpr const char* LOGS_DIR = "sdmc:/switch/armsx2-nx/logs";
	constexpr const char* SETTINGS_PATH = "sdmc:/switch/armsx2-nx/inis/PCSX2.ini";
	constexpr const char* LAUNCH_HANDOFF_PATH = "sdmc:/switch/armsx2-nx/launch.ini";
	// Why the game did not start, for the launcher to show (this core has no game list or landing page of its own).
	constexpr const char* LAUNCH_ERROR_PATH = "sdmc:/switch/armsx2-nx/launch_error.txt";
#else
	constexpr const char* YADOKARI_ROOT = "sdmc:/switch/yadokari";
	constexpr const char* GAMES_DIR = "sdmc:/switch/yadokari/games";
	constexpr const char* LOGS_DIR = "sdmc:/switch/yadokari/logs";
	constexpr const char* SETTINGS_PATH = "sdmc:/switch/yadokari/yadokari.ini";
#endif
	constexpr u64 IDLE_POLL_NS = 8'000'000ULL;

	constexpr u64 INPUT_POLL_NS = 16'000'000ULL;
	constexpr float STICK_DEADZONE = 0.15f;
	constexpr u32 NUM_LOCAL_PLAYERS = 2;
#ifdef __SWITCH2__
	std::string s_launch_image;
	std::unique_ptr<INISettingsInterface> s_launcher_game_settings;
	std::string s_return_nro;
#else
	constexpr const char* USB_SETTINGS_SECTION = "Horizon";
	constexpr const char* USB_GAME_ROOTS_KEY = "ManagedUsbGameRoots";
	constexpr u64 MENU_COMBO = HidNpadButton_Plus | HidNpadButton_Minus;
	std::atomic<u64> s_nav_snapshot{0};
	void PumpMenuNav();
#endif

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

#ifdef __SWITCH2__
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
#endif

	bool InitializeSettings()
	{
		s_settings_interface = std::make_unique<INISettingsInterface>(SETTINGS_PATH);
		Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());

		if (!s_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
		{
			INFO_LOG("Initializing Switch settings at {}", SETTINGS_PATH);
#ifdef __SWITCH2__
			// The launcher may have written its settings before this core ever set the defaults: they win over them.
			const std::vector<IniEntry> launcher_entries = ReadIniEntries(SETTINGS_PATH);
#endif
			VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);
#ifndef __SWITCH2__
			s_settings_interface->SetBoolValue("UI", "EnableFullscreenUI", true);
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", GAMES_DIR);
#endif
			s_settings_interface->SetStringValue("Pad2", "Type", "DualShock2");
#ifdef __SWITCH2__
			for (const IniEntry& entry : launcher_entries)
			{
				if (entry.section != "UI" || entry.key != "SettingsVersion")
					s_settings_interface->SetStringValue(entry.section.c_str(), entry.key.c_str(), entry.value.c_str());
			}
			if (!launcher_entries.empty())
				INFO_LOG("Kept {} settings the launcher wrote", launcher_entries.size());
#endif
		}

#ifdef __SWITCH2__
		// Each core has one GPU driver: the other core's renderer falls back to this core's (Software and Auto stay).
		{
#ifdef ARMSX2_DK3D
			constexpr int native_renderer = static_cast<int>(GSRendererType::DK3D);
#else
			constexpr int native_renderer = static_cast<int>(GSRendererType::VK);
#endif
			const int renderer = Host::GetIntSettingValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::Auto));
			if (renderer != native_renderer && renderer != static_cast<int>(GSRendererType::Auto) &&
				renderer != static_cast<int>(GSRendererType::SW))
			{
				WARNING_LOG("Renderer {} is not in this core: using {}", renderer, native_renderer);
				s_settings_interface->SetIntValue("EmuCore/GS", "Renderer", native_renderer);
			}
		}
#endif

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

#ifndef __SWITCH2__
	bool SyncUsbGameRoots()
	{
		std::vector<std::string> roots;
		for (const HorizonUsbStorage::Volume& volume : HorizonUsbStorage::GetVolumes())
			roots.push_back(volume.root);

		auto lock = Host::GetSettingsLock();
		const std::vector<std::string> old_roots =
			s_settings_interface->GetStringList(USB_SETTINGS_SECTION, USB_GAME_ROOTS_KEY);
		if (roots == old_roots)
			return false;

		for (const std::string& root : old_roots)
			s_settings_interface->RemoveFromStringList("GameList", "RecursivePaths", root.c_str());
		for (const std::string& root : roots)
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", root.c_str());

		s_settings_interface->SetStringList(USB_SETTINGS_SECTION, USB_GAME_ROOTS_KEY, roots);

		Error error;
		if (!s_settings_interface->Save(&error))
			ERROR_LOG("Failed to save USB game roots: {}", error.GetDescription());
		return true;
	}

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

	bool InitializeFullscreenUI()
	{
		if (!ImGuiManager::InitializeFullscreenUI())
		{
			ERROR_LOG("Failed to initialize FullscreenUI");
			return false;
		}

		HorizonException::Breadcrumb("ImGuiManager::InitializeFullscreenUI ok; MTGS::WaitForOpen");
		if (!MTGS::WaitForOpen())
		{
			ERROR_LOG("Failed to open the GS thread for FullscreenUI");
			return false;
		}
		HorizonException::Breadcrumb("MTGS::WaitForOpen ok");

		MTGS::RunOnGSThread([]() { MTGS::SetRunIdle(true); });
		Host::RefreshGameListAsync(false);
		return true;
	}

#else
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

	// The launcher runs next in this process: the exit cleanup stops the threads nobody joined and releases the HOME
	// lock, then drako3d's GPU client goes, and _Exit skips the static destructors (see __wrap_atexit).
	[[noreturn]] void ExitCore(int status)
	{
		Horizon::ShutdownNetwork();
		// (_Exit skips the atexit handler that closes crash.txt and boot.txt)
		HorizonException::Shutdown();
		nx2ExitReleaseLockLast();
		nx2ExitCleanupNow();
		if (void (*release)(void) = std::exchange(s_drako3d_atexit, nullptr))
			release();
		std::_Exit(status);
	}

	// A core that cannot start: back to the launcher, which shows the reason.
	[[noreturn]] void ExitCoreWithError(const char* reason)
	{
		ERROR_LOG("{}", reason);
		FileSystem::WriteStringToFile(LAUNCH_ERROR_PATH, reason);
		ReturnToLauncher();
		ExitCore(1);
	}
#endif

#ifdef __SWITCH2__
	// The game cannot start: back to the launcher, which shows the reason.
	void ReportLaunchError(const std::string& message)
	{
		ERROR_LOG("Launch failed: {}", message);
		if (!FileSystem::WriteStringToFile(LAUNCH_ERROR_PATH, message))
			ERROR_LOG("Could not write {}", LAUNCH_ERROR_PATH);
		HorizonHost::RequestExit();
	}
#endif

	void ReportInitialBootFailure(const VMBootResult result, const Error& error)
	{
		const std::string message = error.IsValid() ? error.GetDescription() : "The virtual machine could not be started.";
		ERROR_LOG("Failed to boot launch image: {} (result {})", message, static_cast<int>(result));

#ifdef __SWITCH2__
		ReportLaunchError(message);
#else
		if (FullscreenUI::IsInitialized())
		{
			MTGS::RunOnGSThread([message]() { ImGuiFullscreen::OpenInfoMessageDialog("Startup Error", message); });
		}
		else
		{
			HorizonHost::RequestExit();
		}
#endif
	}

	bool BootInitialImage(const char* image_path)
	{
		if (!image_path || image_path[0] == '\0')
			return true;

		if (!FileSystem::FileExists(image_path))
		{
			ERROR_LOG("Launch image not found: {}", image_path);
#ifdef __SWITCH2__
			ReportLaunchError(fmt::format("The game file was not found:\n{}", image_path));
#endif
			return false;
		}

		VMBootParameters boot_params;
		boot_params.filename = image_path;
		auto hardcore_disable_callback = [](std::string reason, [[maybe_unused]] VMBootRestartCallback restart_callback) {
#ifdef __SWITCH2__
			ReportLaunchError(fmt::format("This game cannot start in RetroAchievements hardcore mode:\n{}\n"
										  "Turn hardcore mode off in the launcher's RetroAchievements settings.", reason));
#else
			if (!FullscreenUI::IsInitialized())
			{
				ERROR_LOG("Launch image requires disabling RetroAchievements Hardcore Mode: {}", reason);
				HorizonHost::RequestExit();
				return;
			}

			MTGS::RunOnGSThread([reason = std::move(reason), restart_callback = std::move(restart_callback)]() mutable {
				ImGuiFullscreen::OpenConfirmMessageDialog(Achievements::GetHardcoreModeDisableTitle(),
					Achievements::GetHardcoreModeDisableText(reason.c_str()),
					[restart_callback = std::move(restart_callback)](bool confirmed) mutable {
						if (confirmed)
							Host::RunOnCPUThread(std::move(restart_callback));
					});
			});
#endif
		};
		auto done_callback = [](VMBootResult result, const Error& error) {
			if (result == VMBootResult::StartupSuccess)
				VMManager::SetState(VMState::Running);
			else
				ReportInitialBootFailure(result, error);
		};
		VMManager::InitializeAsync(boot_params, std::move(hardcore_disable_callback), std::move(done_callback));
		return true;
	}

#ifndef __SWITCH2__
	bool s_keyboard_shown_for_focus = false;

	void ProcessSoftwareKeyboard()
	{
		if (!ImGuiManager::WantsTextInput())
		{
			s_keyboard_shown_for_focus = false;
			return;
		}

		if (s_keyboard_shown_for_focus)
			return;
		s_keyboard_shown_for_focus = true;

		HorizonHost::SoftwareKeyboardParameters params;
		params.guide_text = "Enter text";
		if (std::optional<std::string> text = HorizonHost::ShowSoftwareKeyboard(params); text.has_value())
			ImGuiManager::AddTextInput(std::move(text.value()));
	}
#endif

	void RunMainLoop()
	{
		while (appletMainLoop())
		{
			Host::PumpMessagesOnCPUThread();
			ProcessAppletLifecycle();
#ifndef __SWITCH2__
			PumpMenuNav();
			ProcessSoftwareKeyboard();

			if (HorizonUsbStorage::ConsumeChange())
			{
				LogUsbVolumes();
				if (SyncUsbGameRoots() && FullscreenUI::IsInitialized())
					Host::RefreshGameListAsync(false);
			}
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

#ifdef __SWITCH2__
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

	void PollPadRumble(RumbleDevice& device, const PadState& pad, u32 player, bool suppressed)
	{
		const HidNpadIdType id = padIsNpadActive(&pad, HidNpadIdType_Handheld) ?
			HidNpadIdType_Handheld : static_cast<HidNpadIdType>(HidNpadIdType_No1 + player);
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
		SendRumble(device, suppressed ? 0 : s_pad_vibration[player].load(std::memory_order_relaxed));
	}
#else
	struct ButtonMap
	{
		u64 nx;
		PadDualshock2::Inputs ps2;
	};
	constexpr ButtonMap BUTTON_MAP[] = {
		{HidNpadButton_Up, PadDualshock2::Inputs::PAD_UP},
		{HidNpadButton_Down, PadDualshock2::Inputs::PAD_DOWN},
		{HidNpadButton_Left, PadDualshock2::Inputs::PAD_LEFT},
		{HidNpadButton_Right, PadDualshock2::Inputs::PAD_RIGHT},
		{HidNpadButton_X, PadDualshock2::Inputs::PAD_TRIANGLE},
		{HidNpadButton_A, PadDualshock2::Inputs::PAD_CIRCLE},
		{HidNpadButton_B, PadDualshock2::Inputs::PAD_CROSS},
		{HidNpadButton_Y, PadDualshock2::Inputs::PAD_SQUARE},
		{HidNpadButton_Minus, PadDualshock2::Inputs::PAD_SELECT},
		{HidNpadButton_Plus, PadDualshock2::Inputs::PAD_START},
		{HidNpadButton_L, PadDualshock2::Inputs::PAD_L1},
		{HidNpadButton_R, PadDualshock2::Inputs::PAD_R1},
		{HidNpadButton_ZL, PadDualshock2::Inputs::PAD_L2},
		{HidNpadButton_ZR, PadDualshock2::Inputs::PAD_R2},
		{HidNpadButton_StickL, PadDualshock2::Inputs::PAD_L3},
		{HidNpadButton_StickR, PadDualshock2::Inputs::PAD_R3},
	};
#endif

#ifndef __SWITCH2__
	struct NavMap
	{
		u64 nx;
		GenericInputBinding generic;
	};
	constexpr NavMap NAV_MAP[] = {
		{HidNpadButton_Up, GenericInputBinding::DPadUp},
		{HidNpadButton_Down, GenericInputBinding::DPadDown},
		{HidNpadButton_Left, GenericInputBinding::DPadLeft},
		{HidNpadButton_Right, GenericInputBinding::DPadRight},
		{HidNpadButton_B, GenericInputBinding::Cross},
		{HidNpadButton_A, GenericInputBinding::Circle},
		{HidNpadButton_Y, GenericInputBinding::Square},
		{HidNpadButton_X, GenericInputBinding::Triangle},
		{HidNpadButton_L, GenericInputBinding::L1},
		{HidNpadButton_R, GenericInputBinding::R1},
		{HidNpadButton_ZL, GenericInputBinding::L2},
		{HidNpadButton_ZR, GenericInputBinding::R2},
		{HidNpadButton_Minus, GenericInputBinding::Select},
		{HidNpadButton_Plus, GenericInputBinding::Start},
	};
#endif

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

	void FeedGamePad(u32 player, PadState& pad, u64 held, bool captured = false)
	{
		if (captured)
			held = 0;
#ifdef __SWITCH2__
		for (size_t i = 0; i < std::size(PS2_BUTTONS); ++i)
		{
			const u64 button = s_pad_buttons[player][i].load(std::memory_order_relaxed);
			Pad::SetControllerState(player, static_cast<u32>(PS2_BUTTONS[i]), (held & button) ? 1.0f : 0.0f);
		}
#else
		for (const ButtonMap& m : BUTTON_MAP)
			Pad::SetControllerState(player, static_cast<u32>(m.ps2), (held & m.nx) ? 1.0f : 0.0f);
#endif

		ApplyStick(player, captured ? HidAnalogStickState{} : padGetStickPos(&pad, 0),
			PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_RIGHT,
			PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN);
		ApplyStick(player, captured ? HidAnalogStickState{} : padGetStickPos(&pad, 1),
			PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_RIGHT,
			PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_DOWN);
	}

#ifndef __SWITCH2__
	void FeedNav(u64 held, u64 changed)
	{
		for (const NavMap& m : NAV_MAP)
		{
			if (changed & m.nx)
				ImGuiManager::ProcessGenericInputEvent(m.generic, InputLayout::Nintendo, (held & m.nx) ? 1.0f : 0.0f);
		}
	}

	void PumpMenuNav()
	{
		static u64 s_prev_nav_held = 0;

		const u64 held = s_nav_snapshot.load(std::memory_order_relaxed);

		if (!FullscreenUI::HasActiveWindow())
		{
			s_prev_nav_held = held;
			return;
		}

		const u64 changed = held ^ s_prev_nav_held;
		s_prev_nav_held = held;
		if (changed)
			FeedNav(held, changed);
	}
#endif

	void InputPollLoop()
	{
		Threading::SetNameOfCurrentThread("Input Poll");
		if (!Horizon::PinCallingThreadToCore3())
			Horizon::ReserveCore3ForCallingThread();
#ifdef __SWITCH2__
		bool was_captured = false;
		std::array<RumbleDevice, NUM_LOCAL_PLAYERS> rumble{};
#else
		bool prev_menu_combo = false;
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
			const bool captured = SwitchQuickMenu::CaptureInput(held[0], padGetButtonsDown(&s_pads[0]));
			const VMState state = VMManager::GetState();
			if ((!captured && state == VMState::Running) || (captured && !was_captured && VMManager::HasValidVM()))
			{
				for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
					FeedGamePad(player, s_pads[player], held[player], captured);
			}
			for (u32 player = 0; player < NUM_LOCAL_PLAYERS; ++player)
				PollPadRumble(rumble[player], s_pads[player], player, captured || state != VMState::Running);
			was_captured = captured;
#else
			s_nav_snapshot.store(held[0], std::memory_order_relaxed);

			if (!FullscreenUI::HasActiveWindow() && VMManager::HasValidVM())
			{
				for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
					FeedGamePad(player, s_pads[player], held[player]);
			}

			const bool menu_combo = (held[0] & MENU_COMBO) == MENU_COMBO;
			if (menu_combo && !prev_menu_combo)
			{
				if (FullscreenUI::IsInitialized() && VMManager::HasValidVM())
					FullscreenUI::OpenPauseMenu();
				else if (!FullscreenUI::IsInitialized())
					HorizonHost::RequestExit();
			}
			prev_menu_combo = menu_combo;
#endif

			svcSleepThread(INPUT_POLL_NS);
		}
#ifdef __SWITCH2__
		for (auto& device : rumble)
			SendRumble(device, 0);
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

#ifdef __SWITCH2__
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
#endif

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
#ifndef __SWITCH2__
	mkdir(GAMES_DIR, 0777);
#endif
	mkdir(LOGS_DIR, 0777);
#ifdef __SWITCH2__
	mkdir("sdmc:/switch/armsx2-nx/inis", 0777);
#endif

	HorizonException::Initialize(LOGS_DIR);
	Horizon::ExitTrace("=== main entered");
	Horizon::AuditHeapToExitTrace("startup");

	Log::SetTimestampsEnabled(true);
#ifdef __SWITCH2__
	// The previous run's log stays as emulog.prev.txt: on the Switch 2 a crash leaves no report, only its log.
	{
		const std::string log_path = Path::Combine(LOGS_DIR, "emulog.txt");
		const std::string prev_path = Path::Combine(LOGS_DIR, "emulog.prev.txt");
		std::remove(prev_path.c_str());
		std::rename(log_path.c_str(), prev_path.c_str());
	}
#endif
#ifdef __SWITCH2__
	// INFO, as the launcher's EnableVerbose=false asks (SetFileLogPath opens it at DEBUG): every line is flushed.
	Log::SetFileOutputLevel(LOGLEVEL_INFO, Path::Combine(LOGS_DIR, "emulog.txt"));
#else
	VMManager::Internal::SetFileLogPath(Path::Combine(LOGS_DIR, "emulog.txt"));
#endif
#ifdef __SWITCH2__
	ReadLaunchHandoff();
	HorizonHost::SetExitWhenVMEnds(!s_launch_image.empty());
#endif

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
#ifdef __SWITCH2__
		ExitCoreWithError("The emulator's resources could not be read (romfs).");
#else
		ERROR_LOG("ROMFS is unavailable and no SD resources directory was found");
		appletUnlockExit();
		return 1;
#endif
	}

	HorizonException::Breadcrumb("initializing settings");
	if (!InitializeSettings())
	{
#ifdef __SWITCH2__
		ExitCoreWithError("The emulator's settings could not be loaded (inis/PCSX2.ini).");
#else
		ERROR_LOG("Failed to initialize Switch settings");
		if (romfs_available)
			romfsExit();
		appletUnlockExit();
		return 1;
#endif
	}

	HorizonException::Breadcrumb("CPUThreadInitialize");
	if (!VMManager::Internal::CPUThreadInitialize())
	{
		VMManager::Internal::CPUThreadShutdown();
#ifdef __SWITCH2__
		ExitCoreWithError("The emulator could not start (CPU thread initialization).");
#else
		ERROR_LOG("Failed to initialize the Switch frontend");
		if (romfs_available)
			romfsExit();
		appletUnlockExit();
		return 1;
#endif
	}

	HorizonHost::SetCPUThread();
	VMManager::ApplySettings();
	HorizonException::Breadcrumb("settings applied");

#ifndef __SWITCH2__
	if (HorizonUsbStorage::Initialize())
	{
		SyncUsbGameRoots();
		LogUsbVolumes();
		HorizonUsbStorage::ConsumeChange();
	}
	else
	{
		SyncUsbGameRoots();
		WARNING_LOG("{}", HorizonUsbStorage::GetError());
	}
#endif

	AppletHookCookie applet_hook{};
	appletHook(&applet_hook, AppletHookCallback, nullptr);
	HorizonException::Breadcrumb("USB + applet hook done");

#ifdef __SWITCH2__
	if (s_launch_image.empty())
	{
		ERROR_LOG("No launcher handoff: ARMSX2-nx's games are started from armsx2-nx2.nro");
		HorizonHost::RequestExit();
	}
	else
	{
		HorizonException::Breadcrumb("booting the launcher's game");
		if (!BootInitialImage(s_launch_image.c_str()))
			HorizonHost::RequestExit();
	}
#else
	const bool fullscreen_ui = Host::GetBaseBoolSettingValue("UI", "EnableFullscreenUI", true);
	HorizonException::Breadcrumb(fullscreen_ui ? "initializing FullscreenUI (GS open)" : "FullscreenUI disabled");
	if (fullscreen_ui && !InitializeFullscreenUI())
		HorizonHost::RequestExit();
	else if (!fullscreen_ui && argc < 2)
	{
		ERROR_LOG("FullscreenUI is disabled and no launch image was provided");
		HorizonHost::RequestExit();
	}

	if (!HorizonHost::IsExitRequested() && argc > 1 && !BootInitialImage(argv[1]) && !FullscreenUI::IsInitialized())
		HorizonHost::RequestExit();
#endif

	HorizonException::Breadcrumb("starting input polling");
	StartInputPolling();

	HorizonException::Breadcrumb("entering main loop");
	RunMainLoop();
	HorizonException::Breadcrumb("main loop exited");
	Horizon::ExitTrace("shutdown begin");

	StopInputPolling();
#ifdef __SWITCH2__
	// (before the log closes)
	Host::SetSwitchCpuBoostEnabled(false);
	ReturnToLauncher();
#endif

	appletUnhook(&applet_hook);
#ifndef __SWITCH2__
	Host::CancelGameListRefresh();
	Catalog::AutoCovers::Shutdown();
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
#ifdef __SWITCH2__
	ExitCore(0);
#else
	appletUnlockExit();
#endif
	return 0;
}

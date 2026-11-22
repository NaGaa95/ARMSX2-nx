// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/SwitchQuickMenu.h"

#include "Achievements.h"
#include "Arcade/ArcadeInput.h"
#include "Autosave.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "MTGS.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "SIO/Sio.h"
#include "SaveState.h"
#include "VMManager.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Horizon/Horizon.h"
#include "common/Image.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "switch/SwitchSettings.h"
#include "switch/lsfg/SwitchLSFG.h"
#include "switch/app/HorizonHost.h"

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
	using namespace SwitchSettings;
	using Action = std::function<void()>;

	struct Row
	{
		std::string label;
		std::string value;
		std::optional<bool> toggle;
		Action select;
		std::function<void(int)> adjust;
		std::string preview;
		// Shown under the page's own detail while this row is selected.
		std::string detail;
		bool disabled = false;
		bool submenu = false;
		bool range = false;
	};
	using Rows = std::vector<Row>;
	using BuildRows = std::function<Rows()>;
	using Choices = std::vector<std::pair<std::string, std::string>>;
	using SetValue = std::function<void(const std::string&)>;

	struct Page
	{
		std::string title;
		BuildRows build;
		std::string detail;
		std::string preview;
		int selected = 0;
		int top = 0;
		bool saves = false;
	};

	struct DisplayRow
	{
		std::string label;
		std::string value;
		std::optional<bool> toggle;
		bool disabled;
		bool submenu;
		bool range;
	};

	struct View
	{
		std::string title;
		std::string detail;
		std::string notice;
		std::vector<DisplayRow> rows;
		int selected = 0;
		int top = 0;
		int tab = 0;
		bool root = false;
		bool saves = false;
		bool has_preview = false;
		u64 preview_id = 0;
	};

	enum Tab
	{
		TAB_GAME,
		TAB_EMULATION,
		TAB_GRAPHICS,
		TAB_AUDIO,
		TAB_INPUT,
	};
	constexpr std::array TABS = {"Game", "Emulation", "Graphics", "Audio", "Input"};
	constexpr std::array SPEED_MODES = {"Normal", "Fast forward", "Slow motion", "Unlimited"};
	constexpr int ROW_LIMIT = 9;
	constexpr u64 MENU_CHORD = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
	constexpr u64 MENU_INPUTS = HidNpadButton_A | HidNpadButton_B | HidNpadButton_Up | HidNpadButton_Down |
		HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_L | HidNpadButton_R;
	constexpr const char* MEMCARD_BUSY = "Wait for the game to finish writing to the memory card.";

	std::vector<Page> s_pages;
	Rows s_rows;
	int s_tab = TAB_GAME;
	std::string s_notice;
	// What the quick menu changed, until the game is closed: the launcher owns the saved settings.
	MemorySettingsInterface s_session_settings;
	std::string s_preview_path;
	u64 s_preview_id = 0;
	// Set when a GS device reset freed the preview texture: the same file is loaded again.
	std::atomic_bool s_preview_lost{false};

	std::shared_ptr<const View> s_display;
	std::mutex s_display_mutex;
	std::unique_ptr<GSTexture> s_preview_texture;
	u64 s_texture_id = 0;
	std::atomic_bool s_open{false};
	std::atomic_bool s_toggle_pending{false};
	std::atomic_bool s_repeat_pending{false};
	bool s_paused_for_menu = false;

	void OpenSettings(int screen);
	void StartTab(int tab);
	void Close(bool resume = true);
	void Publish(bool opening = false);

	struct FilePicker
	{
		std::string title;
		std::string path;
		std::function<bool(std::string_view)> accepts;
		SetValue select;
		std::string empty_label;
	};

	std::string ParentDirectory(const std::string& path);
	void PickFile(FilePicker picker, bool replace = false);

	int Wrap(int value, int count)
	{
		return (value + count) % count;
	}

	Row Link(std::string label, Action select, std::string value = {})
	{
		return {.label = std::move(label), .value = std::move(value), .select = std::move(select), .submenu = true};
	}

	Row ToggleRow(std::string label, bool on, std::function<void(bool)> set)
	{
		Row row{.label = std::move(label), .toggle = on, .select = [set, on]() { set(!on); }};
		row.adjust = [set = std::move(set), on](int direction) {
			if (on != (direction > 0))
				set(direction > 0);
		};
		return row;
	}

	void PushPage(std::string title, BuildRows build, std::string detail = {})
	{
		s_notice.clear();
		s_pages.push_back({std::move(title), std::move(build), std::move(detail)});
	}

	void Choose(std::string label, const Choices& choices, const std::string& current, SetValue set)
	{
		PushPage(std::move(label), [choices, current, set = std::move(set)]() {
			Rows rows;
			for (const auto& [name, value] : choices)
			{
				rows.push_back({.label = name, .value = value == current ? "Selected" : "", .select = [value, set]() {
					s_pages.pop_back();
					set(value);
				}});
			}
			return rows;
		});
		const auto index = std::find_if(choices.begin(), choices.end(), [&](const auto& entry) { return entry.second == current; });
		if (index != choices.end())
			s_pages.back().selected = static_cast<int>(index - choices.begin());
	}

	std::pair<std::string, std::string> SplitKey(std::string_view path)
	{
		const size_t slash = path.rfind('/');
		return {std::string(path.substr(0, slash)), std::string(path.substr(slash + 1))};
	}

	const Opt* FindOption(std::string_view key)
	{
		for (const Screen& screen : g_screens)
		{
			for (int i = 0; i < screen.n; ++i)
			{
				if (screen.opts[i].key && key == screen.opts[i].key)
					return &screen.opts[i];
			}
		}
		return nullptr;
	}

	std::string ReadSetting(std::string_view path, const char* fallback)
	{
		const auto [section, key] = SplitKey(path);
		std::string value = Host::GetStringSettingValue(section.c_str(), key.c_str(), fallback);
		if (path == GS_BACK_THREAD_KEY && value != "0")
			value = "1";
		return value;
	}

	std::string ReadOption(const Opt& option)
	{
		for (const auto& clamp : CLAMP_MODES)
		{
			if (std::string_view(option.key) == clamp.key)
			{
				const auto lock = Host::GetSettingsLock();
				const auto* settings = Host::GetSettingsInterface();
				return settings->GetBoolValue(RECOMPILER_SECTION, clamp.full, false) ? "3" :
					settings->GetBoolValue(RECOMPILER_SECTION, clamp.extra, false) ? "2" :
					settings->GetBoolValue(RECOMPILER_SECTION, clamp.overflow, true) ? "1" : "0";
			}
		}
		std::string value = ReadSetting(option.key, option.def);
		if (option.ch == C_bool)
		{
			if (value == "1")
				value = "true";
			else if (value == "0")
				value = "false";
		}
		return value;
	}

	void SetSessionValue(std::string_view path, const std::string& value)
	{
		{
			auto lock = Host::GetSettingsLock();
			Host::Internal::SetSwitchSessionSettingsLayer(&s_session_settings, lock);
			const auto [section, key] = SplitKey(path);
			const auto clamp = std::find_if(std::begin(CLAMP_MODES), std::end(CLAMP_MODES),
				[path](const ClampMode& mode) { return path == mode.key; });
			if (clamp != std::end(CLAMP_MODES))
			{
				const int mode = std::atoi(value.c_str());
				s_session_settings.SetBoolValue(RECOMPILER_SECTION, clamp->overflow, mode >= 1);
				s_session_settings.SetBoolValue(RECOMPILER_SECTION, clamp->extra, mode >= 2);
				s_session_settings.SetBoolValue(RECOMPILER_SECTION, clamp->full, mode >= 3);
			}
			else
			{
				s_session_settings.SetStringValue(section.c_str(), key.c_str(), value.c_str());
			}
		}
		VMManager::ApplySettings();
	}

	double ParseNumber(const std::string& value, double fallback)
	{
		char* end;
		const double number = std::strtod(value.c_str(), &end);
		return end != value.c_str() && *end == '\0' && std::isfinite(number) ? number : fallback;
	}

	Row ChoiceRow(std::string label, Choices choices, const std::string& current, SetValue set)
	{
		const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& choice) { return choice.second == current; });
		const int index = it == choices.end() ? -1 : static_cast<int>(it - choices.begin());
		if (choices.size() == 2 && choices[0].first == "Off" && choices[1].first == "On")
		{
			return ToggleRow(std::move(label), index == 1,
				[set, choices = std::move(choices)](bool on) { set(choices[on ? 1 : 0].second); });
		}

		Row row = Link(label, [label, choices, current, set]() { Choose(label, choices, current, set); },
			index < 0 ? current : choices[index].first);
		row.adjust = [choices = std::move(choices), index, set = std::move(set)](int direction) {
			const int next = index < 0 ? 0 : Wrap(index + direction, static_cast<int>(choices.size()));
			set(choices[next].second);
		};
		return row;
	}

	Row RangeRow(std::string label, const std::string& current, double lo, double hi, double step,
		double multiplier, const char* suffix, SetValue set)
	{
		const double value = std::clamp(ParseNumber(current, lo) * multiplier, lo, hi);
		const std::string display = fmt::format("{:g}{}", std::round(value * 100.0) / 100.0, suffix ? suffix : "");
		Row row{.label = std::move(label), .value = display, .range = true};
		row.adjust = [=, set = std::move(set)](int direction) {
			set(fmt::format("{:.8g}", std::clamp(value + direction * step, lo, hi) / multiplier));
		};
		row.select = [adjust = row.adjust]() { adjust(1); };
		return row;
	}

	bool MatchesBackend(const Opt& option)
	{
		return !option.renderer || ReadSetting(RENDERER_KEY, "18") == option.renderer;
	}

	bool HasExtension(std::string_view path, std::string_view extensions)
	{
		const size_t dot = path.rfind('.');
		if (dot == std::string_view::npos)
			return false;
		std::string extension(path.substr(dot));
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return ("," + std::string(extensions) + ",").find("," + extension + ",") != std::string::npos;
	}

	Row OptionRow(const Opt& option, SetValue set)
	{
		if (option.type == OT_SUBMENU)
			return Link(option.label, [screen = option.sub]() { OpenSettings(screen); });

		const std::string value = ReadOption(option);
		Row row;
		if (option.type == OT_CHOICE)
		{
			Choices choices;
			for (int i = 0; i < option.nch; ++i)
				choices.emplace_back(option.ch[i].label, option.ch[i].val);
			row = ChoiceRow(option.label, std::move(choices), value, std::move(set));
		}
		else
		{
			row = RangeRow(option.label, value, option.lo, option.hi, option.step, option.multiplier, option.suffix,
				std::move(set));
		}

		if (option.gateKey)
		{
			const Opt* gate = FindOption(option.gateKey);
			row.disabled = (gate ? ReadOption(*gate) : ReadSetting(option.gateKey, option.gateOff)) == option.gateOff;
		}
		const std::string_view key(option.key);
		if (Achievements::IsHardcoreModeActive() && (key == CHEATS_KEY || key == "Framerate/SlomoScalar"))
			row.disabled = true;
		return row;
	}

	Row OptionRow(const Opt& option)
	{
		return OptionRow(option, [key = option.key](const std::string& value) { SetSessionValue(key, value); });
	}

	Rows SettingsRows(int screen)
	{
		Rows rows;
		const Screen& settings = g_screens[screen];
		for (int i = 0; i < settings.n; ++i)
		{
			const Opt& option = settings.opts[i];
			if (option.live && MatchesBackend(option))
				rows.push_back(OptionRow(option));
		}
		return rows;
	}

	Row ScreenLink(int screen, std::string value = {})
	{
		return Link(g_screens[screen].title, [screen]() { OpenSettings(screen); }, std::move(value));
	}

	void SetPreview(const std::string& filename)
	{
		if (s_preview_path == filename && !s_preview_lost.exchange(false, std::memory_order_acq_rel))
			return;
		s_preview_path = filename;
		const u64 id = ++s_preview_id;
		if (!MTGS::IsOpen())
			return;

		u32 width = 0, height = 0;
		std::vector<u32> pixels;
		if (std::string_view(filename).ends_with(".png"))
		{
			RGBA8Image image;
			if (image.LoadFromFile(filename.c_str()))
			{
				width = image.GetWidth();
				height = image.GetHeight();
				pixels.assign(image.GetPixels(), image.GetPixels() + static_cast<size_t>(width) * height);
			}
		}
		else if (!filename.empty() && FileSystem::FileExists(filename.c_str()) &&
			!SaveState_ReadScreenshot(filename, &width, &height, &pixels))
		{
			pixels.clear();
		}
		MTGS::RunOnGSThread([id, width, height, pixels = std::move(pixels)]() {
			GSDrainBackQueue();
			s_preview_texture.reset();
			s_texture_id = id;
			if (pixels.empty() || !g_gs_device)
				return;
			std::unique_ptr<GSTexture> texture(g_gs_device->CreateTexture(width, height, 1, GSTexture::Format::Color));
			if (texture && texture->Update(GSVector4i(0, 0, width, height), pixels.data(), width * sizeof(u32)))
				s_preview_texture = std::move(texture);
		});
	}

	void Publish(bool opening)
	{
		Page& page = s_pages.back();
		s_rows = page.build();
		page.selected = std::clamp(page.selected, 0, std::max(0, static_cast<int>(s_rows.size()) - 1));
		const int max_top = std::max(0, static_cast<int>(s_rows.size()) - ROW_LIMIT);
		page.top = std::clamp(page.top, std::max(0, page.selected - ROW_LIMIT + 1), std::min(page.selected, max_top));
		const Row* selected = s_rows.empty() ? nullptr : &s_rows[page.selected];
		const std::string preview = !page.preview.empty() ? page.preview : (selected ? selected->preview : std::string());
		SetPreview(preview);

		View view;
		view.title = page.title;
		view.detail = page.detail;
		if (selected && !selected->detail.empty())
			view.detail += (view.detail.empty() ? "" : "\n") + selected->detail;
		view.notice = s_notice;
		view.selected = page.selected;
		view.top = page.top;
		view.tab = s_tab;
		view.root = s_pages.size() == 1;
		view.saves = page.saves;
		view.has_preview = !preview.empty();
		view.preview_id = s_preview_id;
		const int last = std::min(static_cast<int>(s_rows.size()), page.top + ROW_LIMIT);
		for (int i = page.top; i < last; ++i)
		{
			const Row& row = s_rows[i];
			view.rows.push_back({row.label, row.value, row.toggle, row.disabled, row.submenu, row.range});
		}
		{
			const std::lock_guard lock(s_display_mutex);
			s_display = std::make_shared<const View>(std::move(view));
		}
		if (opening)
			s_open.store(true, std::memory_order_release);
		MTGS::PresentCurrentFrame();
	}

	void Close(bool resume)
	{
		s_open.store(false, std::memory_order_release);
		Achievements::CancelLeaderboardEntries();
		s_pages.clear();
		s_rows.clear();
		SetPreview({});
		const bool resume_vm = resume && s_paused_for_menu && VMManager::GetState() == VMState::Paused;
		s_paused_for_menu = false;
		if (resume_vm)
			VMManager::SetPaused(false);
		else if (resume && MTGS::IsOpen())
			MTGS::PresentCurrentFrame();
	}

	std::string ParentDirectory(const std::string& path)
	{
		std::string parent(Path::GetDirectory(path));
		if (parent.empty() || parent == "sdmc:")
			parent = "sdmc:/";
		return parent;
	}

	void PickFile(FilePicker picker, bool replace)
	{
		if (!FileSystem::DirectoryExists(picker.path.c_str()))
			picker.path = "sdmc:/";
		if (replace)
			s_pages.pop_back();

		Rows rows;
		const auto navigate = [picker](std::string path) {
			FilePicker next = picker;
			next.path = std::move(path);
			PickFile(std::move(next), true);
		};
		if (!picker.empty_label.empty())
			rows.push_back({.label = picker.empty_label, .select = [select = picker.select]() { select({}); }});
		const std::string parent = ParentDirectory(picker.path);
		if (parent != picker.path)
			rows.push_back(Link("Parent folder", [navigate, parent]() mutable { navigate(parent); }));

		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(picker.path.c_str(), "*",
			FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_SORT_BY_NAME, &files);
		for (const auto& entry : files)
		{
			if (entry.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			{
				const std::string path = Path::Combine(picker.path, entry.FileName);
				rows.push_back(Link(entry.FileName, [navigate, path]() mutable { navigate(path); }));
			}
		}
		for (const auto& entry : files)
		{
			if (!(entry.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) && picker.accepts(entry.FileName))
			{
				const std::string path = Path::Combine(picker.path, entry.FileName);
				rows.push_back({.label = entry.FileName, .select = [path, select = picker.select]() { select(path); }});
			}
		}
		if (rows.empty())
			rows.push_back({.label = "No matching files", .disabled = true});
		PushPage(std::move(picker.title), [rows = std::move(rows)]() { return rows; }, std::move(picker.path));
	}

	void OpenDiscBrowser()
	{
		PickFile({
			.title = "Change disc",
			.path = ParentDirectory(VMManager::GetDiscPath()),
			.accepts = VMManager::IsDiscFileName,
			.select = [](const std::string& path) {
				if (MemcardBusy::IsBusy())
					s_notice = MEMCARD_BUSY;
				else if (VMManager::ChangeDisc(path.empty() ? CDVD_SourceType::NoDisc : CDVD_SourceType::Iso, path))
					Close();
				else
					s_notice = "Could not open the disc. The previous disc is still selected.";
			},
			.empty_label = "Eject disc",
		});
	}

	std::string StateDate(std::time_t timestamp)
	{
		std::tm local{};
		char date[40]{};
		if (localtime_r(&timestamp, &local))
			std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
		return date;
	}

	std::string StateSummary(const std::string& path)
	{
		FILESYSTEM_STAT_DATA stat{};
		return FileSystem::StatFile(path.c_str(), &stat) ? StateDate(stat.ModificationTime) : "Empty";
	}

	// Saving closes the menu: VMManager confirms with its "Saved state to slot" message.
	void SaveSlot(int slot)
	{
		s_notice.clear();
		VMManager::SaveStateToSlot(slot, false, [](const std::string& error) { s_notice = error; });
		if (s_notice.empty())
			Close();
	}

	void LoadState(const std::string& path, int slot, bool backup)
	{
		if (MemcardBusy::IsBusy())
		{
			s_notice = MEMCARD_BUSY;
			return;
		}
		Error error;
		const bool loaded = slot > 0 ? VMManager::LoadStateFromSlot(slot, backup, &error) :
			VMManager::LoadState(path.c_str(), &error);
		if (loaded)
			Close();
		else
			s_notice = error.GetDescription();
	}

	Row StateRow(std::string label, std::string path, std::string date, Action select)
	{
		Row row{.label = std::move(label), .value = std::move(date), .select = std::move(select)};
		row.preview = std::move(path);
		return row;
	}

	void OpenSaveStateList()
	{
		VMManager::WaitForSaveStateFlush();
		PushPage("Save state", []() {
			const std::string serial = VMManager::GetDiscSerial();
			const u32 crc = VMManager::GetDiscCRC();
			Rows rows;
			for (int slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; ++slot)
			{
				std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
				std::string date = StateSummary(path);
				rows.push_back(StateRow(fmt::format("Slot {}", slot), std::move(path), std::move(date), [slot]() { SaveSlot(slot); }));
			}
			return rows;
		});
		s_pages.back().saves = true;
	}

	void OpenLoadStateList()
	{
		VMManager::WaitForSaveStateFlush();
		PushPage("Load state", []() {
			const std::string serial = VMManager::GetDiscSerial();
			const u32 crc = VMManager::GetDiscCRC();
			Rows rows;
			const auto add = [&rows](std::string label, std::string path, int slot, bool backup) {
				if (!FileSystem::FileExists(path.c_str()))
					return;
				std::string date = StateSummary(path);
				rows.push_back(StateRow(std::move(label), path, std::move(date),
					[path, slot, backup]() { LoadState(path, slot, backup); }));
			};
			for (int slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; ++slot)
				add(fmt::format("Slot {}", slot), VMManager::GetSaveStateFileName(serial.c_str(), crc, slot), slot, false);
			for (const auto& entry : Autosave::List(serial.c_str(), crc))
				add(fmt::format("Autosave {}", entry.index), entry.path, 0, false);
			add("Resume state", VMManager::GetSaveStateFileName(serial.c_str(), crc, -1), 0, false);
			for (int slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; ++slot)
				add(fmt::format("Slot {} backup", slot), VMManager::GetSaveStateFileName(serial.c_str(), crc, slot, true), slot, true);
			if (rows.empty())
				rows.push_back({.label = "No save states for this game", .disabled = true});
			return rows;
		});
		s_pages.back().saves = true;
	}

	struct LeaderboardResults
	{
		bool ready = false;
		std::string error;
		std::vector<Achievements::LeaderboardEntry> entries;
	};

	void OpenLeaderboard(const Achievements::LeaderboardInfo& board, bool around_user, bool replace = false)
	{
		if (replace)
			s_pages.pop_back();
		const auto results = std::make_shared<LeaderboardResults>();
		PushPage(board.title, [results, board, around_user]() {
			Rows rows;
			rows.push_back({.label = around_user ? "Show the top scores" : "Show scores near my rank",
				.select = [board, around_user]() { OpenLeaderboard(board, !around_user, true); }});
			if (!results->ready)
				rows.push_back({.label = "Loading scores…", .disabled = true});
			else if (!results->error.empty())
				rows.push_back({.label = results->error, .disabled = true});
			else if (results->entries.empty())
				rows.push_back({.label = "No scores yet", .disabled = true});
			else
			{
				for (const auto& entry : results->entries)
				{
					rows.push_back({.label = fmt::format("#{}  {}", entry.rank, entry.user),
						.value = entry.score, .disabled = true});
				}
			}
			return rows;
		}, board.description);
		Achievements::FetchLeaderboardEntries(board.id, around_user,
			[weak = std::weak_ptr<LeaderboardResults>(results)](auto entries, auto error) {
				Host::RunOnCPUThread([weak, entries = std::move(entries), error = std::move(error)]() mutable {
					if (const auto result = weak.lock(); result && SwitchQuickMenu::IsOpen())
					{
						result->entries = std::move(entries);
						result->error = std::move(error);
						result->ready = true;
						Publish();
					}
				});
			});
	}

	void OpenLeaderboards()
	{
		Rows rows;
		for (const auto& board : Achievements::GetCurrentLeaderboards())
		{
			Row row = Link(board.title, [board]() { OpenLeaderboard(board, false); });
			row.detail = board.description;
			rows.push_back(std::move(row));
		}
		if (rows.empty())
			rows.push_back({.label = "No leaderboards for this game", .disabled = true});
		PushPage("Leaderboards", [rows = std::move(rows)]() { return rows; });
	}

	void OpenAchievements()
	{
		Achievements::GameStats game;
		std::vector<Achievements::AchievementInfo> achievements;
		Achievements::GetCurrentGameStats(&game);
		Achievements::GetCurrentAchievementList(&achievements);
		std::string summary = game.game_id ? fmt::format("{} / {} unlocked   ·   {} / {} points",
			game.unlocked_achievements, game.total_achievements, game.unlocked_points, game.total_points) :
			!Achievements::IsActive() ? "Achievements are off. Turn them on in the launcher's RetroAchievements settings." :
			"No achievements for this game: it has no RetroAchievements set, they are still loading, or you are not "
			"signed in (sign in from the launcher).";
		if (game.game_id && Achievements::IsHardcoreModeActive())
			summary += "   ·   Hardcore";

		Rows rows;
		if (game.has_leaderboards)
			rows.push_back(Link("Leaderboards", OpenLeaderboards));
		for (const auto& achievement : achievements)
		{
			const std::string status = achievement.unlocked ? "Unlocked" :
				achievement.measured_progress.empty() ? "Locked" : achievement.measured_progress;
			Row row{.label = achievement.title, .value = status};
			row.detail = fmt::format("{} ({} points)", achievement.description, achievement.points);
			row.preview = achievement.badge_path;
			rows.push_back(std::move(row));
		}
		if (rows.empty() && game.game_id)
			rows.push_back({.label = "This game has no achievements", .disabled = true});
		PushPage("Achievements", [rows = std::move(rows)]() { return rows; }, std::move(summary));
	}

	// The chosen cheat codes and patches go into the game's own settings file, which the core loads at every boot of the
	// game; every other quick menu change lasts until the game is closed.
	std::string GameSettingsPath()
	{
		const u32 crc = VMManager::GetDiscCRC();
		std::string path = VMManager::GetGameSettingsPath(VMManager::GetSerialForGameSettings(), crc);
		if (!FileSystem::FileExists(path.c_str()))
		{
			std::string legacy = VMManager::GetGameSettingsPath({}, crc);
			if (FileSystem::FileExists(legacy.c_str()))
				path = std::move(legacy);
		}
		return path;
	}

	bool CanSaveGameSettings()
	{
		return VMManager::GetDiscCRC() != 0 && !VMManager::GetDiscSerial().empty();
	}

	void SaveGameSettings(const std::function<void(SettingsInterface&)>& change)
	{
		INISettingsInterface settings(GameSettingsPath());
		settings.Load();
		change(settings);
		if (!settings.Save())
			s_notice = "Could not save this game's settings.";
		VMManager::ReloadGameSettings();
	}

	// The cheats and patches switches last for the session, like every other quick menu setting: the launcher keeps
	// them, for all games or per game.
	void SetGameSwitch(const char* path, const std::string& value)
	{
		SetSessionValue(path, value);
	}

	void SetPatchEnabled(const std::string& name, bool cheats, bool enabled)
	{
		const char* section = cheats ? Patch::CHEATS_CONFIG_SECTION : Patch::PATCHES_CONFIG_SECTION;
		const auto update = [&](SettingsInterface& settings) {
			const auto edit = [&](const char* key, bool add) {
				std::vector<std::string> names = settings.GetStringList(section, key);
				std::erase(names, std::string());
				std::erase(names, name);
				if (add)
					names.push_back(name);
				settings.SetStringList(section, key, names);
			};
			edit(Patch::PATCH_ENABLE_CONFIG_KEY, enabled);
			// A patch the database enables by default needs an entry of its own to stay off.
			if (!cheats)
				edit(Patch::PATCH_DISABLE_CONFIG_KEY, !enabled);
		};
		if (CanSaveGameSettings())
		{
			SaveGameSettings(update);
			return;
		}
		{
			auto lock = Host::GetSettingsLock();
			Host::Internal::SetSwitchSessionSettingsLayer(&s_session_settings, lock);
			update(s_session_settings);
		}
		Patch::UpdateActivePatches(true, false, true, true);
		VMManager::ApplySettings();
	}

	void OpenPatchList(bool cheats, bool all_revisions = false)
	{
		u32 unnamed = 0;
		const u32 crc = ArcadeInput::IsActive() ? 0 : VMManager::GetCurrentCRC();
		auto patches = Patch::GetPatchInfo(VMManager::GetDiscSerial(), crc, cheats, all_revisions, &unnamed);
		std::erase_if(patches, [](const Patch::PatchInfo& patch) { return Patch::IsGloballyToggleablePatch(patch); });
		std::sort(patches.begin(), patches.end(), [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
		std::string detail;
		if (all_revisions)
			detail = "Codes for other revisions apply when the matching game is loaded.";
		if (unnamed)
			detail += (detail.empty() ? "" : "\n") + fmt::format("{} unnamed codes follow the core's automatic loading rules.", unnamed);
		PushPage(cheats ? "Cheat codes" : "Game patches", [cheats, patches = std::move(patches), all_revisions]() {
			Rows rows;
			if (!all_revisions)
				rows.push_back(Link("Show all revisions", [cheats]() { OpenPatchList(cheats, true); }));
			if (patches.empty())
				rows.push_back({.label = "No named codes found", .disabled = true});
			for (const auto& patch : patches)
			{
				Row row = ToggleRow(patch.name, Patch::IsNamedPatchEnabled(patch.name, cheats),
					[name = patch.name, cheats](bool on) { SetPatchEnabled(name, cheats, on); });
				row.disabled = Achievements::IsHardcoreModeActive() || (cheats && !EmuConfig.EnableCheats);
				if (!patch.description.empty())
					row.detail = patch.description;
				rows.push_back(std::move(row));
			}
			return rows;
		}, std::move(detail));
	}

	void OpenCheats()
	{
		PushPage("Cheats and patches", []() {
			const auto game_switch = [](const char* key) {
				return OptionRow(*FindOption(key), [key](const std::string& value) { SetGameSwitch(key, value); });
			};
			Row codes = Link("Cheat codes", []() { OpenPatchList(true); });
			codes.disabled = Achievements::IsHardcoreModeActive();
			return Rows{game_switch(CHEATS_KEY), std::move(codes), game_switch(PATCHES_KEY),
				Link("Game patches", []() { OpenPatchList(false); })};
		}, CanSaveGameSettings() ?
			"The chosen codes are kept for this game; the switches last for this session (the launcher keeps them). "
			"Some codes apply at boot: reset the game to undo one already written to memory." :
			"Some codes apply at boot: reset the game to undo one already written to memory.");
	}

	void OpenSettings(int screen)
	{
		PushPage(g_screens[screen].title, [screen]() {
			Rows rows = SettingsRows(screen);
			if (screen == SCR_FRAMERATE)
			{
				Choices choices;
				for (size_t i = 0; i < SPEED_MODES.size(); ++i)
				{
					if (i == static_cast<size_t>(LimiterModeType::Slomo) && Achievements::IsHardcoreModeActive())
						continue;
					choices.emplace_back(SPEED_MODES[i], std::to_string(i));
				}
				rows.insert(rows.begin(), ChoiceRow("Speed mode", std::move(choices),
					std::to_string(static_cast<int>(VMManager::GetLimiterMode())), [](const std::string& value) {
						VMManager::SetLimiterMode(static_cast<LimiterModeType>(std::atoi(value.c_str())));
					}));
			}
			else if (screen == SCR_GS_ADVANCED)
			{
				rows.push_back({.label = "Clear shader cache", .select = []() {
					u32 removed = 0;
					GSClearShaderCache([&removed](u32 count) { removed = count; });
					MTGS::WaitGS(false);
					s_notice = fmt::format("Removed {} shader cache files", removed);
				}});
			}
			return rows;
		});
	}

	Rows GameRows()
	{
		const bool no_disc = VMManager::GetDiscSerial().empty();
		Row save = Link("Save state", OpenSaveStateList);
		save.disabled = no_disc;
		Row load = Link("Load state", OpenLoadStateList);
		load.disabled = no_disc || Achievements::IsHardcoreModeActive();
		Row cheats = Link("Cheats and patches", OpenCheats);
		cheats.disabled = no_disc || VMManager::IsEmulationOnlyMode();
		return {
			{.label = "Resume game", .select = []() { Close(); }},
			std::move(save),
			std::move(load),
			Link("Change disc", OpenDiscBrowser),
			std::move(cheats),
			Link("Achievements", OpenAchievements),
			{.label = "Reset game", .select = []() {
				if (VMManager::RequestReset())
					Close();
				else
					s_notice = MEMCARD_BUSY;
			}},
			{.label = "Return to launcher", .select = []() {
				if (MemcardBusy::IsBusy())
				{
					s_notice = MEMCARD_BUSY;
					return;
				}
				Close(false);
				HorizonHost::RequestVMShutdown(EmuConfig.SaveStateOnShutdown);
			}},
		};
	}

	// LSFG 2x for the session: the launcher prepares it, and every game starts with it off.
	Row FrameGenerationRow()
	{
		if (GSGetCurrentRenderAPI() != RenderAPI::DK3D && GSGetCurrentRenderAPI() != RenderAPI::Vulkan)
		{
			Row row{.label = "Frame generation", .value = "Vulkan and Drako3D only", .disabled = true};
			row.detail = "LSFG 2x runs on the Vulkan and Drako3D renderers.";
			return row;
		}
		if (!SwitchLSFG_IsPrepared())
		{
			Row row{.label = "Frame generation", .value = "Off in the launcher", .disabled = true};
			row.detail = "Turn on LSFG 2x in the launcher's Frame generation settings (with Lossless.dll).";
			return row;
		}
		Row row = ToggleRow("Frame generation", SwitchLSFG_IsEnabled(), [](bool on) {
			if (SwitchLSFG_RequestEnabled(on, PerformanceMetrics::GetInternalFPS()))
				return;
			s_notice = SwitchLSFG_IsAvailable() ?
				"Frame generation doubles 30 FPS games, and 50/60 FPS games at 120 Hz (handheld, or docked with 120 Hz Docked Mode)." :
				"Frame generation is unavailable on this renderer or display: see logs/emulog.txt.";
		});
		if (SwitchLSFG_IsPreparing())
			row.detail = "Preparing the shaders from Lossless.dll (the first time only).";
		else if (SwitchLSFG_IsHighRatePassthrough())
			row.detail = "Native rate: the game runs at the display's rate on its own.";
		return row;
	}

	Rows TabRows(int tab)
	{
		Rows rows;
		switch (tab)
		{
			case TAB_GAME:
				return GameRows();
			case TAB_EMULATION:
				rows.push_back(ToggleRow("CPU Boost", Host::IsSwitchCpuBoostEnabled(), Host::SetSwitchCpuBoostEnabled));
				rows.push_back(FrameGenerationRow());
				for (int screen : {SCR_CPU, SCR_FRAMERATE, SCR_GAMEFIXES})
					rows.push_back(ScreenLink(screen));
				break;
			case TAB_GRAPHICS:
				rows.push_back(OptionRow(*FindOption(UPSCALE_KEY)));
				for (int screen : {SCR_DISPLAY, SCR_ENHANCE, SCR_POST, SCR_TEXTURES, SCR_OSD})
					rows.push_back(ScreenLink(screen));
				break;
			case TAB_AUDIO:
				return SettingsRows(SCR_AUDIO);
			case TAB_INPUT:
			{
				const Opt& type = *FindOption(PAD2_TYPE_KEY);
				const std::string pad2 = ReadOption(type);
				const auto connected = std::find_if(type.ch, type.ch + type.nch, [&](const Choice& choice) { return pad2 == choice.val; });
				rows.push_back(ScreenLink(SCR_PAD1, "DualShock 2"));
				rows.push_back(ScreenLink(SCR_PAD2, connected != type.ch + type.nch ? connected->label : pad2));
				break;
			}
		}
		return rows;
	}

	void StartTab(int tab)
	{
		s_tab = tab;
		s_pages.clear();
		PushPage(TABS[tab], [tab]() { return TabRows(tab); });
	}

	void HandleInput(u64 pressed)
	{
		if (!SwitchQuickMenu::IsOpen() || !VMManager::HasValidVM())
			return;
		if (pressed & HidNpadButton_B)
		{
			if (s_pages.size() == 1)
				Close();
			else
				s_pages.pop_back();
			s_notice.clear();
		}
		else if (pressed & (HidNpadButton_L | HidNpadButton_R))
		{
			const int direction = (pressed & HidNpadButton_L) ? -1 : 1;
			StartTab(Wrap(s_tab + direction, static_cast<int>(TABS.size())));
		}
		else if (!s_rows.empty() && (pressed & (HidNpadButton_Up | HidNpadButton_Down)))
		{
			const int direction = (pressed & HidNpadButton_Up) ? -1 : 1;
			s_pages.back().selected = Wrap(s_pages.back().selected + direction, static_cast<int>(s_rows.size()));
		}
		else if (!s_rows.empty() && (pressed & (HidNpadButton_A | HidNpadButton_Left | HidNpadButton_Right)))
		{
			const Row row = s_rows[s_pages.back().selected];
			s_notice.clear();
			if (!row.disabled)
			{
				if ((pressed & HidNpadButton_A) && row.select)
					row.select();
				else if (row.adjust)
					row.adjust((pressed & HidNpadButton_Left) ? -1 : 1);
			}
		}
		if (SwitchQuickMenu::IsOpen())
			Publish();
	}
} // namespace

bool SwitchQuickMenu::IsOpen()
{
	return s_open.load(std::memory_order_acquire);
}

namespace
{
	// CPU thread.
	void ToggleNow()
	{
		const VMState state = VMManager::GetState();
		if (state != VMState::Running && state != VMState::Paused)
			return;

		if (SwitchQuickMenu::IsOpen())
		{
			Close();
			return;
		}
		s_paused_for_menu = state == VMState::Running;
		if (s_paused_for_menu)
			VMManager::SetPaused(true);
		StartTab(TAB_GAME);
		Publish(true);
	}
} // namespace

void SwitchQuickMenu::Toggle()
{
	Host::RunOnCPUThread(ToggleNow);
}

bool SwitchQuickMenu::CaptureInput(u64 held, u64 pressed)
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	static bool wait_for_release = false;
	static bool chord_consumed = false;
	static u64 last_direction = 0;
	static Clock::time_point next_repeat{};
	if ((held & MENU_CHORD) == MENU_CHORD && (pressed & MENU_CHORD) && VMManager::HasValidVM())
	{
		if (!s_toggle_pending.exchange(true, std::memory_order_acq_rel))
		{
			// Pending until the menu has opened or closed: the input below waits for it.
			Host::RunOnCPUThread([]() {
				ToggleNow();
				s_toggle_pending.store(false, std::memory_order_release);
			});
		}
		chord_consumed = true;
		wait_for_release = true;
		last_direction = 0;
		return true;
	}
	// The stick directions are not buttons a release waits for.
	constexpr u64 STICK_DIRECTIONS = HidNpadButton_StickLLeft | HidNpadButton_StickLUp | HidNpadButton_StickLRight |
		HidNpadButton_StickLDown | HidNpadButton_StickRLeft | HidNpadButton_StickRUp | HidNpadButton_StickRRight |
		HidNpadButton_StickRDown;
	const u64 buttons_held = held & ~STICK_DIRECTIONS;
	if (s_toggle_pending.load(std::memory_order_acquire) || IsOpen())
	{
		wait_for_release = true;
		if (chord_consumed)
			chord_consumed = buttons_held != 0;
		else if (!s_toggle_pending.load(std::memory_order_acquire))
		{
			constexpr u64 directions = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
			const u64 direction = held & directions;
			const auto now = Clock::now();
			if (direction != last_direction || (pressed & directions))
			{
				last_direction = direction;
				next_repeat = now + 300ms;
			}
			else if (direction && now >= next_repeat && !(pressed & MENU_INPUTS) &&
				!s_toggle_pending.load(std::memory_order_acquire) && !s_repeat_pending.exchange(true, std::memory_order_acq_rel))
			{
				next_repeat = now + 85ms;
				Host::RunOnCPUThread([direction]() {
					HandleInput(direction);
					s_repeat_pending.store(false, std::memory_order_release);
				});
			}
			if (pressed & MENU_INPUTS)
				Host::RunOnCPUThread([pressed]() { HandleInput(pressed); });
		}
		return true;
	}
	chord_consumed = false;
	last_direction = 0;
	if (wait_for_release)
	{
		wait_for_release = buttons_held != 0;
		return true;
	}
	return false;
}

void SwitchQuickMenu::OnVMStopped()
{
	Close(false);
	auto lock = Host::GetSettingsLock();
	Host::Internal::SetSwitchSessionSettingsLayer(nullptr, lock);
	s_session_settings.Clear();
}

void SwitchQuickMenu::OnGSShutdown()
{
	s_preview_texture.reset();
	s_texture_id = 0;
	s_preview_lost.store(true, std::memory_order_release);
}

void SwitchQuickMenu::Render()
{
	if (!IsOpen())
		return;

	std::shared_ptr<const View> view;
	{
		const std::lock_guard lock(s_display_mutex);
		view = s_display;
	}
	if (!view)
		return;

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	const float scale = std::min(screen.x / 1280.0f, screen.y / 720.0f);
	if (scale <= 0.0f)
		return;

	// Wide enough for the five tabs' names at the root.
	const bool picture = view->saves || view->has_preview;
	const float width = (view->saves ? 824.0f : picture ? 700.0f : 640.0f) * scale;
	const float list_width = (view->saves ? 456.0f : picture ? 572.0f : 640.0f) * scale;
	const float padding = 20.0f * scale;
	const float step = 42.0f * scale;
	const float font_size = 20.0f * scale;
	const float detail_size = 17.0f * scale;
	ImFont* font = ImGui::GetFont();
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	const auto text_width = [&](const char* text, float size) {
		return font->CalcTextSizeA(size, width, 0.0f, text).x;
	};
	const auto text = [&](float x, float y, ImU32 color, const char* value, float size = 0.0f) {
		draw->AddText(font, size ? size : font_size, {x, y}, color, value);
	};
	const auto fit_text = [&](float x, float y, float max_width, ImU32 color, const std::string& value, float size) {
		if (text_width(value.c_str(), size) <= max_width)
		{
			text(x, y, color, value.c_str(), size);
			return;
		}
		const char* end = nullptr;
		const float available = std::max(0.0f, max_width - text_width("…", size));
		const float prefix = font->CalcTextSizeA(size, available, 0.0f, value.c_str(), nullptr, &end).x;
		draw->AddText(font, size, {x, y}, color, value.c_str(), end);
		text(x + prefix, y, color, "…", size);
	};
	const auto wrapped_height = [&](const std::string& value) {
		return value.empty() ? 0.0f : font->CalcTextSizeA(detail_size, width, width - 2.0f * padding, value.c_str()).y;
	};

	const float detail_height = std::min(wrapped_height(view->detail), 90.0f * scale);
	const float notice_height = std::min(wrapped_height(view->notice), 54.0f * scale);
	const float rows_height = std::max(step * static_cast<float>(view->rows.size()),
		(view->saves ? 232.0f : picture ? 106.0f : 42.0f) * scale);
	const float height = 76.0f * scale + detail_height + rows_height + notice_height;
	const float left = (screen.x - width) * 0.5f;
	const float top = (screen.y - height) * 0.5f;
	const float list_top = top + 58.0f * scale + detail_height;
	const float list_right = left + list_width - padding;
	const float text_left = left + padding;
	const float right = left + width - padding;

	constexpr ImU32 WHITE = IM_COL32(238, 246, 255, 255);
	constexpr ImU32 MUTED = IM_COL32(154, 179, 202, 255);
	constexpr ImU32 DISABLED = IM_COL32(123, 142, 160, 255);
	constexpr ImU32 ACCENT = IM_COL32(94, 201, 255, 255);
	draw->AddRectFilled({0, 0}, screen, IM_COL32(0, 0, 0, 122));
	draw->AddRectFilled({left, top}, {left + width, top + height}, IM_COL32(13, 22, 36, 249), 12.0f * scale);
	draw->AddRect({left, top}, {left + width, top + height}, IM_COL32(86, 168, 224, 255), 12.0f * scale);
	if (view->root)
	{
		const float tab_width = (width - 2.0f * padding) / static_cast<float>(TABS.size());
		for (size_t i = 0; i < TABS.size(); ++i)
		{
			const float label_width = text_width(TABS[i], font_size);
			const float x = text_left + (static_cast<float>(i) + 0.5f) * tab_width - label_width * 0.5f;
			const bool selected = i == static_cast<size_t>(view->tab);
			text(x, top + padding, selected ? WHITE : MUTED, TABS[i]);
			if (selected)
				draw->AddLine({x, top + 46.0f * scale}, {x + label_width, top + 46.0f * scale}, ACCENT, 2.0f * scale);
		}
	}
	else
	{
		fit_text(text_left, top + padding, width - 2.0f * padding, WHITE, view->title, font_size);
	}
	const auto wrapped = [&](float y, float h, ImU32 color, const std::string& value) {
		if (value.empty())
			return;
		draw->PushClipRect({text_left, y}, {right, y + h}, true);
		draw->AddText(font, detail_size, {text_left, y}, color, value.c_str(), nullptr, width - 2.0f * padding);
		draw->PopClipRect();
	};
	wrapped(top + 49.0f * scale, detail_height, MUTED, view->detail);

	if (picture)
	{
		const float image_left = left + list_width + 10.0f * scale;
		const float image_width = right - image_left;
		const float image_height = (view->saves ? 202.0f : 98.0f) * scale;
		draw->AddRectFilled({image_left, list_top}, {right, list_top + image_height},
			IM_COL32(6, 13, 23, 255), 6.0f * scale);
		if (s_preview_texture && s_texture_id == view->preview_id && view->has_preview)
		{
			const float ratio = std::min(image_width / s_preview_texture->GetWidth(), image_height / s_preview_texture->GetHeight());
			const float w = ratio * s_preview_texture->GetWidth();
			const float h = ratio * s_preview_texture->GetHeight();
			const float x = image_left + (image_width - w) * 0.5f;
			const float y = list_top + (image_height - h) * 0.5f;
			draw->AddImage(reinterpret_cast<ImTextureID>(s_preview_texture->GetNativeHandle()), {x, y}, {x + w, y + h});
		}
		else if (view->saves)
		{
			const char* label = "No preview available";
			text(image_left + (image_width - text_width(label, detail_size)) * 0.5f,
				list_top + (image_height - detail_size) * 0.5f, MUTED, label, detail_size);
		}
	}

	for (size_t i = 0; i < view->rows.size(); ++i)
	{
		const DisplayRow& row = view->rows[i];
		const float y = list_top + step * static_cast<float>(i);
		const float center = y + step * 0.5f;
		const bool selected = view->top + static_cast<int>(i) == view->selected;
		if (selected)
			draw->AddRectFilled({left + 10.0f * scale, y}, {left + list_width - 10.0f * scale, y + step - 3.0f * scale},
				IM_COL32(34, 81, 117, 255), 6.0f * scale);

		float label_right = list_right;
		const ImU32 value_color = row.disabled ? DISABLED : ACCENT;
		if (row.toggle.has_value())
		{
			const bool on = *row.toggle;
			const float pill_width = 66.0f * scale, pill_height = 26.0f * scale;
			const float px = list_right - pill_width;
			draw->AddRectFilled({px, center - pill_height * 0.5f}, {list_right, center + pill_height * 0.5f},
				row.disabled ? IM_COL32(47, 59, 74, 255) : on ? IM_COL32(44, 135, 194, 255) : IM_COL32(50, 68, 86, 255),
				pill_height * 0.5f);
			draw->AddCircleFilled({on ? list_right - 13.0f * scale : px + 13.0f * scale, center}, 7.0f * scale,
				row.disabled ? DISABLED : WHITE);
			const float word_width = text_width(on ? "On" : "Off", 15.0f * scale);
			text(on ? px + 11.0f * scale : list_right - word_width - 9.0f * scale, center - 7.5f * scale,
				row.disabled ? DISABLED : WHITE, on ? "On" : "Off", 15.0f * scale);
			label_right = px - padding * 0.5f;
		}
		else
		{
			if (row.submenu)
			{
				text(list_right - text_width("›", font_size), center - font_size * 0.5f, value_color, "›");
				label_right -= 18.0f * scale;
			}
			if (!row.value.empty())
			{
				const std::string value = row.range ? "‹  " + row.value + "  ›" : row.value;
				const float value_width = std::min(text_width(value.c_str(), detail_size), list_width * 0.43f);
				fit_text(label_right - value_width, center - detail_size * 0.5f, value_width, value_color, value, detail_size);
				label_right -= value_width + padding * 0.5f;
			}
		}
		fit_text(text_left, center - font_size * 0.5f, label_right - text_left,
			row.disabled ? DISABLED : WHITE, row.label, font_size);
	}
	wrapped(list_top + rows_height + 2.0f * scale, notice_height, ACCENT, view->notice);
}

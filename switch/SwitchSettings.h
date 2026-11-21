// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include <cstddef>

// The settings the launcher and the cores' quick menu show, one table for both. Keys are PCSX2's ([Section] Key as
// "Section/Key"), written into inis/PCSX2.ini at launch. The Wrapper/ keys stay in the launcher: the renderer picks the
// core, and a clamping mode is three of PCSX2's switches (CLAMP_MODES).
namespace SwitchSettings
{
inline constexpr char CPU_BOOST_KEY[] = "Switch/KeepCpuBoost";

enum OType { OT_CHOICE, OT_RANGE, OT_SCALED_RANGE, OT_SUBMENU, OT_TEXT, OT_FILE };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
  int sub;
  // Greyed out while the setting gateKey has the value gateOff.
  const char *gateKey;
  const char *gateOff;
  int multiplier;
  const char *suffix;
  // In the quick menu too (false: the launcher only, for what needs a restart or a file picker).
  bool live = true;
  // Only for this renderer ("18" Drako3D, "14" Vulkan).
  const char *renderer = nullptr;
  const char *extension = nullptr;
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, 1, nullptr }
#define O_SCALED_RANGE(l,k,lo,hi,s,d,m,u) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, m, u }
#define O_SCALED_RANGEG(l,k,lo,hi,s,d,m,u,gk,go) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, m, u }
#define O_SUB(l,scr)           { l, nullptr, OT_SUBMENU, nullptr,0, 0,0,0, nullptr, scr, nullptr, nullptr, 1, nullptr }
#define O_CHOICEG(l,k,c,d,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_RANGEG(l,k,lo,hi,s,d,gk,go) { l, k, OT_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, 1, nullptr }
#define O_BOOL(l,k,d) O_CHOICE(l,k,C_bool,d)
#define O_BOOLG(l,k,d,gk,go) O_CHOICEG(l,k,C_bool,d,gk,go)
#define O_BOOT(l,k,c,d) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr, 1, nullptr, false }
#define O_TEXTG(l,k,d,gk,go) { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, gk, go, 1, nullptr, false }
#define O_FILEG(l,k,d,x,gk,go) { l, k, OT_FILE, nullptr,0, 0,0,0, d, 0, gk, go, 1, nullptr, false, nullptr, x }
#define O_FILE(l,k,d,x) O_FILEG(l,k,d,x,nullptr,nullptr)
#define O_RENDERER(l,k,c,d,r,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go, 1, nullptr, true, r }
#define O_VK(l,k,d) O_RENDERER(l,k,C_bool,d,"14",nullptr,nullptr)
#define O_HW_FIX(l,k,c,d) O_CHOICEG(l,k,c,d,GS_FIXES_KEY,"false")
#define O_HW_RANGE(l,k,lo,hi,d) O_RANGEG(l,k,lo,hi,1,d,GS_FIXES_KEY,"false")

inline constexpr char RENDERER_KEY[] = "EmuCore/GS/Renderer";
inline constexpr char GS_FIXES_KEY[] = "EmuCore/GS/UserHacks";
inline constexpr char GS_BACK_THREAD_KEY[] = "EmuCore/GS/GSBackThreadMode";
inline constexpr char ETHERNET_KEY[] = "DEV9/Eth/EthEnable";
inline constexpr char HDD_KEY[] = "DEV9/Hdd/HddEnable";
inline constexpr char CHEATS_KEY[] = "EmuCore/EnableCheats";
inline constexpr char PATCHES_KEY[] = "EmuCore/EnablePatches";

inline constexpr char RECOMPILER_SECTION[] = "EmuCore/CPU/Recompiler";
struct ClampMode { const char *key, *overflow, *extra, *full; };
inline constexpr ClampMode CLAMP_MODES[] = {
  {"Wrapper/EEClampMode","fpuOverflow","fpuExtraOverflow","fpuFullMode"},
  {"Wrapper/VU0ClampMode","vu0Overflow","vu0ExtraOverflow","vu0SignOverflow"},
  {"Wrapper/VU1ClampMode","vu1Overflow","vu1ExtraOverflow","vu1SignOverflow"},
};

inline constexpr Choice C_backend[]  = { {"Drako3D (native)","18"}, {"Vulkan (NVK)","14"} };
inline constexpr Choice C_upscale[]  = { {"1x (native ~480p)","1"},{"1.5x","1.5"},{"2x (~720p)","2"},{"2.5x","2.5"},
                                     {"3x (~1080p)","3"},{"4x (~1440p)","4"},{"5x (~1800p)","5"},{"6x (4K ~2160p)","6"} };
inline constexpr Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
inline constexpr Choice C_offOn[]    = { {"Off","0"}, {"On","1"} };
inline constexpr Choice C_aspect[]   = { {"Auto 4:3/3:2","Auto 4:3/3:2"}, {"4:3","4:3"}, {"16:9","16:9"},
                                     {"10:7","10:7"}, {"21:9","21:9"}, {"20:9","20:9"},
                                     {"19.5:9","19.5:9"}, {"Stretch","Stretch"}, {"Custom","Custom"} };
inline constexpr Choice C_fmvasp[]   = { {"Off","Off"}, {"Auto 4:3/3:2","Auto 4:3/3:2"}, {"4:3","4:3"}, {"16:9","16:9"},
                                     {"21:9","21:9"}, {"20:9","20:9"}, {"19.5:9","19.5:9"}, {"10:7","10:7"}, {"Custom","Custom"} };
inline constexpr Choice C_filter[]   = { {"Nearest","0"}, {"Bilinear (PS2)","2"}, {"Bilinear (Forced)","1"}, {"Bilinear no-sprite","3"} };
inline constexpr Choice C_aniso[]    = { {"Off","0"}, {"2x","2"}, {"4x","4"}, {"8x","8"}, {"16x","16"} };
inline constexpr Choice C_osdpos[]   = { {"Off","0"}, {"Top left","1"}, {"Top center","2"}, {"Top right","3"},
                                     {"Center left","4"}, {"Center","5"}, {"Center right","6"},
                                     {"Bottom left","7"}, {"Bottom center","8"}, {"Bottom right","9"} };
inline constexpr Choice C_present[]  = { {"Nearest","0"}, {"Bilinear (smooth)","1"}, {"Bilinear (sharp)","2"} };
inline constexpr Choice C_queue[]    = { {"Optimal pacing","0"}, {"1 frame","1"}, {"2 frames","2"}, {"3 frames","3"}, {"4 frames","4"}, {"5 frames","5"} };
inline constexpr Choice C_invert[]   = { {"Off","0"}, {"Horizontal","1"}, {"Vertical","2"}, {"Both axes","3"} };
inline constexpr Choice C_rtTexture[]= { {"Disabled","0"}, {"Inside target","1"}, {"Merge targets","2"} };
inline constexpr Choice C_flush[]    = { {"Disabled","0"}, {"Sprites","1"}, {"All primitives","2"} };
inline constexpr Choice C_halfPixel[]= { {"Off","0"}, {"Normal","1"}, {"Texture","2"}, {"Texture (aggressive)","3"},
                                     {"Align to native","4"}, {"Align to native with offset","5"} };
inline constexpr Choice C_blend[]    = { {"Minimum","0"}, {"Basic","1"}, {"Medium","2"}, {"High","3"}, {"Full","4"}, {"Maximum","5"} };
inline constexpr Choice C_deint[]    = { {"Automatic","0"}, {"Off","1"}, {"Weave TFF","2"}, {"Weave BFF","3"}, {"Bob TFF","4"},
                                     {"Bob BFF","5"}, {"Blend TFF","6"}, {"Blend BFF","7"}, {"Adaptive TFF","8"}, {"Adaptive BFF","9"} };
inline constexpr Choice C_dither[]   = { {"Off","0"}, {"Scaled","1"}, {"Unscaled","2"} };
inline constexpr Choice C_trifilter[]= { {"Automatic","-1"}, {"Off","0"}, {"Trilinear (PS2)","1"}, {"Trilinear (Forced)","2"} };
inline constexpr Choice C_preload[]  = { {"None","0"}, {"Partial","1"}, {"Full","2"} };
inline constexpr Choice C_cas[]      = { {"Off","0"}, {"Sharpen only","1"}, {"Sharpen + upscale","2"} };
inline constexpr Choice C_hwdl[]     = { {"Accurate","0"}, {"Accurate, full","1"}, {"Disable readbacks","2"},
                                     {"Unsynchronized","3"}, {"Disabled","4"}, {"Asynchronous","5"} };
inline constexpr Choice C_sync[]     = { {"TimeStretch","TimeStretch"}, {"Off","Disabled"} };
inline constexpr Choice C_eecr[]     = { {"50%","-3"}, {"60%","-2"}, {"75%","-1"}, {"100%","0"}, {"130%","1"}, {"180%","2"}, {"300%","3"} };
inline constexpr Choice C_eecs[]     = { {"Off","0"}, {"Mild","1"}, {"Moderate","2"}, {"Maximum","3"} };
inline constexpr Choice C_rounding[] = { {"Nearest","0"}, {"Negative","1"}, {"Positive","2"}, {"Chop / Zero","3"} };
inline constexpr Choice C_eeClamp[]  = { {"None","0"}, {"Normal","1"}, {"Extra + Preserve Sign","2"}, {"Full","3"} };
inline constexpr Choice C_vuClamp[]  = { {"None","0"}, {"Normal","1"}, {"Extra","2"}, {"Extra + Preserve Sign","3"} };
inline constexpr Choice C_pad[] = {{"Disconnected","None"}, {"DualShock 2","DualShock2"}};
inline constexpr Choice C_npad[] = {
  {"Unbound","None"}, {"A","A"}, {"B","B"}, {"X","X"}, {"Y","Y"}, {"L","L"}, {"R","R"}, {"ZL","ZL"}, {"ZR","ZR"},
  {"Left stick click","StickL"}, {"Right stick click","StickR"}, {"Plus","Plus"}, {"Minus","Minus"},
  {"D-pad up","Up"}, {"D-pad down","Down"}, {"D-pad left","Left"}, {"D-pad right","Right"},
};
// The core's pad poller uses these indices for the corresponding DualShock 2 buttons.
struct PadBinding { const char *label, *keys[2], *default_button; };
inline constexpr PadBinding PAD_BINDINGS[] = {
  {"Pressure modifier", {"Pad1/SwitchPressure","Pad2/SwitchPressure"}, "None"},
  {"D-pad up",           {"Pad1/SwitchUp","Pad2/SwitchUp"}, "Up"},
  {"D-pad down",         {"Pad1/SwitchDown","Pad2/SwitchDown"}, "Down"},
  {"D-pad left",         {"Pad1/SwitchLeft","Pad2/SwitchLeft"}, "Left"},
  {"D-pad right",        {"Pad1/SwitchRight","Pad2/SwitchRight"}, "Right"},
  {"Triangle",          {"Pad1/SwitchTriangle","Pad2/SwitchTriangle"}, "X"},
  {"Circle",            {"Pad1/SwitchCircle","Pad2/SwitchCircle"}, "A"},
  {"Cross",             {"Pad1/SwitchCross","Pad2/SwitchCross"}, "B"},
  {"Square",            {"Pad1/SwitchSquare","Pad2/SwitchSquare"}, "Y"},
  {"Select",            {"Pad1/SwitchSelect","Pad2/SwitchSelect"}, "Minus"},
  {"Start",             {"Pad1/SwitchStart","Pad2/SwitchStart"}, "Plus"},
  {"L1",                {"Pad1/SwitchL1","Pad2/SwitchL1"}, "L"},
  {"R1",                {"Pad1/SwitchR1","Pad2/SwitchR1"}, "R"},
  {"L2",                {"Pad1/SwitchL2","Pad2/SwitchL2"}, "ZL"},
  {"R2",                {"Pad1/SwitchR2","Pad2/SwitchR2"}, "ZR"},
  {"L3",                {"Pad1/SwitchL3","Pad2/SwitchL3"}, "StickL"},
  {"R3",                {"Pad1/SwitchR3","Pad2/SwitchR3"}, "StickR"},
};
inline constexpr Choice C_rotation[] = {{"0°","0"}, {"90°","90"}, {"180°","180"}, {"270°","270"}};
inline constexpr Choice C_tv[] = {{"Off","0"}, {"Scanlines","1"}, {"Diagonal","2"}, {"Triangular","3"},
    {"Wave","4"}, {"Lottes CRT","5"}, {"4xRGSS","6"}, {"NxAGSS","7"}};
inline constexpr Choice C_texup[] = {{"Off","0"}, {"RAISR 2x","1"}, {"RAISR 4x","2"}};
inline constexpr Choice C_upscaler[] = {{"Off","0"}, {"FSR 1","2"}, {"SGSR","3"}, {"SGSR Edge","4"}};
inline constexpr Choice C_barrier[] = {{"Automatic","-1"}, {"Off","0"}, {"On","1"}};
inline constexpr Choice C_depth24[] = {{"Off","0"}, {"Prioritize upper bits","1"}, {"Prioritize lower bits","2"}};
inline constexpr Choice C_clut[] = {{"Off","0"}, {"On","1"}, {"Inside render targets","2"}};
inline constexpr Choice C_cpu_sprite_level[] = {{"Sprites only","0"}, {"Sprites and triangles","1"}, {"Blended sprites and triangles","2"}};
inline constexpr Choice C_cpu_clut[] = {{"Off","0"}, {"Normal","1"}, {"Aggressive","2"}};
inline constexpr Choice C_native_scaling[] = {{"Off","0"}, {"Normal","1"}, {"Aggressive","2"},
    {"Normal (keep upscale)","3"}, {"Aggressive (keep upscale)","4"}};
inline constexpr Choice C_round_sprite[] = {{"Off","0"}, {"Half","1"}, {"Full","2"}};
inline constexpr Choice C_bilinear_hack[] = {{"Automatic","0"}, {"Force bilinear","1"}, {"Force nearest","2"}};
inline constexpr Choice C_depth_feedback[] = {{"None","0"}, {"Automatic","1"}, {"Depth","2"}, {"Depth as render target","3"}};
inline constexpr Choice C_achievements_position[] = {{"Top left","0"}, {"Top center","1"}, {"Top right","2"},
    {"Center left","3"}, {"Center","4"}, {"Center right","5"}, {"Bottom left","6"}, {"Bottom center","7"}, {"Bottom right","8"}};
// "Unset" is the core's default; sockets need no host adapter.
inline constexpr Choice C_network[] = {{"Sockets","Sockets"}, {"Local Link","Local Link"}};
inline constexpr Choice C_dns[] = {{"Manual","Manual"}, {"Automatic","Auto"}, {"Internal","Internal"}};
inline constexpr Choice C_launcherLanguage[] = {
  {"System", "system"}, {"English", "en"}, {"Français", "fr"},
  {"Deutsch", "de"}, {"Español", "es"}, {"Italiano", "it"}, {"Português", "pt"},
  {"简体中文", "zh-Hans"}, {"繁體中文", "zh-Hant"}
};
inline constexpr Choice C_launcherTheme[] = { {"PS2","animated"}, {"Classic","classic"} };
inline constexpr Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
inline constexpr Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };

enum { SCR_GRAPHICS, SCR_DISPLAY, SCR_OSD, SCR_ENHANCE, SCR_GS_FIXES, SCR_AUDIO, SCR_EMU, SCR_ADVANCED,
       SCR_GAMEFIXES, SCR_FRAMERATE, SCR_CONTROLLER, SCR_SYSTEM, SCR_POST, SCR_TEXTURES, SCR_GS_ADVANCED,
       SCR_CPU, SCR_PAD1, SCR_PAD2, SCR_BOOT, SCR_NETWORK, SCR_RA_ADVANCED, SCR_COUNT };

inline constexpr Opt S_graphics[] = {
  O_BOOT("Renderer", RENDERER_KEY, C_backend, "18"),
  O_SUB("Display", SCR_DISPLAY),
  O_SUB("Rendering", SCR_ENHANCE),
  O_SUB("Post-processing", SCR_POST),
  O_SUB("Textures", SCR_TEXTURES),
  O_SUB("Advanced graphics", SCR_GS_ADVANCED),
  O_SUB("On-screen display", SCR_OSD),
  O_SUB("Manual graphics fixes", SCR_GS_FIXES),
};
inline constexpr Opt S_display[] = {
  O_CHOICE("Aspect ratio", "EmuCore/GS/AspectRatio", C_aspect, "Auto 4:3/3:2"),
  O_BOOL("Widescreen patches", "EmuCore/EnableWideScreenPatches", "false"),
  O_BOOL("No-interlace patches", "EmuCore/EnableNoInterlacingPatches", "false"),
  O_CHOICE("FMV aspect ratio", "EmuCore/GS/FMVAspectRatioSwitch", C_fmvasp, "Off"),
  O_SCALED_RANGE("Custom aspect ratio", "EmuCore/GS/CustomAspectRatio", 50, 400, 1, "1.333333", 100, ":100"),
  O_CHOICE("Deinterlacing", "EmuCore/GS/deinterlace_mode", C_deint, "0"),
  O_BOOL("FMV comb filter", "EmuCore/GS/movie_comb_deinterlace", "false"),
  O_CHOICE("Display filtering", "EmuCore/GS/linear_present_mode", C_present, "1"),
  O_BOOL("Integer scaling", "EmuCore/GS/IntegerScaling", "false"),
  O_CHOICE("Rotation", "EmuCore/GS/DisplayRotation", C_rotation, "0"),
  O_BOOL("Anti-blur", "EmuCore/GS/pcrtc_antiblur", "true"),
  O_BOOL("Screen offsets", "EmuCore/GS/pcrtc_offsets", "false"),
  O_BOOL("Show overscan", "EmuCore/GS/pcrtc_overscan", "false"),
  O_BOOL("Disable interlace offset", "EmuCore/GS/disable_interlace_offset", "false"),
  O_SCALED_RANGE("Vertical stretch", "EmuCore/GS/StretchY", 10, 300, 5, "100", 1, "%"),
  O_SCALED_RANGE("Crop left", "EmuCore/GS/CropLeft", 0, 360, 1, "0", 1, " px"),
  O_SCALED_RANGE("Crop right", "EmuCore/GS/CropRight", 0, 360, 1, "0", 1, " px"),
  O_SCALED_RANGE("Crop top", "EmuCore/GS/CropTop", 0, 360, 1, "0", 1, " px"),
  O_SCALED_RANGE("Crop bottom", "EmuCore/GS/CropBottom", 0, 360, 1, "0", 1, " px"),
};
inline constexpr Opt S_osd[] = {
  O_CHOICE("On-screen messages","EmuCore/GS/OsdMessagesPos", C_osdpos, "1"),
  O_CHOICE("Performance overlay position", "EmuCore/GS/OsdPerformancePos", C_osdpos, "3"),
  O_BOOL("Show FPS",         "EmuCore/GS/OsdShowFPS", "false"),
  O_BOOL("Show speed",       "EmuCore/GS/OsdShowSpeed", "false"),
  O_BOOL("Show video rate",  "EmuCore/GS/OsdShowVPS", "false"),
  O_BOOL("Show CPU usage",   "EmuCore/GS/OsdShowCPU", "false"),
  O_BOOL("Show GPU usage",   "EmuCore/GS/OsdShowGPU", "false"),
  O_BOOL("Show resolution",  "EmuCore/GS/OsdShowResolution", "false"),
  O_BOOL("Show frame times", "EmuCore/GS/OsdShowFrameTimes", "false"),
  O_BOOL("Show GS statistics", "EmuCore/GS/OsdShowGSStats", "false"),
  O_BOOL("Show active settings", "EmuCore/GS/OsdShowSettings", "false"),
  O_BOOL("Show active patches", "EmuCore/GS/OsdshowPatches", "false"),
  O_BOOL("Show controller input", "EmuCore/GS/OsdShowInputs", "false"),
  O_BOOL("Show status indicators", "EmuCore/GS/OsdShowIndicators", "true"),
  O_BOOL("Warn about unsafe settings", "EmuCore/WarnAboutUnsafeSettings", "true"),
  O_SCALED_RANGE("Overlay text size", "EmuCore/GS/OsdScale", 50, 200, 5, "100", 1, "%"),
  O_BOOL("Bold overlay text", "EmuCore/GS/OsdBoldText", "false"),
  O_RANGE("Overlay margin", "EmuCore/GS/OsdMargin", 0, 100, 1, "10"),
};
inline constexpr Opt S_enhance[] = {
  O_CHOICE("Resolution scale", "EmuCore/GS/upscale_multiplier", C_upscale, "1"),
  O_CHOICE("Texture filtering", "EmuCore/GS/filter", C_filter, "2"),
  O_CHOICE("Anisotropic filtering", "EmuCore/GS/MaxAnisotropy", C_aniso, "0"),
  O_CHOICE("Blending accuracy","EmuCore/GS/accurate_blending_unit", C_blend, "1"),
  O_CHOICE("Dithering",        "EmuCore/GS/dithering_ps2", C_dither, "2"),
  O_CHOICE("Trilinear filtering", "EmuCore/GS/TriFilter", C_trifilter, "-1"),
  O_BOOL("Mipmapping",       "EmuCore/GS/hw_mipmap", "true"),
  O_BOOL("PS2 antialiasing (AA1)", "EmuCore/GS/HWAA1", "false"),
  O_BOOL("Accurate alpha test", "EmuCore/GS/HWAccurateAlphaTest", "false"),
  O_VK("Raster order views", "EmuCore/GS/HWROV", "false"),
};
inline constexpr Opt S_post[] = {
  O_BOOL("FXAA",             "EmuCore/GS/fxaa", "false"),
  O_CHOICE("CAS sharpening",   "EmuCore/GS/CASMode", C_cas, "0"),
  O_RANGEG("CAS strength",     "EmuCore/GS/CASSharpness", 0, 100, 5, "50", "EmuCore/GS/CASMode", "0"),
  O_RENDERER("Output upscaler", "EmuCore/GS/Upscaler", C_upscaler, "0", "14", nullptr, nullptr),
  O_RANGEG("FSR sharpness", "EmuCore/GS/FSRSharpness", 0, 100, 5, "50", "EmuCore/GS/Upscaler", "0"),
  O_RANGEG("SGSR sharpness", "EmuCore/GS/SGSRSharpness", 0, 100, 5, "100", "EmuCore/GS/Upscaler", "0"),
  O_BOOL("Shade boost",      "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("Brightness", "EmuCore/GS/ShadeBoost_Brightness", 1, 100, 1, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("Contrast", "EmuCore/GS/ShadeBoost_Contrast", 1, 100, 1, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("Gamma", "EmuCore/GS/ShadeBoost_Gamma", 1, 100, 1, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("Saturation", "EmuCore/GS/ShadeBoost_Saturation", 1, 100, 1, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_CHOICE("TV shader", "EmuCore/GS/TVShader", C_tv, "0"),
};
inline constexpr Opt S_textures[] = {
  O_CHOICE("Texture preloading", "EmuCore/GS/texture_preloading", C_preload, "2"),
  O_CHOICE("Texture upscaling", "EmuCore/GS/TextureUpscale", C_texup, "0"),
  O_BOOL("Load texture replacements", "EmuCore/GS/LoadTextureReplacements", "false"),
  O_BOOLG("Asynchronous texture loading", "EmuCore/GS/LoadTextureReplacementsAsync", "true",
          "EmuCore/GS/LoadTextureReplacements", "false"),
  O_BOOLG("Precache replacement textures", "EmuCore/GS/PrecacheTextureReplacements", "false",
          "EmuCore/GS/LoadTextureReplacements", "false"),
};
inline constexpr Opt S_gs_advanced[] = {
  O_CHOICE("Hardware download mode", "EmuCore/GS/HWDownloadMode", C_hwdl, "0"),
  O_CHOICE("GS multithreading", GS_BACK_THREAD_KEY, C_offOn, "0"),
  O_BOOLG("Run the GS back thread on its own core", "EmuCore/GS/GSBackThreadUseCore3", "false", GS_BACK_THREAD_KEY, "0"),
  O_RENDERER("Present from the GS back thread", "EmuCore/GS/GSBackThreadQueueVsync", C_bool, "true", "18", GS_BACK_THREAD_KEY, "0"),
  O_CHOICE("Texture barriers", "EmuCore/GS/OverrideTextureBarriers", C_barrier, "-1"),
  O_CHOICE("Depth feedback", "EmuCore/GS/DepthFeedbackMode", C_depth_feedback, "1"),
  O_BOOL("Coalesce render passes", "EmuCore/GS/CoalesceRenderPasses", "false"),
  O_VK("Precompile pipelines", "EmuCore/GS/PrecompilePipelines", "true"),
  O_VK("Disable framebuffer fetch", "EmuCore/GS/DisableFramebufferFetch", "false"),
  O_VK("Vulkan raster order barriers", "EmuCore/GS/HWROVBarriersVK", "false"),
};
inline constexpr Opt S_gs_fixes[] = {
  O_BOOL("Manual hardware fixes", GS_FIXES_KEY, "false"),
  O_HW_RANGE("Skip draw start", "EmuCore/GS/UserHacks_SkipDraw_Start", 0, 5000, "0"),
  O_HW_RANGE("Skip draw end", "EmuCore/GS/UserHacks_SkipDraw_End", 0, 5000, "0"),
  O_HW_FIX("Auto flush", "EmuCore/GS/UserHacks_AutoFlushLevel", C_flush, "0"),
  O_HW_FIX("CPU framebuffer conversion", "EmuCore/GS/UserHacks_CPU_FB_Conversion", C_bool, "false"),
  O_HW_FIX("Disable depth support", "EmuCore/GS/UserHacks_DisableDepthSupport", C_bool, "false"),
  O_HW_FIX("Texture inside render target", "EmuCore/GS/UserHacks_TextureInsideRt", C_rtTexture, "0"),
  O_HW_FIX("Half-pixel offset", "EmuCore/GS/UserHacks_HalfPixelOffset", C_halfPixel, "0"),
  O_HW_FIX("Align sprites", "EmuCore/GS/UserHacks_align_sprite_X", C_bool, "false"),
  O_HW_RANGE("CPU sprite rendering width (×64 px)", "EmuCore/GS/UserHacks_CPUSpriteRenderBW", 0, 10, "0"),
  O_HW_FIX("CPU sprite rendering level", "EmuCore/GS/UserHacks_CPUSpriteRenderLevel", C_cpu_sprite_level, "0"),
  O_HW_FIX("CPU CLUT rendering", "EmuCore/GS/UserHacks_CPUCLUTRender", C_cpu_clut, "0"),
  O_HW_FIX("GPU target CLUT", "EmuCore/GS/UserHacks_GPUTargetCLUTMode", C_clut, "0"),
  O_HW_FIX("Disable safe features", "EmuCore/GS/UserHacks_Disable_Safe_Features", C_bool, "false"),
  O_HW_FIX("Disable render fixes", "EmuCore/GS/UserHacks_DisableRenderFixes", C_bool, "false"),
  O_HW_FIX("Preload frames with GS data", "EmuCore/GS/preload_frame_with_gs_data", C_bool, "false"),
  O_HW_FIX("Disable partial invalidation", "EmuCore/GS/UserHacks_DisablePartialInvalidation", C_bool, "false"),
  O_HW_FIX("Read texture cache on close", "EmuCore/GS/UserHacks_ReadTCOnClose", C_bool, "false"),
  O_HW_FIX("Estimate texture region", "EmuCore/GS/UserHacks_EstimateTextureRegion", C_bool, "false"),
  O_HW_FIX("Rewrite large ST coordinates", "EmuCore/GS/UserHacks_RewriteLargeST", C_bool, "false"),
  O_HW_FIX("GPU palette conversion", "EmuCore/GS/paltex", C_bool, "false"),
  O_HW_FIX("Draw buffering", "EmuCore/GS/UserHacks_DrawBuffering", C_bool, "false"),
  O_HW_FIX("Limit 24-bit depth", "EmuCore/GS/UserHacks_Limit24BitDepth", C_depth24, "0"),
  O_HW_FIX("Native scaling", "EmuCore/GS/UserHacks_native_scaling", C_native_scaling, "0"),
  O_HW_FIX("Round sprite offsets", "EmuCore/GS/UserHacks_round_sprite_offset", C_round_sprite, "0"),
  O_HW_FIX("Bilinear hack", "EmuCore/GS/UserHacks_BilinearHack", C_bilinear_hack, "0"),
  O_HW_RANGE("Texture coordinate offset X", "EmuCore/GS/UserHacks_TCOffsetX", -4096, 4096, "0"),
  O_HW_RANGE("Texture coordinate offset Y", "EmuCore/GS/UserHacks_TCOffsetY", -4096, 4096, "0"),
  O_HW_FIX("Merge post-processing sprites", "EmuCore/GS/UserHacks_merge_pp_sprite", C_bool, "false"),
  O_HW_FIX("Force even sprite position", "EmuCore/GS/UserHacks_ForceEvenSpritePosition", C_bool, "false"),
  O_HW_FIX("Native palette draws", "EmuCore/GS/UserHacks_NativePaletteDraw", C_bool, "false"),
  O_HW_FIX("Native counter draws", "EmuCore/GS/UserHacks_NativeCounterDraws", C_bool, "false"),
};
// Audout plays stereo only, so no output backend or surround choice.
inline constexpr Opt S_audio[] = {
  O_SCALED_RANGE("Volume", "SPU2/Output/StandardVolume", 0, 200, 5, "100", 1, "%"),
  O_SCALED_RANGE("Fast-forward volume", "SPU2/Output/FastForwardVolume", 0, 200, 5, "100", 1, "%"),
  O_BOOL("Mute", "SPU2/Output/OutputMuted", "false"),
  O_CHOICE("Sync mode", "SPU2/Output/SyncMode", C_sync, "TimeStretch"),
  O_RANGE("Buffer size (ms)", "SPU2/Output/BufferMS", 15, 200, 5, "50"),
  O_BOOL("Minimal output latency", "SPU2/Output/OutputLatencyMinimal", "false"),
  O_RANGEG("Output latency (ms)", "SPU2/Output/OutputLatencyMS", 10, 200, 5, "20",
           "SPU2/Output/OutputLatencyMinimal", "true"),
  O_BOOL("Lightweight audio mixing", "SPU2/Output/LightweightMode", "false"),
};
// CPU Boost stays first: the launcher draws it as a card at the top of the page.
inline constexpr Opt S_emu[] = {
#ifdef __SWITCH2__
  O_BOOT("CPU Boost", CPU_BOOST_KEY, C_bool, "true"),
#endif
  O_BOOL("Automatic game patches", PATCHES_KEY, "true"),
  O_BOOL("Cheats", CHEATS_KEY, "false"),
  O_SUB("Speed - Frame pacing", SCR_FRAMERATE),
  O_SUB("CPU emulation", SCR_CPU),
  O_SUB("Manual game fixes", SCR_GAMEFIXES),
};
inline constexpr Opt S_cpu[] = {
  O_CHOICE("EE cycle rate",    "EmuCore/Speedhacks/EECycleRate", C_eecr, "0"),
  O_CHOICE("EE cycle skip",    "EmuCore/Speedhacks/EECycleSkip", C_eecs, "0"),
  O_BOOL("Dynamic EE cycle rate", "EmuCore/Speedhacks/DynamicEECycleRate", "false"),
  O_BOOL("MTVU (multi-threaded VU1)", "EmuCore/Speedhacks/vuThread", "true"),
  O_BOOL("Instant VU1",      "EmuCore/Speedhacks/vu1Instant", "true"),
  O_BOOL("VU flag hack",     "EmuCore/Speedhacks/vuFlagHack", "true"),
  O_BOOL("VU1 ahead-of-time compiler", "EmuCore/CPU/Recompiler/EnableVUAot", "true"),
  O_BOOL("IOP thread",       "EmuCore/Speedhacks/iopThread", "false"),
  O_BOOL("Fast CDVD",        "EmuCore/Speedhacks/fastCDVD", "false"),
  O_BOOL("Automatic performance tuning", "EmuCore/AutoTune", "false"),
  O_BOOL("Pin emulation threads", "EmuCore/EnableThreadPinning", "false"),
  O_SUB("Advanced CPU settings", SCR_ADVANCED),
};
inline constexpr Opt S_gamefixes[] = {
  O_BOOL("Automatic game fixes", "EmuCore/EnableGameFixes", "true"),
  O_BOOL("FPU multiply hack",       "EmuCore/Gamefixes/FpuMulHack", "false"),
  O_BOOL("Software renderer for FMVs", "EmuCore/Gamefixes/SoftwareRendererFMVHack", "false"),
  O_BOOL("Skip MPEG hack",          "EmuCore/Gamefixes/SkipMPEGHack", "false"),
  O_BOOT("Preload TLB hack (next boot)", "EmuCore/Gamefixes/GoemonTlbHack", C_bool, "false"),
  O_BOOL("EE timing hack",          "EmuCore/Gamefixes/EETimingHack", "false"),
  O_BOOL("Instant DMA hack",        "EmuCore/Gamefixes/InstantDMAHack", "false"),
  O_BOOL("OPH flag hack",           "EmuCore/Gamefixes/OPHFlagHack", "false"),
  O_BOOL("Emulate GIF FIFO",        "EmuCore/Gamefixes/GIFFIFOHack", "false"),
  O_BOOL("DMA busy hack",           "EmuCore/Gamefixes/DMABusyHack", "false"),
  O_BOOL("Delay VIF1 stalls",       "EmuCore/Gamefixes/VIF1StallHack", "false"),
  O_BOOL("Emulate VIF FIFO",        "EmuCore/Gamefixes/VIFFIFOHack", "false"),
  O_BOOL("Full VU0 synchronization","EmuCore/Gamefixes/FullVU0SyncHack", "false"),
  O_BOOL("VU I-bit hack",           "EmuCore/Gamefixes/IbitHack", "false"),
  O_BOOL("VU add/sub hack",         "EmuCore/Gamefixes/VuAddSubHack", "false"),
  O_BOOL("VU overflow hack",        "EmuCore/Gamefixes/VUOverflowHack", "false"),
  O_BOOL("VU synchronization",      "EmuCore/Gamefixes/VUSyncHack", "false"),
  O_BOOL("VU XGKick sync",          "EmuCore/Gamefixes/XgKickHack", "false"),
  O_BOOL("Force blit FPS detection", "EmuCore/Gamefixes/BlitInternalFPSHack", "false"),
};
inline constexpr Opt S_advanced[] = {
  O_CHOICE("EE rounding mode",       "EmuCore/CPU/FPU.Roundmode", C_rounding, "3"),
  O_CHOICE("EE division rounding mode", "EmuCore/CPU/FPUDiv.Roundmode", C_rounding, "0"),
  O_CHOICE("EE clamping mode",       "Wrapper/EEClampMode", C_eeClamp, "1"),
  O_CHOICE("VU0 rounding mode",      "EmuCore/CPU/VU0.Roundmode", C_rounding, "3"),
  O_CHOICE("VU0 clamping mode",      "Wrapper/VU0ClampMode", C_vuClamp, "1"),
  O_CHOICE("VU1 rounding mode",      "EmuCore/CPU/VU1.Roundmode", C_rounding, "3"),
  O_CHOICE("VU1 clamping mode",      "Wrapper/VU1ClampMode", C_vuClamp, "1"),
  O_BOOL("Guarded FPU addition and subtraction", "EmuCore/CPU/Recompiler/fpuGuardedAddSub", "false"),
  O_BOOL("Exact FPU emulation", "EmuCore/CPU/Recompiler/fpuExactMode", "false"),
  O_BOOL("Exact VU0 emulation", "EmuCore/CPU/Recompiler/vu0ExactMode", "false"),
  O_BOOL("Exact VU1 emulation", "EmuCore/CPU/Recompiler/vu1ExactMode", "false"),
  O_BOOL("Wait loop detection",    "EmuCore/Speedhacks/WaitLoop", "true"),
  O_BOOL("INTC spin detection",    "EmuCore/Speedhacks/IntcStat", "true"),
  O_BOOLG("VU program cache", "EmuCore/CPU/Recompiler/EnableVUProgramCache", "false", "EmuCore/CPU/Recompiler/EnableVUAot", "true"),
  O_BOOL("EE recompiler",          "EmuCore/CPU/Recompiler/EnableEE", "true"),
  O_BOOL("VU0 recompiler",         "EmuCore/CPU/Recompiler/EnableVU0", "true"),
  O_BOOL("VU1 recompiler",         "EmuCore/CPU/Recompiler/EnableVU1", "true"),
  O_BOOL("IOP recompiler",         "EmuCore/CPU/Recompiler/EnableIOP", "true"),
};
inline constexpr Opt S_controller[] = {
  O_SUB("Controller 1", SCR_PAD1),
  O_SUB("Controller 2", SCR_PAD2),
};
// Controller 1 is always a DualShock 2 (the launcher writes it); controller 2 can be unplugged. A page holds the
// controller's settings, then the Switch button each DualShock 2 button is read from.
inline constexpr char PAD2_TYPE_KEY[] = "Pad2/Type";
#define PAD_OPTIONS(port, gk, go) \
  O_SCALED_RANGEG("Analog deadzone", port "/Deadzone", 0, 100, 1, "0", 100, "%", gk, go), \
  O_SCALED_RANGEG("Analog sensitivity", port "/AxisScale", 1, 200, 1, "1.33", 100, "%", gk, go), \
  O_CHOICEG("Invert left stick", port "/InvertL", C_invert, "0", gk, go), \
  O_CHOICEG("Invert right stick", port "/InvertR", C_invert, "0", gk, go), \
  O_BOOLG("Diagonal scale correction", port "/UseDiagonalScaleCorrection", "false", gk, go), \
  O_SCALED_RANGEG("Pressure modifier strength", port "/PressureModifier", 1, 100, 1, "0.5", 100, "%", gk, go), \
  O_SCALED_RANGEG("Large motor vibration", port "/LargeMotorScale", 0, 200, 5, "1", 100, "%", gk, go), \
  O_SCALED_RANGEG("Small motor vibration", port "/SmallMotorScale", 0, 200, 5, "1", 100, "%", gk, go)
inline constexpr Opt S_pad1_options[] = { PAD_OPTIONS("Pad1", nullptr, nullptr) };
inline constexpr Opt S_pad2_options[] = {
  O_CHOICE("Controller type", PAD2_TYPE_KEY, C_pad, "DualShock2"),
  PAD_OPTIONS("Pad2", PAD2_TYPE_KEY, "None"),
};
#undef PAD_OPTIONS

template <std::size_t N>
constexpr auto MakePadScreen(const Opt (&settings)[N], std::size_t player, const char *gateKey, const char *gateOff)
{
  std::array<Opt, N + std::size(PAD_BINDINGS)> options{};
  for(std::size_t i=0;i<N;++i)
    options[i]=settings[i];
  for(std::size_t i=0;i<std::size(PAD_BINDINGS);++i){
    const auto &binding=PAD_BINDINGS[i];
    options[N+i]=O_CHOICEG(binding.label,binding.keys[player],C_npad,binding.default_button,gateKey,gateOff);
  }
  return options;
}
inline constexpr auto S_pad1 = MakePadScreen(S_pad1_options, 0, nullptr, nullptr);
inline constexpr auto S_pad2 = MakePadScreen(S_pad2_options, 1, PAD2_TYPE_KEY, "None");
inline constexpr Opt S_system[] = {
  O_BOOL("Memory card 1", "MemoryCards/Slot1_Enable", "true"),
  O_FILEG("Memory card 1 file", "MemoryCards/Slot1_Filename", "Mcd001.ps2", ".ps2,.mcd,.mc,.bin",
          "MemoryCards/Slot1_Enable", "false"),
  O_BOOL("Memory card 2", "MemoryCards/Slot2_Enable", "true"),
  O_FILEG("Memory card 2 file", "MemoryCards/Slot2_Filename", "Mcd002.ps2", ".ps2,.mcd,.mc,.bin",
          "MemoryCards/Slot2_Enable", "false"),
  O_BOOL("Back up memory cards", "MemoryCards/AutoBackup", "false"),
  O_BOOL("Back up overwritten save states", "EmuCore/BackupSavestate", "true"),
  O_BOOL("Save a resume state when exiting", "EmuCore/SaveStateOnShutdown", "false"),
  O_BOOL("Autosave when exiting", "EmuCore/AutosaveOnShutdown", "false"),
  O_RANGE("Autosave every (minutes, 0 = off)", "EmuCore/AutosaveIntervalMinutes", 0, 180, 1, "0"),
  O_RANGE("Autosaves to keep", "EmuCore/AutosaveKeep", 1, 5, 1, "3"),
  O_SUB("BIOS and boot", SCR_BOOT),
  O_SUB("Network and HDD", SCR_NETWORK),
};
inline constexpr Opt S_framerate[] = {
  O_BOOL("VSync", "EmuCore/GS/VsyncEnable", "false"),
  O_BOOL("Sync to host refresh rate", "EmuCore/GS/SyncToHostRefreshRate", "false"),
  O_BOOLG("Use VSync for timing", "EmuCore/GS/UseVSyncForTiming", "false",
          "EmuCore/GS/SyncToHostRefreshRate", "false"),
  O_CHOICE("Maximum frame latency", "EmuCore/GS/VsyncQueueSize", C_queue, "2"),
  O_BOOL("Skip duplicate frames", "EmuCore/GS/SkipDuplicateFrames", "false"),
  O_BOOL("Advanced frame display", "EmuCore/GS/AdvancedFrameDisplay", "false"),
  O_SCALED_RANGE("Normal speed", "Framerate/NominalScalar", 10, 300, 1, "1", 100, "%"),
  O_SCALED_RANGE("Fast-forward speed", "Framerate/TurboScalar", 25, 400, 5, "2", 100, "%"),
  O_SCALED_RANGE("Slow-motion speed", "Framerate/SlomoScalar", 10, 100, 5, "0.5", 100, "%"),
  O_SCALED_RANGE("NTSC frame rate", "EmuCore/GS/FramerateNTSC", 15, 120, 1, "59.94", 1, " Hz"),
  O_SCALED_RANGE("PAL frame rate",  "EmuCore/GS/FrameratePAL",  12, 100, 1, "50", 1, " Hz"),
};
inline constexpr Opt S_boot[] = {
  O_FILE("BIOS", "Filenames/BIOS", "", ".bin,.rom,.bios"),
  O_BOOT("Fast boot", "EmuCore/EnableFastBoot", C_bool, "true"),
  O_BOOT("Fast-forward the boot", "EmuCore/EnableFastBootFastForward", C_bool, "false"),
};
#define O_ETH_BOOL(l,k,d) O_BOOLG(l,k,d,ETHERNET_KEY,"false")
#define O_ETH_TEXT(l,k,d) O_TEXTG(l,k,d,ETHERNET_KEY,"false")
inline constexpr Opt S_network[] = {
  O_BOOL("Ethernet adapter", ETHERNET_KEY, "false"),
  O_CHOICEG("Network mode", "DEV9/Eth/EthApi", C_network, "Sockets", ETHERNET_KEY, "false"),
  O_ETH_BOOL("Intercept DHCP", "DEV9/Eth/InterceptDHCP", "false"),
  O_ETH_TEXT("PS2 IP address", "DEV9/Eth/PS2IP", "0.0.0.0"),
  O_ETH_BOOL("Automatic subnet mask", "DEV9/Eth/AutoMask", "true"),
  O_ETH_TEXT("Subnet mask", "DEV9/Eth/Mask", "0.0.0.0"),
  O_ETH_BOOL("Automatic gateway", "DEV9/Eth/AutoGateway", "true"),
  O_ETH_TEXT("Gateway", "DEV9/Eth/Gateway", "0.0.0.0"),
  O_CHOICEG("Primary DNS mode", "DEV9/Eth/ModeDNS1", C_dns, "Auto", ETHERNET_KEY, "false"),
  O_ETH_TEXT("Primary DNS", "DEV9/Eth/DNS1", "0.0.0.0"),
  O_CHOICEG("Secondary DNS mode", "DEV9/Eth/ModeDNS2", C_dns, "Auto", ETHERNET_KEY, "false"),
  O_ETH_TEXT("Secondary DNS", "DEV9/Eth/DNS2", "0.0.0.0"),
  O_ETH_BOOL("Local Link host", "DEV9/Eth/LocalLinkHost", "false"),
  O_ETH_TEXT("Local Link address", "DEV9/Eth/LocalLinkAddress", ""),
  O_RANGEG("Local Link port", "DEV9/Eth/LocalLinkPort", 1, 65535, 1, "19072", ETHERNET_KEY, "false"),
  O_RANGEG("Local Link peer ID", "DEV9/Eth/LocalLinkPeerId", 1, 65533, 1, "1", ETHERNET_KEY, "false"),
  O_ETH_TEXT("Local Link room code", "DEV9/Eth/LocalLinkRoomCode", ""),
  O_BOOL("Hard disk", HDD_KEY, "false"),
  O_FILEG("Hard disk image", "DEV9/Hdd/HddFile", "DEV9hdd.raw", ".raw,.img,.hdd", HDD_KEY, "false"),
};
#undef O_ETH_BOOL
#undef O_ETH_TEXT
inline constexpr Opt S_ra_advanced[] = {
  O_BOOL("Encore mode", "Achievements/EncoreMode", "false"),
  O_BOOL("Spectator mode", "Achievements/SpectatorMode", "false"),
  O_BOOL("Unofficial achievements", "Achievements/UnofficialTestMode", "false"),
  O_BOOL("Leaderboard notifications", "Achievements/LeaderboardNotifications", "true"),
  O_BOOL("Achievement progress indicators", "Achievements/Overlays", "true"),
  O_BOOL("Leaderboard tracking overlay", "Achievements/LBOverlays", "true"),
  O_RANGE("Notification duration (seconds)", "Achievements/NotificationsDuration", 3, 30, 1, "5"),
  O_RANGE("Leaderboard duration (seconds)", "Achievements/LeaderboardsDuration", 3, 30, 1, "10"),
  O_SCALED_RANGE("Notification size", "Achievements/NotificationScale", 50, 250, 5, "100", 1, "%"),
  O_CHOICE("Notification position", "Achievements/NotificationPosition", C_achievements_position, "8"),
  O_CHOICE("Progress overlay position", "Achievements/OverlayPosition", C_achievements_position, "8"),
  O_BOOL("Sound effects", "Achievements/SoundEffects", "true"),
  O_BOOLG("Game information sound", "Achievements/InfoSound", "true", "Achievements/SoundEffects", "false"),
  O_BOOLG("Achievement unlock sound", "Achievements/UnlockSound", "true", "Achievements/SoundEffects", "false"),
  O_BOOLG("Leaderboard submission sound", "Achievements/LBSubmitSound", "true", "Achievements/SoundEffects", "false"),
};
inline constexpr Opt S_launcher[] = {
  O_CHOICE("Language",          "Wrapper/Language",       C_launcherLanguage, "system"),
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "animated"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "6"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_BOOL("Show game titles",  "Wrapper/ShowGameTitles", "true"),
  O_BOOL("Show region flags", "Wrapper/ShowRegionFlags", "true"),
  O_BOOL("Show custom settings badges", "Wrapper/ShowCustomSettingsBadges", "true"),
  O_BOOL("UI animations",     "Wrapper/UiAnimations",   "true"),
  O_BOOL("Sound effects",     "Wrapper/UiSounds",       "true"),
  O_BOOL("Check updates at boot", "Wrapper/CheckUpdatesAtBoot", "true"),
};
struct Screen { const char *title; const Opt *opts; int n; };
template <int N>
constexpr Screen MakeScreen(const char *title, const Opt (&options)[N])
{
  return {title, options, N};
}
template <std::size_t N>
constexpr Screen MakeScreen(const char *title, const std::array<Opt, N> &options)
{
  return {title, options.data(), static_cast<int>(N)};
}
inline constexpr Screen g_screens[SCR_COUNT] = {
  MakeScreen("Graphics", S_graphics),
  MakeScreen("Display", S_display),
  MakeScreen("On-screen display", S_osd),
  MakeScreen("Rendering", S_enhance),
  MakeScreen("Manual graphics fixes", S_gs_fixes),
  MakeScreen("Audio", S_audio),
  MakeScreen("Emulation", S_emu),
  MakeScreen("Advanced CPU settings", S_advanced),
  MakeScreen("Manual game fixes", S_gamefixes),
  MakeScreen("Speed - Frame pacing", S_framerate),
  MakeScreen("Controllers", S_controller),
  MakeScreen("System", S_system),
  MakeScreen("Post-processing", S_post),
  MakeScreen("Textures", S_textures),
  MakeScreen("Advanced graphics", S_gs_advanced),
  MakeScreen("CPU emulation", S_cpu),
  MakeScreen("Controller 1", S_pad1),
  MakeScreen("Controller 2", S_pad2),
  MakeScreen("BIOS and boot", S_boot),
  MakeScreen("Network and HDD", S_network),
  MakeScreen("Achievement options", S_ra_advanced),
};

#undef O_CHOICE
#undef O_RANGE
#undef O_SCALED_RANGE
#undef O_SCALED_RANGEG
#undef O_SUB
#undef O_CHOICEG
#undef O_RANGEG
#undef O_BOOL
#undef O_BOOLG
#undef O_BOOT
#undef O_TEXTG
#undef O_FILEG
#undef O_FILE
#undef O_RENDERER
#undef O_VK
#undef O_HW_FIX
#undef O_HW_RANGE
} // namespace SwitchSettings

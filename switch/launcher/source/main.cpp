#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <map>
#include <unordered_map>
#include <iterator>
#include <array>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <unordered_set>
#include <deque>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include "griddb.h"
#include "ui_audio.h"
#include "retroachievements.h"
#include "launcher_update.h"
#include "localization.h"

#include "nx2_platform.h"

// SDL uses Xbox button names.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A
#define BTN_X        SDL_CONTROLLER_BUTTON_Y
#define BTN_Y        SDL_CONTROLLER_BUTTON_X

// ARMSX2-nx's data root. The cores read inis/PCSX2.ini (the settings written here before each launch) and launch.ini
// (the game to boot and this launcher's path, to come back to), and keep their own folders (memcards, sstates, ...)
// under it.
static const char *DATA_DIR    = "sdmc:/switch/armsx2-nx";
static const char *LAUNCHER_INI= "sdmc:/switch/armsx2-nx/launcher.ini";
static const char *INIS_DIR    = "sdmc:/switch/armsx2-nx/inis";
static const char *EMU_INI     = "sdmc:/switch/armsx2-nx/inis/PCSX2.ini";
static const char *LAUNCH_INI  = "sdmc:/switch/armsx2-nx/launch.ini";
// Written by a core whose game could not start (it has no screen of its own for that): shown here, then removed.
static const char *LAUNCH_ERROR= "sdmc:/switch/armsx2-nx/launch_error.txt";
static const char *LOGS_DIR    = "sdmc:/switch/armsx2-nx/logs";
static const char *LAUNCHER_LOG= "sdmc:/switch/armsx2-nx/logs/launcher.log";
static const char *LAUNCHER_PREV_LOG= "sdmc:/switch/armsx2-nx/logs/launcher.prev.log";
static const char *COVERS_DIR  = "sdmc:/switch/armsx2-nx/covers";
static const char *GAMECFG_DIR = "sdmc:/switch/armsx2-nx/gamecfg";
static const char *DEF_GAMEDIR = "sdmc:/switch/armsx2-nx/games";
static const char *BIOS_DIR    = "sdmc:/switch/armsx2-nx/bios";
static const char *EMU_HOST_DIR= "sdmc:/switch/armsx2-nx/.emu";
static const char *LAUNCHER_NRO= "sdmc:/switch/armsx2-nx2.nro";
static std::string g_launcherNroPath = LAUNCHER_NRO;

// The launcher's log. Every line is flushed: on the Switch 2 a crash leaves no report behind.
static FILE *g_logFile = nullptr;
static void launcherLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void launcherLog(const char *format, ...) {
  if(!g_logFile) return;
  const unsigned long long ms=armTicksToNs(armGetSystemTick())/1000000ULL;
  fprintf(g_logFile,"[%6llu.%03llu] ",ms/1000,ms%1000);
  va_list args;
  va_start(args,format);
  vfprintf(g_logFile,format,args);
  va_end(args);
  fputc('\n',g_logFile);
  fflush(g_logFile);
}
static void openLauncherLog() {
  mkdir("sdmc:/switch",0777);
  mkdir(DATA_DIR,0777);
  mkdir(LOGS_DIR,0777);
  // The previous run's log stays next to this one: a crash is only told by the log it left.
  remove(LAUNCHER_PREV_LOG);
  rename(LAUNCHER_LOG,LAUNCHER_PREV_LOG);
  g_logFile=fopen(LAUNCHER_LOG,"w");
}
static void closeLauncherLog() {
  if(g_logFile) fclose(g_logFile);
  g_logFile=nullptr;
}

struct KV { std::string k, v; };
struct Store {
  std::vector<KV> kv;
  mutable std::unordered_map<std::string,size_t> index;
  mutable size_t indexedSize=SIZE_MAX;
};

static Store g_global;
static Store g_game;
static Store g_titles;
static Store g_library;
static Store *g_active = &g_global;
static const char *TITLES_INI = "sdmc:/switch/armsx2-nx/titles.ini";
static LauncherLocalization g_localization;

static const char* tr(const char* source)
{
  return source ? g_localization.Translate(source).data() : "";
}

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
static void ensureStoreIndex(const Store &s) {
  if(s.indexedSize==s.kv.size())return;
  s.index.clear();s.index.reserve(s.kv.size());
  for(size_t item=0;item<s.kv.size();item++)s.index[s.kv[item].k]=item;
  s.indexedSize=s.kv.size();
}
static void invalidateStoreIndex(Store &s) {
  s.index.clear();s.indexedSize=SIZE_MAX;
}
static const char *storeGet(Store &s, const char *key, const char *def) {
  ensureStoreIndex(s);const auto found=s.index.find(key);
  return found==s.index.end()?def:s.kv[found->second].v.c_str();
}
static void storeSet(Store &s, const char *key, const char *val) {
  ensureStoreIndex(s);const auto found=s.index.find(key);
  if(found!=s.index.end()){s.kv[found->second].v=val;return;}
  s.kv.push_back({key,val});s.index[s.kv.back().k]=s.kv.size()-1;s.indexedSize=s.kv.size();
}
static void storeRemove(Store &s, const char *key) {
  ensureStoreIndex(s);const auto found=s.index.find(key);if(found==s.index.end())return;
  s.kv.erase(s.kv.begin()+found->second);invalidateStoreIndex(s);
}
static bool storeHas(const Store &s, const char *key) {
  ensureStoreIndex(s);return s.index.find(key)!=s.index.end();
}
static void storeRemovePrefix(Store &s, const char *prefix) {
  const size_t length = strlen(prefix);
  s.kv.erase(std::remove_if(s.kv.begin(), s.kv.end(), [&](const KV &entry) {
    return entry.k.compare(0, length, prefix) == 0;
  }), s.kv.end());
  invalidateStoreIndex(s);
}
static bool recoverAtomicFile(const std::string &path);
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  invalidateStoreIndex(s);
  if (!recoverAtomicFile(path)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof(line), f)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (!k.empty()) s.kv.push_back({ k, v });
  }
  fclose(f);
}

static bool queryRegularFile(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return S_ISREG(st.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static bool regularFileExists(const std::string &path) {
  bool exists = false;
  return queryRegularFile(path, exists) && exists;
}

// This launcher's NRO (argv[0]): the cores chainload it when a game ends, and the updater replaces it.
static std::string resolveLauncherPath(const char *argument) {
  std::string path=argument?argument:"";
  if(path.rfind("/switch/",0)==0) path.insert(0,"sdmc:");
  else if(path.rfind("switch/",0)==0) path.insert(0,"sdmc:/");
  if(path.rfind("sdmc:/",0)!=0||path.size()>=FS_MAX_PATH||path.find_first_of("\"\r\n")!=std::string::npos) return {};
  std::string extension=path.size()>=4?path.substr(path.size()-4):std::string();
  std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){ return (char)std::tolower(c); });
  return extension==".nro"&&regularFileExists(path)?path:std::string();
}

static bool normalizeLauncherTheme(Store &values){
  const char *theme=storeGet(values,"Wrapper/Theme","animated");
  if(!strcmp(theme,"animated")||!strcmp(theme,"classic")) return false;
  storeSet(values,"Wrapper/Theme","animated");
  return true;
}

static bool setStoreDefault(Store &store,const char *key,const char *value) {
  if(storeHas(store,key)) return false;
  storeSet(store,key,value);
  return true;
}

static bool setStoreValue(Store &store,const char *key,const char *value) {
  if(!strcmp(storeGet(store,key,""),value)&&storeHas(store,key)) return false;
  storeSet(store,key,value);
  return true;
}

static bool normalizeRetroAchievementsStore(Store &store) {
  bool changed=false;
  changed|=setStoreDefault(store,"Achievements/Enabled","false");
  changed|=setStoreDefault(store,"Achievements/Username","");
  changed|=setStoreDefault(store,"Achievements/Token","");
  changed|=setStoreDefault(store,"Achievements/ChallengeMode","false");
  changed|=setStoreDefault(store,"Achievements/Notifications","true");
  changed|=setStoreDefault(store,"Achievements/SoundEffects","true");
  changed|=setStoreDefault(store,"Achievements/NotificationsDuration","5");
  if((!storeGet(store,"Achievements/Username","")[0]||
      !storeGet(store,"Achievements/Token","")[0])&&
      !strcmp(storeGet(store,"Achievements/Enabled","false"),"true"))
    changed|=setStoreValue(store,"Achievements/Enabled","false");
  return changed;
}

static void applyGlobalRetroAchievementsSettings(Store &effective) {
  storeRemovePrefix(effective,"Achievements/");
  for(const auto &entry:g_global.kv)
    if(entry.k.compare(0,13,"Achievements/")==0)
      storeSet(effective,entry.k.c_str(),entry.v.c_str());
  normalizeRetroAchievementsStore(effective);
}

static bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, currentExists) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists)) return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0) return false;
    fsdevCommitDevice("sdmc");
    currentExists = true;
    oldExists = false;
  }
  if (tmpExists && remove(tmp.c_str()) != 0) return false;
  if (currentExists && oldExists && remove(old.c_str()) != 0) return false;
  if (tmpExists || oldExists) fsdevCommitDevice("sdmc");
  return true;
}

static bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, hadCurrent) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists) || !tmpExists) return false;
  if (oldExists && remove(old.c_str()) != 0) return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0) return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0) fsdevCommitDevice("sdmc");
  return true;
}

static bool writeAtomicText(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  if (!recoverAtomicFile(path)) return false;
  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file) return false;
  bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
  if (fclose(file) != 0) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!replaceAtomic(path, tmp)) { remove(tmp.c_str()); return false; }
  return true;
}

static bool storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  std::string text = "# ARMSX2-nx launcher\n";
  for (auto &e : s.kv) text += e.k + " = " + e.v + "\n";
  return writeAtomicText(path, text);
}

// The cores' settings: inis/PCSX2.ini, PCSX2's base settings layer ([Section] then Key = Value). The launcher's keys are
// "Section/Key" (the section is everything before the last '/'); only the keys of PCSX2's own sections are written, and
// only those, so what the cores saved themselves (folders, FullscreenUI choices) stays as it was.
static bool emulatorSettingsKey(const std::string &key) {
  static const char *const sections[]={"EmuCore","SPU2","Framerate","Achievements","Pad1","Pad2","Logging"};
  for(const char *section:sections){
    const size_t length=strlen(section);
    if(key.size()>length&&key.compare(0,length,section)==0&&key[length]=='/') return true;
  }
  return false;
}

static bool writeEmulatorSettings(const Store &settings) {
  struct Section { std::string name; std::vector<std::string> lines; };
  std::vector<Section> sections(1);  // [0] holds what comes before the first [section]
  if(!recoverAtomicFile(EMU_INI)) return false;
  if(FILE *file=fopen(EMU_INI,"r")){
    char line[2048];
    while(fgets(line,sizeof(line),file)){
      std::string text=line;
      while(!text.empty()&&(text.back()=='\n'||text.back()=='\r')) text.pop_back();
      const std::string trimmed=trim(text);
      if(trimmed.size()>=2&&trimmed.front()=='['&&trimmed.back()==']')
        sections.push_back({trimmed.substr(1,trimmed.size()-2),{}});
      else sections.back().lines.push_back(text);
    }
    fclose(file);
  }
  auto keyOf=[](const std::string &line){
    const size_t equals=line.find('=');
    return equals==std::string::npos?std::string{}:trim(line.substr(0,equals));
  };
  auto set=[&](const std::string &sectionName,const std::string &key,const std::string &value){
    auto section=std::find_if(sections.begin()+1,sections.end(),[&](const Section &item){ return item.name==sectionName; });
    if(section==sections.end()){ sections.push_back({sectionName,{}}); section=sections.end()-1; }
    for(auto &line:section->lines) if(keyOf(line)==key){ line=key+" = "+value; return; }
    auto end=section->lines.end();
    while(end!=section->lines.begin()&&trim(*(end-1)).empty()) --end;
    section->lines.insert(end,key+" = "+value);
  };
  // No [UI] SettingsVersion here: a core that finds none writes PCSX2's defaults, then puts these keys back.
  for(const auto &entry:settings.kv){
    if(!emulatorSettingsKey(entry.k)) continue;
    const size_t slash=entry.k.rfind('/');
    set(entry.k.substr(0,slash),entry.k.substr(slash+1),entry.v);
  }

  std::string text;
  for(size_t index=0;index<sections.size();index++){
    if(index>0) text+="["+sections[index].name+"]\n";
    for(const auto &line:sections[index].lines) text+=line+"\n";
    if(index>0&&(sections[index].lines.empty()||!trim(sections[index].lines.back()).empty())) text+="\n";
  }
  mkdir(INIS_DIR,0777);
  return writeAtomicText(EMU_INI,text);
}

// The account in the cores' settings follows the launcher's, sign-out included.
static bool syncRetroAchievementsToEmulatorConfig() {
  Store achievements;
  applyGlobalRetroAchievementsSettings(achievements);
  const bool saved=writeEmulatorSettings(achievements);
  launcherLog("RetroAchievements settings %s PCSX2.ini",saved?"written to":"could not be written to");
  return saved;
}

static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game)
    return storeHas(g_game,key) ? storeGet(g_game,key,def) : storeGet(g_global,key,def);
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

enum OType { OT_CHOICE, OT_RANGE, OT_SCALED_RANGE, OT_SUBMENU };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
  int sub;
  const char *gateKey;
  const char *gateOff;
  int multiplier;
  const char *suffix;
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, 1, nullptr }
#define O_SCALED_RANGE(l,k,lo,hi,s,d,m,u) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, m, u }
#define O_SUB(l,scr)           { l, nullptr, OT_SUBMENU, nullptr,0, 0,0,0, nullptr, scr, nullptr, nullptr, 1, nullptr }
#define O_CHOICEG(l,k,c,d,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_RANGEG(l,k,lo,hi,s,d,gk,go) { l, k, OT_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, 1, nullptr }

static const Choice C_backend[]  = { {"Drako3D (native)","18"}, {"Vulkan (NVK)","14"} };
static const Choice C_upscale[]  = { {"1x (native ~480p)","1"},{"1.5x","1.5"},{"2x (~720p)","2"},{"2.5x","2.5"},
                                     {"3x (~1080p)","3"},{"4x (~1440p)","4"},{"5x (~1800p)","5"},{"6x (4K ~2160p)","6"} };
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_offOn[]    = { {"Off","0"}, {"On","1"} };
static const Choice C_aspect[]   = { {"Auto 4:3/3:2","Auto 4:3/3:2"}, {"4:3","4:3"}, {"16:9","16:9"}, {"Stretch","Stretch"} };
static const Choice C_fmvasp[]   = { {"Off","Off"}, {"Auto 4:3/3:2","Auto 4:3/3:2"}, {"4:3","4:3"}, {"16:9","16:9"} };
static const Choice C_filter[]   = { {"Nearest","0"}, {"Bilinear (PS2)","2"}, {"Bilinear (Forced)","1"}, {"Bilinear no-sprite","3"} };
static const Choice C_aniso[]    = { {"Off","0"}, {"2x","2"}, {"4x","4"}, {"8x","8"}, {"16x","16"} };
static const Choice C_osdpos[]   = { {"Off","0"}, {"Top left","1"}, {"Top center","2"}, {"Top right","3"},
                                     {"Bottom left","7"}, {"Bottom center","8"}, {"Bottom right","9"} };
static const Choice C_blend[]    = { {"Minimum","0"}, {"Basic","1"}, {"Medium","2"}, {"High","3"}, {"Full","4"}, {"Maximum","5"} };
static const Choice C_deint[]    = { {"Automatic","0"}, {"Off","1"}, {"Weave TFF","2"}, {"Weave BFF","3"}, {"Bob TFF","4"},
                                     {"Bob BFF","5"}, {"Blend TFF","6"}, {"Blend BFF","7"}, {"Adaptive TFF","8"}, {"Adaptive BFF","9"} };
static const Choice C_dither[]   = { {"Off","0"}, {"Scaled","1"}, {"Unscaled","2"} };
static const Choice C_trifilter[]= { {"Automatic","-1"}, {"Off","0"}, {"Trilinear (PS2)","1"}, {"Trilinear (Forced)","2"} };
static const Choice C_preload[]  = { {"None","0"}, {"Partial","1"}, {"Full","2"} };
static const Choice C_cas[]      = { {"Off","0"}, {"Sharpen only","1"}, {"Sharpen + upscale","2"} };
static const Choice C_hwdl[]     = { {"Accurate","0"}, {"Accurate, full","1"}, {"Disable readbacks","2"},
                                     {"Unsynchronized","3"}, {"Disabled","4"}, {"Asynchronous","5"} };
static const Choice C_sync[]     = { {"TimeStretch","TimeStretch"}, {"Off","Disabled"} };
static const Choice C_eecr[]     = { {"50%","-3"}, {"60%","-2"}, {"75%","-1"}, {"100%","0"}, {"130%","1"}, {"180%","2"}, {"300%","3"} };
static const Choice C_eecs[]     = { {"Off","0"}, {"Mild","1"}, {"Moderate","2"}, {"Maximum","3"} };
static const Choice C_rounding[] = { {"Nearest","0"}, {"Negative","1"}, {"Positive","2"}, {"Chop / Zero","3"} };
static const Choice C_eeClamp[]  = { {"None","0"}, {"Normal","1"}, {"Extra + Preserve Sign","2"}, {"Full","3"} };
static const Choice C_vuClamp[]  = { {"None","0"}, {"Normal","1"}, {"Extra","2"}, {"Extra + Preserve Sign","3"} };
static const Choice C_players[]  = { {"1","None"}, {"2","DualShock2"} };
static const Choice C_launcherLanguage[] = {
  {"System", "system"}, {"English", "en"}, {"Français", "fr"},
  {"Deutsch", "de"}, {"Español", "es"}, {"Italiano", "it"}, {"Português", "pt"},
  {"简体中文", "zh-Hans"}, {"繁體中文", "zh-Hant"}
};
static const Choice C_launcherTheme[] = { {"PS2","animated"}, {"Classic","classic"} };
static const Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
static const Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };

enum { SCR_GRAPHICS, SCR_ENHANCE, SCR_AUDIO, SCR_EMU, SCR_ADVANCED, SCR_GAMEFIXES, SCR_FRAMERATE, SCR_CONTROLLER,
       SCR_LOGGING, SCR_COUNT };

// Keys are PCSX2's ([Section] Key as "Section/Key"), written into inis/PCSX2.ini at launch. The Wrapper/ keys stay in
// the launcher: the renderer picks the core, and a clamping mode is three of PCSX2's switches (expandClampingModes).
static const Opt S_graphics[] = {
  O_CHOICE("Renderer",         "EmuCore/GS/Renderer", C_backend, "18"),
  O_CHOICE("Resolution scale", "EmuCore/GS/upscale_multiplier", C_upscale, "1"),
  O_CHOICE("Aspect ratio",     "EmuCore/GS/AspectRatio", C_aspect, "Auto 4:3/3:2"),
  O_CHOICE("FMV aspect",       "EmuCore/GS/FMVAspectRatioSwitch", C_fmvasp, "Off"),
  O_CHOICE("VSync",            "EmuCore/GS/VsyncEnable", C_bool, "false"),
  O_CHOICE("Skip duplicate frames", "EmuCore/GS/SkipDuplicateFrames", C_bool, "false"),
  O_CHOICE("GS multithreading", "EmuCore/GS/GSBackThreadMode", C_offOn, "0"),
  O_CHOICEG("Run the GS back thread on its own core", "EmuCore/GS/GSBackThreadUseCore3", C_bool, "false",
            "EmuCore/GS/GSBackThreadMode", "0"),
  O_CHOICEG("Present from the GS back thread", "EmuCore/GS/GSBackThreadQueueVsync", C_bool, "true",
            "EmuCore/GS/GSBackThreadMode", "0"),
  O_CHOICE("Texture filtering","EmuCore/GS/filter", C_filter, "2"),
  O_CHOICE("Anisotropic",      "EmuCore/GS/MaxAnisotropy", C_aniso, "0"),
  O_CHOICE("Show FPS",         "EmuCore/GS/OsdShowFPS", C_bool, "false"),
  O_CHOICE("Show speed",       "EmuCore/GS/OsdShowSpeed", C_bool, "false"),
  O_CHOICE("Show CPU usage",   "EmuCore/GS/OsdShowCPU", C_bool, "false"),
  O_CHOICE("Show GPU usage",   "EmuCore/GS/OsdShowGPU", C_bool, "false"),
  O_CHOICE("Show resolution",  "EmuCore/GS/OsdShowResolution", C_bool, "false"),
  O_CHOICE("On-screen messages","EmuCore/GS/OsdMessagesPos", C_osdpos, "1"),
  O_CHOICE("Widescreen patch", "EmuCore/EnableWideScreenPatches", C_bool, "false"),
  O_CHOICE("No-interlace patch","EmuCore/EnableNoInterlacingPatches", C_bool, "false"),
  O_SUB   ("Enhancements...",  SCR_ENHANCE),
};
static const Opt S_enhance[] = {
  O_CHOICE("Blending accuracy","EmuCore/GS/accurate_blending_unit", C_blend, "1"),
  O_CHOICE("Deinterlacing",    "EmuCore/GS/deinterlace_mode", C_deint, "0"),
  O_CHOICE("Dithering",        "EmuCore/GS/dithering_ps2", C_dither, "2"),
  O_CHOICE("Trilinear",        "EmuCore/GS/TriFilter", C_trifilter, "-1"),
  O_CHOICE("Mipmapping",       "EmuCore/GS/hw_mipmap", C_bool, "true"),
  O_CHOICE("Texture preload",  "EmuCore/GS/texture_preloading", C_preload, "2"),
  O_CHOICE("GPU palette conv", "EmuCore/GS/paltex", C_bool, "false"),
  O_CHOICE("Anti-blur",        "EmuCore/GS/pcrtc_antiblur", C_bool, "true"),
  O_CHOICE("FXAA",             "EmuCore/GS/fxaa", C_bool, "false"),
  O_CHOICE("CAS sharpening",   "EmuCore/GS/CASMode", C_cas, "0"),
  O_RANGEG("CAS strength",     "EmuCore/GS/CASSharpness", 0, 100, 5, "50", "EmuCore/GS/CASMode", "0"),
  O_CHOICE("Shade boost",      "EmuCore/GS/ShadeBoost", C_bool, "false"),
  O_RANGEG("  Brightness",     "EmuCore/GS/ShadeBoost_Brightness", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("  Contrast",       "EmuCore/GS/ShadeBoost_Contrast", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("  Saturation",     "EmuCore/GS/ShadeBoost_Saturation", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_CHOICE("Load texture replacements", "EmuCore/GS/LoadTextureReplacements", C_bool, "false"),
  O_CHOICEG("Asynchronous texture loading", "EmuCore/GS/LoadTextureReplacementsAsync", C_bool, "true",
            "EmuCore/GS/LoadTextureReplacements", "false"),
  O_CHOICE("HW download mode", "EmuCore/GS/HWDownloadMode", C_hwdl, "0"),
};
static const Opt S_audio[] = {
  O_RANGE ("Volume",           "SPU2/Output/StandardVolume", 0, 200, 5, "100"),
  O_CHOICE("Sync mode",        "SPU2/Output/SyncMode", C_sync, "TimeStretch"),
  O_RANGE ("Buffer size (ms)", "SPU2/Output/BufferMS", 15, 200, 5, "50"),
  O_RANGE ("Output latency (ms)", "SPU2/Output/OutputLatencyMS", 10, 200, 5, "20"),
};
static const Opt S_emu[] = {
  O_CHOICE("EE cycle rate",    "EmuCore/Speedhacks/EECycleRate", C_eecr, "0"),
  O_CHOICE("EE cycle skip",    "EmuCore/Speedhacks/EECycleSkip", C_eecs, "0"),
  O_CHOICE("Fast boot",        "EmuCore/EnableFastBoot", C_bool, "true"),
  O_CHOICE("MTVU (multi-VU)",  "EmuCore/Speedhacks/vuThread", C_bool, "true"),
  O_CHOICE("Instant VU1",      "EmuCore/Speedhacks/vu1Instant", C_bool, "true"),
  O_CHOICE("VU flag hack",     "EmuCore/Speedhacks/vuFlagHack", C_bool, "true"),
  O_CHOICE("Fast CDVD",        "EmuCore/Speedhacks/fastCDVD", C_bool, "false"),
  O_SUB   ("Frame rate control...", SCR_FRAMERATE),
  O_CHOICE("Sync to refresh",  "EmuCore/GS/SyncToHostRefreshRate", C_bool, "false"),
  O_CHOICE("Game patches",     "EmuCore/EnablePatches", C_bool, "true"),
  O_CHOICE("Cheats",           "EmuCore/EnableCheats", C_bool, "false"),
};
static const Opt S_gamefixes[] = {
  O_CHOICE("FPU multiply hack",       "EmuCore/Gamefixes/FpuMulHack", C_bool, "false"),
  O_CHOICE("Software renderer for FMVs", "EmuCore/Gamefixes/SoftwareRendererFMVHack", C_bool, "false"),
  O_CHOICE("Skip MPEG hack",          "EmuCore/Gamefixes/SkipMPEGHack", C_bool, "false"),
  O_CHOICE("Preload TLB hack",        "EmuCore/Gamefixes/GoemonTlbHack", C_bool, "false"),
  O_CHOICE("EE timing hack",          "EmuCore/Gamefixes/EETimingHack", C_bool, "false"),
  O_CHOICE("Instant DMA hack",        "EmuCore/Gamefixes/InstantDMAHack", C_bool, "false"),
  O_CHOICE("OPH flag hack",           "EmuCore/Gamefixes/OPHFlagHack", C_bool, "false"),
  O_CHOICE("Emulate GIF FIFO",        "EmuCore/Gamefixes/GIFFIFOHack", C_bool, "false"),
  O_CHOICE("DMA busy hack",           "EmuCore/Gamefixes/DMABusyHack", C_bool, "false"),
  O_CHOICE("Delay VIF1 stalls",       "EmuCore/Gamefixes/VIF1StallHack", C_bool, "false"),
  O_CHOICE("Emulate VIF FIFO",        "EmuCore/Gamefixes/VIFFIFOHack", C_bool, "false"),
  O_CHOICE("Full VU0 synchronization","EmuCore/Gamefixes/FullVU0SyncHack", C_bool, "false"),
  O_CHOICE("VU I-bit hack",           "EmuCore/Gamefixes/IbitHack", C_bool, "false"),
  O_CHOICE("VU add/sub hack",         "EmuCore/Gamefixes/VuAddSubHack", C_bool, "false"),
  O_CHOICE("VU overflow hack",        "EmuCore/Gamefixes/VUOverflowHack", C_bool, "false"),
  O_CHOICE("VU synchronization",      "EmuCore/Gamefixes/VUSyncHack", C_bool, "false"),
  O_CHOICE("VU XGKick sync",          "EmuCore/Gamefixes/XgKickHack", C_bool, "false"),
  O_CHOICE("Force blit FPS detection", "EmuCore/Gamefixes/BlitInternalFPSHack", C_bool, "false"),
};
static const Opt S_advanced[] = {
  O_CHOICE("EE rounding mode",       "EmuCore/CPU/FPU.Roundmode", C_rounding, "3"),
  O_CHOICE("EE clamping mode",       "Wrapper/EEClampMode", C_eeClamp, "1"),
  O_CHOICE("EE recompiler",          "EmuCore/CPU/Recompiler/EnableEE", C_bool, "true"),
  O_CHOICE("EE cache (slow)",        "EmuCore/CPU/Recompiler/EnableEECache", C_bool, "false"),
  O_CHOICE("Wait loop detection",    "EmuCore/Speedhacks/WaitLoop", C_bool, "true"),
  O_CHOICE("INTC spin detection",    "EmuCore/Speedhacks/IntcStat", C_bool, "true"),
  O_CHOICE("Pause on TLB miss",      "EmuCore/CPU/Recompiler/PauseOnTLBMiss", C_bool, "false"),
  O_CHOICE("VU0 rounding mode",      "EmuCore/CPU/VU0.Roundmode", C_rounding, "3"),
  O_CHOICE("VU0 clamping mode",      "Wrapper/VU0ClampMode", C_vuClamp, "1"),
  O_CHOICE("VU0 recompiler",         "EmuCore/CPU/Recompiler/EnableVU0", C_bool, "true"),
  O_CHOICE("VU1 rounding mode",      "EmuCore/CPU/VU1.Roundmode", C_rounding, "3"),
  O_CHOICE("VU1 clamping mode",      "Wrapper/VU1ClampMode", C_vuClamp, "1"),
  O_CHOICE("VU1 recompiler",         "EmuCore/CPU/Recompiler/EnableVU1", C_bool, "true"),
  O_CHOICE("IOP recompiler",         "EmuCore/CPU/Recompiler/EnableIOP", C_bool, "true"),
  O_CHOICE("Automatic game fixes",   "EmuCore/EnableGameFixes", C_bool, "true"),
};
static const Opt S_controller[] = {
  O_CHOICE("Controller ports", "Pad2/Type", C_players, "DualShock2"),
};
static const Opt S_framerate[] = {
  O_SCALED_RANGE("Normal speed", "Framerate/NominalScalar", 10, 300, 1, "1", 100, "%"),
  O_SCALED_RANGE("NTSC frame rate", "EmuCore/GS/FramerateNTSC", 15, 120, 1, "59.94", 1, " Hz"),
  O_SCALED_RANGE("PAL frame rate",  "EmuCore/GS/FrameratePAL",  12, 100, 1, "50", 1, " Hz"),
};
static const Opt S_logging[] = {
  O_CHOICE("Verbose log", "Logging/EnableVerbose", C_bool, "false"),
};
static const Opt S_launcher[] = {
  O_CHOICE("Language",          "Wrapper/Language",       C_launcherLanguage, "system"),
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "animated"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "6"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_CHOICE("Show game titles",  "Wrapper/ShowGameTitles", C_bool,          "true"),
  O_CHOICE("Show region flags", "Wrapper/ShowRegionFlags", C_bool,         "true"),
  O_CHOICE("Show custom settings badges", "Wrapper/ShowCustomSettingsBadges", C_bool, "true"),
  O_CHOICE("UI animations",     "Wrapper/UiAnimations",   C_bool,          "true"),
  O_CHOICE("Sound effects",     "Wrapper/UiSounds",       C_bool,          "true"),
  O_CHOICE("Check updates at boot", "Wrapper/CheckUpdatesAtBoot", C_bool,   "true"),
};
struct Screen { const char *title; const Opt *opts; int n; };
static const Screen g_screens[SCR_COUNT] = {
  { "Graphics",            S_graphics,   (int)(sizeof(S_graphics)/sizeof(Opt))   },
  { "Enhancements",        S_enhance,    (int)(sizeof(S_enhance)/sizeof(Opt))    },
  { "Audio",               S_audio,      (int)(sizeof(S_audio)/sizeof(Opt))      },
  { "Emulation / System",  S_emu,        (int)(sizeof(S_emu)/sizeof(Opt))        },
  { "Advanced",            S_advanced,   (int)(sizeof(S_advanced)/sizeof(Opt))   },
  { "Game Fixes",          S_gamefixes,  (int)(sizeof(S_gamefixes)/sizeof(Opt))  },
  { "Frame Rate Control",  S_framerate,  (int)(sizeof(S_framerate)/sizeof(Opt))  },
  { "Controller",          S_controller, (int)(sizeof(S_controller)/sizeof(Opt)) },
  { "Logging",             S_logging,    (int)(sizeof(S_logging)/sizeof(Opt))    },
};

// Clamping modes are one choice in the launcher and separate switches in PCSX2.
static void expandClampingModes(Store &effective) {
  const int ee=atoi(storeGet(effective,"Wrapper/EEClampMode","1"));
  storeSet(effective,"EmuCore/CPU/Recompiler/fpuOverflow",ee>=1?"true":"false");
  storeSet(effective,"EmuCore/CPU/Recompiler/fpuExtraOverflow",ee>=2?"true":"false");
  storeSet(effective,"EmuCore/CPU/Recompiler/fpuFullMode",ee>=3?"true":"false");
  for(int vu=0;vu<2;vu++){
    const int mode=atoi(storeGet(effective,vu?"Wrapper/VU1ClampMode":"Wrapper/VU0ClampMode","1"));
    const std::string prefix=std::string("EmuCore/CPU/Recompiler/vu")+(vu?"1":"0");
    storeSet(effective,(prefix+"Overflow").c_str(),mode>=1?"true":"false");
    storeSet(effective,(prefix+"ExtraOverflow").c_str(),mode>=2?"true":"false");
    storeSet(effective,(prefix+"SignOverflow").c_str(),mode>=3?"true":"false");
  }
}

struct SettingHelpEntry {
  const char *key;
  const char *kind;
  const char *text;
};

static const SettingHelpEntry SETTING_HELP[] = {
  {"Wrapper/Language", "Launcher language",
   "Selects the language used by the SDL launcher. System follows the language configured on the Switch. Game names, paths, server messages and user-entered text are never translated."},
  {"EmuCore/GS/Renderer", "Graphics backend",
   "Chooses the core the game runs in. Drako3D renders PS2 GS draws natively on the Switch 2 GPU; Vulkan runs PCSX2's Vulkan renderer on mesa-nx's NVK driver. Each renderer is its own emulator program, started by the launcher."},
  {"EmuCore/GS/upscale_multiplier", "Resolution / performance",
   "Sets the internal resolution used for 3D rendering. Higher scales improve clarity but increase GPU load. Pre-rendered FMV resolution does not change."},
  {"EmuCore/GS/AspectRatio", "Display geometry",
   "Chooses the shape of the final game image. Auto follows the game (4:3, or 16:9 with a widescreen patch), 4:3 preserves the original PS2 display, and Stretch fills the screen by distorting the image."},
  {"EmuCore/GS/FMVAspectRatioSwitch", "Video display",
   "Overrides the aspect ratio used for pre-rendered movie sequences independently of gameplay. Leave it Off unless a game's FMV is shown with the wrong shape."},
  {"EmuCore/GS/VsyncEnable", "Frame pacing / latency",
   "Synchronizes presentation with the display to reduce tearing. It can add latency."},
  {"EmuCore/GS/SkipDuplicateFrames", "Performance / latency trade-off",
   "Avoids presenting unchanged frames in many 25/30 FPS games. The frame is still emulated; only the repeated present is skipped. It can free GPU time but may worsen pacing."},
  {"EmuCore/GS/GSBackThreadMode", "GS multithreading",
   "Runs GS rendering on a second thread. It helps heavy rendering such as high upscaling; if a game shows graphics glitches, turn it back off."},
  {"EmuCore/GS/GSBackThreadUseCore3", "GS multithreading",
   "Moves the GS front thread to the console's spare CPU core (the last of the Switch 2's six) and gives the GS back thread the core it leaves, so each GS thread runs on a core of its own."},
  {"EmuCore/GS/GSBackThreadQueueVsync", "GS multithreading",
   "Lets the GS back thread present each frame itself (Drako3D). Turn it off if presentation stutters with GS multithreading on."},
  {"EmuCore/GS/filter", "Texture filtering",
   "Selects where bilinear filtering is used on PS2 textures. PS2 follows each game's request; Forced smooths more surfaces, while Nearest keeps hard pixel edges. Forced filtering can blur 2D sprites."},
  {"EmuCore/GS/MaxAnisotropy", "Texture filtering",
   "Improves texture clarity at steep viewing angles. Higher values add some GPU work and are most useful when rendering above native resolution."},
  {"EmuCore/GS/OsdShowFPS", "Performance display",
   "Shows the emulated frame rate while a game is running."},
  {"EmuCore/GS/OsdShowSpeed", "Performance display",
   "Shows the emulation speed as a percentage of full speed."},
  {"EmuCore/GS/OsdShowCPU", "Performance display",
   "Shows the load of the emulation threads (EE, GS, VU) on the CPU."},
  {"EmuCore/GS/OsdShowGPU", "Performance display",
   "Shows the time the GPU spends on each frame."},
  {"EmuCore/GS/OsdShowResolution", "Performance display",
   "Shows the internal rendering resolution."},
  {"EmuCore/GS/OsdMessagesPos", "On-screen display",
   "Places the emulator's status and warning messages on the screen, or hides them."},
  {"EmuCore/EnableWideScreenPatches", "Game patches",
   "Applies widescreen patches from the bundled patch database when one exists for the game. Combine with an Auto or 16:9 aspect ratio."},
  {"EmuCore/EnableNoInterlacingPatches", "Game patches",
   "Applies patches that remove interlacing for games that have one, giving a sharper, steadier image without deinterlacing."},
  {"EmuCore/GS/accurate_blending_unit", "Rendering accuracy",
   "Controls how exactly PS2 blending is emulated. Higher levels fix effects in more games but cost GPU time; Basic is the recommended default."},
  {"EmuCore/GS/deinterlace_mode", "Video display",
   "Chooses how interlaced output is combined. Automatic picks a mode per game; Off shows the raw fields."},
  {"EmuCore/GS/dithering_ps2", "Rendering accuracy",
   "Emulates the PS2's color dithering. Unscaled keeps the original pattern size at higher resolutions; Off removes the pattern."},
  {"EmuCore/GS/TriFilter", "Texture filtering",
   "Controls trilinear filtering between mipmap levels. Automatic uses it where games ask for it."},
  {"EmuCore/GS/hw_mipmap", "Rendering accuracy",
   "Emulates PS2 mipmaps on the GPU. Some games show wrong textures without it; turning it off saves GPU time."},
  {"EmuCore/GS/texture_preloading", "Performance",
   "Uploads whole textures at once instead of only the used areas. Full is usually fastest; lower it if a game runs out of GPU memory."},
  {"EmuCore/GS/paltex", "Rendering",
   "Converts paletted textures on the GPU. It can reduce CPU work but breaks some games."},
  {"EmuCore/GS/pcrtc_antiblur", "Video display",
   "Removes the blur some games apply to their output image."},
  {"EmuCore/GS/fxaa", "Post-processing",
   "Applies FXAA anti-aliasing to the final image."},
  {"EmuCore/GS/CASMode", "Post-processing",
   "AMD FidelityFX Contrast Adaptive Sharpening. Sharpen only keeps the resolution; Sharpen + upscale also scales the image."},
  {"EmuCore/GS/CASSharpness", "Post-processing",
   "Sets how strong the CAS sharpening is."},
  {"EmuCore/GS/ShadeBoost", "Post-processing",
   "Enables brightness, contrast and saturation adjustment of the final image."},
  {"EmuCore/GS/LoadTextureReplacements", "Custom textures",
   "Loads replacement textures from the textures folder of the data root."},
  {"EmuCore/GS/LoadTextureReplacementsAsync", "Custom textures",
   "Loads replacement textures in the background to avoid stutter; they may appear a moment later."},
  {"EmuCore/GS/HWDownloadMode", "Rendering accuracy",
   "Controls how GPU results are read back for the emulated CPU. Accurate is safest; Accurate, full downloads whole targets, which can mean fewer readbacks. Asynchronous (experimental) reads back a frame late. The others are faster but break effects in some games."},
  {"SPU2/Output/StandardVolume", "Audio",
   "Sets the output volume of the emulated sound."},
  {"SPU2/Output/SyncMode", "Audio",
   "TimeStretch keeps sound smooth when the game runs below full speed. Off plays sound as produced."},
  {"SPU2/Output/BufferMS", "Audio latency",
   "Sets how much sound is buffered. Larger buffers absorb uneven performance; smaller ones lower the delay."},
  {"SPU2/Output/OutputLatencyMS", "Audio latency",
   "Sets the output device's latency target."},
  {"EmuCore/Speedhacks/EECycleRate", "Performance",
   "Underclocks (below 100%) or overclocks the emulated Emotion Engine. Underclocking saves CPU time in heavy games but can slow them down."},
  {"EmuCore/Speedhacks/EECycleSkip", "Performance",
   "Skips EE cycles to reduce CPU load. It can cause slowdowns or glitches."},
  {"EmuCore/EnableFastBoot", "Boot",
   "Skips the PS2 BIOS intro and boots the game directly."},
  {"EmuCore/Speedhacks/vuThread", "Performance",
   "Runs VU1 on its own thread (MTVU). A large speedup in most games."},
  {"EmuCore/Speedhacks/vu1Instant", "Performance",
   "Runs VU1 programs immediately. Faster; turn it off only for a game that needs it."},
  {"EmuCore/Speedhacks/vuFlagHack", "Performance",
   "Updates VU status flags only when they are read. Safe for most games."},
  {"EmuCore/Speedhacks/fastCDVD", "Performance",
   "Speeds up disc reads. Shortens loading, but some games break with it."},
  {"EmuCore/GS/SyncToHostRefreshRate", "Frame pacing",
   "Adjusts the emulation speed slightly to match the display's refresh rate."},
  {"EmuCore/EnablePatches", "Game patches",
   "Applies the bundled game fixes and patches from the patch database."},
  {"EmuCore/EnableCheats", "Cheats",
   "Loads cheat files (.pnach) from the cheats folder of the data root. Individual cheats are picked in game from the pause menu (L + R + Plus)."},
  {"EmuCore/CPU/FPU.Roundmode", "EE floating point",
   "Sets the EE FPU rounding mode. Keep Chop / Zero unless a game needs another."},
  {"Wrapper/EEClampMode", "EE floating point",
   "Sets how out-of-range EE float values are clamped. Higher modes fix some games but are slower."},
  {"EmuCore/CPU/Recompiler/EnableEE", "Recompiler",
   "Runs EE code with the ARM64 recompiler. Off uses the interpreter, which is far slower."},
  {"EmuCore/CPU/Recompiler/EnableEECache", "Recompiler",
   "Emulates the EE cache. Needed by very few games and slow."},
  {"EmuCore/Speedhacks/WaitLoop", "Performance",
   "Detects idle loops and skips them."},
  {"EmuCore/Speedhacks/IntcStat", "Performance",
   "Detects spinning on the interrupt status and skips ahead."},
  {"EmuCore/CPU/Recompiler/PauseOnTLBMiss", "Debugging",
   "Pauses when the EE misses its TLB. For debugging only."},
  {"EmuCore/CPU/VU0.Roundmode", "VU floating point", "Sets the VU0 rounding mode."},
  {"Wrapper/VU0ClampMode", "VU floating point", "Sets how out-of-range VU0 float values are clamped."},
  {"EmuCore/CPU/Recompiler/EnableVU0", "Recompiler", "Runs VU0 code with the recompiler."},
  {"EmuCore/CPU/VU1.Roundmode", "VU floating point", "Sets the VU1 rounding mode."},
  {"Wrapper/VU1ClampMode", "VU floating point", "Sets how out-of-range VU1 float values are clamped."},
  {"EmuCore/CPU/Recompiler/EnableVU1", "Recompiler", "Runs VU1 code with the recompiler."},
  {"EmuCore/CPU/Recompiler/EnableIOP", "Recompiler", "Runs IOP code with the recompiler."},
  {"EmuCore/EnableGameFixes", "Game fixes",
   "Applies the per-game fixes from the bundled GameIndex automatically."},
  {"Framerate/NominalScalar", "Frame rate", "Sets the normal emulation speed target."},
  {"EmuCore/GS/FramerateNTSC", "Video timing",
   "Sets the NTSC video rate. Changing it alters game timing."},
  {"EmuCore/GS/FrameratePAL", "Video timing",
   "Sets the PAL video rate. Changing it alters game timing."},
  {"Pad2/Type", "Controllers",
   "Sets how many PS2 controller ports are connected. Use 2 for local multiplayer; the second Switch controller drives port 2. In game, L + R + Plus opens the pause menu."},
  {"Logging/EnableVerbose", "Logging",
   "Adds detailed messages to the emulator's log. Turn it on to report a problem; it can slow emulation down. ARMSX2 writes sdmc:/switch/armsx2-nx/logs/emulog.txt, with boot.txt and exit.txt beside it, and this launcher writes logs/launcher.log there. Send those files with a bug report."},
  {"Wrapper/Theme", "Launcher appearance",
   "Changes the launcher background and visual style. It does not affect gameplay rendering or performance inside the emulator."},
  {"Wrapper/GridColumns", "Library layout",
   "Sets how many game covers appear across each library row. More columns make each cover smaller."},
  {"Wrapper/GridRows", "Library layout",
   "Sets how many cover rows appear on one library page. More rows make each cover smaller."},
  {"Wrapper/ShowGameTitles", "Library layout",
   "Shows or hides game names below their covers in the launcher library."},
  {"Wrapper/ShowRegionFlags", "Library layout",
   "Shows or hides the region flag in the top-left corner of each game cover."},
  {"Wrapper/ShowCustomSettingsBadges", "Library layout",
   "Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed."},
  {"Wrapper/UiAnimations", "Launcher appearance",
   "Enables launcher transitions, moving selection highlights, and animated theme effects."},
  {"Wrapper/UiSounds", "Launcher audio",
   "Enables navigation, confirmation, and back sound effects in the SDL launcher."},
  {"Wrapper/CheckUpdatesAtBoot", "Launcher updates",
   "Checks the latest published ARMSX2-nx GitHub release in the background when the launcher opens. Turn it off to check only from the launcher settings screen."},
};

struct SettingHelpInfo {
  const char *kind;
  std::string text;
};

static SettingHelpInfo settingHelpFor(const Opt &option) {
  if(option.key)
    for(const SettingHelpEntry &entry:SETTING_HELP)
      if(!strcmp(entry.key,option.key)) return {entry.kind,entry.text};
  if(option.type==OT_SUBMENU){
    if(option.sub==SCR_ENHANCE)
      return {"Settings group","Contains GS accuracy controls, visual enhancements and custom textures. Higher accuracy often requires more GPU performance."};
    if(option.sub==SCR_FRAMERATE)
      return {"Settings group","Contains the normal speed target and the base NTSC/PAL video rates. Changing base video timing can affect game logic."};
  }
  return {"Setting","Changes this launcher or emulator option. Use the default value when troubleshooting an unexpected game-specific problem."};
}

static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (o.key && (o.type == OT_CHOICE || o.type == OT_RANGE || o.type == OT_SCALED_RANGE)) {
        std::string v = iniGet(o.key, o.def);
        iniSet(o.key, v.c_str());
      }
    }
}

static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr,
                    *g_font_caption = nullptr, *g_font_hint = nullptr;
static PlSharedFontType g_loadedFontType = PlSharedFontType_Total;
static SDL_Texture  *g_logo = nullptr;
// The UI is laid out in a fixed 1280x720 logical space; SDL scales it to the
// real output size so docked mode is the same layout, just sharper.
static int SW = 1280, SH = 720;
static int g_outputW = 1280, g_outputH = 720;
static float g_uiScale = 1.0f;    // output pixels per logical unit
static float g_fontScale = 1.0f;  // scale the currently-loaded fonts were opened at
static bool g_presentVsync = false;
static bool g_frameHasScrollingText = false;

// Layout uses 720p coordinates; text textures retain the output resolution.
static int fontMetric(int pixels){ return (int)std::ceil(pixels/g_fontScale); }
static int fontHeight(TTF_Font *font){ return font?fontMetric(TTF_FontHeight(font)):0; }

static void configureLauncherScale(){
  SW=1280; SH=720;
  if(g_outputW<1) g_outputW=1280;
  if(g_outputH<1) g_outputH=720;
  g_uiScale=std::min(g_outputW/1280.0f,g_outputH/720.0f);
  if(!g_ren) return;
  SDL_RenderSetViewport(g_ren,nullptr);
  SDL_RenderSetScale(g_ren,g_uiScale,g_uiScale);
}

static bool g_romfsReady = false;
static bool g_sdlReady = false;
static bool g_ttfReady = false;
static bool g_imgReady = false;
static bool g_plReady = false;
static bool g_griddbReady = false;
static bool g_storageSocketReady = false;
static std::string g_updateNoticeTag;
static std::string g_updateNotifiedTag;
static Uint32 g_updateNoticeUntil = 0;

enum class LauncherTheme { PS2, Classic };
static LauncherTheme g_launcherTheme = LauncherTheme::PS2;
static bool g_uiAnimations = true;
static bool g_showGameTitles = true;
static bool g_showRegionFlags = true;
static bool g_showCustomSettingsBadges = true;
static int g_gridColumns = 6;
static int g_gridRows = 2;
static SDL_Texture *g_glowTexture = nullptr;
static SDL_Texture *g_roundTexture = nullptr;
// Footer hit rects live here (not next to drawFooterHints) so clearUiBackground()
// can drop them at the top of every frame: a screen that draws no footer of its
// own must never inherit the previous screen's tap targets.
static SDL_Rect g_footHit[10]; static int g_footButton[10]; static int g_footN=0;
static float g_hy = -1;
static Uint32 g_fxT = 0;

static SDL_Color COL_BG    = { 8, 12, 24, 255 };
static SDL_Color COL_TXT   = { 235, 239, 247, 255 };
static SDL_Color COL_DIM   = { 151, 163, 184, 255 };
static SDL_Color COL_HI    = { 100, 211, 255, 255 };
static SDL_Color COL_VAL   = { 255, 215, 120, 255 };
static SDL_Color COL_SEL   = { 116, 200, 255, 255 };
static SDL_Color COL_PANEL = { 16, 23, 39, 255 };
static SDL_Color COL_CARD  = { 22, 30, 49, 214 };
static SDL_Color COL_FOCUS = { 28, 69, 92, 255 };

// Snap fills to whole output pixels so fractional UI scales cannot leave seams
// between adjacent rectangles or turn hairlines into 1-or-2px lines.
static SDL_FRect pixelAlignedRect(int x,int y,int w,int h){
  float sx=1,sy=1;SDL_RenderGetScale(g_ren,&sx,&sy);
  const float left=std::round(x*sx),top=std::round(y*sy);
  return {left/sx,top/sy,(std::round((x+w)*sx)-left)/sx,(std::round((y+h)*sy)-top)/sy};
}
static void fillRect(int x,int y,int w,int h, SDL_Color c){
  if(w<=0||h<=0) return;
  const SDL_FRect r=pixelAlignedRect(x,y,w,h);
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_RenderFillRectF(g_ren,&r);
}
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }

struct TextKey {
  TTF_Font *font;
  Uint32 color;
  std::string text;
  bool operator==(const TextKey &other) const {
    return font == other.font && color == other.color && text == other.text;
  }
};

struct TextKeyHash {
  size_t operator()(const TextKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextEntry {
  SDL_Texture *texture;
  float width;
  float height;
  size_t bytes;
  Uint64 use;
};

struct MetricKey {
  TTF_Font *font;
  std::string text;
  bool operator==(const MetricKey &other) const { return font == other.font && text == other.text; }
};

struct MetricKeyHash {
  size_t operator()(const MetricKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    return hash ^ (std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
  }
};

struct MetricEntry { int width; Uint64 use; };

struct EllipsisKey {
  TTF_Font *font;
  int maxWidth;
  std::string text;
  bool operator==(const EllipsisKey &other) const {
    return font == other.font && maxWidth == other.maxWidth && text == other.text;
  }
};

struct EllipsisKeyHash {
  size_t operator()(const EllipsisKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry { std::string text; Uint64 use; };

static std::unordered_map<TextKey, TextEntry, TextKeyHash> g_textCache;
static std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> g_metricCache;
static std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> g_ellipsisCache;
static size_t g_textCacheBytes = 0;
static Uint64 g_textUseSerial = 0;
static constexpr size_t TEXT_CACHE_LIMIT = 512;
static constexpr size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
static constexpr size_t METRIC_CACHE_LIMIT = 2048;
static constexpr size_t ELLIPSIS_CACHE_LIMIT = 512;

static Uint32 packColor(SDL_Color color) {
  return (Uint32)color.r | ((Uint32)color.g << 8) | ((Uint32)color.b << 16) | ((Uint32)color.a << 24);
}

static void rememberTextMetric(TTF_Font *font, const std::string &text, int width) {
  MetricKey key{font, text};
  auto found = g_metricCache.find(key);
  if (found != g_metricCache.end()) {
    found->second.width = width;
    found->second.use = ++g_textUseSerial;
    return;
  }
  if (g_metricCache.size() >= METRIC_CACHE_LIMIT) {
    auto victim = g_metricCache.begin();
    for (auto it = std::next(g_metricCache.begin()); it != g_metricCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    g_metricCache.erase(victim);
  }
  g_metricCache.emplace(std::move(key), MetricEntry{width, ++g_textUseSerial});
}

static void evictTextEntries(size_t incomingBytes) {
  while (!g_textCache.empty() &&
         (g_textCache.size() >= TEXT_CACHE_LIMIT || g_textCacheBytes > TEXT_CACHE_BYTES - incomingBytes)) {
    auto victim = g_textCache.begin();
    for (auto it = std::next(g_textCache.begin()); it != g_textCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    SDL_DestroyTexture(victim->second.texture);
    g_textCacheBytes -= victim->second.bytes;
    g_textCache.erase(victim);
  }
}

static void clearTextCaches() {
  for (auto &entry : g_textCache) SDL_DestroyTexture(entry.second.texture);
  g_textCache.clear();
  g_metricCache.clear();
  g_ellipsisCache.clear();
  g_textCacheBytes = 0;
  g_textUseSerial = 0;
}

static PlSharedFontType launcherFontType()
{
  const std::string_view code = g_localization.ResolvedCode();
  if (code == "zh-Hans") return PlSharedFontType_ChineseSimplified;
  if (code == "zh-Hant") return PlSharedFontType_ChineseTraditional;
  return PlSharedFontType_Standard;
}

static bool reloadLauncherFonts()
{
  if (!g_plReady || !g_ttfReady) return false;
  const PlSharedFontType requestedType = launcherFontType();
  if (g_loadedFontType == requestedType && g_font_sm && g_font && g_font_big &&
      g_font_caption && g_font_hint && g_fontScale == g_uiScale)
    return true;

  PlFontData fontData{};
  if (R_FAILED(plGetSharedFontByType(&fontData, requestedType)) ||
      !fontData.address || !fontData.size || fontData.size > INT_MAX)
    return false;

  // Fonts are opened at the real output resolution and measured back in logical
  // units, so docked mode gets crisp glyphs with an identical layout.
  const auto openFont = [&](int size) -> TTF_Font* {
    SDL_RWops* rw = SDL_RWFromConstMem(fontData.address, static_cast<int>(fontData.size));
    return rw ? TTF_OpenFontRW(rw, 1, (int)std::lround(size * g_uiScale)) : nullptr;
  };
  TTF_Font* small = openFont(20);
  TTF_Font* normal = openFont(26);
  TTF_Font* large = openFont(32);
  TTF_Font* caption = openFont(14);
  TTF_Font* hint = openFont(18);
  if (!small || !normal || !large || !caption || !hint)
  {
    if (small) TTF_CloseFont(small);
    if (normal) TTF_CloseFont(normal);
    if (large) TTF_CloseFont(large);
    if (caption) TTF_CloseFont(caption);
    if (hint) TTF_CloseFont(hint);
    return false;
  }

  clearTextCaches();
  if (g_font_sm) TTF_CloseFont(g_font_sm);
  if (g_font) TTF_CloseFont(g_font);
  if (g_font_big) TTF_CloseFont(g_font_big);
  if (g_font_caption) TTF_CloseFont(g_font_caption);
  if (g_font_hint) TTF_CloseFont(g_font_hint);
  g_fontScale = g_uiScale;
  g_font_sm = small;
  g_font = normal;
  g_font_big = large;
  g_font_caption = caption;
  g_font_hint = hint;
  g_loadedFontType = requestedType;
  return true;
}

// Docking or undocking changes the renderer output size while the launcher is
// running. Re-establish the 1280x720 logical space at the new scale and reopen
// the fonts at the new resolution; the layout is unchanged, only sharper.
static void refreshLauncherDisplayScale()
{
  if (!g_ren) return;
  int width = 0, height = 0;
  if (SDL_GetRendererOutputSize(g_ren, &width, &height) != 0 || width < 1 || height < 1) return;
  if (width == g_outputW && height == g_outputH) return;
  g_outputW = width;
  g_outputH = height;
  configureLauncherScale();
  // reloadLauncherFonts() clears the text caches itself when it reopens them.
  if (g_fontScale != g_uiScale && !reloadLauncherFonts()) clearTextCaches();
}

static bool setLauncherLanguage(std::string_view preference)
{
  const std::string previous(g_localization.Preference());
  g_localization.SetLanguage(preference);
  if (g_plReady && !reloadLauncherFonts())
  {
    g_localization.SetLanguage(previous);
    return false;
  }
  clearTextCaches();
  return true;
}

static void applyLauncherAppearance() {
  LauncherTheme previous = g_launcherTheme;
  const char *theme = storeGet(g_global, "Wrapper/Theme", "animated");
  g_launcherTheme = !strcmp(theme, "classic") ? LauncherTheme::Classic : LauncherTheme::PS2;
  normalizeLauncherTheme(g_global);
  g_uiAnimations = strcmp(storeGet(g_global, "Wrapper/UiAnimations", "true"), "false") != 0;
  g_showGameTitles = strcmp(storeGet(g_global, "Wrapper/ShowGameTitles", "true"), "false") != 0;
  g_showRegionFlags = strcmp(storeGet(g_global, "Wrapper/ShowRegionFlags", "true"), "false") != 0;
  g_showCustomSettingsBadges =
      strcmp(storeGet(g_global, "Wrapper/ShowCustomSettingsBadges", "true"), "false") != 0;
  g_gridColumns = std::max(3, std::min(8, atoi(storeGet(g_global, "Wrapper/GridColumns", "6"))));
  g_gridRows = std::max(1, std::min(3, atoi(storeGet(g_global, "Wrapper/GridRows", "2"))));

  COL_BG={8,12,24,255}; COL_TXT={235,239,247,255}; COL_DIM={151,163,184,255};
  COL_HI={100,211,255,255}; COL_VAL={255,215,120,255}; COL_SEL={116,200,255,255};
  COL_PANEL={16,23,39,255}; COL_CARD={22,30,49,214}; COL_FOCUS={28,69,92,255};
  if (previous != g_launcherTheme && g_ren)
    clearTextCaches();
}

static void ensureGlowTexture() {
  if (g_glowTexture || !g_ren) return;
  constexpr int size=256;
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);
  if(!surface) return;
  if(SDL_LockSurface(surface)==0){
    for(int y=0;y<size;y++){
      auto *row=(Uint32*)((Uint8*)surface->pixels+y*surface->pitch);
      for(int x=0;x<size;x++){
        float dx=(x-(size-1)*0.5f)/(size*0.5f),dy=(y-(size-1)*0.5f)/(size*0.5f);
        float distance=sqrtf(dx*dx+dy*dy);
        float strength=distance>=1.f?0.f:1.f-distance;
        Uint8 alpha=(Uint8)(255.f*strength*strength);
        row[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
      }
    }
    SDL_UnlockSurface(surface);
    g_glowTexture=SDL_CreateTextureFromSurface(g_ren,surface);
    if(g_glowTexture) SDL_SetTextureBlendMode(g_glowTexture,SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

static bool hasAnimatedBackground() {
  return g_launcherTheme==LauncherTheme::PS2;
}

static void drawGlow(float x,float y,float radius,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha) {
  int diameter=(int)(SH*radius);
  SDL_Rect destination={(int)(SW*x)-diameter/2,(int)(SH*y)-diameter/2,diameter,diameter};
  SDL_SetTextureColorMod(g_glowTexture,red,green,blue);
  SDL_SetTextureAlphaMod(g_glowTexture,alpha);
  SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&destination);
}

static void drawBackgroundParticles(float time,SDL_Color color,int count,float speed) {
  for(int i=0;i<count;i++){
    float travel=fmodf(i*0.371f+time*speed*(0.65f+(i%5)*0.11f),1.12f)-0.06f;
    float y=fmodf(i*0.217f+0.11f*sinf(time*0.29f+i*1.73f),1.f);
    float pulse=0.45f+0.55f*sinf(time*(0.9f+(i%4)*0.17f)+i);
    Uint8 alpha=(Uint8)(color.a*(0.55f+0.45f*pulse));
    int size=(i%9==0)?3:2;
    fillRect((int)(travel*SW),(int)(y*SH),size,size,(SDL_Color){color.r,color.g,color.b,alpha});
  }
}

// --- marquee text: per-position, restarts when the text changes, rests 0.9s at
// each end so a long label is actually readable at a standstill.
struct TextScroll { std::string text; Uint32 since=0; };
static std::map<std::pair<int,int>,TextScroll> g_textScroll;
static int textScrollOffset(int x,int y,int span,const char *text){
  g_frameHasScrollingText=true;
  if(g_textScroll.size()>64) g_textScroll.clear();
  auto &scroll=g_textScroll[{x,y}];
  const Uint32 now=SDL_GetTicks();
  if(scroll.text!=text){ scroll.text=text; scroll.since=now; }
  const float travel=span/45.0f,pause=0.9f;
  const float phase=std::fmod((now-scroll.since)/1000.0f,2*(travel+pause));
  if(phase<pause) return 0;
  if(phase<pause+travel) return (int)((phase-pause)*45);
  if(phase<2*pause+travel) return span;
  return std::max(0,span-(int)((phase-2*pause-travel)*45));
}

static void drawClassicBackground();

static void drawGradient(const SDL_Rect &rect,SDL_Color first,SDL_Color last,bool horizontal=false){
  const float x=(float)rect.x,y=(float)rect.y,w=(float)rect.w,h=(float)rect.h;
  const SDL_Vertex vertices[]={{{x,y},first,{0,0}},{{x+w,y},horizontal?last:first,{0,0}},
    {{x,y+h},horizontal?first:last,{0,0}},{{x+w,y+h},last,{0,0}}};
  const int indices[]={0,1,2,2,1,3};
  SDL_RenderGeometry(g_ren,nullptr,vertices,4,indices,6);
}

static void clearUiBackground() {
  // Every frame starts with no live footer tap targets; only the screens that
  // actually draw a footer this frame register any.
  g_footN=0;
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255);
  SDL_RenderClear(g_ren);
  if(g_launcherTheme==LauncherTheme::Classic){ drawClassicBackground(); return; }
  ensureGlowTexture();
  float time=g_uiAnimations?SDL_GetTicks()/1000.f:0.f;
  if(!g_glowTexture) return;
  drawGlow(0.10f+0.13f*sinf(time*0.43f),0.20f+0.11f*cosf(time*0.37f),0.90f,45,140,255,128);
  drawGlow(0.84f+0.12f*cosf(time*0.34f),0.34f+0.10f*sinf(time*0.41f),0.78f,154,75,255,112);
  drawGlow(0.54f+0.10f*sinf(time*0.29f),0.91f+0.06f*cosf(time*0.33f),0.94f,0,210,190,94);
  drawGlow(0.42f+0.08f*cosf(time*0.25f),0.48f+0.09f*sinf(time*0.31f),0.58f,64,125,255,67);
  drawBackgroundParticles(time,(SDL_Color){182,224,255,88},28,0.011f);
  SDL_SetTextureColorMod(g_glowTexture,255,255,255);
  SDL_SetTextureAlphaMod(g_glowTexture,255);
}

// One antialiased 32x32 disc, sliced into four corners, plus plain fills for the
// middle. Built lazily on first use and destroyed in cleanupLauncher().
static void roundedRect(int x,int y,int width,int height,int radius,SDL_Color color){
  if(width<=0||height<=0)return;
  const int r=std::min({radius,width/2,height/2});
  if(!g_roundTexture){
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,32,32,32,SDL_PIXELFORMAT_RGBA32);
    if(surface){
      for(int py=0;py<32;py++){
        auto *row=(Uint32*)((Uint8*)surface->pixels+py*surface->pitch);
        for(int px=0;px<32;px++){
          const float dx=px-15.5f,dy=py-15.5f;
          const Uint8 alpha=(Uint8)(255*std::clamp(16.f-std::sqrt(dx*dx+dy*dy),0.f,1.f));
          row[px]=SDL_MapRGBA(surface->format,255,255,255,alpha);
        }
      }
      g_roundTexture=SDL_CreateTextureFromSurface(g_ren,surface);SDL_FreeSurface(surface);
      if(g_roundTexture)SDL_SetTextureBlendMode(g_roundTexture,SDL_BLENDMODE_BLEND);
    }
  }
  if(!g_roundTexture||r<1){ fillRect(x,y,width,height,color); return; }
  fillRect(x+r,y,width-r*2,height,color);
  fillRect(x,y+r,r,height-r*2,color); fillRect(x+width-r,y+r,r,height-r*2,color);
  SDL_SetTextureColorMod(g_roundTexture,color.r,color.g,color.b);SDL_SetTextureAlphaMod(g_roundTexture,color.a);
  for(int corner=0;corner<4;corner++){
    const SDL_Rect source={(corner&1)*16,(corner>>1)*16,16,16};
    const SDL_FRect destination=pixelAlignedRect(x+((corner&1)?width-r:0),y+((corner>>1)?height-r:0),r,r);
    SDL_RenderCopyF(g_ren,g_roundTexture,&source,&destination);
  }
}

static void roundedPanel(int x,int y,int width,int height,SDL_Color face,SDL_Color edge,int radius=8,int thickness=1){
  roundedRect(x,y,width,height,radius,edge);
  roundedRect(x+thickness,y+thickness,width-2*thickness,height-2*thickness,std::max(0,radius-thickness),face);
}
static void glassPanel(int x,int y,int width,int height) {
  roundedPanel(x,y,width,height,COL_PANEL,(SDL_Color){255,255,255,24});
}
static void drawButtonPanel(int x,int y,int width,int height,bool selected){
  roundedPanel(x,y,width,height,selected?COL_FOCUS:COL_CARD,selected?COL_SEL:SDL_Color{255,255,255,28},6);
}
static void drawProgressBar(int x,int y,int width,int height,double fraction){
  roundedRect(x,y,width,height,height/2,COL_CARD);
  const int filled=(int)(width*std::clamp(fraction,0.0,1.0));
  if(filled>0)roundedRect(x,y,filled,height,height/2,COL_SEL);
}

static void drawRowHighlight(int x,int y,int width,int height){
  roundedRect(x,y,width,height,4,COL_FOCUS);
  fillRect(x,y+height/5,3,height*3/5,COL_SEL);
}

static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  TextKey key{f,packColor(c),s};
  auto found=g_textCache.find(key);
  if(found!=g_textCache.end()){
    found->second.use=++g_textUseSerial;
    SDL_FRect d={(float)x,(float)y,found->second.width,found->second.height};
    SDL_RenderCopyF(g_ren,found->second.texture,nullptr,&d);
    return;
  }
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf);
  const size_t bytes=(size_t)sf->w*(size_t)sf->h*4;
  const float w=sf->w/g_fontScale,h=sf->h/g_fontScale; SDL_FreeSurface(sf);
  if(!t) return;
  rememberTextMetric(f,s,(int)std::ceil(w));
  if(bytes<=TEXT_CACHE_BYTES){
    evictTextEntries(bytes);
    TextEntry entry{t,w,h,bytes,++g_textUseSerial};
    auto inserted=g_textCache.emplace(std::move(key),entry);
    g_textCacheBytes+=bytes;
    SDL_FRect d={(float)x,(float)y,w,h}; SDL_RenderCopyF(g_ren,inserted.first->second.texture,nullptr,&d);
  } else {
    SDL_FRect d={(float)x,(float)y,w,h}; SDL_RenderCopyF(g_ren,t,nullptr,&d); SDL_DestroyTexture(t);
  }
}
static int textW(TTF_Font*f,const char*s){
  if(!f||!s||!*s) return 0;
  MetricKey key{f,s}; auto found=g_metricCache.find(key);
  if(found!=g_metricCache.end()){ found->second.use=++g_textUseSerial; return found->second.width; }
  int w=0,h=0; if(TTF_SizeUTF8(f,s,&w,&h)!=0) return 0;
  w=fontMetric(w); rememberTextMetric(f,s,w); return w;
}

static const std::string &ellipsizedText(TTF_Font *font, const std::string &text, int maxWidth) {
  EllipsisKey key{font,maxWidth,text};
  auto found=g_ellipsisCache.find(key);
  if(found!=g_ellipsisCache.end()){ found->second.use=++g_textUseSerial; return found->second.text; }

  std::vector<size_t> boundaries{0};
  for(size_t i=0;i<text.size();){
    const unsigned char lead=(unsigned char)text[i];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(i+length>text.size()) length=1;
    for(size_t j=1;j<length;j++) if(((unsigned char)text[i+j]&0xc0)!=0x80){ length=1; break; }
    i+=length; boundaries.push_back(i);
  }
  size_t low=0,high=boundaries.size()-1;
  while(low<high){
    size_t middle=(low+high+1)/2;
    std::string candidate=text.substr(0,boundaries[middle])+"...";
    if(textW(font,candidate.c_str())<=maxWidth) low=middle; else high=middle-1;
  }
  std::string shortened=text.substr(0,boundaries[low])+"...";
  if(g_ellipsisCache.size()>=ELLIPSIS_CACHE_LIMIT){
    auto victim=g_ellipsisCache.begin();
    for(auto it=std::next(g_ellipsisCache.begin());it!=g_ellipsisCache.end();++it)
      if(it->second.use<victim->second.use) victim=it;
    g_ellipsisCache.erase(victim);
  }
  auto inserted=g_ellipsisCache.emplace(std::move(key),EllipsisEntry{std::move(shortened),++g_textUseSerial});
  return inserted.first->second.text;
}
// ellipsizedText always appends "..."; fittedText only shortens when it has to.
static std::string fittedText(TTF_Font *font,const std::string &text,int maxWidth){
  if(maxWidth<=0) return {};
  if(textW(font,text.c_str())<=maxWidth) return text;
  return ellipsizedText(font,text,maxWidth);
}
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();
static void toast(const char *msg);
static void toastStatic(const char *msg);
static std::string uiText(const char *text);
static void modalMessage(const char *title, const std::vector<std::string> &lines);
static void modalMessageStatic(const char *title,std::initializer_list<const char*> lines);
static bool confirmBox(const char *title, const std::vector<std::string> &lines);
static bool confirmBoxStatic(const char *title,std::initializer_list<const char*> lines);
static void runUpdateScreen();
static std::string installedReleaseTag();
static void pollUpdateNotification();
static void drawUpdateNotification();
static int dropdown(const char *title, const char *const *labels, int n, int cur,
                    bool localizeTitle=false,bool localizeChoices=false);
static void beginScreenFx();
static void drawFadeIn();
static void waitForNextFrame(bool forceAnimation=false);
static int topBarH();
static int settingsRowH();
static int settingsListY();
static int settingsFooterReserve();
static void drawHeader(const char *title,const char *ctx);
static void drawLocalizedHeader(const char *title,const char *ctx);
static void drawScrollTextR(TTF_Font *font,int xRight,int y,int maxWidth,const char *text,SDL_Color color);
static void drawScrollTextL(TTF_Font *font,int x,int y,int maxWidth,const char *text,SDL_Color color);
static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color);
static void drawWrappedCentered(TTF_Font *font,int centerX,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color);
static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height,bool preserveAspect=false);
static bool g_rescanAfterSettings = false;

static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr };
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_Texture *previous=SDL_GetRenderTarget(g_ren);
  float scaleX=1.0f,scaleY=1.0f; SDL_RenderGetScale(g_ren,&scaleX,&scaleY);
  SDL_SetRenderTarget(g_ren,t);
  SDL_RenderSetScale(g_ren,1.0f,1.0f);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,previous);
  SDL_RenderSetScale(g_ren,scaleX,scaleY);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,
                   *g_gLeftRight=nullptr,*g_gUpDown=nullptr,*g_gTouch=nullptr;
static SDL_Texture *g_settingsIcon=nullptr;
// Supersampling keeps the downscaled glyphs crisp.
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;
  const int S=GLYPH_SS, base=26;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_Texture *previous=SDL_GetRenderTarget(g_ren);
  float scaleX=1.0f,scaleY=1.0f; SDL_RenderGetScale(g_ren,&scaleX,&scaleY);
  SDL_SetRenderTarget(g_ren,t);
  SDL_RenderSetScale(g_ren,1.0f,1.0f);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  const SDL_Color face={242,244,246,255},ink={18,20,24,255};
  roundedRect(0,0,W,H,H/2,face);
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,previous);
  SDL_RenderSetScale(g_ren,scaleX,scaleY);
  return t;
}

static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false);
  g_gLeftRight=makeGlyph("< >",true); g_gUpDown=makeGlyph("^ v",true);
  g_gTouch=makeGlyph("Touch",true);
  g_settingsIcon=IMG_LoadTexture(g_ren,"romfs:/icons/settings.svg");
}

struct FootItem { const char *button; const char *label; };

static SDL_Texture *buttonGlyph(const char *button){
  if(!button) return nullptr;
  if(!strcmp(button,"A")) return g_gA;
  if(!strcmp(button,"B")) return g_gB;
  if(!strcmp(button,"X")) return g_gX;
  if(!strcmp(button,"Y")) return g_gY;
  if(!strcmp(button,"+")) return g_gPlus;
  if(!strcmp(button,"Left / Right")) return g_gLeftRight;
  if(!strcmp(button,"Up / Down")) return g_gUpDown;
  if(!strcmp(button,"Touch")) return g_gTouch;
  return nullptr;
}

static void buttonHintSize(const char *button,int &width,int &height){
  SDL_Texture *glyph=buttonGlyph(button);
  width=height=0;
  if(glyph){
    SDL_QueryTexture(glyph,nullptr,nullptr,&width,&height);
    width/=GLYPH_SS; height/=GLYPH_SS;
  } else if(button&&button[0]) {
    width=textW(g_font_hint,button)+14;
    height=fontHeight(g_font_hint)+6;
  }
}

static void drawButtonGlyph(int x,int cy,const char *button){
  SDL_Texture *glyph=buttonGlyph(button);
  int width=0,height=0; buttonHintSize(button,width,height);
  if(glyph){
    SDL_Rect destination={x,cy-height/2,width,height};
    SDL_RenderCopy(g_ren,glyph,nullptr,&destination);
  } else if(button&&button[0]) {
    roundedPanel(x,cy-height/2,width,height,COL_CARD,COL_DIM,height/2);
    drawTextC(g_font_hint,x+width/2,cy-fontHeight(g_font_hint)/2,button,COL_TXT);
  }
}

static std::string g_toastMessage;
static Uint32 g_toastUntil=0;

static bool toastVisible(){
  return !g_toastMessage.empty()&&!SDL_TICKS_PASSED(SDL_GetTicks(),g_toastUntil);
}

static void drawToastOverlay(){
  if(!toastVisible()){
    if(!g_toastMessage.empty())g_toastMessage.clear();
    return;
  }
  const int pw=std::min(820,SW-48),ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
  glassPanel(px,py,pw,ph);
  drawTextC(g_font,SW/2,py+46,
            ellipsizedText(g_font,g_toastMessage,pw-48).c_str(),COL_TXT);
}

static void presentUiNow(){
  drawToastOverlay();
  SDL_RenderPresent(g_ren);
}

// Keep every presentation path consistent: launcher-owned nonblocking overlays
// are composited immediately before SDL presents the frame.
#define SDL_RenderPresent(renderer) presentUiNow()

// Footer hints are measured separately from drawing so the library grid can
// reserve exactly the space the (possibly wrapped) hint row really needs.
struct FooterLayout {
  int itemWidth[10]{},gapAfter[10]{},rowStart[10]{},rowEnd[10]{},rowWidth[10]{};
  int rowCount=0,rowSpacing=0,height=0;
};
static FooterLayout measureFooter(const FootItem *it,int n){
  FooterLayout result;
  const int gap=8,pairGap=26,glyphGap=16;
  const int fh=fontHeight(g_font_hint),maxWidth=std::max(80,SW-160);
  for(int i=0;i<n&&i<10;i++){
    int gw=0,gh=0; buttonHintSize(it[i].button,gw,gh);
    const bool label=it[i].label&&it[i].label[0];
    result.itemWidth[i]=gw+(label?gap+textW(g_font_hint,tr(it[i].label)):0);
    result.gapAfter[i]=label?pairGap:glyphGap;
  }
  for(int first=0;first<n&&first<10;){
    int last=first,width=0;
    while(last<n&&last<10){
      const int added=result.itemWidth[last]+(last>first?result.gapAfter[last-1]:0);
      if(last>first&&width+added>maxWidth) break;
      width+=added; last++;
    }
    result.rowStart[result.rowCount]=first;
    result.rowEnd[result.rowCount]=last;
    result.rowWidth[result.rowCount]=width;
    result.rowCount++; first=last;
  }
  result.rowSpacing=fh+22;
  result.height=(std::max(1,result.rowCount)-1)*result.rowSpacing+fh+32;
  return result;
}
static void drawLauncherMark(int cy){
  if(!g_logo) return;
  int w=0,h=0; SDL_QueryTexture(g_logo,nullptr,nullptr,&w,&h);
  if(h<1) return;
  const SDL_Rect rect{40,cy-48,w*64/h,64};
  SDL_RenderCopy(g_ren,g_logo,nullptr,&rect);
}

static void drawFooterHints(const FootItem *it,int n,int cy){
  const FooterLayout layout=measureFooter(it,n);
  const int rowCount=layout.rowCount,rowSpacing=layout.rowSpacing;
  const int fh=fontHeight(g_font_hint),gap=8;
  const bool screenBottom=cy>=SH-64;
  if(screenBottom) drawLauncherMark(cy);
  g_footN=0;
  for(int row=0;row<rowCount;row++){
    const int rowY=cy-(rowCount-1-row)*rowSpacing;
    int x=screenBottom?SW-40-layout.rowWidth[row]:(SW-layout.rowWidth[row])/2;
    for(int i=layout.rowStart[row];i<layout.rowEnd[row];i++){
      int gw=0,gh=0; buttonHintSize(it[i].button,gw,gh);
      const int x0=x;
      drawButtonGlyph(x,rowY,it[i].button);
      x+=gw;
      const bool label=it[i].label&&it[i].label[0];
      if(label){
        const char *localized=tr(it[i].label);
        x+=gap;
        drawText(g_font_hint,x,rowY-fh/2,localized,COL_TXT);
        x+=textW(g_font_hint,localized);
      }
      if(g_footN<10){
        g_footHit[g_footN]={x0-4,rowY-std::max(gh,fh)/2-8,(x-x0)+8,std::max(gh,fh)+16};
        const char *button=it[i].button;
        g_footButton[g_footN]=
          !button?-1:
          !strcmp(button,"A")?BTN_CONFIRM:
          !strcmp(button,"B")?BTN_CANCEL:
          !strcmp(button,"X")?BTN_X:
          !strcmp(button,"Y")?BTN_Y:
          !strcmp(button,"+")?SDL_CONTROLLER_BUTTON_START:-1;
        g_footN++;
      }
      if(i+1<layout.rowEnd[row]) x+=layout.gapAfter[i];
    }
  }
}
// Shared by renderGrid (to draw) and gridLayout (to reserve the right height).
static std::array<FootItem,3> libraryFooter(){
  return {{{"A","Launch"},{"Y","Game menu"},{"B","Quit"}}};
}

static void drawSettingsFooter(const char *text,int cy=-1){
  if(!text||!text[0]) return;
  std::vector<std::string> tokens;
  const std::string source=text;
  size_t cursor=0;
  while(cursor<source.size()){
    while(cursor<source.size()&&source[cursor]==' ') cursor++;
    if(cursor>=source.size()) break;
    size_t end=cursor;
    while(end<source.size()&&!(source[end]==' '&&end+1<source.size()&&source[end+1]==' ')) end++;
    std::string token=trim(source.substr(cursor,end-cursor));
    if(!token.empty()) tokens.push_back(std::move(token));
    cursor=end;
    while(cursor<source.size()&&source[cursor]==' ') cursor++;
  }
  std::vector<FootItem> hints;
  hints.reserve(tokens.size()/2);
  for(size_t index=0;index+1<tokens.size();index+=2)
    hints.push_back({tokens[index].c_str(),tokens[index+1].c_str()});
  if(hints.empty()) drawTextC(g_font_sm,SW/2,cy>=0?cy-fontHeight(g_font_sm)/2:SH-38,text,COL_DIM);
  else drawFooterHints(hints.data(),(int)hints.size(),cy>=0?cy:SH-26);
}

// Every footer hint doubles as a touch target: tapping one synthesises the
// controller button it stands for, so no screen needs its own tap plumbing.
static bool pressFooterButton(int x,int y){
  for(int i=0;i<g_footN;i++){
    const SDL_Rect &rect=g_footHit[i];
    if(g_footButton[i]<0||x<rect.x||x>=rect.x+rect.w||y<rect.y||y>=rect.y+rect.h) continue;
    SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN;
    press.cbutton.button=(Uint8)g_footButton[i];
    return SDL_PushEvent(&press)==1;
  }
  return false;
}

enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R, TOUCH_SCROLL_UP, TOUCH_SCROLL_DOWN };
struct TouchG {
  bool active=false, vertical=false;
  SDL_FingerID fid=0;
  float x0=0,y0=0,lastY=0;
  Uint32 t0=0;
};
static TouchG g_touch;
static int g_touchScrollSteps=1;
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90, SCROLL_STEP=30; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.vertical=false; g_touch.fid=e.tfinger.fingerId;
    g_touch.x0=e.tfinger.x*SW; g_touch.y0=e.tfinger.y*SH; g_touch.lastY=g_touch.y0; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERMOTION && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    if(!g_touch.vertical && fabsf(dy)>TAP_MOVE && fabsf(dy)>fabsf(dx)*1.15f) g_touch.vertical=true;
    if(g_touch.vertical){
      float step=uy-g_touch.lastY;
      if(fabsf(step)>=SCROLL_STEP){
        g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(step)/SCROLL_STEP)));
        g_touch.lastY=uy;
        if(ox) *ox=(int)ux;
        if(oy) *oy=(int)uy;
        return step<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
      }
    }
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(ox) *ox=(int)ux;
    if(oy) *oy=(int)uy;
    if(g_touch.vertical || (fabsf(dy)>=55 && fabsf(dy)>fabsf(dx)*1.15f)){
      float remaining=uy-g_touch.lastY;
      if(fabsf(remaining)<18 && g_touch.vertical) return TOUCH_NONE;
      g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(g_touch.vertical?remaining:dy)/SCROLL_STEP)));
      return (g_touch.vertical?remaining:dy)<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
    }
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS)
      return pressFooterButton((int)ux,(int)uy)?TOUCH_NONE:TOUCH_TAP;
  }
  return TOUCH_NONE;
}

static bool touchScrollList(TouchKind kind,int &sel,int &top,int count,int visible){
  if((kind!=TOUCH_SCROLL_UP && kind!=TOUCH_SCROLL_DOWN) || count<=0) return false;
  const int previous=sel;
  int delta=(kind==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
  sel=std::max(0,std::min(count-1,sel+delta));
  if(sel<top) top=sel;
  if(sel>=top+visible) top=sel-visible+1;
  if(top<0) top=0;
  if(sel!=previous) uiAudioPlay(UiSound::Navigate);
  return true;
}

static bool g_stickXLatched=false, g_stickYLatched=false;
static char stickNav(const SDL_Event &e){
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!g_stickXLatched && e.caxis.value<-TH){ g_stickXLatched=true; return 'L'; }
    if(!g_stickXLatched && e.caxis.value> TH){ g_stickXLatched=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickXLatched=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!g_stickYLatched && e.caxis.value<-TH){ g_stickYLatched=true; return 'U'; }
    if(!g_stickYLatched && e.caxis.value> TH){ g_stickYLatched=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickYLatched=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static SDL_GameController *g_pad=nullptr;
static std::array<bool,2> g_libraryTriggers{};
static bool g_exitRequested=false;
static int g_navHeld=0;
static Uint32 g_navSince=0,g_navLast=0;
static std::deque<SDL_Event> g_waitedEvents;
static Uint32 g_interactionAnimationUntil=0;
static Uint32 g_nextFrameDeadline=0;
static void pumpCoverDecodeResults();
static void cancelQueuedCoverDecodes();
static void stopCoverDecodeWorker();

static void openController(int index) {
  if (!g_pad && index >= 0 && SDL_IsGameController(index))
    g_pad = SDL_GameControllerOpen(index);
}

static void closeController() {
  if (!g_pad) return;
  SDL_GameControllerClose(g_pad);
  g_pad = nullptr;
  g_stickXLatched = g_stickYLatched = false;
  g_libraryTriggers={};
  g_navHeld = 0;
  g_navSince = g_navLast = 0;
}

static bool beginUiFrame() {
  if (g_exitRequested) return false;
  if (!appletMainLoop()) {
    g_exitRequested = true;
    return false;
  }
  if(g_pad&&!SDL_GameControllerGetAttached(g_pad)) closeController();
  // Horizon does not always raise a window event when the console is docked or
  // undocked, so the output size is re-checked here as well; it is a no-op when
  // nothing changed.
  refreshLauncherDisplayScale();
  pumpCoverDecodeResults();
  g_frameHasScrollingText=false;
  return true;
}

static bool pollUiEvent(SDL_Event &event) {
  for (;;) {
    if(!g_waitedEvents.empty()){
      event=g_waitedEvents.front();
      g_waitedEvents.pop_front();
    } else if(!SDL_PollEvent(&event)) return false;
    if (event.type == SDL_QUIT) {
      g_exitRequested = true;
      continue;
    }
    if (event.type == SDL_WINDOWEVENT &&
        (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
         event.window.event == SDL_WINDOWEVENT_RESIZED)) {
      refreshLauncherDisplayScale();
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      openController(event.cdevice.which);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      if (g_pad) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pad);
        if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which)
          closeController();
      }
      continue;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
        case BTN_CONFIRM: uiAudioPlay(UiSound::Confirm); break;
        case BTN_CANCEL: uiAudioPlay(UiSound::Back); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          uiAudioPlay(UiSound::Navigate); break;
        default: break;
      }
    }
    switch(event.type){
      case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
      case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
      case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION:
      case SDL_MOUSEWHEEL: case SDL_KEYDOWN: case SDL_KEYUP:
        g_interactionAnimationUntil=SDL_GetTicks()+220;
        break;
      default: break;
    }
    return true;
  }
}

// Animation other than the theme's background: fades, scrolling text, interaction, navigation, touch.
static bool frameNeedsForegroundAnimation()
{
  const Uint32 now=SDL_GetTicks();
  const bool fade=g_uiAnimations&&g_fxT&&now-g_fxT<180;
  const bool interaction=g_interactionAnimationUntil&&
                         !SDL_TICKS_PASSED(now,g_interactionAnimationUntil);
  return fade||g_frameHasScrollingText||interaction||g_navHeld||g_touch.active;
}

static bool frameNeedsAnimation()
{
  return (g_uiAnimations&&hasAnimatedBackground())||frameNeedsForegroundAnimation();
}

static void waitForNextFrame(bool forceAnimation)
{
  if(g_exitRequested) return;
  const bool animate=forceAnimation||frameNeedsAnimation();
  // Only the background moves: 30 FPS is enough for it (instead of the display's 120 Hz handheld).
  const bool backgroundOnly=animate&&!forceAnimation&&!frameNeedsForegroundAnimation();
  const Uint32 frameInterval=backgroundOnly?33:16;
  // The present already blocked on the display; do not sleep on top of it.
  if(animate&&g_presentVsync&&!backgroundOnly) return;
  if(!animate){
    g_nextFrameDeadline=0;
    for(;;){
      SDL_Event event{};
      const Uint32 before=SDL_GetTicks();
      int timeout=250;
      if(toastVisible())timeout=std::max(1,std::min(timeout,(int)(g_toastUntil-before)));
      if(SDL_WaitEventTimeout(&event,timeout)){
        g_waitedEvents.push_back(event);
        return;
      }
      if(!appletMainLoop()){
        g_exitRequested=true;
        return;
      }
      // The 250 ms timeout is only a Horizon lifecycle check. Keep waiting
      // without returning/redrawing unless a real UI deadline expired.
      if(!g_toastMessage.empty()&&!toastVisible())return;
    }
  }
  const Uint32 now=SDL_GetTicks();
  if(!g_nextFrameDeadline||SDL_TICKS_PASSED(now,g_nextFrameDeadline+64))
    g_nextFrameDeadline=now+frameInterval;
  int timeout=SDL_TICKS_PASSED(now,g_nextFrameDeadline)?0:(int)(g_nextFrameDeadline-now);
  SDL_Event event{};
  if(timeout>0&&SDL_WaitEventTimeout(&event,timeout))
    g_waitedEvents.push_back(event);
  const Uint32 after=SDL_GetTicks();
  do g_nextFrameDeadline+=frameInterval; while(SDL_TICKS_PASSED(after,g_nextFrameDeadline));
}

static void navRepeat(){
  if(!g_pad||!SDL_GameControllerGetAttached(g_pad)) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)   || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  Uint32 now=SDL_GetTicks();
  if(dir!=g_navHeld){ g_navHeld=dir; g_navSince=now; g_navLast=now; return; }
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-g_navSince<DELAY || now-g_navLast<RATE) return;
  g_navLast=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

struct Game {
  std::string path;
  std::string sourceRoot;
  std::string storageId;
  std::string file;
  std::string title;
  std::string key;
  std::string legacyKey;
  std::string pathKey;
  std::string fingerprint;
  SDL_Texture *cover = nullptr;
  Uint32 coverAt = 0;
  Uint64 coverUse = 0;
  Uint64 coverRequest = 0;
  bool coverQueued = false;
  bool triedCover = false;
  SDL_Texture *background = nullptr;
  Uint64 backgroundUse = 0;
  Uint64 backgroundRequest = 0;
  bool backgroundQueued = false;
  bool triedBackground = false;
  bool hasCfg = false;
  bool legacyUnique = false;
  int region = 0;
  long long played = 0;
  uint64_t fileSize = 0;
  long long modified = 0;
};

static void releaseGameArtwork(Game &game){
  if(game.cover) SDL_DestroyTexture(game.cover);
  if(game.background) SDL_DestroyTexture(game.background);
  game.cover=game.background=nullptr;
}

struct LibraryIdentityRecord {
  std::string id;
  std::string fingerprint;
  std::string canonicalPath;
  std::string storageId;
  std::string cachedTitle;
  int cachedRegion = 0;
  uint64_t fileSize = 0;
  long long modified = 0;
};

struct LibraryScanState {
  std::atomic_bool cancel{false};
  std::atomic_bool complete{false};
  std::mutex mutex;
  std::deque<Game> ready;
  size_t unsortedPublished=0;
};

static std::vector<LibraryIdentityRecord> g_libraryIdentities;
static std::unordered_set<std::string> g_claimedLibraryIds;
static bool g_libraryIdentitiesDirty=false;
static std::shared_ptr<LibraryScanState> g_libraryScan;
static std::thread g_libraryScanThread;
static std::vector<size_t> g_visibleGames;
static std::unordered_set<std::string> g_favorites;
enum class LibraryView { Recent, All, Favorites };
static LibraryView g_libraryView=LibraryView::Recent;
static bool g_libraryHeaderFocused=false;
static int g_libraryHeaderSelection=0;
static std::array<float,3> g_libraryTabOpen{1,0,0};
static Uint32 g_libraryAnimationTick=0;
static float g_recentPosition=-1;
static std::array<std::string,3> g_librarySelections;

static std::vector<Game> g_games;
static Uint64 g_coverUseSerial = 0;
static constexpr size_t COVER_CACHE_LIMIT = 64;
static constexpr size_t BACKGROUND_CACHE_LIMIT = 4;
static std::string g_backgroundTarget,g_backgroundCurrent,g_backgroundPrevious;
static Uint32 g_backgroundSince=0;

static std::string foldedKey(std::string key);
static Store g_recent;
static const char *RECENT_INI = "sdmc:/switch/armsx2-nx/recent.ini";
static const char *LIBRARY_INI = "sdmc:/switch/armsx2-nx/library.ini";

static void loadLibraryOrganization(){
  g_favorites.clear();
  const int count=std::clamp(atoi(storeGet(g_global,"Library/FavoriteCount","0")),0,65536);
  for(int index=0;index<count;index++){
    const char *id=storeGet(g_global,("Library/Favorite"+std::to_string(index)).c_str(),"");
    if(*id) g_favorites.insert(id);
  }
  g_libraryView=LibraryView::Recent;
}

static void saveLibraryOrganization(){
  storeRemovePrefix(g_global,"Library/Favorite");
  storeSet(g_global,"Library/FavoriteCount",std::to_string(g_favorites.size()).c_str());
  size_t index=0;
  for(const auto &id:g_favorites)
    storeSet(g_global,("Library/Favorite"+std::to_string(index++)).c_str(),id.c_str());
  storeSave(g_global,LAUNCHER_INI);
}

static void rebuildVisibleGames(){
  g_visibleGames.clear();
  for(size_t index=0;index<g_games.size();index++){
    const Game &game=g_games[index];
    if(g_libraryView==LibraryView::Recent&&game.played<=0) continue;
    if(g_libraryView==LibraryView::Favorites&&!g_favorites.count(game.key)) continue;
    g_visibleGames.push_back(index);
  }
  if(g_libraryView==LibraryView::Recent)
    std::stable_sort(g_visibleGames.begin(),g_visibleGames.end(),[](size_t left,size_t right){
      return g_games[left].played>g_games[right].played;
    });
}

static Game *visibleGame(int index)
{
  return index>=0&&index<(int)g_visibleGames.size()?&g_games[g_visibleGames[index]]:nullptr;
}

// Maps a stable game key back to its row in the current visible order, so a
// streaming scan can publish new rows without dragging the cursor around.
static int visibleIndexForKey(const std::string &key)
{
  if(key.empty()) return 0;
  for(int index=0;index<(int)g_visibleGames.size();index++)
    if(g_games[g_visibleGames[index]].key==key) return index;
  return 0;
}

static int libraryTabCount(){ return 3; }

static int libraryTabIndex(){ return (int)g_libraryView; }

static void selectLibraryTab(int tab,int &selection){
  if(Game *game=visibleGame(selection)) g_librarySelections[libraryTabIndex()]=game->key;
  tab=std::clamp(tab,0,libraryTabCount()-1);
  g_libraryView=(LibraryView)tab;
  rebuildVisibleGames();
  selection=visibleIndexForKey(g_librarySelections[tab]);
  g_libraryHeaderSelection=tab;
  g_recentPosition=-1;
  beginScreenFx();
}

static int detectRegion(const std::string &file) {
  std::string tags; int depth = 0;
  for (char c : file) {
    if (c=='('||c=='[') depth++;
    else if (c==')'||c==']') { if (depth) depth--; if (depth==0) tags += '|'; }
    else if (depth) tags += (char)tolower((unsigned char)c);
  }
  auto has = [&](const char *s){ return tags.find(s) != std::string::npos; };
  if (has("japan")||has("ntsc-j")||has("jpn")||has("(j)")) return 3;
  if (has("usa")||has("ntsc-u")||has("america")||has("(u)")) return 1;
  if (has("europe")||has("pal")||has("australia")||has("(uk")||has("france")||
      has("germany")||has("spain")||has("ital")||has("(e)")) return 2;
  std::string l; for (char c : file) l += (char)tolower((unsigned char)c);
  if (l.find("ntsc-j")!=std::string::npos) return 3;
  if (l.find("ntsc-u")!=std::string::npos) return 1;
  return 0;
}
static void sortLibraryGames() {
  auto cmpTitle = [](const Game &a, const Game &b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; };
  std::sort(g_games.begin(), g_games.end(), [&](const Game &a, const Game &b){
    return cmpTitle(a, b);
  });
  rebuildVisibleGames();
}
static void recordPlayed(const Game &game){
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b);
  storeSet(g_recent,game.key.c_str(),b);
  if (game.legacyUnique && !game.legacyKey.empty())
    storeRemove(g_recent, game.legacyKey.c_str());
}

static bool hasDiscExt(const char *n) {
  const char *e = strrchr(n, '.');
  if (!e) return false;
  static const char *x[] = { ".iso",".chd",".cso",".zso",".bin",".mdf",".img",".gz" };
  for (auto s : x) if (!strcasecmp(e, s)) return true;
  return false;
}
static std::string toEmu(const std::string &path) {
  return path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
}
static std::string join(const std::string &b, const std::string &n) { std::string r=b; if(!r.empty()&&r.back()=='/') r.pop_back(); return r+"/"+n; }
static std::string foldedKey(std::string key);

static std::string normalizeLocationPath(const std::string &input) {
  std::string path=trim(input);
  if(path.empty()) return {};
  std::string output;
  output.reserve(path.size()+1);
  bool slash=false;
  for(char c:path){
    if(c=='\\') c='/';
    if(c=='/'){
      if(slash) continue;
      slash=true;
    } else slash=false;
    output+=c;
  }
  size_t colon=output.find(':');
  if(colon!=std::string::npos && colon+1==output.size()) output+='/';
  size_t minimum=colon==std::string::npos?1:colon+2;
  while(output.size()>minimum && output.back()=='/') output.pop_back();
  return output;
}

static std::string pathIdentity(const std::string &input) {
  return foldedKey(normalizeLocationPath(input));
}

static std::vector<std::string> loadGameSources() {
  std::vector<std::string> paths;
  const int count=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","0"))));
  for(int i=0;i<count;i++){
    std::string path=normalizeLocationPath(storeGet(g_global,("Wrapper/GamePath"+std::to_string(i)).c_str(),""));
    if(!path.empty()) paths.push_back(std::move(path));
  }
  std::unordered_set<std::string> seen;
  paths.erase(std::remove_if(paths.begin(),paths.end(),[&](const std::string &path){
    return !seen.insert(pathIdentity(path)).second;
  }),paths.end());
  return paths;
}

static void saveGameSources(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty() && seen.insert(pathIdentity(path)).second && paths.size()<16) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Wrapper/GamePath");
  storeSet(g_global,"Wrapper/GamePathCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++)
    storeSet(g_global,("Wrapper/GamePath"+std::to_string(i)).c_str(),paths[i].c_str());
}

static std::vector<std::string> loadFavoriteFolders() {
  std::vector<std::string> paths;
  int count=std::max(0,std::min(24,atoi(storeGet(g_global,"Browser/FavoriteCount","0"))));
  std::unordered_set<std::string> seen;
  for(int i=0;i<count;i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
    if(!path.empty() && seen.insert(pathIdentity(path)).second) paths.push_back(std::move(path));
  }
  return paths;
}

static void saveFavoriteFolders(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty()&&seen.insert(pathIdentity(path)).second&&paths.size()<24) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Browser/Favorite");
  storeSet(g_global,"Browser/FavoriteCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}

// Wakes the UI loop, which waits for events, from a worker thread.
static void wakeUi(int code) {
  SDL_Event wake{};
  wake.type=SDL_USEREVENT;
  wake.user.code=code;
  SDL_PushEvent(&wake);
}

static void updaterWake(void *) { wakeUi(0x55504454); } // UPDT

static bool isJunkToken(const std::string &tok) {
  std::string l;
  for (char c : tok) l += (char)tolower((unsigned char)c);
  static const char *junk[] = {
    "pal","ntsc","ntsc-u","ntsc-j","ntscu","ntscj","usa","us","europe","eu","japan","jp","jpn",
    "world","korea","asia","multi","multi3","multi5","nkit","redump","proper","unl","disc","cd","dvd",
    "iso","chd","cso","zso","enfrespt",
  };
  for (auto j : junk) if (l == j) return true;
  if (l.size() >= 2 && l[0] == 'v' && isdigit((unsigned char)l[1])) return true;
  return false;
}
static std::string cleanTitle(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o; int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
    else if (!depth) o += (c == '_') ? ' ' : c;
  }
  std::string w; bool sp = true;
  for (char c : o) { if (isspace((unsigned char)c)) { if (!sp) w += ' '; sp = true; } else { w += c; sp = false; } }
  o = trim(w);
  std::string filtered;
  for(size_t start=0;start<o.size();){
    size_t end=o.find(' ',start);
    std::string token=o.substr(start,end==std::string::npos?std::string::npos:end-start);
    if(foldedKey(token)!="enfrespt"){
      if(!filtered.empty()) filtered+=' ';
      filtered+=token;
    }
    if(end==std::string::npos) break;
    start=end+1;
  }
  o=std::move(filtered);
  for (;;) {
    size_t p = o.find_last_of(" -");
    std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
    if (!last.empty() && isJunkToken(last) && p != std::string::npos) {
      o = trim(o.substr(0, p));
      while (!o.empty() && (o.back() == '-' || o.back() == ' ' || o.back() == '.')) o.pop_back();
    } else break;
  }
  return trim(o);
}
static std::string sanitize(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o;
  for (char c : s) o += (isalnum((unsigned char)c) || c=='-'||c=='_') ? c : '_';
  return o;
}

static std::string foldedKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return key;
}

static std::string makeLegacyPathKey(const std::string &file, const std::string &path) {
  std::string base = sanitize(file);
  if (base.empty()) base = "game";
  if (base.size() > 80) base.resize(80);

  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : pathIdentity(path)) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  return base + suffix;
}

static const char *gameStoreGet(Store &store, const Game &game, const char *def) {
  const char *value = storeGet(store, game.key.c_str(), "");
  if (*value) return value;
  if(!game.pathKey.empty()){
    value=storeGet(store,game.pathKey.c_str(),"");
    if(*value) return value;
  }
  if (!game.legacyUnique || game.legacyKey.empty()) return def;
  return storeGet(store, game.legacyKey.c_str(), def);
}

static bool gameFileExists(const char *dir, const Game &game, const char *extension) {
  if (regularFileExists(std::string(dir) + "/" + game.key + extension))
    return true;
  if (!game.pathKey.empty() && regularFileExists(std::string(dir)+"/"+game.pathKey+extension))
    return true;
  return game.legacyUnique && !game.legacyKey.empty() &&
         regularFileExists(std::string(dir) + "/" + game.legacyKey + extension);
}

static void stopGameScan();
static void pumpGameScan();
static bool pathAtOrBelow(const std::string &path,const std::string &root);

static void fingerprintMix(uint64_t &first,uint64_t &second,const void *data,size_t size)
{
  const auto *bytes=static_cast<const unsigned char*>(data);
  for(size_t index=0;index<size;index++){
    first^=bytes[index]; first*=1099511628211ULL;
    second^=(uint64_t)bytes[index]+0x9e3779b97f4a7c15ULL+(second<<6)+(second>>2);
    second*=0x100000001b3ULL;
  }
}

static std::string fingerprintGameFile(const std::string &path,uint64_t size)
{
  FILE *file=fopen(path.c_str(),"rb");
  if(!file) return {};
  uint64_t first=1469598103934665603ULL,second=0xcbf29ce484222325ULL;
  fingerprintMix(first,second,&size,sizeof(size));
  constexpr size_t blockSize=64*1024;
  std::vector<unsigned char> block(blockSize);
  std::array<uint64_t,3> offsets{{0,size>blockSize?size/2:0,size>blockSize?size-blockSize:0}};
  for(uint64_t offset:offsets){
    if(fseeko(file,(off_t)offset,SEEK_SET)!=0) continue;
    const size_t count=fread(block.data(),1,block.size(),file);
    fingerprintMix(first,second,block.data(),count);
  }
  fclose(file);
  char result[40];
  snprintf(result,sizeof(result),"%016llx%016llx",
           (unsigned long long)first,(unsigned long long)second);
  return result;
}

static void loadLibraryIdentities()
{
  g_libraryIdentities.clear();
  const int count=std::max(0,std::min(4096,atoi(storeGet(g_library,"IdentityCount","0"))));
  std::unordered_set<std::string> ids;
  for(int index=0;index<count;index++){
    const std::string prefix="Identity"+std::to_string(index)+"/";
    LibraryIdentityRecord record;
    record.id=storeGet(g_library,(prefix+"Id").c_str(),"");
    record.fingerprint=storeGet(g_library,(prefix+"Fingerprint").c_str(),"");
    record.canonicalPath=normalizeLocationPath(storeGet(g_library,(prefix+"Path").c_str(),""));
    record.storageId=storeGet(g_library,(prefix+"Storage").c_str(),"");
    record.cachedTitle=storeGet(g_library,(prefix+"Title").c_str(),"");
    record.cachedRegion=atoi(storeGet(g_library,(prefix+"Region").c_str(),"0"));
    record.fileSize=strtoull(storeGet(g_library,(prefix+"Size").c_str(),"0"),nullptr,10);
    record.modified=atoll(storeGet(g_library,(prefix+"Modified").c_str(),"0"));
    if(!record.id.empty()&&!record.fingerprint.empty()&&ids.insert(record.id).second)
      g_libraryIdentities.push_back(std::move(record));
  }
}

static void saveLibraryIdentities()
{
  if(!g_libraryIdentitiesDirty) return;
  g_library.kv.clear();
  storeSet(g_library,"IdentityCount",std::to_string(g_libraryIdentities.size()).c_str());
  for(size_t index=0;index<g_libraryIdentities.size();index++){
    const auto &record=g_libraryIdentities[index];
    const std::string prefix="Identity"+std::to_string(index)+"/";
    storeSet(g_library,(prefix+"Id").c_str(),record.id.c_str());
    storeSet(g_library,(prefix+"Fingerprint").c_str(),record.fingerprint.c_str());
    storeSet(g_library,(prefix+"Path").c_str(),record.canonicalPath.c_str());
    storeSet(g_library,(prefix+"Storage").c_str(),record.storageId.c_str());
    storeSet(g_library,(prefix+"Title").c_str(),record.cachedTitle.c_str());
    storeSet(g_library,(prefix+"Region").c_str(),std::to_string(record.cachedRegion).c_str());
    storeSet(g_library,(prefix+"Size").c_str(),std::to_string(record.fileSize).c_str());
    storeSet(g_library,(prefix+"Modified").c_str(),std::to_string(record.modified).c_str());
  }
  if(storeSave(g_library,LIBRARY_INI)) g_libraryIdentitiesDirty=false;
}

static LibraryIdentityRecord &assignStableIdentity(Game &game)
{
  const std::string normalized=pathIdentity(game.path);
  LibraryIdentityRecord *matched=nullptr;
  for(auto &record:g_libraryIdentities){
    if(g_claimedLibraryIds.count(record.id)) continue;
    if(pathIdentity(record.canonicalPath)!=normalized)continue;
    if(record.fingerprint==game.fingerprint){matched=&record;break;}
    // A different disc replaced the file at this exact path. Retire the path
    // association so the old game's settings/covers cannot silently migrate.
    record.canonicalPath.clear();
    g_libraryIdentitiesDirty=true;
  }
  if(!matched){
    for(auto &record:g_libraryIdentities){
      if(g_claimedLibraryIds.count(record.id)||record.fingerprint!=game.fingerprint)continue;
      const bool sameStorage=!record.storageId.empty()?record.storageId==game.storageId:
        pathAtOrBelow(record.canonicalPath,game.sourceRoot);
      if(!sameStorage)continue;
      // Reserve identities whose original file still exists. This makes the
      // path phase deterministic when byte-identical duplicates are scanned in
      // a different directory order; fingerprint fallback is only for moves.
      struct stat existing{};
      if(!record.canonicalPath.empty()&&stat(record.canonicalPath.c_str(),&existing)==0&&
         S_ISREG(existing.st_mode))continue;
      matched=&record;break;
    }
  }
  if(!matched){
    LibraryIdentityRecord record;
    std::string stem="ps2-"+(game.fingerprint.empty()?std::string("unknown"):game.fingerprint.substr(0,20));
    record.id=stem;
    for(unsigned suffix=2;std::any_of(g_libraryIdentities.begin(),g_libraryIdentities.end(),
        [&](const auto &item){ return item.id==record.id; });suffix++) record.id=stem+"-"+std::to_string(suffix);
    g_libraryIdentities.push_back(std::move(record));
    matched=&g_libraryIdentities.back();
  }
  const std::string canonicalPath=normalizeLocationPath(game.path);
  const bool metadataChanged=matched->fileSize!=game.fileSize||matched->modified!=game.modified;
  const bool identityChanged=matched->fingerprint!=game.fingerprint||
      pathIdentity(matched->canonicalPath)!=pathIdentity(canonicalPath);
  game.key=matched->id;
  game.title=(!metadataChanged&&!matched->cachedTitle.empty())?matched->cachedTitle:cleanTitle(game.file);
  game.region=metadataChanged?detectRegion(game.file):matched->cachedRegion;
  const bool cacheChanged=identityChanged||metadataChanged||
      matched->cachedTitle!=game.title||matched->cachedRegion!=game.region;
  matched->fingerprint=game.fingerprint;
  matched->canonicalPath=canonicalPath;
  matched->storageId=game.storageId;
  matched->cachedTitle=game.title;
  matched->cachedRegion=game.region;
  matched->fileSize=game.fileSize;
  matched->modified=game.modified;
  g_claimedLibraryIds.insert(game.key);
  if(cacheChanged) g_libraryIdentitiesDirty=true;
  return *matched;
}

static void stopGameScan()
{
  if(g_libraryScan) g_libraryScan->cancel.store(true,std::memory_order_release);
  if(g_libraryScanThread.joinable()) g_libraryScanThread.join();
  g_libraryScan.reset();
}

static std::string storageIdForSource(const std::string &source)
{
  return pathIdentity(source).rfind("sdmc:",0)==0?"sdmc":"path:"+pathIdentity(source);
}

static void scanGames(const std::vector<std::string> &sourcePaths)
{
  stopGameScan();
  cancelQueuedCoverDecodes();
  for(auto &game:g_games) releaseGameArtwork(game);
  g_games.clear(); g_visibleGames.clear(); g_coverUseSerial=0; g_claimedLibraryIds.clear();
  std::vector<std::pair<std::string,std::string>> sources;
  sources.reserve(sourcePaths.size());
  for(const auto &path:sourcePaths) sources.emplace_back(path,storageIdForSource(path));
  // Unchanged files retain their sampled content fingerprint. This avoids reading
  // up to 192 KiB from every disc image on each launcher start. A changed size or
  // mtime falls through to fingerprintGameFile(), refreshing identity and metadata.
  std::unordered_map<std::string,LibraryIdentityRecord> identityCache;
  identityCache.reserve(g_libraryIdentities.size());
  for(const auto &record:g_libraryIdentities){
    if(!record.canonicalPath.empty()&&!record.fingerprint.empty())
      identityCache[pathIdentity(record.canonicalPath)]=record;
  }
  auto state=std::make_shared<LibraryScanState>();
  g_libraryScan=state;
  const int firstPage=std::max(1,g_gridColumns*g_gridRows);
  g_libraryScanThread=std::thread([state,sources=std::move(sources),
                                   identityCache=std::move(identityCache),firstPage]{
    std::unordered_set<std::string> seenPaths;
    size_t queued=0;
    for(const auto &[source,storageId]:sources){
      if(state->cancel.load(std::memory_order_acquire)) break;
      DIR *directory=opendir(source.c_str());
      if(!directory) continue;
      while(!state->cancel.load(std::memory_order_acquire)){
        dirent *entry=readdir(directory);
        if(!entry) break;
        if(entry->d_name[0]=='.'||!hasDiscExt(entry->d_name)) continue;
        std::string full=join(source,entry->d_name);
        if(!seenPaths.insert(pathIdentity(full)).second) continue;
        struct stat info{};
        if(stat(full.c_str(),&info)!=0||!S_ISREG(info.st_mode)) continue;
        Game game;
        game.file=entry->d_name; game.path=std::move(full);
        game.sourceRoot=source; game.storageId=storageId;
        game.legacyKey=sanitize(game.file);
        game.pathKey=makeLegacyPathKey(game.file,game.path);
        game.fileSize=static_cast<uint64_t>(info.st_size);
        game.modified=static_cast<long long>(info.st_mtime);
        const auto cached=identityCache.find(pathIdentity(game.path));
        if(cached!=identityCache.end()&&cached->second.fileSize==game.fileSize&&
           cached->second.modified==game.modified)
          game.fingerprint=cached->second.fingerprint;
        else
          game.fingerprint=fingerprintGameFile(game.path,game.fileSize);
        if(game.fingerprint.empty()) continue;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->ready.push_back(std::move(game));
          queued++;
        }
        if(queued==(size_t)firstPage||queued%8==0){
          SDL_Event wake{}; wake.type=SDL_USEREVENT; wake.user.code=0x5343414e; // SCAN
          SDL_PushEvent(&wake);
        }
      }
      closedir(directory);
    }
    state->complete.store(true,std::memory_order_release);
    SDL_Event wake{}; wake.type=SDL_USEREVENT; wake.user.code=0x5343414e;
    SDL_PushEvent(&wake);
  });
}

static void pumpGameScan()
{
  auto state=g_libraryScan;
  if(!state) return;
  // Identity migration and per-game file checks stay on the SDL thread: publish for at most 4 ms per frame, so input
  // and rendering stay smooth. (A scan starts from an empty list, and the scan thread skips repeated paths.)
  const Uint32 start=SDL_GetTicks();
  size_t published=0;
  for(;;){
    Game game;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if(state->ready.empty()) break;
      game=std::move(state->ready.front());
      state->ready.pop_front();
    }
    assignStableIdentity(game);
    const char *customTitle=gameStoreGet(g_titles,game,"");
    if(*customTitle) game.title=customTitle;
    game.played=atoll(gameStoreGet(g_recent,game,"0"));
    game.hasCfg=gameFileExists(GAMECFG_DIR,game,".ini");
    g_games.push_back(std::move(game));
    published++;
    if(SDL_GetTicks()-start>=4) break;
  }
  if(published){
    state->unsortedPublished+=published;
    if(g_games.size()<=24||state->unsortedPublished>=16){
      std::map<std::string,size_t> counts;
      for(const auto &game:g_games)counts[foldedKey(game.legacyKey)]++;
      for(auto &game:g_games)game.legacyUnique=counts[foldedKey(game.legacyKey)]==1;
      sortLibraryGames();state->unsortedPublished=0;
    }else rebuildVisibleGames();
  }
  bool drained=false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    drained=state->ready.empty();
  }
  if(state->complete.load(std::memory_order_acquire)&&drained){
    if(g_libraryScanThread.joinable()) g_libraryScanThread.join();
    std::map<std::string,size_t> counts;
    for(const auto &game:g_games)counts[foldedKey(game.legacyKey)]++;
    for(auto &game:g_games)game.legacyUnique=counts[foldedKey(game.legacyKey)]==1;
    sortLibraryGames();
    g_libraryScan.reset();
    saveLibraryIdentities();
  }
}
static std::string coverPath(const Game &g) {
  return std::string(COVERS_DIR) + "/" + g.key + ".png";
}
static std::string backgroundPath(const Game &game){
  return std::string(COVERS_DIR)+"/"+game.key+".background.png";
}

static std::string artworkKey(const Game &game,const char *field){
  return "Artwork/"+game.key+"/"+field;
}

static std::vector<std::string> coverCandidatePaths(const Game &g) {
  std::vector<std::string> paths{coverPath(g)};
  if(!g.pathKey.empty()){
    const std::string legacy=std::string(COVERS_DIR)+"/"+g.pathKey+".png";
    if(std::find(paths.begin(),paths.end(),legacy)==paths.end())paths.emplace_back(legacy);
  }
  if(g.legacyUnique&&!g.legacyKey.empty()){
    const std::string legacy=std::string(COVERS_DIR)+"/"+g.legacyKey+".png";
    if(std::find(paths.begin(),paths.end(),legacy)==paths.end())paths.emplace_back(legacy);
  }
  return paths;
}
static std::string existingCoverPath(const Game &g) {
  const std::vector<std::string> paths=coverCandidatePaths(g);
  for(const std::string &path:paths)if(regularFileExists(path))return path;
  return paths.front();
}

static Game *findGameByKey(const std::string &key) {
  if(key.empty()) return nullptr;
  for (auto &game : g_games)
    if (game.key == key) return &game;
  for(auto &game:g_games) if(game.pathKey==key) return &game;
  Game *match = nullptr;
  for (auto &game : g_games) {
    if (!game.legacyUnique || game.legacyKey != key) continue;
    if (match) return nullptr;
    match = &game;
  }
  return match;
}

static constexpr int COVER_REQUEST_BUDGET = 48;
static constexpr int COVER_UPLOAD_BUDGET = 2;
static constexpr size_t COVER_JOB_LIMIT = 96;
static constexpr size_t COVER_READY_LIMIT = 4;
static int g_cover_budget = 1 << 30;

struct CoverDecodeJob {
  std::string key;
  bool background=false;
  std::vector<std::string> paths;
  Uint64 request=0;
  Uint64 epoch=0;
};
struct CoverDecodeResult {
  std::string key;
  bool background=false;
  Uint64 request=0;
  int width=0,height=0;
  std::vector<Uint8> pixels;
};
static std::mutex g_coverDecodeMutex;
static std::condition_variable g_coverDecodeCondition;
static std::deque<CoverDecodeJob> g_coverDecodeJobs;
static std::deque<CoverDecodeResult> g_coverDecodeReady;
static std::thread g_coverDecodeWorker;
static bool g_coverDecodeStarted=false,g_coverDecodeStop=false;
static Uint64 g_coverDecodeEpoch=1,g_coverRequestSerial=0;

static CoverDecodeResult decodeCover(const CoverDecodeJob &job) {
  CoverDecodeResult result;result.key=job.key;result.background=job.background;result.request=job.request;
  SDL_Surface *source=nullptr;
  for(const std::string &path:job.paths){source=IMG_Load(path.c_str());if(source)break;}
  if(!source||source->w<1||source->h<1||source->w>8192||source->h>8192||
     (Uint64)source->w*(Uint64)source->h>16ull*1024*1024){
    if(source)SDL_FreeSurface(source);
    return result;
  }
  const int maxWidth=job.background?1280:360,maxHeight=job.background?720:540;
  int width=source->w,height=source->h;
  if(width>maxWidth){height=(int)((long long)height*maxWidth/width);width=maxWidth;}
  if(height>maxHeight){width=(int)((long long)width*maxHeight/height);height=maxHeight;}
  width=std::max(1,width);height=std::max(1,height);
  SDL_Surface *rgba=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!rgba){SDL_FreeSurface(source);return result;}
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  const bool converted=SDL_BlitScaled(source,nullptr,rgba,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);SDL_FreeSurface(source);
  if(!converted){SDL_FreeSurface(rgba);return result;}
  const bool mustLock=SDL_MUSTLOCK(rgba);
  if(mustLock&&SDL_LockSurface(rgba)!=0){SDL_FreeSurface(rgba);return result;}
  result.pixels.resize((size_t)width*(size_t)height*4);
  for(int row=0;row<height;row++)memcpy(
      result.pixels.data()+(size_t)row*(size_t)width*4,
      (const Uint8*)rgba->pixels+(size_t)row*(size_t)rgba->pitch,(size_t)width*4);
  if(mustLock)SDL_UnlockSurface(rgba);
  SDL_FreeSurface(rgba);result.width=width;result.height=height;
  return result;
}

static void coverDecodeThread() {
  for(;;){
    CoverDecodeJob job;
    {
      std::unique_lock<std::mutex> lock(g_coverDecodeMutex);
      g_coverDecodeCondition.wait(lock,[]{return g_coverDecodeStop||
          (!g_coverDecodeJobs.empty()&&g_coverDecodeReady.size()<COVER_READY_LIMIT);});
      if(g_coverDecodeStop)return;
      job=std::move(g_coverDecodeJobs.front());g_coverDecodeJobs.pop_front();
    }
    CoverDecodeResult result=decodeCover(job);bool publish=false;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(!g_coverDecodeStop&&job.epoch==g_coverDecodeEpoch){
        g_coverDecodeReady.emplace_back(std::move(result));publish=true;
      }
    }
    if(publish){SDL_Event wake{};wake.type=SDL_USEREVENT;wake.user.code=0x434f5652;SDL_PushEvent(&wake);}
  }
}

static void startCoverDecodeWorker() {
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
  if(g_coverDecodeStarted)return;
  g_coverDecodeStop=false;g_coverDecodeStarted=true;
  g_coverDecodeWorker=std::thread(coverDecodeThread);
}

static void stopCoverDecodeWorker() {
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    if(!g_coverDecodeStarted)return;
    g_coverDecodeStop=true;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  g_coverDecodeCondition.notify_all();
  if(g_coverDecodeWorker.joinable())g_coverDecodeWorker.join();
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);g_coverDecodeStarted=false;
}

static void cancelQueuedCoverDecodes() {
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    ++g_coverDecodeEpoch;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  for(Game &game:g_games){game.coverQueued=game.backgroundQueued=false;game.coverRequest=game.backgroundRequest=0;}
  g_coverDecodeCondition.notify_all();
}

static void queueCoverDecode(Game &game,bool priority,bool background=false) {
  bool &queued=background?game.backgroundQueued:game.coverQueued;
  bool &tried=background?game.triedBackground:game.triedCover;
  Uint64 &request=background?game.backgroundRequest:game.coverRequest;
  if((background?game.background:game.cover)||tried) return;
  if(queued){
    if(priority){
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      const auto found=std::find_if(g_coverDecodeJobs.begin(),g_coverDecodeJobs.end(),
          [&](const CoverDecodeJob &job){return job.request==request;});
      if(found!=g_coverDecodeJobs.end()&&found!=g_coverDecodeJobs.begin()){
        CoverDecodeJob job=std::move(*found);g_coverDecodeJobs.erase(found);
        g_coverDecodeJobs.emplace_front(std::move(job));g_coverDecodeCondition.notify_one();
      }
    }
    return;
  }
  if(g_cover_budget<=0)return;
  --g_cover_budget;
  CoverDecodeJob job;job.key=game.key;job.background=background;
  job.paths=background?std::vector<std::string>{backgroundPath(game)}:coverCandidatePaths(game);
  job.request=++g_coverRequestSerial;request=job.request;queued=true;
  CoverDecodeJob dropped;bool didDrop=false;
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);job.epoch=g_coverDecodeEpoch;
    if(g_coverDecodeJobs.size()>=COVER_JOB_LIMIT){
      dropped=std::move(g_coverDecodeJobs.back());g_coverDecodeJobs.pop_back();didDrop=true;
    }
    if(priority)g_coverDecodeJobs.emplace_front(std::move(job));
    else g_coverDecodeJobs.emplace_back(std::move(job));
  }
  if(didDrop) if(Game *old=findGameByKey(dropped.key)){
    Uint64 &oldRequest=dropped.background?old->backgroundRequest:old->coverRequest;
    if(oldRequest==dropped.request){
      (dropped.background?old->backgroundQueued:old->coverQueued)=false;
      oldRequest=0;
    }
  }
  g_coverDecodeCondition.notify_one();
}

static void touchCover(Game &g) {
  if (g.cover) g.coverUse = ++g_coverUseSerial;
}

static void evictLeastRecentlyUsedCover() {
  Game *victim = nullptr;
  for (auto &candidate : g_games)
    if (candidate.cover && (!victim || candidate.coverUse < victim->coverUse)) victim = &candidate;
  if (!victim) return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->coverUse = 0;
  victim->triedCover = false;
}

static void installCover(Game &g, SDL_Texture *cover) {
  if (!cover) return;
  size_t resident = 0;
  for (const auto &candidate : g_games) if (candidate.cover) resident++;
  if (resident >= COVER_CACHE_LIMIT) evictLeastRecentlyUsedCover();
  g.cover = cover;
  g.coverAt = SDL_GetTicks();
  touchCover(g);
}

static void installBackground(Game &game,SDL_Texture *texture){
  size_t resident=0;
  Game *victim=nullptr;
  for(Game &candidate:g_games){
    if(!candidate.background) continue;
    ++resident;
    if(candidate.key==g_backgroundCurrent||candidate.key==g_backgroundPrevious) continue;
    if(!victim||candidate.backgroundUse<victim->backgroundUse) victim=&candidate;
  }
  if(resident>=BACKGROUND_CACHE_LIMIT){
    if(!victim){ SDL_DestroyTexture(texture); return; }
    SDL_DestroyTexture(victim->background);victim->background=nullptr;
    victim->backgroundUse=0;victim->triedBackground=false;
  }
  game.background=texture;game.backgroundUse=++g_coverUseSerial;
}

static SDL_Texture *uploadCoverTexture(const CoverDecodeResult &result) {
  if(result.width<1||result.height<1||result.pixels.empty()||!g_ren)return nullptr;
  SDL_Texture *texture=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STATIC,result.width,result.height);
  if(texture&&SDL_UpdateTexture(texture,nullptr,result.pixels.data(),result.width*4)!=0){
    SDL_DestroyTexture(texture);texture=nullptr;
  }
  if(!texture){
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<Uint8*>(result.pixels.data()),result.width,result.height,
        32,result.width*4,SDL_PIXELFORMAT_RGBA32);
    if(surface){texture=SDL_CreateTextureFromSurface(g_ren,surface);SDL_FreeSurface(surface);}
  }
  if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static void pumpCoverDecodeResults() {
  int uploads=0,processed=0;
  while(processed<12){
    CoverDecodeResult result;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(g_coverDecodeReady.empty())break;
      if(!g_coverDecodeReady.front().pixels.empty()&&uploads>=COVER_UPLOAD_BUDGET)break;
      result=std::move(g_coverDecodeReady.front());g_coverDecodeReady.pop_front();
    }
    g_coverDecodeCondition.notify_one();++processed;
    Game *game=findGameByKey(result.key);
    if(!game||(result.background?game->backgroundRequest:game->coverRequest)!=result.request)continue;
    (result.background?game->backgroundQueued:game->coverQueued)=false;
    (result.background?game->triedBackground:game->triedCover)=true;
    if(!result.pixels.empty()){
      SDL_Texture *texture=uploadCoverTexture(result);++uploads;
      if(texture){
        if(result.background) installBackground(*game,texture);
        else installCover(*game,texture);
      }
    }
  }
}

static void ensureCover(Game &g,bool priority=false) {
  if (g.cover) { touchCover(g); return; }
  queueCoverDecode(g,priority);
}
static void reloadCover(Game &g) {
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.coverUse=0;g.triedCover=false;g.coverQueued=false;g.coverRequest=0;
  g_cover_budget=std::max(g_cover_budget,1);queueCoverDecode(g,true);
}

static void reloadBackground(Game &game){
  if(game.background) SDL_DestroyTexture(game.background);
  game.background=nullptr;game.backgroundUse=0;game.triedBackground=false;
  game.backgroundQueued=false;game.backgroundRequest=0;
}

static SDL_Rect fitArtworkRect(int width,int height,const SDL_Rect &bounds){
  if(width<1||height<1||bounds.w<1||bounds.h<1) return {bounds.x,bounds.y,0,0};
  int w=bounds.w,h=std::max(1,(int)((long long)height*w/width));
  if(h>bounds.h){ h=bounds.h;w=std::max(1,(int)((long long)width*h/height)); }
  return {bounds.x+(bounds.w-w)/2,bounds.y+(bounds.h-h)/2,w,h};
}

static void drawBackgroundArtwork(const std::string &key,Uint8 alpha){
  Game *game=findGameByKey(key);
  if(!game||!alpha) return;
  SDL_Texture *texture=game->background?game->background:game->triedBackground?game->cover:nullptr;
  if(!texture) return;
  if(game->background) game->backgroundUse=++g_coverUseSerial;
  else touchCover(*game);
  int width=0,height=0;SDL_QueryTexture(texture,nullptr,nullptr,&width,&height);
  if(width<1||height<1) return;
  SDL_Rect src{0,0,width,height},destination{0,0,SW,SH};
  if((long long)width*SH>(long long)height*SW){src.w=height*SW/SH;src.x=(width-src.w)/2;}
  else {src.h=width*SH/SW;src.y=(height-src.h)/2;}
  SDL_SetTextureColorMod(texture,205,205,205);SDL_SetTextureAlphaMod(texture,alpha);
  SDL_RenderCopy(g_ren,texture,&src,&destination);
  SDL_SetTextureColorMod(texture,255,255,255);SDL_SetTextureAlphaMod(texture,255);
}

static void drawClassicBackground(){
  Game *target=findGameByKey(g_backgroundTarget);
  bool ready=!target;
  if(target){
    if(target->background) target->backgroundUse=++g_coverUseSerial;
    else queueCoverDecode(*target,true,true);
    if(target->triedBackground&&!target->background) ensureCover(*target,true);
    ready=target->background||(target->triedBackground&&(target->cover||target->triedCover));
  }
  if(ready&&g_backgroundCurrent!=g_backgroundTarget){
    g_backgroundPrevious=g_backgroundCurrent;g_backgroundCurrent=g_backgroundTarget;
    g_backgroundSince=SDL_GetTicks();
  }
  const Uint32 elapsed=SDL_GetTicks()-g_backgroundSince;
  const Uint8 alpha=!g_uiAnimations||elapsed>=300?255:(Uint8)(elapsed*255/300);
  if(alpha==255) g_backgroundPrevious.clear();
  else g_interactionAnimationUntil=SDL_GetTicks()+100;
  drawBackgroundArtwork(g_backgroundPrevious,255);
  drawBackgroundArtwork(g_backgroundCurrent,alpha);
  fillRect(0,0,SW,SH,{0,0,0,25});
  drawGradient({0,0,SW*2/3,SH},{0,0,0,120},{0,0,0,0},true);
  drawGradient({0,SH/2,SW,SH-SH/2},{0,0,0,0},{0,0,0,220});
}

static bool promptTextMode(const char *header, const char *initial, char *out, size_t outSize,
                           bool password, bool allowEmpty,
                           const char *subText=nullptr, const char *guideText=nullptr) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  if(password) swkbdConfigMakePresetPassword(&kbd); else swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (subText) swkbdConfigSetSubText(&kbd, subText);
  if (guideText) swkbdConfigSetGuideText(&kbd, guideText);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && (allowEmpty || out[0]);
}
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  return promptTextMode(header,initial,out,outSize,false,false);
}

struct FileClipboard {
  std::string path;
  bool move=false;
};
static FileClipboard g_fileClipboard;

static bool filesystemRoot(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t colon=normalized.find(':');
  if(colon==std::string::npos) return normalized=="/";
  for(size_t i=colon+1;i<normalized.size();i++) if(normalized[i]!='/') return false;
  return true;
}

static std::string parentFolder(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  if(filesystemRoot(normalized)) return {};
  size_t slash=normalized.find_last_of('/');
  if(slash==std::string::npos) return {};
  size_t colon=normalized.find(':');
  if(colon!=std::string::npos && slash<=colon+1) return normalized.substr(0,colon+2);
  return normalized.substr(0,slash);
}

static std::string fileNameOf(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t slash=normalized.find_last_of('/');
  return slash==std::string::npos?normalized:normalized.substr(slash+1);
}

static std::string humanBytes(uint64_t value) {
  static const char *units[]={"B","KiB","MiB","GiB","TiB"};
  double amount=(double)value;
  size_t unit=0;
  while(amount>=1024.0&&unit+1<sizeof(units)/sizeof(*units)){ amount/=1024.0; unit++; }
  char text[64];
  snprintf(text,sizeof(text),unit==0?"%.0f %s":"%.1f %s",amount,units[unit]);
  return text;
}

static std::string deviceOf(const std::string &path) {
  size_t colon=path.find(':');
  return foldedKey(colon==std::string::npos?std::string{}:path.substr(0,colon));
}

static bool pathAtOrBelow(const std::string &path,const std::string &root) {
  std::string candidate=pathIdentity(path), base=pathIdentity(root);
  if(base.empty()||candidate.size()<base.size()||candidate.compare(0,base.size(),base)!=0) return false;
  if(candidate.size()==base.size()) return true;
  return base.back()=='/'||candidate[base.size()]=='/';
}

static void replaceSavedPathPrefix(const std::string &oldPath,const std::string &newPath) {
  const std::string normalizedOld=normalizeLocationPath(oldPath);
  const std::string normalizedNew=normalizeLocationPath(newPath);
  const std::string oldIdentity=pathIdentity(normalizedOld);
  auto replace=[&](std::vector<std::string> &paths){
    for(auto &path:paths){
      const std::string normalizedPath=normalizeLocationPath(path);
      const std::string identity=pathIdentity(normalizedPath);
      if(identity==oldIdentity) path=normalizedNew;
      else if(identity.size()>oldIdentity.size() && identity.compare(0,oldIdentity.size(),oldIdentity)==0 && identity[oldIdentity.size()]=='/')
        path=normalizeLocationPath(normalizedNew+normalizedPath.substr(normalizedOld.size()));
    }
  };
  auto sources=loadGameSources(); replace(sources); saveGameSources(sources);
  auto favorites=loadFavoriteFolders(); replace(favorites); saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty() && pathAtOrBelow(g_fileClipboard.path,normalizedOld)){
    const std::string clipboardPath=normalizeLocationPath(g_fileClipboard.path);
    g_fileClipboard.path=normalizeLocationPath(normalizedNew+clipboardPath.substr(normalizedOld.size()));
  }
  g_rescanAfterSettings=true;
}

static bool validEntryName(const std::string &name) {
  if(name.empty()||name=="."||name==".."||name.size()>255) return false;
  for(unsigned char c:name) if(c<' '||c=='/'||c=='\\'||c==':') return false;
  return true;
}

static bool removeTreeInternal(const std::string &path) {
  if(filesystemRoot(path)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0) return errno==ENOENT;
  if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) return remove(path.c_str())==0;
  if(!S_ISDIR(st.st_mode)) return false;
  DIR *dir=opendir(path.c_str()); if(!dir) return false;
  bool ok=true; struct dirent *entry;
  while(ok&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=removeTreeInternal(join(path,entry->d_name));
  }
  if(closedir(dir)!=0) ok=false;
  return ok&&rmdir(path.c_str())==0;
}

struct TransferState {
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> done{0};
  std::string current,error;
  std::vector<unsigned char> buffer=std::vector<unsigned char>(1<<18);
  std::mutex detailMutex;
  std::atomic<bool> cancelled{false};
};

static void setTransferDetail(TransferState &state,const std::string &current,const std::string &error={}) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  if(!current.empty()) state.current=current;
  if(!error.empty()) state.error=error;
}

static std::string transferError(TransferState &state) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  return state.error;
}

static bool transferFrame(TransferState &state) {
  if(!beginUiFrame()){ state.cancelled.store(true); return false; }
  SDL_Event event;
  while(pollUiEvent(event)){
    pumpStick(event);
    int tx=0,ty=0;
    if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-100) state.cancelled.store(true);
    if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) state.cancelled.store(true);
  }
  std::string current;
  { std::lock_guard<std::mutex> lock(state.detailMutex); current=state.current; }
  clearUiBackground();
  drawLocalizedHeader("File transfer",nullptr);
  int bw=SW*2/3,bx=(SW-bw)/2,by=SH/2-24,bh=42;
  glassPanel(bx-32,settingsListY(),bw+64,by+132-settingsListY());
  drawTextC(g_font_sm,SW/2,settingsListY()+20,ellipsizedText(g_font_sm,current,bw).c_str(),COL_DIM);
  uint64_t done=state.done.load(std::memory_order_relaxed);
  uint64_t total=state.total.load(std::memory_order_relaxed);
  uint64_t progress=total?std::min(done,total):0;
  drawProgressBar(bx,by,bw,bh,total?(double)progress/total:0.0);
  char text[96];
  int percent=total?(int)(progress*100/total):0;
  snprintf(text,sizeof(text),"%d%%  -  %.1f / %.1f MiB",percent,done/1048576.0,total/1048576.0);
  drawTextC(g_font,SW/2,by+66,text,COL_TXT);
  if(state.cancelled.load()) drawTextC(g_font_sm,SW/2,SH-72,"Cancelling...",COL_VAL);
  else drawSettingsFooter("B  Cancel");
  SDL_RenderPresent(g_ren);
  return !state.cancelled.load();
}

static bool measureTree(const std::string &path,TransferState &state) {
  if(state.cancelled.load(std::memory_order_relaxed)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)){ state.total.fetch_add((uint64_t)st.st_size,std::memory_order_relaxed); return true; }
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  DIR *dir=opendir(path.c_str()); if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=measureTree(join(path,entry->d_name),state);
  }
  if(closedir(dir)!=0) ok=false;
  return ok;
}

static bool copyFileAtomic(const std::string &source,const std::string &destination,TransferState &state) {
  setTransferDetail(state,fileNameOf(source));
  const std::string partial=destination+".nx-part", backup=destination+".nx-old";
  remove(partial.c_str());
  FILE *input=fopen(source.c_str(),"rb");
  if(!input){ setTransferDetail(state,{},"Could not open the source file"); return false; }
  FILE *output=fopen(partial.c_str(),"wb");
  if(!output){ fclose(input); setTransferDetail(state,{},"Could not create the destination file"); return false; }
  bool ok=true;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)){
    size_t count=fread(state.buffer.data(),1,state.buffer.size(),input);
    if(count){
      if(fwrite(state.buffer.data(),1,count,output)!=count){ setTransferDetail(state,{},"Write failed; check free space and permissions"); ok=false; break; }
      state.done.fetch_add(count,std::memory_order_relaxed);
    }
    if(count<state.buffer.size()){
      if(ferror(input)){ setTransferDetail(state,{},"Read failed"); ok=false; }
      break;
    }
  }
  if(state.cancelled.load()) ok=false;
  if(ok&&fflush(output)!=0){ setTransferDetail(state,{},"Could not flush the destination file"); ok=false; }
  if(ok&&fsync(fileno(output))!=0){ setTransferDetail(state,{},"Could not commit the destination file"); ok=false; }
  if(fclose(input)!=0&&ok){ setTransferDetail(state,{},"Could not close the source file"); ok=false; }
  if(fclose(output)!=0&&ok){ setTransferDetail(state,{},"Could not close the destination file"); ok=false; }
  if(!ok||state.cancelled.load()){ remove(partial.c_str()); return false; }
  struct stat destinationStat{}; bool existed=stat(destination.c_str(),&destinationStat)==0;
  if(existed){
    struct stat backupStat{};
    if(lstat(backup.c_str(),&backupStat)==0){ setTransferDetail(state,{},"A previous backup file blocks this operation"); remove(partial.c_str()); return false; }
    if(rename(destination.c_str(),backup.c_str())!=0){ setTransferDetail(state,{},"Could not preserve the existing destination"); remove(partial.c_str()); return false; }
  }
  if(rename(partial.c_str(),destination.c_str())!=0){
    if(existed) rename(backup.c_str(),destination.c_str());
    setTransferDetail(state,{},"Could not finalize the copied file"); remove(partial.c_str()); return false;
  }
  if(existed) remove(backup.c_str());
  return true;
}

static bool copyTree(const std::string &source,const std::string &destination,TransferState &state) {
  struct stat st{};
  if(lstat(source.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)) return copyFileAtomic(source,destination,state);
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  if(mkdir(destination.c_str(),0777)!=0){ setTransferDetail(state,{},"Could not create a destination folder"); return false; }
  DIR *dir=opendir(source.c_str());
  if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=copyTree(join(source,entry->d_name),join(destination,entry->d_name),state);
  }
  if(closedir(dir)!=0&&ok){ setTransferDetail(state,{},"Could not close a source folder"); ok=false; }
  return ok&&!state.cancelled.load();
}

static bool enoughFreeSpace(const std::string &folder,uint64_t bytes) {
  struct statvfs info{};
  if(statvfs(folder.c_str(),&info)!=0||!info.f_frsize) return true;
  return bytes<=static_cast<uint64_t>(info.f_bavail)*info.f_frsize;
}

static bool executePaste(const std::string &folder) {
  if(g_fileClipboard.path.empty()) return false;
  struct stat sourceStat{};
  if(lstat(g_fileClipboard.path.c_str(),&sourceStat)!=0){ modalMessageStatic("Paste failed",{"The copied item is no longer available."}); g_fileClipboard={}; return false; }
  const std::string destination=join(folder,fileNameOf(g_fileClipboard.path));
  if(pathIdentity(destination)==pathIdentity(g_fileClipboard.path) ||
     (S_ISDIR(sourceStat.st_mode)&&pathAtOrBelow(destination,g_fileClipboard.path))){
    modalMessageStatic("Paste failed",{"The destination cannot be inside the source."}); return false;
  }
  struct stat destinationStat{}; bool destinationExists=lstat(destination.c_str(),&destinationStat)==0;
  if(destinationExists&&S_ISDIR(sourceStat.st_mode)){
    modalMessage(uiText("Folder already exists").c_str(),{uiText("Choose another destination or rename the folder first."),destination}); return false;
  }
  if(destinationExists&&!S_ISREG(destinationStat.st_mode)){
    modalMessageStatic("Paste failed",{"The destination is not a regular file."}); return false;
  }
  if(destinationExists&&!confirmBox(uiText("Replace existing file?").c_str(),{fileNameOf(destination),"",uiText("The existing file will be replaced.")})) return false;

  bool sameDevice=deviceOf(g_fileClipboard.path)==deviceOf(destination);
  if(g_fileClipboard.move&&sameDevice){
    const std::string backup=destination+".nx-old";
    bool preserved=false;
    if(destinationExists){
      struct stat backupStat{};
      if(lstat(backup.c_str(),&backupStat)==0||rename(destination.c_str(),backup.c_str())!=0){ modalMessageStatic("Move failed",{"Could not preserve the existing destination."}); return false; }
      preserved=true;
    }
    if(rename(g_fileClipboard.path.c_str(),destination.c_str())==0){
      if(preserved) remove(backup.c_str());
      replaceSavedPathPrefix(g_fileClipboard.path,destination);
      g_fileClipboard={}; toastStatic("Move complete"); return true;
    }
    if(preserved) rename(backup.c_str(),destination.c_str());
  }

  TransferState state;
  setTransferDetail(state,"Preparing transfer...");
  bool ok=false;
  std::atomic<bool> complete{false};
  nx2SetCpuBoost(true);
  std::thread worker([&](){
    ok=measureTree(g_fileClipboard.path,state);
    if(ok&&!state.cancelled.load()&&!enoughFreeSpace(folder,state.total.load(std::memory_order_relaxed))){
      setTransferDetail(state,{},"The destination does not have enough available space");
      ok=false;
    }
    if(ok&&!state.cancelled.load()){
      setTransferDetail(state,fileNameOf(g_fileClipboard.path));
      ok=copyTree(g_fileClipboard.path,destination,state);
    }
    complete.store(true,std::memory_order_release);
  });
  while(!complete.load(std::memory_order_acquire)){
    transferFrame(state);
    waitForNextFrame();
  }
  worker.join();
  nx2SetCpuBoost(false);
  if(!ok&&S_ISDIR(sourceStat.st_mode)) removeTreeInternal(destination);
  if(ok&&g_fileClipboard.move){
    if(removeTreeInternal(g_fileClipboard.path)) replaceSavedPathPrefix(g_fileClipboard.path,destination);
    else { modalMessageStatic("Move incomplete",{"The copy completed, but the original could not be removed completely.","Review both locations before trying again."}); ok=false; }
  }
  if(ok){ g_rescanAfterSettings=true; if(g_fileClipboard.move) g_fileClipboard={}; toastStatic("Transfer complete"); }
  else if(state.cancelled.load()){ toastStatic("Transfer cancelled"); }
  else { std::string error=transferError(state); modalMessage(uiText("Transfer failed").c_str(),{error.empty()?uiText("The file transfer could not be completed."):error}); }
  return ok;
}

static void drawBusyTaskFrame(const std::string &title,const std::string &detail,bool cancellable,bool cancelled){
  clearUiBackground();
  drawLocalizedHeader(title.c_str(),nullptr);
  const int width=std::min(920,SW-180),height=250;
  const int x=(SW-width)/2,y=(SH-height)/2;
  glassPanel(x,y,width,height);
  std::string working=tr("Working");
  working.append((SDL_GetTicks()/220)%4,'.');
  drawTextC(g_font_big,SW/2,y+32,working.c_str(),COL_VAL);
  drawTextC(g_font_sm,SW/2,y+92,fittedText(g_font_sm,detail,width-108).c_str(),COL_DIM);
  drawWrappedCentered(g_font_sm,SW/2,y+136,width-108,32,2,
      tr(cancelled?"Cancelling at the next safe point...":
         "Do not remove the active storage device or close ARMSX2-nx."),COL_DIM);
  if(cancellable&&!cancelled) drawSettingsFooter("B  Cancel",y+height-28);
}

static void runBusyTask(const std::string &title,const std::string &detail,
                        const std::function<void()> &task,std::atomic_bool *cancel=nullptr) {
  std::atomic_bool complete{false};
  nx2SetCpuBoost(true);
  std::thread worker([&]{ task(); complete.store(true,std::memory_order_release); wakeUi(0x42555359); });
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){ if(cancel) cancel->store(true,std::memory_order_release); waitForNextFrame(); continue; }
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);
      if(cancel&&event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)
        cancel->store(true,std::memory_order_release);
    }
    drawBusyTaskFrame(title,detail,cancel!=nullptr,cancel&&cancel->load(std::memory_order_acquire));
    SDL_RenderPresent(g_ren);
    waitForNextFrame();
  }
  worker.join();
  nx2SetCpuBoost(false);
}

enum class BrowserMode { SelectFolder, SelectImage, Manage };
enum class BrowserItemKind { Use, Up, Paste, Favorite, Directory, File, Location };
struct BrowserItem {
  std::string label,path;
  BrowserItemKind kind=BrowserItemKind::File;
  bool directory=false;
  std::string value;
  BrowserItem(std::string itemLabel,std::string itemPath,BrowserItemKind itemKind,
              bool isDirectory,std::string itemValue)
      : label(std::move(itemLabel)),path(std::move(itemPath)),kind(itemKind),directory(isDirectory),
        value(std::move(itemValue)) {}
};

static std::vector<BrowserItem> browserItems(const std::string &current,BrowserMode mode,bool &opened) {
  std::vector<BrowserItem> items; opened=true;
  if(current.empty()){
    items.push_back({"SD card","sdmc:/",BrowserItemKind::Location,true,"Internal SD storage"});
    for(const auto &favorite:loadFavoriteFolders())
      items.push_back({"Pinned - "+favorite,favorite,BrowserItemKind::Location,true,"Pinned folder"});
    return items;
  }
  if(mode==BrowserMode::SelectFolder) items.push_back({"[ Use this folder ]",current,BrowserItemKind::Use,true,current});
  if(mode==BrowserMode::Manage&&!g_fileClipboard.path.empty())
    items.push_back({std::string("[ Paste ")+(g_fileClipboard.move?"moved":"copied")+" item here ]",current,
                     BrowserItemKind::Paste,true,(g_fileClipboard.move?"Move ":"Copy ")+fileNameOf(g_fileClipboard.path)});
  if(mode==BrowserMode::Manage){
    auto favorites=loadFavoriteFolders();
    bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(current); });
    items.push_back({pinned?"[ Unpin this folder ]":"[ Pin this folder ]",current,BrowserItemKind::Favorite,true,
                     pinned?"Remove shortcut":"Keep in Locations"});
  }
  items.push_back({"[ .. locations / parent ]",parentFolder(current),BrowserItemKind::Up,true,"Parent folder"});
  DIR *dir=opendir(current.c_str());
  if(!dir){ opened=false; return items; }
  std::vector<BrowserItem> entries; struct dirent *entry;
  while((entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    std::string path=join(current,entry->d_name); struct stat info{};
    if(stat(path.c_str(),&info)!=0) continue;
    bool directory=S_ISDIR(info.st_mode);
    if(!directory&&mode==BrowserMode::SelectFolder) continue;
    if(!directory&&mode==BrowserMode::SelectImage){
      const size_t dot=path.find_last_of('.');
      std::string extension=dot==std::string::npos?std::string{}:path.substr(dot);
      std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char value){return (char)std::tolower(value);});
      if(extension!=".png"&&extension!=".jpg"&&extension!=".jpeg"&&extension!=".webp"&&extension!=".bmp") continue;
    }
    entries.push_back({std::string(entry->d_name)+(directory?"/":""),path,
                       directory?BrowserItemKind::Directory:BrowserItemKind::File,directory,
                       directory?"Folder":humanBytes((uint64_t)info.st_size)});
  }
  closedir(dir);
  std::sort(entries.begin(),entries.end(),[](const BrowserItem &left,const BrowserItem &right){
    if(left.directory!=right.directory) return left.directory>right.directory;
    return strcasecmp(left.label.c_str(),right.label.c_str())<0;
  });
  items.insert(items.end(),std::make_move_iterator(entries.begin()),std::make_move_iterator(entries.end()));
  return items;
}

static bool toggleFavorite(const std::string &path) {
  auto favorites=loadFavoriteFolders(); std::string identity=pathIdentity(path);
  auto iterator=std::find_if(favorites.begin(),favorites.end(),[&](const std::string &entry){ return pathIdentity(entry)==identity; });
  bool pinned=iterator==favorites.end();
  if(pinned){
    if(favorites.size()>=24){ toastStatic("Maximum of 24 pinned folders"); return false; }
    favorites.push_back(normalizeLocationPath(path));
  }
  else favorites.erase(iterator);
  saveFavoriteFolders(favorites); toastStatic(pinned?"Folder pinned":"Folder unpinned"); return true;
}

// Mirrors the two early-outs in browserActions(): the synthetic rows (Paste,
// Pin, parent, locations) have no menu, and outside Manage mode the menu only
// exists for a folder that can be pinned.
static bool browserHasActions(const BrowserItem &item,BrowserMode mode) {
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File&&
     item.kind!=BrowserItemKind::Use) return false;
  return mode==BrowserMode::Manage || item.directory;
}

static bool browserActions(const BrowserItem &item,BrowserMode mode) {
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File&&item.kind!=BrowserItemKind::Use) return false;
  std::vector<std::string> labels;
  if(mode==BrowserMode::Manage){ labels={"Copy","Move","Rename"}; }
  bool canPin=item.directory;
  bool pinned=false;
  if(canPin){
    auto favorites=loadFavoriteFolders();
    pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(item.path); });
    labels.push_back(pinned?"Unpin folder":"Pin folder");
  }
  if(labels.empty()) return false;
  std::vector<const char*> choices; for(const auto &label:labels) choices.push_back(label.c_str());
  int action=dropdown("File options",choices.data(),(int)choices.size(),0,true,true);
  if(action<0) return false;
  if(mode==BrowserMode::Manage&&action==0){ g_fileClipboard={item.path,false}; toastStatic("Copied to clipboard"); return false; }
  if(mode==BrowserMode::Manage&&action==1){ g_fileClipboard={item.path,true}; toastStatic("Move queued"); return false; }
  if(mode==BrowserMode::Manage&&action==2){
    char name[256]; std::string oldName=fileNameOf(item.path);
    if(!promptText("Rename",oldName.c_str(),name,sizeof(name))) return false;
    std::string newName=trim(name);
    if(!validEntryName(newName)){ modalMessageStatic("Invalid name",{"Names cannot contain /, \\, :, or control characters."}); return false; }
    std::string destination=join(parentFolder(item.path),newName); struct stat st{};
    if(lstat(destination.c_str(),&st)==0){ modalMessageStatic("Rename failed",{"An item with that name already exists."}); return false; }
    if(rename(item.path.c_str(),destination.c_str())!=0){ modalMessage(uiText("Rename failed").c_str(),{strerror(errno)}); return false; }
    replaceSavedPathPrefix(item.path,destination); toastStatic("Renamed"); return true;
  }
  if(canPin) return toggleFavorite(item.path);
  return false;
}

static std::string runFileBrowser(const std::string &start,BrowserMode mode) {
  std::string current=normalizeLocationPath(start);
  int sel=0,top=0;
  // One source of truth for the row geometry, shared by the draw and the tap test.
  const int listY=topBarH()+32,rowH=46;
  for(;;){
    bool opened=false; auto items=browserItems(current,mode,opened);
    if(!opened){ modalMessage(uiText("Folder unavailable").c_str(),{current,"",uiText("The device may be disconnected.")}); current.clear(); sel=top=0; continue; }
    int n=(int)items.size(),vis=std::max(1,(SH-listY-settingsFooterReserve())/rowH); if(n==0){ current.clear(); continue; }
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return {};
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48){ uiAudioPlay(UiSound::Back); return {}; }
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowH; if(ty>=y&&ty<y+rowH-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL){ if(current.empty()) return {}; current=parentFolder(current); sel=top=0; rebuild=true; }
        else if(event.cbutton.button==BTN_X){ if(browserActions(items[sel],mode)) rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&mode==BrowserMode::Manage&&!current.empty()&&!g_fileClipboard.path.empty()){ executePaste(current); rebuild=true; }
        else if(event.cbutton.button==BTN_CONFIRM){
          const BrowserItem item=items[sel];
          if(item.kind==BrowserItemKind::Use) return item.path;
          if(mode==BrowserMode::SelectImage&&item.kind==BrowserItemKind::File) return item.path;
          if(item.kind==BrowserItemKind::Paste){ executePaste(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Favorite){ toggleFavorite(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Up){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Directory){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Location){ current=item.path; sel=top=0; rebuild=true; }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      const char *title=mode==BrowserMode::Manage?"File manager":
                        mode==BrowserMode::SelectImage?"Select local cover":"Select game folder";
      const std::string location=current.empty()?uiText("Locations"):current;
      drawHeader(tr(title),location.c_str());
      glassPanel(42,listY-11,SW-84,std::max(1,std::min(vis,n-top))*rowH+16);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=listY+row*rowH; bool selected=index==sel; const auto &item=items[index];
        if(selected) drawRowHighlight(54,y-3,SW-108,rowH-4);
        SDL_Color color=item.kind==BrowserItemKind::Use||item.kind==BrowserItemKind::Paste||item.kind==BrowserItemKind::Favorite?COL_HI:(item.directory?COL_TXT:(SDL_Color){120,220,120,255});
        const std::string value=ellipsizedText(g_font_sm,item.value,SW/3);
        const int valueWidth=value.empty()?0:textW(g_font_sm,value.c_str());
        const int labelWidth=std::max(80,SW-180-(valueWidth?valueWidth+32:0));
        if(selected) drawScrollTextL(g_font,80,y,labelWidth,item.label.c_str(),COL_VAL);
        else drawText(g_font,80,y,fittedText(g_font,item.label,labelWidth).c_str(),color);
        if(!value.empty()) drawTextR(g_font_sm,SW-80,y+3,value.c_str(),selected?COL_VAL:COL_DIM);
      }
      // A both descends into folders and picks files, so it is "Select"; the
      // rest of the row depends on what this row and the clipboard can do.
      const BrowserItem &row=items[sel];
      const bool canPaste=mode==BrowserMode::Manage&&!current.empty()&&!g_fileClipboard.path.empty();
      const bool canOpen=!(mode==BrowserMode::Manage&&row.kind==BrowserItemKind::File);
      FootItem footer[4];int hintCount=0;
      if(canOpen) footer[hintCount++]={"A","Select"};
      if(browserHasActions(row,mode))
        footer[hintCount++]={"X",mode==BrowserMode::Manage?"Actions":"Pin"};
      if(canPaste) footer[hintCount++]={"Y","Paste"};
      footer[hintCount++]={"B","Back"};
      drawFooterHints(footer,hintCount,SH-26);
      SDL_RenderPresent(g_ren); waitForNextFrame();
    }
  }
}

static std::string browseFolder(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectFolder);
}

static std::string browseCoverImage(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectImage);
}

static void runFileManager() {
  runFileBrowser({},BrowserMode::Manage);
}

static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key,o.def);
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
static bool optEnabled(const Opt &o) {
  return !o.gateKey || strcmp(iniGet(o.gateKey, ""), o.gateOff) != 0;
}
static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); snprintf(out,n,"%s", i>=0?tr(o.ch[i].label):iniGet(o.key,o.def)); }
  else if (o.type==OT_RANGE) snprintf(out,n,"%s", iniGet(o.key,o.def));
  else if (o.type==OT_SCALED_RANGE) {
    int value=(int)std::lround(std::strtod(iniGet(o.key,o.def),nullptr)*o.multiplier);
    snprintf(out,n,"%d%s",value,o.suffix?o.suffix:"");
  }
  else if (o.type==OT_SUBMENU) snprintf(out,n,">");
}
static void optSetChoice(const Opt &o,const char *value) {
  iniSet(o.key,value);
  if(o.key && !strcmp(o.key,"Wrapper/Language")){
    const std::string previous(g_localization.Preference());
    if(!setLauncherLanguage(value)) iniSet(o.key,previous.c_str());
  }
}
static void optAdjust(const Opt &o, int dir) {
  if (!optEnabled(o)) return;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch; optSetChoice(o,o.ch[i].val); }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
  else if (o.type==OT_SCALED_RANGE){
    int v=(int)std::lround(std::strtod(iniGet(o.key,o.def),nullptr)*o.multiplier)+dir*o.step;
    if(v<o.lo)v=o.lo;
    if(v>o.hi)v=o.hi;
    char b[24]; snprintf(b,sizeof(b),"%g",(double)v/o.multiplier); iniSet(o.key,b);
  }
}

// The questions a footer asks about the currently selected row, so that a hint
// appears only when its button really does something on this row.
static bool canResetOption(const Opt &option) {
  return option.key && option.type!=OT_SUBMENU && optEnabled(option);
}
// Left / Right only moves a value that optAdjust() knows how to move.
static bool canAdjustOption(const Opt &option) {
  return optEnabled(option) &&
         (option.type==OT_CHOICE || option.type==OT_RANGE || option.type==OT_SCALED_RANGE);
}
// What A does on this row, or nullptr when it does nothing at all.
static const char *confirmLabelForOption(const Opt &option) {
  if (option.type==OT_SUBMENU) return "Open";
  if (!optEnabled(option)) return nullptr;
  // "Choose" is only honest where A opens the value popup. Everywhere else it
  // falls through to optAdjust(+1), which is exactly what Right already does.
  if (option.type==OT_CHOICE && option.nch>2) return "Choose";
  if (option.type==OT_CHOICE) return "Toggle";
  return "Increase";
}

static bool resetOption(const Opt &option)
{
  if (!canResetOption(option))
    return false;
  if (g_active==&g_global)
    storeSet(g_global,option.key,option.def);
  else
    storeRemove(*g_active,option.key); // Per-game Reset means inherit the global value.
  if (!strcmp(option.key,"Wrapper/Language"))
    setLauncherLanguage(storeGet(g_global,"Wrapper/Language","system"));
  return true;
}

static void beginScreenFx(){ g_fxT = SDL_GetTicks(); g_hy = -1; g_textScroll.clear(); g_footN=0; g_libraryTriggers={}; }
static void drawFadeIn(){
  if(!g_uiAnimations) return;
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
// top-bar height, shared by the grid header (gridLayout y0) and the settings header
static int topBarH(){ return 80; }
static void drawPageHeader(const char *title,const char *eyebrow,const char *summary,const char *detail=nullptr){
  const int height=topBarH(),margin=40,left=margin;
  drawGradient({0,0,SW,height+48},{0,0,0,125},{0,0,0,0});
  const int metadataWidth=std::min(SW/3,std::max(textW(g_font_sm,summary),textW(g_font_sm,detail)));
  const int titleWidth=SW-left-margin-(metadataWidth?metadataWidth+32:0);
  drawText(g_font_caption,left,9,fittedText(g_font_caption,eyebrow?eyebrow:"",titleWidth).c_str(),COL_DIM);
  drawScrollTextL(g_font_big,left,31,titleWidth,title?title:"",COL_TXT);
  if(summary) drawTextR(g_font_sm,SW-margin,16,fittedText(g_font_sm,summary,metadataWidth).c_str(),COL_TXT);
  if(detail) drawTextR(g_font_sm,SW-margin,44,fittedText(g_font_sm,detail,metadataWidth).c_str(),COL_DIM);
}
static void drawHeader(const char *title, const char *ctx){
  drawPageHeader(title,"ARMSX2-nx",ctx);
}
static void drawLocalizedHeader(const char *title,const char *ctx){
  const std::string_view shown=g_localization.Translate(title?title:"");
  drawHeader(shown.data(),ctx);
}
// Labels a group of rows inside a page: accent tick, heading, hairline rule.
static void drawSectionHeading(const char *title,int x,int y,int width){
  const std::string shown=fittedText(g_font_sm,tr(title),width-24);
  const int height=fontHeight(g_font_sm),textWidth=textW(g_font_sm,shown.c_str());
  fillRect(x,y+4,3,height-8,COL_SEL);
  drawText(g_font_sm,x+14,y,shown.c_str(),COL_HI);
  fillRect(x+textWidth+30,y+height/2,width-textWidth-30,1,(SDL_Color){255,255,255,28});
}
// The centred settings column geometry: one source of truth for every list
// screen's row height, its list origin and the space its footer needs.
static int settingsRowH(){ return 44; }
static int settingsListY(){ return topBarH()+24; }
static int settingsFooterReserve(){ return 80; }
#define ROW_H (settingsRowH())
#define LIST_Y0 (settingsListY())
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int w = SW-180; if (w>980) w=980;
  *colW=w; *colX=(SW-w)/2; *labelX=*colX+40; *valX=*colX+w-40;
}
static int listVis(){ int v=(SH-LIST_Y0-settingsFooterReserve())/ROW_H; return v<1?1:v; }

// One settings row: a hairline separator under unselected rows, the value in the
// small font, a drawn chevron when the value is a literal ">", and a marquee on
// the label of the selected row. Label and value widths are computed against
// each other so a long label can never collide with a long value.
static void drawSettingsRowText(const char *label,const char *value,
                                int slotY,int colW,int labelX,int valX,
                                bool current,SDL_Color labelColor,
                                SDL_Color valueColor,bool scrollValue=false,
                                int rowHeight=0){
  const int actualRowHeight=rowHeight>0?rowHeight:settingsRowH();
  if(!current) fillRect(labelX,slotY+actualRowHeight-1,valX-labelX,1,(SDL_Color){255,255,255,10});
  const bool submenu=value&&strcmp(value,">")==0;
  const int valueWidth=submenu?16:std::min(textW(g_font_sm,value),colW/2-32);
  const int labelWidth=std::max(40,valX-labelX-valueWidth-24);
  const int y=slotY+(actualRowHeight-fontHeight(g_font))/2;
  if(current) drawScrollTextL(g_font,labelX,y,labelWidth,label,labelColor);
  else drawText(g_font,labelX,y,fittedText(g_font,label?label:"",labelWidth).c_str(),labelColor);
  if(submenu){
    const int cy=slotY+actualRowHeight/2;
    SDL_SetRenderDrawColor(g_ren,valueColor.r,valueColor.g,valueColor.b,valueColor.a);
    SDL_RenderDrawLine(g_ren,valX-10,cy-6,valX-4,cy);
    SDL_RenderDrawLine(g_ren,valX-4,cy,valX-10,cy+6);
  }else{
    const int valueY=slotY+(actualRowHeight-fontHeight(g_font_sm))/2;
    if(scrollValue) drawScrollTextR(g_font_sm,valX,valueY,colW/2-32,value,valueColor);
    else drawTextR(g_font_sm,valX,valueY,fittedText(g_font_sm,value?value:"",colW/2-32).c_str(),valueColor);
  }
}

static void showHelpCard(const char *section,const char *title,const char *kind,
                         const std::string &description,const char *current,
                         const char *scope) {
  const std::string translatedSection=uiText(section&&*section?section:"Settings");
  const std::string translatedTitle=uiText(title&&*title?title:"Setting help");
  const std::string translatedKind=uiText(kind&&*kind?kind:"Setting");
  const std::string translatedDescription=uiText(description.c_str());
  const std::string translatedScope=uiText(scope&&*scope?scope:"");
  const std::string currentPrefix=uiText("Current: ");
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);
      int touchX=0,touchY=0;
      if(touchFeed(event,&touchX,&touchY)==TOUCH_TAP) return;
      if(event.type==SDL_CONTROLLERBUTTONDOWN&&
         (event.cbutton.button==BTN_CONFIRM||
          event.cbutton.button==BTN_CANCEL||
          event.cbutton.button==BTN_X)) return;
    }

    clearUiBackground();
    const int panelWidth=std::min(SW-120,1000);
    const int panelHeight=std::min(SH-96,500);
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    drawText(g_font_sm,panelX+40,panelY+24,translatedSection.c_str(),COL_DIM);
    drawText(g_font_big,panelX+40,panelY+58,translatedTitle.c_str(),COL_VAL);

    std::string metadata=translatedKind;
    if(!translatedScope.empty()){ metadata+="  |  "; metadata+=translatedScope; }
    drawText(g_font_sm,panelX+40,panelY+114,metadata.c_str(),COL_SEL);
    int bodyY=panelY+164;
    if(current&&*current){
      const char *prefix=currentPrefix.c_str();
      drawText(g_font_sm,panelX+40,panelY+146,prefix,COL_DIM);
      drawScrollTextL(g_font_sm,panelX+40+textW(g_font_sm,prefix),panelY+146,
                      panelWidth-80-textW(g_font_sm,prefix),current,COL_TXT);
      bodyY=panelY+198;
    }
    fillRect(panelX+40,bodyY-18,panelWidth-80,2,(SDL_Color){70,78,92,210});
    drawWrapped(g_font,panelX+40,bodyY,panelWidth-80,32,7,
                translatedDescription.c_str(),COL_TXT);
    // A, B and X all close; naming one is enough. The bare A / B glyphs this
    // used to draw read as broken hints with no caption beside them.
    FootItem footer[]={{"A","Close"},{"Touch","anywhere to close"}};
    drawFooterHints(footer,2,panelY+panelHeight-30);
    SDL_RenderPresent(g_ren);
    waitForNextFrame();
  }
}

static void showOptionHelp(const char *section,const Opt &option,
                           const char *scope) {
  SettingHelpInfo help=settingHelpFor(option);
  char value[256]={};
  const char *current=nullptr;
  if(option.type!=OT_SUBMENU){
    optValue(option,value,sizeof(value));
    current=value;
  }
  showHelpCard(section,option.label,help.kind,help.text,current,scope);
}

static const char *settingsScreenDescription(int screen) {
  switch(screen){
    case SCR_GRAPHICS:
      return "Selects the renderer (Drako3D or Vulkan), internal resolution, display shape, GS multithreading, texture filtering, on-screen display, and common visual patches.";
    case SCR_ENHANCE:
      return "Contains GS accuracy controls, post-processing, custom-texture support, and readback behavior. Higher accuracy often requires more GPU time.";
    case SCR_AUDIO:
      return "Controls volume, synchronization, and buffering. Smaller buffers reduce audio delay but are less tolerant of uneven performance.";
    case SCR_EMU:
      return "Controls EE/VU performance settings, boot behavior, frame-rate targets, game patches and cheats. In game, L + R + Plus opens ARMSX2's pause menu (save states, cheats, settings, exit).";
    case SCR_ADVANCED:
      return "Exposes per-game EE, VU and IOP execution, rounding, clamping, cache, and diagnostic controls. Defaults are strongly recommended unless a game needs a documented override.";
    case SCR_GAMEFIXES:
      return "Provides manual per-game workarounds. Automatic fixes from GameIndex.yaml remain active; leave these extra switches Off unless a game specifically needs one.";
    case SCR_FRAMERATE:
      return "Controls the normal target speed and the base NTSC/PAL video rates. Base-rate changes can alter game timing.";
    case SCR_CONTROLLER:
      return "Sets the connected PS2 controller ports. In game, L + R + Plus opens ARMSX2's pause menu.";
    case SCR_LOGGING:
      return "Controls the emulator's log. ARMSX2 writes sdmc:/switch/armsx2-nx/logs/emulog.txt, with boot.txt and exit.txt beside it, and the launcher writes logs/launcher.log there. Send those files with a bug report.";
    default:
      return "Opens this group of emulator settings.";
  }
}

static void renderSettings(int scr,int sel,int top,const char *ctx){
  clearUiBackground();
  const Screen &S=g_screens[scr];
  drawHeader(tr(S.title), ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  // The panel hugs the rows that are actually drawn: a short page no longer
  // leaves an empty card below the last row.
  glassPanel(colX-12,LIST_Y0-10,colW+24,std::max(1,std::min(vis,S.n-top))*ROW_H+18);
  float ty = (float)(LIST_Y0 + (sel-top)*ROW_H + 1);
  g_hy = (!g_uiAnimations||g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  drawRowHighlight(colX,(int)g_hy,colW,ROW_H-2);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,slotY=LIST_Y0+r*ROW_H; bool cur=(i==sel); bool en=optEnabled(S.opts[i]);
    SDL_Color lc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_TXT);
    SDL_Color vc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_DIM);
    char v[96]; optValue(S.opts[i],v,sizeof(v));
    drawSettingsRowText(tr(S.opts[i].label),v,slotY,colW,labelX,valX,cur,lc,vc);
  }
  if(S.n>vis){
    int trH=vis*ROW_H, trX=colX+colW+16, trY=LIST_Y0-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  // Only advertise what this row actually answers to: a submenu has nothing to
  // nudge with Left / Right, a status row has nothing to change at all, and a
  // gated-off row has nothing to reset.
  const Opt *row=(sel>=0&&sel<S.n)?&S.opts[sel]:nullptr;
  const char *confirm=row?confirmLabelForOption(*row):nullptr;
  FootItem footer[5];int hintCount=0;
  if(row&&canAdjustOption(*row)) footer[hintCount++]={"Left / Right","Change"};
  if(confirm) footer[hintCount++]={"A",confirm};
  footer[hintCount++]={"X","Info"};
  if(row&&canResetOption(*row)) footer[hintCount++]={"Y","Reset"};
  footer[hintCount++]={"B","Back"};
  drawFooterHints(footer,hintCount,SH-26);
  drawFadeIn();
  SDL_RenderPresent(g_ren);
}

static int dropdown(const char *title, const char *const *labels, int n, int cur,
                    bool localizeTitle,bool localizeChoices) {
  if(n<1) return -1;
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  // Geometry is hoisted out of the loop so the hit test and the draw can never
  // disagree about where a row is.
  const int rowH=settingsRowH()+8,vis=std::min(n,std::max(1,(SH-240)/rowH));
  const int pw=std::min(900,SW-64),ph=140+vis*rowH,px=(SW-pw)/2,py=(SH-ph)/2,ly=py+80;
  beginScreenFx();
  for (;;) {
    if(!beginUiFrame()) return -1;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP&&tx>=px+8&&tx<px+pw-8){
          for(int r=0;r<vis&&top+r<n;r++) if(ty>=ly+r*rowH&&ty<ly+(r+1)*rowH) return top+r;
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return -1;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    const std::string_view heading=localizeTitle?g_localization.Translate(title):std::string_view(title);
    drawScrollTextL(g_font_big,px+28,py+22,pw-56,heading.data(),COL_TXT);
    fillRect(px+28,py+66,pw-56,1,(SDL_Color){255,255,255,24});
    for(int r=0;r<vis && top+r<n;r++){
      const int i=top+r,y=ly+r*rowH; const bool curr=(i==sel);
      if(curr) drawRowHighlight(px+8,y,pw-16,rowH-4);
      const std::string_view shown=localizeChoices?g_localization.Translate(labels[i]):std::string_view(labels[i]);
      const int textY=y+(rowH-fontHeight(g_font))/2;
      if(curr) drawScrollTextL(g_font,px+32,textY,pw-76,shown.data(),COL_VAL);
      else drawText(g_font,px+32,textY,fittedText(g_font,std::string(shown),pw-76).c_str(),COL_TXT);
    }
    if(n>vis){
      const int height=vis*rowH,thumb=std::max(12,height*vis/n);
      fillRect(px+pw-18,ly,3,height,COL_CARD);
      fillRect(px+pw-18,ly+(height-thumb)*top/(n-vis),3,thumb,COL_SEL);
    }
    FootItem footer[]={{"A","Select"},{"B","Back"}};
    drawFooterHints(footer,2,py+ph-28);
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextFrame();
  }
}
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  std::vector<std::string> translated;
  std::vector<const char*> labels;
  int n = o.nch>32?32:o.nch;
  translated.reserve(n); labels.reserve(n);
  for(int i=0;i<n;i++) translated.emplace_back(g_localization.Translate(o.ch[i].label));
  for(const std::string &label:translated) labels.push_back(label.c_str());
  int idx = dropdown(tr(o.label), labels.data(), n, choiceIdx(o));
  if(idx>=0 && idx<o.nch) optSetChoice(o,o.ch[idx].val);
}

static int s_setSel[SCR_COUNT]={0}, s_setTop[SCR_COUNT]={0};
static void runSettings(int scr, const char *ctx) {
  const Screen &S=g_screens[scr];
  int sel=s_setSel[scr],top=s_setTop[scr];
  if(sel<0||sel>=S.n) sel=0;
  if(top<0||top>=S.n) top=0;
  while(sel<S.n-1 && !optEnabled(S.opts[sel])) sel++;
  auto nav=[&](int dir){ for(int k=0;k<S.n;k++){ sel=(sel+dir+S.n)%S.n; if(optEnabled(S.opts[sel])) break; } };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        int visible=listVis();
        if(touchScrollList(tk,sel,top,S.n,visible)){ s_setSel[scr]=sel; s_setTop[scr]=top; continue; }
        if(tk==TOUCH_SWIPE_L){ optAdjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ optAdjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=LIST_Y0+r*ROW_H;
            if(ty>=y && ty<y+ROW_H){ int ni=top+r; if(optEnabled(S.opts[ni])){ sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); } }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  optAdjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: optAdjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_SUBMENU){ runSettings(o.sub,ctx); beginScreenFx(); }
          else if(o.type==OT_CHOICE && o.nch>2 && optEnabled(o)){ optChoosePopup(o); beginScreenFx(); }
          else optAdjust(o,1);
          break;
        }
        case BTN_X:
          showOptionHelp(tr(S.title),S.opts[sel],ctx&&*ctx?tr("Per-game setting"):tr("Global setting"));
          beginScreenFx();
          break;
        case SDL_CONTROLLER_BUTTON_X:
          if(resetOption(S.opts[sel])){
            toast(tr("Setting reset to default"));
            beginScreenFx();
          }
          break;
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
      s_setSel[scr]=sel; s_setTop[scr]=top;
    }
    renderSettings(scr,sel,top,ctx);
    waitForNextFrame();
  }
}
static std::string launcherUpdateStatusText() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  switch(snapshot.state){
    case LauncherUpdateState::Checking: return "Checking...";
    case LauncherUpdateState::UpdateAvailable: return snapshot.release.tag+" available";
    case LauncherUpdateState::UpToDate: return "Up to date";
    case LauncherUpdateState::Downloading: {
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const uint64_t percent=total?std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      return "Downloading "+std::to_string(percent)+"%";
    }
    case LauncherUpdateState::ReadyToInstall: return "Ready to install";
    case LauncherUpdateState::Installing: return "Installing...";
    case LauncherUpdateState::Installed: return "Ready to exit";
    case LauncherUpdateState::Cancelled: return "Cancelled";
    case LauncherUpdateState::Error: return "Check failed";
    case LauncherUpdateState::Idle: break;
  }
  return std::string("Installed ")+installedReleaseTag();
}

static void launcherSettingsScreen() {
  static int savedSelection=0;
  const int optionCount=(int)(sizeof(S_launcher)/sizeof(Opt));
  const int listCount=optionCount;
  const int updateRow=listCount,selectionCount=listCount+1;
  int sel=std::max(0,std::min(savedSelection,selectionCount-1)),top=0;
  // Scroll the saved row back into view, or the highlight is drawn for a row
  // the window does not contain.
  {
    const int firstVisible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
    if(sel<listCount&&sel>=firstVisible)
      top=std::min(sel-firstVisible+1,std::max(0,listCount-firstVisible));
  }
  auto applyChange=[&](){
    setLauncherLanguage(storeGet(g_global,"Wrapper/Language","system"));
    applyLauncherAppearance();
    uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  };
  auto finish=[&](){
    savedSelection=sel;
    storeSave(g_global,LAUNCHER_INI);
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ finish(); return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
      const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
      const int buttonX=(SW-buttonWidth)/2;
      const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
      if(touchScrollList(touch,sel,top,listCount,visible)) continue;
      if(touch==TOUCH_SWIPE_L&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); continue; }
      if(touch==TOUCH_SWIPE_R&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ finish(); return; }
        if(tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight){
          sel=updateRow;
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
          continue;
        }
        for(int row=0;row<visible&&top+row<listCount;row++){
          int y=LIST_Y0+row*ROW_H;
          if(ty>=y&&ty<y+ROW_H){
            sel=top+row;
            SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+selectionCount-1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); }
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==updateRow){ runUpdateScreen(); beginScreenFx(); }
        else {
          const Opt &option=S_launcher[sel];
          if(option.type==OT_CHOICE&&option.nch>2){ optChoosePopup(option); beginScreenFx(); }
          else optAdjust(option,1);
          applyChange();
        }
      } else if(event.cbutton.button==BTN_X){
        if(sel<optionCount)
          showOptionHelp("Launcher",S_launcher[sel],"Launcher setting");
        else
          showHelpCard("Launcher","Check for updates","Verified launcher update",
                       "Checks the latest published ARMSX2-nx GitHub release, verifies its SHA-256 digest and NRO structure, then safely replaces this launcher with rollback recovery.",
                       nullptr,"Launcher action");
        beginScreenFx();
      } else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&sel<optionCount){
        if(resetOption(S_launcher[sel])){
          applyChange();
          toast(tr("Setting reset to default"));
          beginScreenFx();
        }
      } else if(event.cbutton.button==BTN_CANCEL){ finish(); return; }
      if(sel<listCount){
        if(sel<top) top=sel;
        if(sel>=top+visible) top=sel-visible+1;
      }
    }

    clearUiBackground();
    drawHeader(tr("Launcher"),nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
    const int rowFontHeight=fontHeight(g_font);
    glassPanel(colX-12,LIST_Y0-10,colW+24,std::max(1,std::min(visible,listCount-top))*ROW_H+18);
    if(sel<listCount){
      float target=(float)(LIST_Y0+(sel-top)*ROW_H+1);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      drawRowHighlight(colX,(int)g_hy,colW,ROW_H-2);
    }
    for(int row=0;row<visible&&top+row<listCount;row++){
      int index=top+row,slotY=LIST_Y0+row*ROW_H; bool current=index==sel;
      char value[96]; optValue(S_launcher[index],value,sizeof(value));
      SDL_Color valueColor=current?COL_VAL:COL_DIM;
      drawSettingsRowText(tr(S_launcher[index].label),value,slotY,colW,labelX,valX,current,
                          current?COL_VAL:COL_TXT,valueColor);
    }
    const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
    const int buttonX=(SW-buttonWidth)/2;
    const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
    const bool updateSelected=sel==updateRow;
    drawButtonPanel(buttonX,buttonY,buttonWidth,buttonHeight,updateSelected);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-rowFontHeight)/2,tr("Check for Updates"),updateSelected?COL_VAL:COL_TXT);
    const std::string updateStatus=launcherUpdateStatusText();
    drawTextC(g_font_sm,SW/2,buttonY+buttonHeight+8,updateStatus.c_str(),updateSelected?COL_VAL:COL_DIM);
    const bool onOption=sel>=0&&sel<optionCount;
    const char *confirm=onOption?confirmLabelForOption(S_launcher[sel]):"Select";
    FootItem footer[5];int hintCount=0;
    if(onOption&&canAdjustOption(S_launcher[sel])) footer[hintCount++]={"Left / Right","Change"};
    if(confirm) footer[hintCount++]={"A",confirm};
    footer[hintCount++]={"X","Info"};
    if(onOption&&canResetOption(S_launcher[sel])) footer[hintCount++]={"Y","Reset"};
    footer[hintCount++]={"B","Back"};
    drawFooterHints(footer,hintCount,SH-26);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static void gameSourcesScreen() {
  int sel=0,top=0;
  // Same shared metrics as every other list screen, so the rows clear the
  // header band and the measured footer.
  const int listY=settingsListY(),rowH=settingsRowH()+8;
  for(;;){
    auto sources=loadGameSources(); int n=1+(int)sources.size();
    int vis=std::max(1,(SH-listY-settingsFooterReserve())/rowH);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty<topBarH()||ty>=SH-40) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowH; if(ty>=y&&ty<y+rowH-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(sources.size()>=16){ toastStatic("Maximum of 16 game folders"); continue; }
            std::string selected=browseFolder({});
            if(!selected.empty()){
              std::string identity=pathIdentity(selected);
              if(std::any_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
                toastStatic("Folder already added");
              } else {
                sources.push_back(selected); saveGameSources(sources); g_rescanAfterSettings=true; sel=(int)sources.size();
              }
              rebuild=true;
            }
          } else {
            const char *actions[]={"Change folder","Move up","Move down","Remove"};
            int action=dropdown("Game folder",actions,4,0,true,true); size_t index=(size_t)(sel-1);
            if(action==0){
              std::string selected=browseFolder(sources[index]);
              if(!selected.empty()){
                std::string identity=pathIdentity(selected); bool duplicate=false;
                for(size_t i=0;i<sources.size();i++) if(i!=index&&pathIdentity(sources[i])==identity) duplicate=true;
                if(duplicate){ toastStatic("Folder already added"); }
                else { sources[index]=selected; saveGameSources(sources); g_rescanAfterSettings=true; }
                rebuild=true;
              }
            } else if(action==1&&index>0){ std::swap(sources[index],sources[index-1]); saveGameSources(sources); sel--; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==2&&index+1<sources.size()){ std::swap(sources[index],sources[index+1]); saveGameSources(sources); sel++; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==3&&confirmBox(uiText("Remove game folder?").c_str(),{sources[index],"",uiText("No files will be deleted.")})){
              sources.erase(sources.begin()+index); saveGameSources(sources); sel=std::max(0,sel-1); g_rescanAfterSettings=true; rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      drawLocalizedHeader("Game folders",uiText("All folders are scanned by ARMSX2-nx").c_str());
      const int shownRows=std::max(1,std::min(vis,n-top));
      glassPanel(44,listY-10,SW-88,shownRows*rowH+18);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=listY+row*rowH; bool current=index==sel;
        if(current) drawRowHighlight(56,y,SW-112,rowH-4);
        std::string label=index==0?("[ "+uiText("Add game folder")+" ]"):sources[index-1];
        drawText(g_font,82,y+(rowH-fontHeight(g_font))/2,
                 ellipsizedText(g_font,label,SW-170).c_str(),current?COL_VAL:(index==0?COL_HI:COL_TXT));
      }
      drawSettingsFooter("A  Select       B  Back");
      SDL_RenderPresent(g_ren); waitForNextFrame();
    }
  }
}

static void libraryStorageScreen() {
  static int savedSelection=0;
  constexpr int rowCount=3;
  const int rowHeight=settingsRowH(),startY=settingsListY()+40;
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  auto openRow=[&](){
    if(sel==0) gameSourcesScreen();
    else if(sel==1) runFileManager();
    else downloadAllCovers();
    beginScreenFx();
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<rowCount;row++){ int y=startY+row*rowHeight; if(ty>=y&&ty<y+rowHeight){ sel=row; openRow(); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM) openRow();
      else if(event.cbutton.button==BTN_X&&sel==2){
        showHelpCard("Library & storage","Download covers","Library artwork",
                     "Downloads missing covers and Classic backgrounds from SteamGridDB. Existing local artwork is kept.",
                     nullptr,"Launcher action");
        beginScreenFx();
      }
      else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    clearUiBackground();
    drawLocalizedHeader("Library & storage",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    drawSectionHeading("General",colX,startY-44,colW);
    glassPanel(colX-12,startY-8,colW+24,rowCount*rowHeight+16);
    float target=(float)(startY+sel*rowHeight+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    drawRowHighlight(colX,(int)g_hy,colW,rowHeight-4);
    size_t folderCount=loadGameSources().size();
    std::string folderValue=std::to_string(folderCount)+(folderCount==1?" folder":" folders");
    const char *labels[rowCount]={"Game folders","File manager","Download covers"};
    const char *values[rowCount]={folderValue.c_str(),tr("SD card"),"SteamGridDB"};
    for(int row=0;row<rowCount;row++){
      const int slot=startY+row*rowHeight; const bool current=row==sel;
      drawSettingsRowText(tr(labels[row]),values[row],slot,colW,labelX,valX,current,
                          current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
    }
    FootItem footer[3]={{"A",sel==2?"Select":"Open"},{"X","Info"},{"B","Back"}};
    if(sel==2) drawFooterHints(footer,3,SH-26);
    else { footer[1]=footer[2];drawFooterHints(footer,2,SH-26); }
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static bool retroAchievementsLoggedIn() {
  return storeGet(g_global,"Achievements/Username","")[0]&&
         storeGet(g_global,"Achievements/Token","")[0];
}

static bool retroAchievementsSignIn() {
  if(!g_griddbReady){
    modalMessageStatic("RetroAchievements",{"The network service is unavailable.","Check the Switch internet connection and try again."});
    return false;
  }

  const std::string initialUsername=storeGet(g_global,"Achievements/Username","");
  char usernameBuffer[96]{};
  snprintf(usernameBuffer,sizeof(usernameBuffer),"%s",
           initialUsername.c_str());
  if(!promptTextMode("RetroAchievements username",initialUsername.c_str(),
                     usernameBuffer,sizeof(usernameBuffer),false,false,
                     "Sign in to enable achievements.","Username"))
    return false;
  const std::string username=trim(usernameBuffer);
  if(username.empty()||username.find_first_of("\r\n")!=std::string::npos){
    modalMessageStatic("RetroAchievements",{"Enter a valid username."});
    return false;
  }

  char password[192]{};
  if(!promptTextMode("RetroAchievements password","",password,sizeof(password),
                     true,false,"","Password")){
    retroAchievementsClearSecret(password,sizeof(password));
    return false;
  }

  toastStatic("Signing in to RetroAchievements...");
  SDL_RenderPresent(g_ren);
  RetroAchievementsLoginResponse response;
  const RetroAchievementsLoginResult result=
      retroAchievementsLoginWithPassword(username,password,response);
  retroAchievementsClearSecret(password,sizeof(password));
  if(result!=RetroAchievementsLoginResult::Success){
    modalMessage(uiText("RetroAchievements sign-in failed").c_str(),{
      response.error.empty()?"The account could not be signed in.":response.error
    });
    return false;
  }
  if(response.username.find_first_of("\r\n")!=std::string::npos||
     response.token.find_first_of("\r\n")!=std::string::npos||
     response.token.size()>=256){
    modalMessageStatic("RetroAchievements",{"The server returned an invalid account token."});
    return false;
  }

  storeSet(g_global,"Achievements/Username",response.username.c_str());
  storeSet(g_global,"Achievements/Token",response.token.c_str());
  storeSet(g_global,"Achievements/Enabled","true");
  normalizeRetroAchievementsStore(g_global);
  if(!storeSave(g_global,LAUNCHER_INI)){
    modalMessageStatic("RetroAchievements",{"Signed in, but launcher.ini could not be saved."});
    return false;
  }
  syncRetroAchievementsToEmulatorConfig();
  modalMessage(uiText("RetroAchievements").c_str(),{uiText("Signed in as ")+response.username+".",
               uiText("Achievements will activate the next time a game starts.")});
  return true;
}

static void retroAchievementsScreen() {
  enum { RA_ACCOUNT,RA_ENABLED,RA_HARDCORE,RA_NOTIFICATIONS,
         RA_SIGN_IN,RA_SIGN_OUT,RA_BACK,RA_ROW_COUNT };
  static int savedSelection=0;
  int sel=std::max(0,std::min(savedSelection,RA_ROW_COUNT-1));
  const int rowH=settingsRowH(),y0=settingsListY()+40;
  auto setToggle=[&](const char *key,bool value){
    storeSet(g_global,key,value?"true":"false");
    normalizeRetroAchievementsStore(g_global);
    storeSave(g_global,LAUNCHER_INI);
    syncRetroAchievementsToEmulatorConfig();
  };
  auto activate=[&](int direction){
    const bool loggedIn=retroAchievementsLoggedIn();
    if(sel==RA_ACCOUNT||sel==RA_SIGN_IN){
      retroAchievementsSignIn();
    } else if(sel==RA_ENABLED){
      if(!loggedIn){ retroAchievementsSignIn(); return; }
      const bool current=!strcmp(storeGet(g_global,"Achievements/Enabled","false"),"true");
      setToggle("Achievements/Enabled",direction?direction>0:!current);
    } else if(sel==RA_HARDCORE){
      const bool current=!strcmp(storeGet(g_global,"Achievements/ChallengeMode","false"),"true");
      setToggle("Achievements/ChallengeMode",direction?direction>0:!current);
    } else if(sel==RA_NOTIFICATIONS){
      const bool current=!strcmp(storeGet(g_global,"Achievements/Notifications","true"),"true");
      setToggle("Achievements/Notifications",direction?direction>0:!current);
    } else if(sel==RA_SIGN_OUT&&loggedIn&&
              confirmBoxStatic("Sign out of RetroAchievements?",{
                "The saved account token will be removed.","Achievements will be disabled."})){
      storeSet(g_global,"Achievements/Enabled","false");
      storeSet(g_global,"Achievements/Username","");
      storeSet(g_global,"Achievements/Token","");
      normalizeRetroAchievementsStore(g_global);
      storeSave(g_global,LAUNCHER_INI);
      syncRetroAchievementsToEmulatorConfig();
      // The core keeps the token in its secrets file, which holds nothing else.
      remove((std::string(INIS_DIR)+"/secrets.ini").c_str());
    }
  };

  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SWIPE_L&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS){ activate(-1); continue; }
      if(touch==TOUCH_SWIPE_R&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS){ activate(1); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<RA_ROW_COUNT;row++){
          int y=y0+row*rowH;
          if(ty>=y&&ty<y+rowH){ sel=row; if(sel==RA_BACK){ savedSelection=sel; return; } activate(0); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+RA_ROW_COUNT-1)%RA_ROW_COUNT;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%RA_ROW_COUNT;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS) activate(-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS) activate(1);
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==RA_BACK){ savedSelection=sel; return; }
        activate(0);
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    const bool loggedIn=retroAchievementsLoggedIn();
    const bool enabled=loggedIn&&!strcmp(storeGet(g_global,"Achievements/Enabled","false"),"true");
    clearUiBackground();
    drawLocalizedHeader("RetroAchievements",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    drawSectionHeading("Account",colX,y0-44,colW);
    glassPanel(colX-12,y0-8,colW+24,RA_ROW_COUNT*rowH+16);
    float target=(float)(y0+sel*rowH+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    drawRowHighlight(colX,(int)g_hy,colW,rowH-4);
    const char *labels[RA_ROW_COUNT]={"Account","Achievements","Hardcore mode","Notifications",
                                      loggedIn?"Change account":"Sign in","Sign out","Back"};
    const char *values[RA_ROW_COUNT]={
      loggedIn?storeGet(g_global,"Achievements/Username",""):"Not signed in",
      enabled?"On":"Off",
      !strcmp(storeGet(g_global,"Achievements/ChallengeMode","false"),"true")?"On":"Off",
      !strcmp(storeGet(g_global,"Achievements/Notifications","true"),"true")?"On":"Off",
      ">",loggedIn?">":"Unavailable","<"
    };
    for(int row=0;row<RA_ROW_COUNT;row++){
      const int slot=y0+row*rowH;
      const bool current=row==sel;
      const bool available=row!=RA_SIGN_OUT||loggedIn;
      drawSettingsRowText(tr(labels[row]),values[row],slot,colW,labelX,valX,current,
                          current&&available?COL_VAL:(available?COL_TXT:COL_DIM),
                          current&&available?COL_VAL:COL_DIM,false,rowH);
    }
    // "Sign out" is inert until an account is signed in, and Left / Right only
    // moves the three On/Off rows.
    FootItem footer[3];int hintCount=0;
    if(sel!=RA_SIGN_OUT||loggedIn) footer[hintCount++]={"A","Select"};
    if(sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS) footer[hintCount++]={"Left / Right","Change"};
    footer[hintCount++]={"B","Back"};
    drawFooterHints(footer,hintCount,SH-26);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static void runSettingsRoot(const char *ctx) {
  const bool global=!(ctx&&*ctx);
  static const int globalOrder[] = { SCR_EMU, SCR_GRAPHICS, SCR_AUDIO, SCR_CONTROLLER, SCR_LOGGING };
  static const int gameOrder[] = { SCR_EMU, SCR_ADVANCED, SCR_GAMEFIXES, SCR_GRAPHICS, SCR_AUDIO, SCR_CONTROLLER };
  const int *order=global?globalOrder:gameOrder;
  const int nscr=global?(int)(sizeof(globalOrder)/sizeof(*globalOrder)):
                         (int)(sizeof(gameOrder)/sizeof(*gameOrder));
  const int launcherRow=0,libraryRow=1,retroAchievementsRow=2;
  // Rows above this index form the "General" group (global settings only); the
  // rest are emulator setting categories under their own section heading.
  const int screenStart=global?3:0;
  const int topGroupRows=screenStart;
  const int n=nscr+screenStart;
  int sel=0,top=0;
  const int rowH=settingsRowH(),y0=settingsListY()+40,sectionGap=topGroupRows>0?56:0;
  const int vis=std::max(1,(SH-y0-settingsFooterReserve()-sectionGap)/rowH);
  auto rowY=[&](int index){ return y0+(index-top)*rowH+(topGroupRows>0&&index>=topGroupRows?sectionGap:0); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touchScrollList(touch,sel,top,n,vis)) continue;
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40) return;
        for(int row=0;row<vis&&top+row<n;row++){ int index=top+row,y=rowY(index); if(ty>=y&&ty<y+rowH){ sel=index; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(global&&sel==launcherRow) launcherSettingsScreen();
        else if(global&&sel==libraryRow) libraryStorageScreen();
        else if(global&&sel==retroAchievementsRow) retroAchievementsScreen();
        else runSettings(order[sel-screenStart],ctx);
        beginScreenFx();
      } else if(event.cbutton.button==BTN_X){
        if(global&&sel==launcherRow)
          showHelpCard("Settings","Launcher","Launcher appearance",
                       "Changes the SDL launcher's theme, library grid, labels, animations, sounds, and update checks.",
                       nullptr,"Settings category");
        else if(global&&sel==libraryRow)
          showHelpCard("Settings","Library & storage","Game and file management",
                       "Manages the scanned game folders on the SD card, files, and cover downloads.",
                       nullptr,"Settings category");
        else if(global&&sel==retroAchievementsRow)
          showHelpCard("Settings","RetroAchievements","Online achievements",
                       "Signs in to RetroAchievements and controls achievements, hardcore mode, and unlock notifications. Account credentials are stored as a reusable token after sign-in.",
                       nullptr,"Settings category");
        else {
          const int screen=order[sel-screenStart];
          showHelpCard(global?"Settings":"Game settings",g_screens[screen].title,
                       "Settings category",settingsScreenDescription(screen),nullptr,
                       global?"Global settings":"Per-game overrides");
        }
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL) return;
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
    }

    clearUiBackground();
    drawLocalizedHeader(global?"Settings":"Game settings",
                        global?uiText("Global settings").c_str():ctx);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int shown=std::min(vis,n);
    if(topGroupRows>0){
      drawSectionHeading("General",colX,rowY(0)-44,colW);
      glassPanel(colX-12,rowY(0)-8,colW+24,topGroupRows*rowH+16);
      drawSectionHeading("Emulator",colX,rowY(topGroupRows)-44,colW);
      glassPanel(colX-12,rowY(topGroupRows)-8,colW+24,std::max(1,n-topGroupRows)*rowH+16);
    } else {
      drawSectionHeading("Emulator",colX,rowY(0)-44,colW);
      glassPanel(colX-12,rowY(0)-8,colW+24,shown*rowH+16);
    }
    float target=(float)(rowY(sel)+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    drawRowHighlight(colX,(int)g_hy,colW,rowH-4);
    for(int row=0;row<vis&&top+row<n;row++){
      const int index=top+row,slot=rowY(index); const bool current=index==sel;
      const SDL_Color labelColor=current?COL_VAL:COL_TXT,valueColor=current?COL_VAL:COL_DIM;
      if(global&&index==launcherRow){
        const char *theme=storeGet(g_global,"Wrapper/Theme","animated");
        const char *value=!strcmp(theme,"classic")?"Classic":"PS2";
        drawSettingsRowText(tr("Launcher"),value,slot,colW,labelX,valX,current,labelColor,valueColor,false,rowH);
      } else if(global&&index==libraryRow){
        drawSettingsRowText(tr("Library & storage"),tr("games / files / covers"),
                            slot,colW,labelX,valX,current,labelColor,valueColor,false,rowH);
      } else if(global&&index==retroAchievementsRow){
        const bool loggedIn=retroAchievementsLoggedIn();
        const bool enabled=loggedIn&&!strcmp(storeGet(g_global,"Achievements/Enabled","false"),"true");
        drawSettingsRowText(tr("RetroAchievements"),
                            enabled?storeGet(g_global,"Achievements/Username",""):(loggedIn?tr("Off"):tr("Not signed in")),
                            slot,colW,labelX,valX,current,labelColor,valueColor,false,rowH);
      } else {
        drawSettingsRowText(tr(g_screens[order[index-screenStart]].title),">",
                            slot,colW,labelX,valX,current,labelColor,valueColor,false,rowH);
      }
    }
    if(n>vis){ int trackH=vis*rowH,trackX=colX+colW+16; fillRect(trackX,y0,4,trackH,(SDL_Color){40,44,54,255}); int thumbH=std::max(16,trackH*vis/n),denom=std::max(1,n-vis); fillRect(trackX,y0+(trackH-thumbH)*top/denom,4,thumbH,COL_SEL); }
    drawSettingsFooter("A  Open       X  Help       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static void toast(const char *msg) {
  g_toastMessage=msg?msg:"";
  g_toastUntil=SDL_GetTicks()+1800;
  SDL_Event wake{};wake.type=SDL_USEREVENT;wake.user.code=0x544f4153; // TOAS
  SDL_PushEvent(&wake);
}

static std::string uiText(const char *text){
  return std::string(g_localization.Translate(text?text:""));
}

static void toastStatic(const char *msg){
  const std::string translated=uiText(msg);
  toast(translated.c_str());
}

static void modalMessage(const char *title, const std::vector<std::string> &lines) {
  for (;;) {
    if(!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    clearUiBackground();
    int pw=std::min(SW-96,1080),ph=std::min(SH-96,220+(int)lines.size()*64);
    int px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    drawText(g_font_big,px+28,py+22,fittedText(g_font_big,title?title:"",pw-56).c_str(),COL_TXT);
    fillRect(px+28,py+72,pw-56,1,(SDL_Color){255,255,255,24});
    SDL_Rect clip={px+40,py+84,pw-80,std::max(1,ph-142)}; SDL_RenderSetClipRect(g_ren,&clip);
    int y=py+88;
    for(const auto &line:lines){
      drawWrapped(g_font,px+48,y,pw-96,36,2,line.c_str(),COL_TXT);
      y+=line.empty()?30:72;
      if(y>py+ph-76) break;
    }
    SDL_RenderSetClipRect(g_ren,nullptr);
    FootItem footer[]={{"A","Continue"}};
    drawFooterHints(footer,1,py+ph-30);
    SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static void modalMessageStatic(const char *title,std::initializer_list<const char*> lines){
  std::vector<std::string> translated;
  translated.reserve(lines.size());
  for(const char *line:lines)translated.emplace_back(uiText(line));
  const std::string shown=uiText(title);
  modalMessage(shown.c_str(),translated);
}

static bool confirmBox(const char *title, const std::vector<std::string> &lines) {
  int pw=std::min(SW-96,1080),ph=std::min(SH-96,260+(int)lines.size()*62);
  int px=(SW-pw)/2,py=(SH-ph)/2;
  int bw=210, bh=56, bby=py+ph-bh-22, yesx=SW/2-bw-18, nox=SW/2+18;
  for(;;){
    if(!beginUiFrame()) return false;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    drawText(g_font_big,px+28,py+22,fittedText(g_font_big,title?title:"",pw-56).c_str(),(SDL_Color){238,135,135,255});
    fillRect(px+28,py+72,pw-56,1,(SDL_Color){255,255,255,24});
    SDL_Rect clip={px+40,py+84,pw-80,std::max(1,bby-py-100)}; SDL_RenderSetClipRect(g_ren,&clip);
    int y=py+88;
    for(const auto &line:lines){
      drawWrapped(g_font,px+48,y,pw-96,34,2,line.c_str(),COL_TXT);
      y+=line.empty()?28:68;
      if(y>=bby-12) break;
    }
    SDL_RenderSetClipRect(g_ren,nullptr);
    // Rounded choices carrying the real controller glyph next to the label.
    const auto drawChoice=[&](int x,const char *button,const char *label){
      const char *shown=tr(label);
      int gw=0,gh=0; buttonHintSize(button,gw,gh);
      const int labelWidth=textW(g_font,shown),left=x+(bw-gw-8-labelWidth)/2;
      if(SDL_Texture *glyph=buttonGlyph(button)){
        SDL_Rect icon={left,bby+(bh-gh)/2,gw,gh};
        SDL_RenderCopy(g_ren,glyph,nullptr,&icon);
      }
      drawText(g_font,left+gw+8,bby+(bh-fontHeight(g_font))/2,shown,COL_TXT);
    };
    roundedPanel(yesx,bby,bw,bh,(SDL_Color){115,44,51,255},(SDL_Color){238,135,135,255},6);
    drawChoice(yesx,"A","Yes");
    roundedPanel(nox,bby,bw,bh,COL_FOCUS,COL_SEL,6);
    drawChoice(nox,"B","No");
    SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static bool confirmBoxStatic(const char *title,std::initializer_list<const char*> lines){
  std::vector<std::string> translated;
  translated.reserve(lines.size());
  for(const char *line:lines)translated.emplace_back(uiText(line));
  const std::string shown=uiText(title);
  return confirmBox(shown.c_str(),translated);
}

static std::string installedReleaseTag() {
  const std::string built=LauncherUpdate_BuiltReleaseTag();
  const std::string stored=storeGet(g_global,"Wrapper/InstalledReleaseTag","");
  if(stored.empty()) return built;
  return LauncherUpdate_IsNewer(stored,built)?stored:built;
}

static std::vector<size_t> utf8Boundaries(const std::string &text) {
  std::vector<size_t> boundaries{0};
  for(size_t index=0;index<text.size();){
    const unsigned char lead=(unsigned char)text[index];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(index+length>text.size()) length=1;
    for(size_t part=1;part<length;part++) if(((unsigned char)text[index+part]&0xc0)!=0x80){ length=1; break; }
    index+=length;
    boundaries.push_back(index);
  }
  return boundaries;
}

static std::vector<std::string> wrapReleaseNotes(const std::string &notes,int maxWidth) {
  std::vector<std::string> lines;
  size_t paragraphStart=0;
  while(paragraphStart<=notes.size()){
    size_t paragraphEnd=notes.find('\n',paragraphStart);
    if(paragraphEnd==std::string::npos) paragraphEnd=notes.size();
    std::string paragraph=notes.substr(paragraphStart,paragraphEnd-paragraphStart);
    if(!paragraph.empty()&&paragraph.back()=='\r') paragraph.pop_back();
    for(char &value:paragraph) if(value=='\t'||(unsigned char)value<0x20) value=' ';
    while(!paragraph.empty()&&paragraph.back()==' ') paragraph.pop_back();
    if(paragraph.empty()) lines.emplace_back();
    else {
      bool continuation=false;
      while(!paragraph.empty()){
        while(!paragraph.empty()&&paragraph.front()==' ') paragraph.erase(paragraph.begin());
        if(paragraph.empty()) break;
        const std::string prefix=continuation&&paragraph.rfind("- ",0)!=0?"  ":"";
        if(textW(g_font_sm,(prefix+paragraph).c_str())<=maxWidth){ lines.push_back(prefix+paragraph); break; }
        const auto boundaries=utf8Boundaries(paragraph);
        size_t low=1,high=boundaries.size()-1;
        while(low<high){
          size_t middle=(low+high+1)/2;
          if(textW(g_font_sm,(prefix+paragraph.substr(0,boundaries[middle])).c_str())<=maxWidth) low=middle;
          else high=middle-1;
        }
        size_t split=boundaries[low];
        size_t space=paragraph.rfind(' ',split);
        if(space!=std::string::npos&&space>0&&space>=split/3) split=space;
        lines.push_back(prefix+paragraph.substr(0,split));
        paragraph.erase(0,split);
        continuation=true;
      }
    }
    if(paragraphEnd==notes.size()) break;
    paragraphStart=paragraphEnd+1;
  }
  if(lines.empty()) lines.emplace_back("No release notes were provided.");
  return lines;
}

static void requestLauncherExitAfterUpdate() {
  g_exitRequested=true;
}

static void runUpdateScreen() {
  if(!g_griddbReady){
    modalMessageStatic("Update check unavailable",{
      "The launcher could not initialize its network connection.",
      "Check the connection and try again."
    });
    return;
  }
  LauncherUpdateSnapshot initial=LauncherUpdate_GetSnapshot();
  if(initial.state==LauncherUpdateState::Idle)
    LauncherUpdate_StartCheck(installedReleaseTag());

  int scroll=0;
  bool cancelRequested=false,installedSaved=false;
  std::string wrappedTag,wrappedBody;
  std::vector<std::string> wrappedLines;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){
      LauncherUpdate_Cancel();
      return;
    }
    LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
    if(snapshot.state==LauncherUpdateState::ReadyToInstall){
      if(g_romfsReady){ romfsExit(); g_romfsReady=false; }
      LauncherUpdate_InstallDownloaded(g_launcherNroPath);
      snapshot=LauncherUpdate_GetSnapshot();
      if(snapshot.state==LauncherUpdateState::Error&&!g_romfsReady&&R_SUCCEEDED(romfsInit()))
        g_romfsReady=true;
    }
    if(snapshot.state==LauncherUpdateState::Installed&&!installedSaved){
      storeSet(g_global,"Wrapper/InstalledReleaseTag",snapshot.release.tag.c_str());
      storeSave(g_global,LAUNCHER_INI);
      g_updateNoticeTag.clear();
      installedSaved=true;
    }

    const int panelWidth=SW*7/8,panelHeight=SH*4/5;
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    const int bodyX=panelX+42,bodyY=panelY+126,bodyWidth=panelWidth-84;
    const int footerHeight=108,bodyBottom=panelY+panelHeight-footerHeight;
    const int lineHeight=fontHeight(g_font_sm)+8;
    const int visibleLines=std::max(1,(bodyBottom-bodyY)/lineHeight);
    if(snapshot.release.tag!=wrappedTag||snapshot.release.notes!=wrappedBody){
      wrappedTag=snapshot.release.tag;
      wrappedBody=snapshot.release.notes;
      wrappedLines=wrapReleaseNotes(wrappedBody.empty()?"Release notes will appear here.":wrappedBody,bodyWidth-20);
      scroll=0;
    }
    const int maxScroll=std::max(0,(int)wrappedLines.size()-visibleLines);
    scroll=std::max(0,std::min(scroll,maxScroll));

    SDL_Event event;
    navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SCROLL_UP) scroll=std::min(maxScroll,scroll+std::max(1,g_touchScrollSteps));
      else if(touch==TOUCH_SCROLL_DOWN) scroll=std::max(0,scroll-std::max(1,g_touchScrollSteps));
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) scroll=std::max(0,scroll-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) scroll=std::min(maxScroll,scroll+1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER) scroll=std::max(0,scroll-visibleLines);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) scroll=std::min(maxScroll,scroll+visibleLines);
      else if(event.cbutton.button==BTN_CANCEL){
        if(snapshot.state==LauncherUpdateState::Downloading){ LauncherUpdate_Cancel(); cancelRequested=true; }
        else if(snapshot.state!=LauncherUpdateState::Installed) return;
      } else if(event.cbutton.button==BTN_CONFIRM){
        if(snapshot.state==LauncherUpdateState::UpdateAvailable){
          if(LauncherUpdate_StartDownload(g_launcherNroPath)) cancelRequested=false;
        } else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled){
          cancelRequested=false;
          LauncherUpdate_StartCheck(installedReleaseTag());
        } else if(snapshot.state==LauncherUpdateState::Installed){
          requestLauncherExitAfterUpdate();
          return;
        }
      }
    }

    snapshot=LauncherUpdate_GetSnapshot();
    clearUiBackground();
    fillRect(0,0,SW,SH,(SDL_Color){0,0,0,105});
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    drawText(g_font_big,bodyX,panelY+22,"ARMSX2-nx Update",COL_TXT);
    fillRect(bodyX,panelY+72,bodyWidth,1,(SDL_Color){255,255,255,24});

    std::string status;
    switch(snapshot.state){
      case LauncherUpdateState::Idle: status="Ready to check for updates"; break;
      case LauncherUpdateState::Checking: status="Checking GitHub for the latest release..."; break;
      case LauncherUpdateState::UpdateAvailable:
        status="Version "+snapshot.release.tag+" is available    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::UpToDate:
        status="You are up to date    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::Downloading:
        status=cancelRequested?"Cancelling download...":"Downloading "+snapshot.release.assetName; break;
      case LauncherUpdateState::ReadyToInstall: status="Preparing installation..."; break;
      case LauncherUpdateState::Installing: status="Installing update..."; break;
      case LauncherUpdateState::Installed: status="Update installed successfully - relaunch ARMSX2-nx manually"; break;
      case LauncherUpdateState::Cancelled: status="Update cancelled"; break;
      case LauncherUpdateState::Error: status=snapshot.error.empty()?"Update failed":snapshot.error; break;
    }
    drawScrollTextL(g_font_sm,bodyX,panelY+92,bodyWidth,status.c_str(),
      snapshot.state==LauncherUpdateState::Error?(SDL_Color){235,125,115,255}:COL_VAL);

    SDL_Rect clip={bodyX,bodyY,bodyWidth,bodyBottom-bodyY};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int row=0;row<visibleLines&&scroll+row<(int)wrappedLines.size();row++)
      drawText(g_font_sm,bodyX,bodyY+row*lineHeight,wrappedLines[scroll+row].c_str(),COL_TXT);
    SDL_RenderSetClipRect(g_ren,nullptr);
    if((int)wrappedLines.size()>visibleLines){
      const int trackX=panelX+panelWidth-25,trackHeight=bodyBottom-bodyY;
      fillRect(trackX,bodyY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=std::max(18,trackHeight*visibleLines/(int)wrappedLines.size());
      fillRect(trackX,bodyY+(trackHeight-thumbHeight)*scroll/std::max(1,maxScroll),4,thumbHeight,COL_SEL);
    }

    if(snapshot.state==LauncherUpdateState::Downloading){
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const int percent=total?(int)std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      const int barX=bodyX,barY=panelY+panelHeight-82,barWidth=bodyWidth,barHeight=24;
      drawProgressBar(barX,barY,barWidth,barHeight,total?(double)snapshot.downloaded/total:0.0);
      char progress[96];
      snprintf(progress,sizeof(progress),"%d%%    %.1f / %.1f MiB",percent,
        snapshot.downloaded/(1024.0*1024.0),total/(1024.0*1024.0));
      drawTextC(g_font_sm,SW/2,barY+30,progress,COL_DIM);
    } else {
      // A only means something in three of the states, B no longer leaves once
      // the update is installed, and the scroll hints need notes to scroll.
      const char *confirm=
        snapshot.state==LauncherUpdateState::UpdateAvailable?"Download":
        snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled?"Retry":
        snapshot.state==LauncherUpdateState::Installed?"Exit ARMSX2-nx":nullptr;
      FootItem footer[4];int hintCount=0;
      if(confirm) footer[hintCount++]={"A",confirm};
      if(snapshot.state!=LauncherUpdateState::Installed) footer[hintCount++]={"B","Back"};
      if(maxScroll>0){
        footer[hintCount++]={"Up / Down","Scroll"};
        footer[hintCount++]={"L / R","Page"};
      }
      drawFooterHints(footer,hintCount,panelY+panelHeight-38);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextFrame();
  }
}

static void pollUpdateNotification() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  if(snapshot.state==LauncherUpdateState::UpdateAvailable&&!snapshot.release.tag.empty()&&
     snapshot.release.tag!=g_updateNotifiedTag){
    g_updateNotifiedTag=snapshot.release.tag;
    g_updateNoticeTag=snapshot.release.tag;
    g_updateNoticeUntil=SDL_GetTicks()+9000;
  }
}

static void drawUpdateNotification() {
  if(g_updateNoticeTag.empty()||SDL_TICKS_PASSED(SDL_GetTicks(),g_updateNoticeUntil)){
    g_updateNoticeTag.clear();
    return;
  }
  const int width=std::min(540,SW-40),height=92,x=SW-width-24,y=SH-height-58;
  glassPanel(x,y,width,height);
  const std::string title="ARMSX2-nx "+g_updateNoticeTag+" is available";
  drawText(g_font,x+22,y+16,ellipsizedText(g_font,title,width-44).c_str(),COL_VAL);
  drawText(g_font_sm,x+22,y+54,"Open Settings > Launcher > Check for Updates",COL_TXT);
}

static bool biosPresent() {
  DIR *d = opendir(BIOS_DIR);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    // A PS2 BIOS dump is 4 MiB or more (not a readme, or a PS1 BIOS).
    struct stat st;
    if (stat((std::string(BIOS_DIR) + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      found = true; break;
    }
  }
  closedir(d);
  return found;
}

static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color) {
  if(!text||!*text) return;
  std::string input=text,line; int drawn=0;
  auto emit=[&](const std::string &value){ if(drawn<maxLines){ drawText(font,x,y+drawn*lineHeight,value.c_str(),color); drawn++; } };
  size_t index=0;
  while(index<input.size()&&drawn<maxLines){
    size_t end=index; while(end<input.size()&&input[end]!=' '&&input[end]!='\n') end++;
    std::string word=input.substr(index,end-index);
    std::string candidate=line.empty()?word:line+" "+word;
    if(textW(font,candidate.c_str())>maxWidth&&!line.empty()){ emit(line); line=word; }
    else line=std::move(candidate);
    if(end<input.size()&&input[end]=='\n'){ emit(line); line.clear(); }
    index=end+1;
  }
  if(!line.empty()&&drawn<maxLines) emit(line);
}

// The requested size is in 720p logical units, but the image is presented at the
// real output resolution: decode straight to output pixels so docked mode is not
// an upscale of a 720p decode.
static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height,bool preserveAspect) {
  if(width<1||height<1) return nullptr;
  const float scale=g_uiScale>0.0f?g_uiScale:1.0f;
  int pixelWidth=std::max(1,(int)std::lround(width*scale));
  int pixelHeight=std::max(1,(int)std::lround(height*scale));
  SDL_Surface *source=IMG_Load(path.c_str());
  if(!source) return nullptr;
  if(preserveAspect){
    const SDL_Rect fitted=fitArtworkRect(source->w,source->h,{0,0,pixelWidth,pixelHeight});
    pixelWidth=fitted.w;pixelHeight=fitted.h;
  }
  SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,pixelWidth,pixelHeight,32,SDL_PIXELFORMAT_RGBA32);
  if(!scaled){ SDL_FreeSurface(source); return nullptr; }
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  bool ok=SDL_BlitScaled(source,nullptr,scaled,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);
  SDL_FreeSurface(source);
  if(!ok){ SDL_FreeSurface(scaled); return nullptr; }
  SDL_Texture *texture=SDL_CreateTextureFromSurface(g_ren,scaled);
  SDL_FreeSurface(scaled);
  if(texture) SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

// One shared geometry for every per-game detail screen: a portrait cover on the
// left, a content column beside it. PS2 box art is 2:3, matching the grid cells.
struct GameDetailLayout { SDL_Rect preview,content; };
static GameDetailLayout gameDetailLayout(){
  const int top=topBarH()+24,bottom=SH-settingsFooterReserve()-12;
  const int width=260,height=width*3/2;
  return {{56,top+(bottom-top-height)/2,width,height},{360,top,SW-416,bottom-top}};
}
static void drawArtworkPreview(SDL_Texture *texture,const SDL_Rect &rect,bool selected=false){
  if(selected) roundedPanel(rect.x-10,rect.y-10,rect.w+20,rect.h+20,COL_PANEL,COL_SEL);
  else glassPanel(rect.x-10,rect.y-10,rect.w+20,rect.h+20);
  fillRect(rect.x,rect.y,rect.w,rect.h,COL_CARD);
  if(texture){
    SDL_SetTextureAlphaMod(texture,255); SDL_SetTextureColorMod(texture,255,255,255);
    int w=0,h=0;SDL_QueryTexture(texture,nullptr,nullptr,&w,&h);
    const SDL_Rect fitted=fitArtworkRect(w,h,rect);
    SDL_RenderCopy(g_ren,texture,nullptr,&fitted);
  } else {
    drawTextC(g_font_sm,rect.x+rect.w/2,rect.y+(rect.h-fontHeight(g_font_sm))/2,
              tr("NO COVER"),COL_DIM);
  }
}
static void drawGamePreview(Game &game,const SDL_Rect &rect){
  g_cover_budget=1; ensureCover(game);
  drawArtworkPreview(game.cover,rect);
}

static const char *gridDbErrorText(int result) {
  if(result==GRIDDB_NO_KEY) return "The SteamGridDB API key was rejected.";
  if(result==GRIDDB_NO_NET) return "Could not connect to SteamGridDB.";
  if(result==GRIDDB_NOT_FOUND) return "No matching artwork was found.";
  return "SteamGridDB returned an unexpected error.";
}

static GameDetailLayout artworkPickerLayout(bool background){
  if(!background) return gameDetailLayout();
  const int top=topBarH()+24,bottom=SH-settingsFooterReserve()-12;
  const int width=680,height=width*9/16;
  return {{40,top+(bottom-top-height)/2,width,height},{768,top,SW-824,bottom-top}};
}

static void drawArtworkPicker(const std::vector<GridDbArtwork> &artworks,const char *gameName,
                              bool background,int sel,int top,SDL_Texture *preview,bool previewFailed){
  const GameDetailLayout layout=artworkPickerLayout(background);
  const int rowHeight=settingsRowH()+8,listX=layout.content.x,listWidth=layout.content.w;
  const int startY=layout.content.y+44,visible=std::max(1,(layout.content.h-52)/rowHeight);
  clearUiBackground(); drawLocalizedHeader(background?"Choose background artwork":"Choose cover artwork",gameName);
  drawSectionHeading("Online artwork",listX,startY-44,listWidth);
  glassPanel(listX-8,startY-8,listWidth+16,std::min(visible,(int)artworks.size())*rowHeight+16);
  for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
    int index=top+row,itemY=startY+row*rowHeight,textY=itemY+(rowHeight-fontHeight(g_font))/2; bool current=index==sel;
    if(current) drawRowHighlight(listX,itemY,listWidth,rowHeight-3);
    std::string label=uiText("Artwork")+" "+std::to_string(index+1);
    drawText(g_font,listX+26,textY,label.c_str(),current?COL_VAL:COL_TXT);
    if(artworks[index].width>0&&artworks[index].height>0){
      std::string dimensions=std::to_string(artworks[index].width)+"x"+std::to_string(artworks[index].height);
      drawTextR(g_font_sm,listX+listWidth-20,textY+(fontHeight(g_font)-fontHeight(g_font_sm))/2,dimensions.c_str(),current?COL_VAL:COL_DIM);
    }
  }
  drawArtworkPreview(preview,layout.preview,preview!=nullptr);
  if(previewFailed){
    const SDL_Rect &rect=layout.preview;
    fillRect(rect.x,rect.y,rect.w,rect.h,COL_CARD);
    drawWrappedCentered(g_font_sm,rect.x+rect.w/2,rect.y+rect.h/2-30,rect.w-32,fontHeight(g_font_sm)+6,3,
          tr("Preview unavailable"),COL_DIM);
  }
  FootItem footer[]={{"A","Use artwork"},{"B",background?"Skip background":"Back"}};
  drawFooterHints(footer,2,SH-26);
}

static int chooseCoverArtwork(const std::vector<GridDbArtwork> &artworks,const char *gameName,bool background=false) {
  if(artworks.empty()) return -1;
  const GameDetailLayout layout=artworkPickerLayout(background);
  const int rowHeight=settingsRowH()+8,listX=layout.content.x,listWidth=layout.content.w;
  const int startY=layout.content.y+44;
  const int visible=std::max(1,(layout.content.h-52)/rowHeight);
  const int previewWidth=layout.preview.w,previewHeight=layout.preview.h;
  const std::string temporary=std::string(COVERS_DIR)+"/.sgdb-preview.img";
  int sel=0,top=0,loaded=-1;
  SDL_Texture *preview=nullptr;
  bool previewFailed=false;
  auto releasePreview=[&](){ if(preview) SDL_DestroyTexture(preview); preview=nullptr; remove(temporary.c_str()); };
  auto loadPreview=[&](int index){
    releasePreview(); loaded=index; previewFailed=false;
    const std::string &url=artworks[index].thumbnailUrl.empty()?artworks[index].url:artworks[index].thumbnailUrl;
    std::atomic_bool cancel{false};int result=GRIDDB_ERROR;
    runBusyTask("Loading artwork preview",gameName?gameName:"",
                [&]{result=griddb_download_image(url,temporary,&cancel);},&cancel);
    if(result==GRIDDB_OK) preview=loadScaledTexture(temporary,previewWidth,previewHeight,true);
    previewFailed=preview==nullptr; remove(temporary.c_str()); beginScreenFx();
  };
  mkdir(COVERS_DIR,0777);
  loadPreview(0);
  for(;;){
    if(!beginUiFrame()){ releasePreview(); return -1; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty); int oldSelection=sel;
      if(touchScrollList(touch,sel,top,(int)artworks.size(),visible)){ if(sel!=oldSelection) loadPreview(sel); continue; }
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){ releasePreview(); return -1; }
        if(tx>=listX&&tx<listX+listWidth) for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
          int itemY=startY+row*rowHeight;
          if(ty>=itemY&&ty<itemY+rowHeight){ sel=top+row; if(loaded!=sel) loadPreview(sel); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int previous=sel;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+(int)artworks.size()-1)%(int)artworks.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%(int)artworks.size();
      else if(event.cbutton.button==BTN_CONFIRM){ releasePreview(); return sel; }
      else if(event.cbutton.button==BTN_CANCEL){ releasePreview(); return -1; }
      if(sel<top) top=sel;
      if(sel>=top+visible) top=sel-visible+1;
      if(sel!=previous) loadPreview(sel);
    }
    drawArtworkPicker(artworks,gameName,background,sel,top,loaded==sel?preview:nullptr,loaded==sel&&previewFailed);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextFrame();
  }
}

static void downloadCover(Game &g) {
  std::string key=storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(key.empty()){
    char buffer[128];
    if(promptText("Enter your free SteamGridDB API key","",buffer,sizeof(buffer))){ key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI); }
    else { toastStatic("A SteamGridDB API key is required"); return; }
  }
  mkdir(COVERS_DIR,0777);
  std::string query=g.title;
  GridDbGameResult selectedGame;
  for(;;){
    std::vector<GridDbGameResult> matches;
    std::atomic_bool cancel{false}; int result=GRIDDB_NO_NET;
    runBusyTask("Searching SteamGridDB",query,[&]{ result=griddb_search_games(key,query,matches,&cancel); },&cancel);
    if(cancel.load(std::memory_order_acquire)) return;
    if(result==GRIDDB_NO_KEY){
      // As in the other ports' launchers: ask for the key again, then search again.
      char buffer[128];
      if(!promptText("SteamGridDB API key rejected",key.c_str(),buffer,sizeof(buffer))||!*buffer) return;
      key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI);
      continue;
    }
    if(result!=GRIDDB_OK&&result!=GRIDDB_NOT_FOUND){ modalMessage(uiText("Cover search failed").c_str(),{gridDbErrorText(result)}); return; }
    std::vector<std::string> labels={"Custom search..."};
    for(const auto &match:matches) labels.push_back(match.name);
    std::vector<const char*> names; names.reserve(labels.size());
    for(const auto &label:labels) names.push_back(label.c_str());
    int gameIndex=dropdown("Choose matching title",names.data(),(int)names.size(),-1,true,false);
    if(gameIndex<0) return;
    if(gameIndex==0){
      char custom[256];
      if(!promptText("Custom SteamGridDB search",query.c_str(),custom,sizeof(custom))) continue;
      std::string nextQuery=trim(custom); if(!nextQuery.empty()) query=std::move(nextQuery);
      continue;
    }
    selectedGame=matches[gameIndex-1]; break;
  }
  std::vector<GridDbArtwork> artworks;
  std::atomic_bool artworkCancel{false}; int result=GRIDDB_NO_NET;
  runBusyTask("Loading available artwork",selectedGame.name,
              [&]{ result=griddb_fetch_artworks(key,selectedGame.id,artworks,&artworkCancel); },&artworkCancel);
  if(artworkCancel.load(std::memory_order_acquire)) return;
  if(result!=GRIDDB_OK){ modalMessage(uiText("Artwork search failed").c_str(),{gridDbErrorText(result)}); return; }
  int artworkIndex=chooseCoverArtwork(artworks,selectedGame.name.c_str());
  if(artworkIndex<0) return;
  std::atomic_bool downloadCancel{false};
  runBusyTask("Downloading selected cover",selectedGame.name,[&]{
    result=griddb_download_image(artworks[artworkIndex].url,coverPath(g),&downloadCancel);
  },&downloadCancel);
  if(result!=GRIDDB_OK){
    if(!downloadCancel.load(std::memory_order_acquire)) toastStatic("Cover download failed");
    return;
  }
  reloadCover(g);
  storeSet(g_global,artworkKey(g,"GameId").c_str(),std::to_string(selectedGame.id).c_str());
  storeSet(g_global,artworkKey(g,"BackgroundMissing").c_str(),"false");
  storeSave(g_global,LAUNCHER_INI);
  if(downloadCancel.load(std::memory_order_acquire)) return;
  std::vector<GridDbArtwork> backgrounds;
  std::atomic_bool backgroundCancel{false};
  runBusyTask("Loading background artwork",selectedGame.name,[&]{
    result=griddb_fetch_backgrounds(key,selectedGame.id,backgrounds,&backgroundCancel);
  },&backgroundCancel);
  if(backgroundCancel.load(std::memory_order_acquire)) return;
  if(result==GRIDDB_NOT_FOUND){
    storeSet(g_global,artworkKey(g,"BackgroundMissing").c_str(),"true");
    storeSave(g_global,LAUNCHER_INI);
    toastStatic("Cover downloaded. No background artwork was found.");
    return;
  }
  if(result!=GRIDDB_OK){
    modalMessage(uiText("Background search failed").c_str(),{uiText("Cover downloaded"),uiText(gridDbErrorText(result))});
    return;
  }
  const int backgroundIndex=chooseCoverArtwork(backgrounds,selectedGame.name.c_str(),true);
  if(backgroundIndex<0){ toastStatic("Cover downloaded");return; }
  runBusyTask("Downloading selected background",selectedGame.name,[&]{
    result=griddb_download_image(backgrounds[backgroundIndex].url,backgroundPath(g),&backgroundCancel);
  },&backgroundCancel);
  if(result==GRIDDB_OK){ reloadBackground(g);toastStatic("Cover and background downloaded"); }
  else if(!backgroundCancel.load(std::memory_order_acquire))
    modalMessage(uiText("Background download failed").c_str(),{uiText("Cover downloaded"),uiText(gridDbErrorText(result))});
}

static void importCoverFromFile(Game &g) {
  const std::string selected=browseCoverImage(parentFolder(g.path));
  if(selected.empty()) return;
  mkdir(COVERS_DIR,0777);
  const std::string destination=coverPath(g),temporary=destination+".tmp";
  std::atomic_bool cancel{false};bool imported=false;std::string reason,detail;
  runBusyTask("Importing local cover",fileNameOf(selected),[&]{
    const auto fail=[&](const char *message,const char *technical=nullptr){reason=message;if(technical)detail=technical;remove(temporary.c_str());};
    struct stat sourceInfo{};
    if(cancel.load())return;
    if(stat(selected.c_str(),&sourceInfo)!=0||!S_ISREG(sourceInfo.st_mode)){fail("The selected cover file is unavailable.",strerror(errno));return;}
    if(sourceInfo.st_size<1||(uint64_t)sourceInfo.st_size>32ull*1024*1024){fail("The selected cover file is too large.");return;}
    if(!recoverAtomicFile(destination)){fail("ARMSX2-nx could not prepare the cover file safely.",strerror(errno));return;}
    using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)>;
    Surface source{IMG_Load(selected.c_str()),SDL_FreeSurface};
    if(!source){fail("The selected file is not a supported image.",IMG_GetError());return;}
    if(source->w<=0||source->h<=0||source->w>8192||source->h>8192||
       (uint64_t)source->w*(uint64_t)source->h>16ull*1024*1024){fail("The selected image dimensions are too large.");return;}
    if(cancel.load())return;
    Surface converted{SDL_ConvertSurfaceFormat(source.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface};
    source.reset();
    if(!converted||IMG_SavePNG(converted.get(),temporary.c_str())!=0){fail("ARMSX2-nx could not convert the selected image to PNG.",IMG_GetError());return;}
    converted.reset();
    if(cancel.load()){remove(temporary.c_str());return;}
    Surface verify{IMG_Load(temporary.c_str()),SDL_FreeSurface};
    if(!verify||verify->w<=0||verify->h<=0){fail("ARMSX2-nx could not verify the converted cover.",IMG_GetError());return;}
    verify.reset();
    FILE *saved=fopen(temporary.c_str(),"rb+");
    if(!saved){fail("ARMSX2-nx could not save the converted cover.",strerror(errno));return;}
    const bool synced=fsync(fileno(saved))==0,closed=fclose(saved)==0;
    if(!synced||!closed){fail("ARMSX2-nx could not save the converted cover.",strerror(errno));return;}
    if(cancel.load()){remove(temporary.c_str());return;}
    if(!replaceAtomic(destination,temporary)){fail("ARMSX2-nx could not replace the current cover safely.",strerror(errno));return;}
    imported=true;
  },&cancel);
  if(imported){reloadCover(g);toastStatic("Cover imported");return;}
  if(cancel.load())return;
  modalMessage(uiText("Cover import failed").c_str(),{uiText(reason.empty()?"The selected cover could not be imported safely.":reason.c_str()),detail});
}

static void coverSettings(Game &g) {
  int selection=0;
  const GameDetailLayout layout=gameDetailLayout();
  const int gap=20,cardH=(layout.content.h-gap)/2;
  const SDL_Rect cards[2]={{layout.content.x,layout.content.y,layout.content.w,cardH},
                           {layout.content.x,layout.content.y+cardH+gap,layout.content.w,cardH}};
  const char *titles[2]={"Download from SteamGridDB","Import cover from file"};
  const char *kinds[2]={"Online artwork","Local image"};
  const char *descriptions[2]={
    "Choose a game cover, then select optional background artwork for the Classic theme.",
    "Choose a PNG, JPEG, WebP or BMP image from the SD card. It is validated and saved safely as PNG."
  };
  const auto inside=[](const SDL_Rect &r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;};
  const auto removeCustom=[&]{
    const std::string active=existingCoverPath(g);
    if(!regularFileExists(active)||!confirmBox(uiText("Remove custom cover?").c_str(),{
       uiText("The downloaded or imported cover will be deleted."),
       uiText("The game will show no cover until you download or import one.")}))return;
    int error=0;
    if(remove(active.c_str())!=0&&errno!=ENOENT)error=errno;
    if(active!=coverPath(g))remove(coverPath(g).c_str());
    if(error)modalMessage(uiText("Cover removal failed").c_str(),{strerror(error)});
    else{fsdevCommitDevice("sdmc");reloadCover(g);toastStatic("Custom cover removed");}
  };
  // Hoisted out of the frame loop: this used to stat the cover file every frame.
  bool hasCustom=regularFileExists(existingCoverPath(g));
  beginScreenFx();
  for(;;){
    if(!beginUiFrame())return;
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);bool choose=false;
      if(touch==TOUCH_TAP){if(inside(cards[0],tx,ty)){selection=0;choose=true;}else if(inside(cards[1],tx,ty)){selection=1;choose=true;}else if(ty>=SH-48)return;}
      if(event.type==SDL_CONTROLLERBUTTONDOWN){
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)selection=0;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)selection=1;
        else if(event.cbutton.button==BTN_CONFIRM)choose=true;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&hasCustom){removeCustom();hasCustom=regularFileExists(existingCoverPath(g));beginScreenFx();}
        else if(event.cbutton.button==BTN_CANCEL)return;
      }
      if(choose){if(selection==0)downloadCover(g);else importCoverFromFile(g);hasCustom=regularFileExists(existingCoverPath(g));beginScreenFx();}
    }
    clearUiBackground();drawLocalizedHeader("Cover settings",g.title.c_str());
    drawGamePreview(g,layout.preview);
    for(int index=0;index<2;index++){
      const SDL_Rect &card=cards[index];const bool current=index==selection;
      glassPanel(card.x,card.y,card.w,card.h);
      if(current)drawRowHighlight(card.x+8,card.y+8,card.w-16,card.h-16);
      const int x=card.x+28,width=card.w-56;
      drawText(g_font_sm,x,card.y+24,tr(kinds[index]),COL_HI);
      drawScrollTextL(g_font,x,card.y+62,width,tr(titles[index]),current?COL_VAL:COL_TXT);
      drawWrapped(g_font_sm,x,card.y+112,width,fontHeight(g_font_sm)+7,
                  std::max(1,(card.h-132)/(fontHeight(g_font_sm)+7)),tr(descriptions[index]),COL_DIM);
    }
    if(hasCustom){FootItem hints[]={{"A","Choose"},{"Y","Remove custom cover"},{"B","Back"}};drawFooterHints(hints,3,SH-26);}
    else{FootItem hints[]={{"A","Choose"},{"B","Back"}};drawFooterHints(hints,2,SH-26);}
    drawFadeIn();SDL_RenderPresent(g_ren);waitForNextFrame();
  }
}

static void downloadAllCovers() {
  std::string key=storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(key.empty()){
    char buffer[128];
    if(promptText("Enter your free SteamGridDB API key","",buffer,sizeof(buffer))){ key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI); }
    else { toastStatic("A SteamGridDB API key is required"); return; }
  }
  mkdir(COVERS_DIR,0777);
  struct CoverJob { std::string gameKey,title,cover,background; long id=0; };
  struct ArtworkResult { std::string gameKey; long id=0; bool cover=false,background=false,backgroundMissing=false; };
  std::vector<CoverJob> pending;
  for(const Game &game:g_games){
    const bool cover=!regularFileExists(existingCoverPath(game));
    const bool background=!regularFileExists(backgroundPath(game))&&
        strcmp(storeGet(g_global,artworkKey(game,"BackgroundMissing").c_str(),"false"),"true");
    if(cover||background)
      pending.push_back({game.key,game.title,cover?coverPath(game):std::string{},background?backgroundPath(game):std::string{},
                        strtol(storeGet(g_global,artworkKey(game,"GameId").c_str(),"0"),nullptr,10)});
  }
  if(pending.empty()){ toastStatic("All artwork already downloaded"); return; }
  std::atomic_bool cancel{false};
  std::atomic_int ok{0},fail{0};
  std::vector<ArtworkResult> downloaded;
  runBusyTask("Downloading artwork",std::to_string(pending.size())+" games queued",[&]{
    for(const auto &job:pending){
      if(cancel.load(std::memory_order_acquire)) break;
      ArtworkResult saved;saved.gameKey=job.gameKey;saved.id=job.id;
      int result=GRIDDB_OK;
      if(saved.id<=0){
        std::vector<GridDbGameResult> matches;
        result=griddb_search_games(key,job.title,matches,&cancel);
        if(result==GRIDDB_OK) saved.id=matches.front().id;
      }
      if(result!=GRIDDB_OK){
        if(result!=GRIDDB_CANCELLED) fail.fetch_add(1,std::memory_order_relaxed);
        continue;
      }
      bool failed=false;
      if(!job.cover.empty()){
        std::vector<GridDbArtwork> artworks;
        result=griddb_fetch_artworks(key,saved.id,artworks,&cancel);
        if(result==GRIDDB_OK) result=griddb_download_image(artworks.front().url,job.cover,&cancel);
        saved.cover=result==GRIDDB_OK;
        failed=result!=GRIDDB_OK&&result!=GRIDDB_CANCELLED;
      }
      if(!job.background.empty()&&!cancel.load(std::memory_order_acquire)){
        result=griddb_fetch_background(key,saved.id,job.background,&cancel);
        saved.background=result==GRIDDB_OK;saved.backgroundMissing=result==GRIDDB_NOT_FOUND;
        failed|=result!=GRIDDB_OK&&result!=GRIDDB_NOT_FOUND&&result!=GRIDDB_CANCELLED;
      }
      if(saved.cover||saved.background) ok.fetch_add(1,std::memory_order_relaxed);
      if(failed) fail.fetch_add(1,std::memory_order_relaxed);
      downloaded.emplace_back(std::move(saved));
    }
  },&cancel);
  for(const auto &saved:downloaded) if(Game *game=findGameByKey(saved.gameKey)){
    storeSet(g_global,artworkKey(*game,"GameId").c_str(),std::to_string(saved.id).c_str());
    if(saved.background||saved.backgroundMissing)
      storeSet(g_global,artworkKey(*game,"BackgroundMissing").c_str(),saved.backgroundMissing?"true":"false");
    if(saved.cover) reloadCover(*game);
    if(saved.background) reloadBackground(*game);
  }
  storeSave(g_global,LAUNCHER_INI);
  char message[128]; snprintf(message,sizeof(message),tr("Artwork: %d updated, %d failed%s"),ok.load(),fail.load(),cancel.load()?tr(" (cancelled)"):"");
  toast(message);
}

static void drawWrappedCentered(TTF_Font *font,int centerX,int y,int maxWidth,int lineHeight,
                                int maxLines,const char *text,SDL_Color color) {
  if(!text||!*text) return;
  std::string input=text,line; int drawn=0;
  auto emit=[&](const std::string &value){
    if(drawn<maxLines){ drawTextC(font,centerX,y+drawn*lineHeight,value.c_str(),color); drawn++; }
  };
  size_t index=0;
  while(index<input.size()&&drawn<maxLines){
    size_t end=index; while(end<input.size()&&input[end]!=' '&&input[end]!='\n') end++;
    std::string word=input.substr(index,end-index);
    std::string candidate=line.empty()?word:line+" "+word;
    if(textW(font,candidate.c_str())>maxWidth&&!line.empty()){ emit(line); line=word; }
    else line=std::move(candidate);
    if(end<input.size()&&input[end]=='\n'){ emit(line); line.clear(); }
    index=end+1;
  }
  if(!line.empty()&&drawn<maxLines) emit(line);
}

static const char *GAME_MENU_ITEMS[]={"Launch","Game settings","Rename game","Cover settings",
  "Favorite","Clear game settings","Delete game"};
static constexpr int GAME_MENU_COUNT=7;
// Everything from this index down is destructive and sits in its own group.
static constexpr int GAME_MENU_MANAGE_FIRST=5;
struct GameMenuLayout {
  GameDetailLayout detail;
  int start,rowHeight;
  int rowY(int index)const{ return start+index*rowHeight+(index>=GAME_MENU_MANAGE_FIRST?56:0); }
};
static GameMenuLayout gameMenuLayout(){
  const GameDetailLayout detail=gameDetailLayout();
  const int rowHeight=settingsRowH(),height=GAME_MENU_COUNT*rowHeight+56+44+8;
  return {detail,detail.content.y+44+std::max(0,(detail.content.h-height)/2),rowHeight};
}
static void drawGameMenu(Game &game,int selection){
  const GameMenuLayout layout=gameMenuLayout();
  const SDL_Rect &menu=layout.detail.content;
  clearUiBackground();
  drawPageHeader(game.title.c_str(),uiText("Game menu").c_str(),game.file.c_str());
  drawGamePreview(game,layout.detail.preview);
  drawSectionHeading("General",menu.x,layout.start-44,menu.w);
  glassPanel(menu.x-8,layout.start-8,menu.w+16,GAME_MENU_MANAGE_FIRST*layout.rowHeight+16);
  drawSectionHeading("Manage game",menu.x,layout.rowY(GAME_MENU_MANAGE_FIRST)-44,menu.w);
  glassPanel(menu.x-8,layout.rowY(GAME_MENU_MANAGE_FIRST)-8,menu.w+16,
             (GAME_MENU_COUNT-GAME_MENU_MANAGE_FIRST)*layout.rowHeight+16);
  const float target=(float)(layout.rowY(selection)+2);
  g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
  drawRowHighlight(menu.x,(int)g_hy,menu.w,layout.rowHeight-4);
  for(int index=0;index<GAME_MENU_COUNT;index++){
    const bool current=index==selection;
    const bool submenu=index==1||index==3;
    const SDL_Color color=index==GAME_MENU_COUNT-1?(SDL_Color){238,135,135,255}:(current?COL_VAL:COL_TXT);
    drawSettingsRowText(tr(GAME_MENU_ITEMS[index]),index==4?(g_favorites.count(game.key)?tr("Yes"):tr("No")):(submenu?">":""),layout.rowY(index),menu.w,
                        menu.x+24,menu.x+menu.w-24,current,color,current?COL_VAL:COL_DIM,false,
                        layout.rowHeight);
  }
  FootItem footer[]={{"A",selection==0?"Launch":"Select"},{"B","Back"}};
  drawFooterHints(footer,2,SH-26);
}

static int perGameMenu(Game &g) {
  int n=GAME_MENU_COUNT, sel=0;
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  std::string pathGp = std::string(GAMECFG_DIR) + "/" + g.pathKey + ".ini";
  std::string legacyGp = std::string(GAMECFG_DIR) + "/" + g.legacyKey + ".ini";
  if(regularFileExists(gp)) storeLoad(g_game,gp.c_str());
  else if(!g.pathKey.empty()&&regularFileExists(pathGp)) storeLoad(g_game,pathGp.c_str());
  else if(g.legacyUnique&&!g.legacyKey.empty()&&regularFileExists(legacyGp)) storeLoad(g_game,legacyGp.c_str());
  else g_game.kv.clear();
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return 0;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(ty<topBarH()||ty>=SH-40){ return 0; }
          const GameMenuLayout menuLayout=gameMenuLayout();
          const SDL_Rect &menu=menuLayout.detail.content;
          if(tx>=menu.x&&tx<menu.x+menu.w)
            for(int i=0;i<n;i++){ const int y=menuLayout.rowY(i);
              if(ty>=y&&ty<y+menuLayout.rowHeight){ sel=i;
                SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(g.title.c_str());
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            bool saved=true;
            if(g_game.kv.empty()){
              if(remove(gp.c_str())!=0&&errno!=ENOENT) saved=false;
            } else saved=storeSave(g_game,gp.c_str());
            if(saved){ if(pathGp!=gp) remove(pathGp.c_str()); if(g.legacyUnique&&legacyGp!=gp) remove(legacyGp.c_str()); }
            g.hasCfg=saved&&!g_game.kv.empty();
            if(!saved) modalMessageStatic("Game settings",{"Could not save the per-game settings."});
            beginScreenFx();
          }
          else if(sel==2){
            char buf[128];
            if(promptText("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              if(!g.pathKey.empty()) storeRemove(g_titles,g.pathKey.c_str());
              if(g.legacyUnique&&!g.legacyKey.empty()) storeRemove(g_titles,g.legacyKey.c_str());
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==3){ coverSettings(g); beginScreenFx(); }
          else if(sel==4){
            if(!g_favorites.erase(g.key)) g_favorites.insert(g.key);
            saveLibraryOrganization(); rebuildVisibleGames(); beginScreenFx();
          }
          else if(sel==5&&confirmBoxStatic("Clear game settings?",{"This game's settings return to the global settings."})){
            g_game.kv.clear(); remove(gp.c_str());
            if(!g.pathKey.empty()) remove(pathGp.c_str());
            if(g.legacyUnique&&!g.legacyKey.empty()) remove(legacyGp.c_str());
            g.hasCfg=false; toastStatic("Game settings cleared"); beginScreenFx();
          }
          else if(sel==6){
            if(confirmBox(uiText("Delete game?").c_str(), { g.title, "", uiText("This permanently deletes the game file from"),
                                            uiText("its storage device. This cannot be undone.") })){
              if(remove(g.path.c_str())!=0){
                modalMessage(uiText("Delete failed").c_str(),{strerror(errno)});
                beginScreenFx();
                break;
              }
              remove(coverPath(g).c_str());
              remove(backgroundPath(g).c_str());
              storeRemovePrefix(g_global,("Artwork/"+g.key+"/").c_str());
              g_favorites.erase(g.key);saveLibraryOrganization();
              remove(gp.c_str());
              if(!g.pathKey.empty()){
                remove((std::string(COVERS_DIR)+"/"+g.pathKey+".png").c_str());
                remove(pathGp.c_str());
                storeRemove(g_titles,g.pathKey.c_str()); storeRemove(g_recent,g.pathKey.c_str());
              }
              if(g.legacyUnique&&!g.legacyKey.empty()){
                remove((std::string(COVERS_DIR)+"/"+g.legacyKey+".png").c_str());
                remove(legacyGp.c_str());
                storeRemove(g_titles,g.legacyKey.c_str());
                storeRemove(g_recent,g.legacyKey.c_str());
              }
              storeRemove(g_titles,g.key.c_str()); storeSave(g_titles,TITLES_INI);
              storeRemove(g_recent,g.key.c_str()); storeSave(g_recent,RECENT_INI);
              toastStatic("Game deleted");
              return 2;
            }
          }
          break;
      }
    }
    drawGameMenu(g,sel);
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextFrame();
  }
}

// ---------------------------------------------------------------------------
// First-launch setup progress
// ---------------------------------------------------------------------------
// The renderer's core is copied out of romfs on the first launch after an
// update. The copy stays on the UI thread and repaints between chunks; nothing
// is drawn until a byte is actually written, so an up-to-date install still
// launches instantly.
static long long   g_setupTotal=0,g_setupDone=0;
static int         g_setupPct=-1;
static bool        g_setupAborted=false;
static std::string g_setupDetail;

static void drawSetupProgress(int pct,const char *msg,const char *detail) {
  clearUiBackground();
  const int barW=SW*2/3,barX=(SW-barW)/2,barH=36;
  const bool hasDetail=detail&&*detail;
  const int barY=SH/2+(hasDetail?56:40);
  glassPanel(barX-40,SH/2-208,barW+80,400);
  if(g_logo){ const int size=140; SDL_Rect dst={(SW-size)/2,SH/2-180,size,size}; SDL_RenderCopy(g_ren,g_logo,nullptr,&dst); }
  drawTextC(g_font,SW/2,SH/2-14,fittedText(g_font,msg?msg:"",barW).c_str(),COL_TXT);
  if(hasDetail)
    drawTextC(g_font_sm,SW/2,SH/2+22,fittedText(g_font_sm,detail,barW).c_str(),COL_DIM);
  drawProgressBar(barX,barY,barW,barH,pct/100.0);
  char text[16]; snprintf(text,sizeof(text),"%d%%",pct);
  drawTextC(g_font_sm,SW/2,barY+barH+14,text,COL_DIM);
  presentUiNow();
}

// Repaints only when the whole-percent figure moves.
static void setupTick(long long bytes) {
  g_setupDone+=bytes;
  if(g_setupTotal<=0||g_setupAborted) return;
  const long long done=std::min(g_setupDone,g_setupTotal);
  const int pct=(int)(done*100/g_setupTotal);
  if(pct==g_setupPct) return;
  g_setupPct=pct;
  if(!beginUiFrame()){ g_setupAborted=true; return; }
  SDL_Event event; while(pollUiEvent(event)) pumpStick(event);
  drawSetupProgress(pct,uiText("Preparing the emulator").c_str(),g_setupDetail.c_str());
}

static long long setupFileBytes(const char *path) {
  struct stat st{};
  return stat(path,&st)==0&&S_ISREG(st.st_mode)?(long long)st.st_size:0;
}

static bool extractFromRomfs(const char *src, const char *dst, bool force=false) {

  struct stat ss{},ds{};
  if(stat(src,&ss)!=0||!S_ISREG(ss.st_mode)||!recoverAtomicFile(dst)) return false;
  if (!force && stat(dst,&ds)==0 && ds.st_size==ss.st_size) return true;
  std::string tmp = std::string(dst) + ".tmp";
  FILE *in=fopen(src,"rb"), *out=fopen(tmp.c_str(),"wb");
  if(!in||!out){ if(in)fclose(in); if(out)fclose(out); return false; }
  static char buf[1<<16]; size_t n; bool ok=true;
  while((n=fread(buf,1,sizeof(buf),in))>0){
    if(fwrite(buf,1,n,out)!=n){ ok=false; break; }
    setupTick((long long)n);
    if(g_setupAborted){ ok=false; break; }
  }
  if(ferror(in)) ok=false;
  if(fflush(out)!=0||fsync(fileno(out))!=0) ok=false;
  if(fclose(in)!=0) ok=false;
  if(fclose(out)!=0) ok=false;
  if(!ok){ remove(tmp.c_str()); return false; }
  struct stat temporary{};
  if(stat(tmp.c_str(),&temporary)!=0||temporary.st_size!=ss.st_size||!replaceAtomic(dst,tmp)){
    remove(tmp.c_str());
    return false;
  }
  return stat(dst,&ds)==0 && ds.st_size==ss.st_size;
}

static std::string readFirstLine(const std::string &path) {
  char line[256]={};
  FILE *file=fopen(path.c_str(),"r");
  if(!file) return {};
  if(!fgets(line,sizeof(line),file)) line[0]=0;
  fclose(file);
  return trim(line);
}

// The cores travel in this launcher's romfs (emu/<core>.nro, with emu/<core>.sha256 from the Makefile) and run from the
// SD card: the copy in .emu is replaced only when its marker does not hold this build's SHA-256 or its size differs.
static bool ensureEmu(const char *core) {
  const std::string source=std::string("romfs:/emu/")+core+".nro";
  const std::string destination=std::string(EMU_HOST_DIR)+"/"+core+".nro";
  const std::string marker=destination+".sha256";
  const std::string expected=readFirstLine(std::string("romfs:/emu/")+core+".sha256");
  struct stat sourceStat{},destinationStat{};
  if(stat(source.c_str(),&sourceStat)!=0){ launcherLog("not in romfs: %s",source.c_str()); return false; }
  if(!expected.empty()&&readFirstLine(marker)==expected&&
     stat(destination.c_str(),&destinationStat)==0&&destinationStat.st_size==sourceStat.st_size)
    return true;
  launcherLog("extracting %s (%lld bytes, sha256 %s)",core,(long long)sourceStat.st_size,expected.c_str());
  remove(marker.c_str());
  if(!extractFromRomfs(source.c_str(),destination.c_str(),true)) return false;
  return expected.empty()||writeAtomicText(marker,expected+"\n");
}

// The core's handoff: [Launch] Path (the game) and Launcher (this NRO, chainloaded when the game ends). The core reads
// it once and deletes it.
static bool writeLaunchHandoff(const std::string &gamePath) {
  if(gamePath.find_first_of("\r\n")!=std::string::npos||g_launcherNroPath.find_first_of("\r\n")!=std::string::npos)
    return false;
  return writeAtomicText(LAUNCH_INI,"[Launch]\nPath = "+gamePath+"\nLauncher = "+g_launcherNroPath+"\n");
}

// A game's launch: the effective settings (global, then the game's own) into inis/PCSX2.ini, the renderer's core out of
// romfs, then the handoff. False, after a message, when a step failed.
static bool prepareLaunch(const std::string &key,const std::string &pathKey,const std::string &legacyKey,
                          bool legacyUnique,const std::string &gamePath,std::string &emulatorNro) {
  launcherLog("launching %s",gamePath.c_str());
  Store effective=g_global;
  if(!key.empty()){
    std::string profile=std::string(GAMECFG_DIR)+"/"+key+".ini";
    if(!regularFileExists(profile)&&!pathKey.empty()) profile=std::string(GAMECFG_DIR)+"/"+pathKey+".ini";
    if(!regularFileExists(profile)&&legacyUnique&&!legacyKey.empty()) profile=std::string(GAMECFG_DIR)+"/"+legacyKey+".ini";
    Store overrides; storeLoad(overrides,profile.c_str());
    for(const auto &entry:overrides.kv) storeSet(effective,entry.k.c_str(),entry.v.c_str());
    if(!overrides.kv.empty()) launcherLog("per-game settings: %s (%zu values)",profile.c_str(),overrides.kv.size());
  }
  applyGlobalRetroAchievementsSettings(effective);
  expandClampingModes(effective);
  const bool vulkan=!strcmp(storeGet(effective,"EmuCore/GS/Renderer","18"),"14");
  storeSet(effective,"EmuCore/GS/Renderer",vulkan?"14":"18");
  storeSet(effective,"Pad1/Type","DualShock2");
  // The cores log to logs/emulog.txt, flushed every line on the Switch 2: the record of a crash.
  storeSet(effective,"Logging/EnableFileLogging","true");
  storeSet(effective,"Logging/EnableTimestamps","true");

  const char *core=vulkan?"armsx2_vulkan":"armsx2_drako3d";
  emulatorNro=std::string(EMU_HOST_DIR)+"/"+core+".nro";
  launcherLog("renderer %s, core %s",vulkan?"Vulkan":"Drako3D",emulatorNro.c_str());
  const std::string source=std::string("romfs:/emu/")+core+".nro";
  nx2SetCpuBoost(true);
  g_setupTotal=setupFileBytes(source.c_str());
  g_setupDone=0; g_setupPct=-1; g_setupAborted=false;
  g_setupDetail=uiText(vulkan?"Vulkan core":"Drako3D core");
  const bool haveEmulator=ensureEmu(core);
  g_setupDone=std::max(g_setupDone,g_setupTotal);
  if(g_setupPct>=0) setupTick(0);   // land on 100% before the screen goes away
  g_setupTotal=0;
  nx2SetCpuBoost(false);

  const bool configSaved=haveEmulator&&writeEmulatorSettings(effective);
  const bool handoffSaved=configSaved&&writeLaunchHandoff(gamePath);
  launcherLog("core %s, PCSX2.ini %s, launch.ini %s",haveEmulator?"ready":"FAILED",
              configSaved?"written":"not written",handoffSaved?"written":"not written");
  if(!haveEmulator){
    if(regularFileExists(source))
      modalMessageStatic("Launch failed",{"Could not extract the emulator (SD full?)"});
    else
      modalMessageStatic("Launch failed",{"This launcher was built without the selected renderer's core."});
    return false;
  }
  if(!configSaved||!handoffSaved){
    modalMessageStatic("Launch failed",{"Could not write the launch configuration"});
    return false;
  }
  return true;
}

static std::string libraryTabLabel(int tab){
  static const char *labels[]={"Recent","All games","Favorites"};
  return tab>=0&&tab<libraryTabCount()?uiText(labels[tab]):std::string{};
}

enum class LibraryIcon { Recent, Games, Favorite, Settings };

static void drawLibraryIcon(LibraryIcon icon,int cx,int cy,int size,SDL_Color color){
  if(icon==LibraryIcon::Settings){
    if(g_settingsIcon){
      const SDL_Rect rect{cx-size/2,cy-size/2,size,size};
      SDL_SetTextureColorMod(g_settingsIcon,color.r,color.g,color.b);
      SDL_SetTextureAlphaMod(g_settingsIcon,color.a);
      SDL_RenderCopy(g_ren,g_settingsIcon,nullptr,&rect);
    }
  } else if(icon==LibraryIcon::Games){
    const int cell=std::max(3,size/4),gap=std::max(2,size/7);
    for(int row=0;row<2;row++) for(int column=0;column<3;column++)
      roundedRect(cx-(3*cell+2*gap)/2+column*(cell+gap),cy-(2*cell+gap)/2+row*(cell+gap),cell,cell,1,color);
  } else {
    const int points=icon==LibraryIcon::Favorite?10:48;
    const auto point=[&](int index,float radius){
      const float angle=index*6.2831853f/points-1.5707963f;
      if(icon==LibraryIcon::Favorite&&index%2) radius*=0.45f;
      return SDL_FPoint{cx+std::cos(angle)*radius,cy+std::sin(angle)*radius};
    };
    std::array<SDL_Vertex,288> vertices;
    int count=0;
    for(int i=0;i<points;i++){
      const SDL_FPoint outer=point(i,size*0.5f),next=point(i+1,size*0.5f);
      if(icon==LibraryIcon::Favorite){
        vertices[count++]={{(float)cx,(float)cy},color,{0,0}};
        vertices[count++]={outer,color,{0,0}}; vertices[count++]={next,color,{0,0}};
      } else {
        const SDL_FPoint inner=point(i,size*0.5f-2),innerNext=point(i+1,size*0.5f-2);
        vertices[count++]={outer,color,{0,0}}; vertices[count++]={next,color,{0,0}};
        vertices[count++]={inner,color,{0,0}}; vertices[count++]={inner,color,{0,0}};
        vertices[count++]={next,color,{0,0}}; vertices[count++]={innerNext,color,{0,0}};
      }
    }
    SDL_RenderGeometry(g_ren,nullptr,vertices.data(),count,nullptr,0);
    if(icon==LibraryIcon::Recent){
      fillRect(cx-1,cy-size/3,2,size/3+1,color);
      fillRect(cx,cy-1,size/4,2,color);
    }
  }
}

struct LibraryHeaderLayout {
  std::array<SDL_Rect,3> tabs;
  SDL_Rect settings;
};

static LibraryHeaderLayout libraryHeaderLayout(){
  LibraryHeaderLayout layout{};
  int x=40;
  for(int tab=0;tab<libraryTabCount();tab++){
    const std::string label=libraryTabLabel(tab);
    const int width=28+(int)std::lround((12+textW(g_font,label.c_str()))*g_libraryTabOpen[tab]);
    layout.tabs[tab]={x-12,22,width+24,48};
    x+=width+32;
  }
  layout.settings={SW-82,22,54,48};
  return layout;
}

static int libraryHeaderHit(int x,int y){
  const LibraryHeaderLayout layout=libraryHeaderLayout();
  SDL_Point point{x,y};
  if(SDL_PointInRect(&point,&layout.settings)) return libraryTabCount();
  for(size_t index=0;index<layout.tabs.size();index++)
    if(SDL_PointInRect(&point,&layout.tabs[index])) return (int)index;
  return -1;
}

struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  g.gapx=32; g.gapy=20; g.titleH=g_showGameTitles?fontHeight(g_font_sm):0;
  // The footer reserve is measured from the real hint row, so a footer that
  // wraps to two rows can no longer overlap the covers.
  const auto hints=libraryFooter();
  int topBar=112,footer=std::max(88,measureFooter(hints.data(),(int)hints.size()).height+32);
  const bool recent=g_libraryView==LibraryView::Recent;
  g.rows=recent?1:g_gridRows;
  int availH = SH - topBar - footer;
  int caption=g.titleH?g.titleH+8:0;
  int maxCoverH=(availH-(g.rows-1)*g.gapy-g.rows*caption)/g.rows;
  if(maxCoverH<72) maxCoverH=72;
  int margin = 56;
  int autoWidth=maxCoverH*2/3;
  g.cols=recent?5:g_gridColumns;
  int maxCoverW=(SW-2*margin-(g.cols-1)*g.gapx)/g.cols;
  g.cw=std::max(48,std::min(autoWidth,maxCoverW));
  g.chh=std::min(maxCoverH,g.cw*3/2);
  g.cw=g.chh*2/3;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  int gridH=g.rows*(g.chh+caption)+(g.rows-1)*g.gapy;
  g.y0=topBar+std::max(0,(availH-gridH)/2);
  return g;
}
static void drawLibraryHeader(){
  const Uint32 now=SDL_GetTicks();
  const float dt=std::min(0.05f,(now-g_libraryAnimationTick)/1000.0f);
  g_libraryAnimationTick=now;
  for(int tab=0;tab<libraryTabCount();tab++){
    const float target=tab==libraryTabIndex()?1.0f:0.0f;
    if(!g_uiAnimations) g_libraryTabOpen[tab]=target;
    else g_libraryTabOpen[tab]+=(target-g_libraryTabOpen[tab])*(1-std::exp(-18*dt));
    if(std::fabs(target-g_libraryTabOpen[tab])<0.004f) g_libraryTabOpen[tab]=target;
    else g_interactionAnimationUntil=now+100;
  }
  drawGradient({0,0,SW,140},{0,0,0,110},{0,0,0,0});
  const LibraryHeaderLayout layout=libraryHeaderLayout();
  for(int tab=0;tab<libraryTabCount();tab++){
    const SDL_Rect &rect=layout.tabs[tab];
    const bool focused=g_libraryHeaderFocused&&g_libraryHeaderSelection==tab;
    const SDL_Color color=tab==libraryTabIndex()||focused?COL_TXT:COL_DIM;
    if(focused) roundedPanel(rect.x,rect.y,rect.w,rect.h,{255,255,255,8},COL_SEL,14,2);
    drawLibraryIcon(tab==0?LibraryIcon::Recent:tab==1?LibraryIcon::Games:LibraryIcon::Favorite,rect.x+26,46,28,color);
    if(rect.w>52){
      const SDL_Rect clip{rect.x+52,rect.y,rect.w-52,rect.h};
      SDL_RenderSetClipRect(g_ren,&clip);
      drawText(g_font,clip.x,46-fontHeight(g_font)/2,libraryTabLabel(tab).c_str(),color);
      SDL_RenderSetClipRect(g_ren,nullptr);
    }
  }
  const bool focused=g_libraryHeaderFocused&&g_libraryHeaderSelection==libraryTabCount();
  if(focused) roundedPanel(layout.settings.x,layout.settings.y,layout.settings.w,layout.settings.h,{255,255,255,8},COL_SEL,14,2);
  drawLibraryIcon(LibraryIcon::Settings,layout.settings.x+27,46,28,focused?COL_TXT:COL_DIM);
}

struct RecentLayout { int x,y,focusedWidth,width,titleY; };

static RecentLayout recentLayout(){ return {40,126,240,200,440}; }

static SDL_Rect recentCoverRect(int index,const RecentLayout &layout){
  const float slot=index-g_recentPosition;
  const float focus=std::max(0.0f,1.0f-std::fabs(slot));
  const int step=layout.width+18,expanded=layout.focusedWidth+16-step;
  const int x=layout.x+(int)std::lround(slot*step+std::clamp(slot,0.0f,1.0f)*expanded);
  const int width=layout.width+(int)std::lround((layout.focusedWidth-layout.width)*focus);
  return {x,layout.y,width,width*3/2};
}

static int gridHitTest(int px,int py,int top,int selection){
  const int n=(int)g_visibleGames.size();
  if(g_libraryView==LibraryView::Recent){
    const RecentLayout recent=recentLayout();
    SDL_Point point{px,py};
    if(selection>=0&&selection<n){
      const SDL_Rect rect=recentCoverRect(selection,recent);
      if(SDL_PointInRect(&point,&rect)) return selection;
    }
    for(int index=std::max(0,(int)std::floor(g_recentPosition)-1);index<std::min(n,(int)std::ceil(g_recentPosition)+6);index++){
      const SDL_Rect rect=recentCoverRect(index,recent);
      if(SDL_PointInRect(&point,&rect)) return index;
    }
    return -1;
  }
  GLay L=gridLayout();
  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+(L.titleH?L.titleH+8:0)) return idx;
  }
  return -1;
}
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    const std::string &shortened=ellipsizedText(f,title,cellW);
    drawTextC(f,cx,y,shortened.c_str(),col);
    return;
  }
  drawScrollTextL(f,x0,y,cellW,title.c_str(),col);
}

static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?fontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  drawText(f,x0-textScrollOffset(x0,y,tw-maxW,s),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawText(f,x,y,s,c); return; }
  SDL_Rect clip={x,y-2,maxW,(f?fontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  drawText(f,x-textScrollOffset(x,y,tw-maxW,s),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawLibraryCover(Game &game,const SDL_Rect &rect,bool selected){
  const int x=rect.x,y=rect.y,w=rect.w,h=rect.h;
  ensureCover(game,true);
  fillRect(x+4,y+6,w,h,(SDL_Color){0,0,0,55});
  fillRect(x+2,y+3,w,h,(SDL_Color){0,0,0,70});
  if(game.cover){
    const Uint32 elapsed=SDL_GetTicks()-game.coverAt;
    const Uint8 opacity=!g_uiAnimations?255:elapsed<180?(Uint8)(255*elapsed/180):255;
    SDL_SetTextureAlphaMod(game.cover,opacity);
    const Uint8 tint=selected?255:225;
    SDL_SetTextureColorMod(game.cover,tint,tint,tint);
    SDL_RenderCopy(g_ren,game.cover,nullptr,&rect);
  } else {
    fillRect(x,y,w,h,COL_CARD);
    drawTextC(g_font_sm,x+w/2,y+h/2-8,fittedText(g_font_sm,tr("NO COVER"),w-12).c_str(),COL_DIM);
  }
  border(x,y,w,h,1,(SDL_Color){12,13,18,255});
  fillRect(x,y,w,1,(SDL_Color){255,255,255,26});
  if(selected) border(x-3,y-3,w+6,h+6,2,COL_SEL);
  if(g_showRegionFlags&&game.region>0&&g_flag[game.region]){
    const int fw=std::clamp(w*26/100,16,30),fh=fw*2/3;
    SDL_Rect flag{x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[game.region],nullptr,&flag);
    border(flag.x,flag.y,fw,fh,1,(SDL_Color){10,12,18,255});
  }
  if(g_showCustomSettingsBadges&&game.hasCfg){
    const int size=26,icon=18,bx=x+w-size-6,by=y+6;
    roundedPanel(bx,by,size,size,{8,12,24,232},{235,239,247,80},size/2);
    drawLibraryIcon(LibraryIcon::Settings,bx+size/2,by+size/2,icon,COL_TXT);
  }
  if(g_favorites.count(game.key)){
    roundedRect(x+w-31,y+h-31,26,26,13,(SDL_Color){8,12,24,225});
    drawLibraryIcon(LibraryIcon::Favorite,x+w-18,y+h-18,18,COL_VAL);
  }
}

static SDL_Rect libraryEmptyActionRect(){ return {SW/2-148,SH/2+82,296,48}; }

static void drawLibraryEmpty(){
  const bool scanning=g_libraryScan!=nullptr&&g_games.empty();
  const char *title=scanning?"Loading game library...":g_libraryView==LibraryView::Recent?"Nothing played yet":
      g_libraryView==LibraryView::Favorites?"No favorites yet":"No games found";
  const char *description=scanning?"The first page will appear as soon as it is ready.":
      g_libraryView==LibraryView::Recent?"Games you launch appear here, most recent first.":
      g_libraryView==LibraryView::Favorites?"Mark games as favorites from their game menu.":
      "Add game folders in Settings to populate your library.";
  drawTextC(g_font_big,SW/2,SH/2-80,tr(title),COL_TXT);
  drawWrappedCentered(g_font_sm,SW/2,SH/2-22,SW-240,30,2,tr(description),COL_DIM);
  const SDL_Rect action=libraryEmptyActionRect();
  roundedPanel(action.x,action.y,action.w,action.h,COL_FOCUS,g_libraryHeaderFocused?COL_DIM:COL_SEL,12);
  drawTextC(g_font_sm,SW/2,action.y+(action.h-fontHeight(g_font_sm))/2,
            tr(g_libraryView==LibraryView::All?"Settings":"All games"),COL_TXT);
}

static void renderGrid(int selection,int top){
  g_cover_budget=COVER_REQUEST_BUDGET;
  Game *selected=visibleGame(selection);
  g_backgroundTarget=selected?selected->key:std::string{};
  clearUiBackground();
  if(g_launcherTheme==LauncherTheme::Classic){
    for(int direction:{-1,1}) if(Game *neighbor=visibleGame(selection+direction))
      queueCoverDecode(*neighbor,false,true);
  }
  if(selected) ensureCover(*selected,true);
  const GLay layout=gridLayout();
  const int count=(int)g_visibleGames.size(),per=layout.cols*layout.rows;
  const int page=count?selection/per:0;
  drawLibraryHeader();
  if(count&&g_libraryView==LibraryView::Recent){
    const RecentLayout recent=recentLayout();
    const float target=(float)selection;
    if(g_recentPosition<0||!g_uiAnimations) g_recentPosition=target;
    else g_recentPosition+=(target-g_recentPosition)*0.24f;
    if(std::fabs(target-g_recentPosition)<0.005f) g_recentPosition=target;
    else g_interactionAnimationUntil=SDL_GetTicks()+100;
    const SDL_Rect clip{0,recent.y-8,SW,recent.focusedWidth*3/2+16};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int index=std::max(0,(int)std::floor(g_recentPosition)-1);index<std::min(count,(int)std::ceil(g_recentPosition)+6);index++)
      if(index!=selection) drawLibraryCover(*visibleGame(index),recentCoverRect(index,recent),false);
    const SDL_Rect focused=recentCoverRect(selection,recent);
    drawLibraryCover(*selected,focused,!g_libraryHeaderFocused);
    SDL_RenderSetClipRect(g_ren,nullptr);
    const int titleX=focused.x+focused.w+16;
    drawScrollTextL(g_font,titleX,recent.titleY,SW-40-titleX,selected->title.c_str(),COL_TXT);
  } else if(count){
    const int rowStride=layout.chh+(layout.titleH?layout.titleH+8:0)+layout.gapy;
    for(int row=0;row<layout.rows;row++) for(int column=0;column<layout.cols;column++){
      const int index=(top+row)*layout.cols+column;
      if(index>=count) continue;
      Game &game=*visibleGame(index);
      const SDL_Rect rect{layout.x0+column*(layout.cw+layout.gapx),layout.y0+row*rowStride,layout.cw,layout.chh};
      const bool current=index==selection&&!g_libraryHeaderFocused;
      drawLibraryCover(game,rect,current);
      if(g_showGameTitles) drawTitleCell(rect.x+rect.w/2,rect.w,rect.y+rect.h+6,game.title,current,current?COL_VAL:COL_DIM);
    }
    const int prefetchStart=(page+1)*per;
    for(int index=prefetchStart;index<std::min(count,prefetchStart+per);index++) ensureCover(*visibleGame(index));
  } else drawLibraryEmpty();
  drawUpdateNotification();
  if(g_libraryHeaderFocused){
    FootItem hints[]={{"A",g_libraryHeaderSelection==libraryTabCount()?"Settings":"Open"},{"B","Back"}};
    drawFooterHints(hints,2,SH-50);
  } else {
    if(count){
      const auto hints=libraryFooter();
      drawFooterHints(hints.data(),(int)hints.size(),SH-50);
    } else {
      FootItem hints[]={{"A",g_libraryView==LibraryView::All?"Settings":"All games"},{"B","Quit"}};
      drawFooterHints(hints,2,SH-50);
    }
  }
  drawFadeIn(); SDL_RenderPresent(g_ren);
}

static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

static bool ensureDirectory(const char *path) {
  if(mkdir(path,0777)==0) return true;
  if(errno!=EEXIST) return false;
  struct stat st{};
  return stat(path,&st)==0&&S_ISDIR(st.st_mode);
}

static void cleanupLauncher() {
  LauncherUpdate_Shutdown();
  stopGameScan();
  stopCoverDecodeWorker();
  saveLibraryIdentities();
  for(auto &game:g_games) releaseGameArtwork(game);
  clearTextCaches();
  for(int index=1;index<4;index++){ if(g_flag[index]) SDL_DestroyTexture(g_flag[index]); g_flag[index]=nullptr; }
  SDL_Texture **glyphs[]={&g_gA,&g_gB,&g_gX,&g_gY,&g_gPlus,
                          &g_gLeftRight,&g_gUpDown,&g_gTouch,&g_settingsIcon};
  for(SDL_Texture **glyph:glyphs){ if(*glyph) SDL_DestroyTexture(*glyph); *glyph=nullptr; }
  if(g_logo) SDL_DestroyTexture(g_logo);
  g_logo=nullptr;
  if(g_glowTexture) SDL_DestroyTexture(g_glowTexture);
  g_glowTexture=nullptr;
  if(g_roundTexture) SDL_DestroyTexture(g_roundTexture);
  g_roundTexture=nullptr;
  if(g_font) TTF_CloseFont(g_font);
  if(g_font_sm) TTF_CloseFont(g_font_sm);
  if(g_font_big) TTF_CloseFont(g_font_big);
  if(g_font_caption) TTF_CloseFont(g_font_caption);
  if(g_font_hint) TTF_CloseFont(g_font_hint);
  g_font=g_font_sm=g_font_big=g_font_caption=g_font_hint=nullptr;
  g_loadedFontType=PlSharedFontType_Total;
  if(g_plReady) plExit();
  g_plReady=false;
  uiAudioShutdown();
  closeController();
  if(g_ren) SDL_DestroyRenderer(g_ren);
  if(g_win) SDL_DestroyWindow(g_win);
  g_ren=nullptr; g_win=nullptr;
  if(g_imgReady) IMG_Quit();
  if(g_ttfReady) TTF_Quit();
  if(g_sdlReady) SDL_Quit();
  g_imgReady=g_ttfReady=g_sdlReady=false;
  if(g_griddbReady) griddb_global_exit();
  g_griddbReady=false;
  if(g_storageSocketReady) socketExit();
  g_storageSocketReady=false;
  if(g_romfsReady) romfsExit();
  g_romfsReady=false;
}

static int startupFailure(const char *message) {
  launcherLog("startup failed: %s",message);
  if(g_sdlReady) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"ARMSX2-nx Launcher",message,g_win);
  cleanupLauncher();
  closeLauncherLog();
  return 1;
}

int main(int argc, char **argv){
  // 1920x1080 at 120 Hz handheld, the TV's resolution docked (switch/nx2/nx2_platform.h).
  nx2DisplayInit();
  openLauncherLog();
  launcherLog("ARMSX2-nx launcher %s",ARMSX2NX_VERSION);
  if(argc>=1&&argv[0]&&argv[0][0]){
    const std::string resolvedLauncherPath=resolveLauncherPath(argv[0]);
    if(!resolvedLauncherPath.empty()) g_launcherNroPath=resolvedLauncherPath;
  }
  launcherLog("launcher NRO: %s",g_launcherNroPath.c_str());
  std::string updateRecoveryError;
  const bool updateRecoveryOk=LauncherUpdate_RecoverInstallation(g_launcherNroPath,updateRecoveryError);
  if(R_FAILED(romfsInit())){ launcherLog("romfsInit failed"); closeLauncherLog(); return 1; }
  g_romfsReady=true;
  // The program before this one in the process (the Homebrew Menu, or a Drako3D core on the way back) used the GPU:
  // Mesa needs a usable device and a new GSV root before SDL opens EGL.
  nx2MesaGpuRecover();
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO)!=0) return startupFailure("SDL initialization failed.");
  g_sdlReady=true;
  uiAudioInit();
  if(TTF_Init()!=0) return startupFailure("Font initialization failed.");
  g_ttfReady=true;
  const int imageFlags=IMG_INIT_PNG|IMG_INIT_JPG;
  if((IMG_Init(imageFlags)&imageFlags)!=imageFlags) return startupFailure("Image initialization failed.");
  g_imgReady=true;
  // (the Switch 2's handheld screen is 1080p as well)
  g_win=SDL_CreateWindow("ARMSX2-nx",0,0,1920,1080,SDL_WINDOW_FULLSCREEN);
  if(!g_win) return startupFailure("Could not create the launcher window.");
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!g_ren) return startupFailure("Could not create the launcher renderer.");
  SDL_RendererInfo rendererInfo{};
  g_presentVsync=SDL_GetRendererInfo(g_ren,&rendererInfo)==0&&
                 (rendererInfo.flags&SDL_RENDERER_PRESENTVSYNC)!=0;
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  if(SDL_GetRendererOutputSize(g_ren,&g_outputW,&g_outputH)!=0)
    return startupFailure("Could not query the display size.");
  launcherLog("SDL renderer %s, output %dx%d",rendererInfo.name?rendererInfo.name:"?",g_outputW,g_outputH);
  // Establishes the fixed 1280x720 logical space; must run before the fonts and
  // the render-to-texture glyph/flag builders read g_uiScale.
  configureLauncherScale();
  if(SDL_Surface *logo=IMG_Load("romfs:/logo.png")){ g_logo=SDL_CreateTextureFromSurface(g_ren,logo); SDL_FreeSurface(logo); }
  makeFlags();
  for(int index=0;index<SDL_NumJoysticks();index++) if(SDL_IsGameController(index)){ openController(index); break; }

  setLauncherLanguage("system");
  if(R_FAILED(plInitialize(PlServiceType_User))) return startupFailure("System font service initialization failed.");
  g_plReady=true;
  if(!reloadLauncherFonts())
    return startupFailure("Could not load the system font.");
  makeGlyphs();

  g_griddbReady=griddb_global_init();
  if(!g_griddbReady&&R_SUCCEEDED(socketInitializeDefault())) g_storageSocketReady=true;
  launcherLog("network %s",g_griddbReady?"ready":"unavailable");
  const char *directories[]={"sdmc:/switch",DATA_DIR,INIS_DIR,LOGS_DIR,EMU_HOST_DIR,COVERS_DIR,GAMECFG_DIR,DEF_GAMEDIR,BIOS_DIR};
  for(const char *directory:directories) if(!ensureDirectory(directory)) return startupFailure("Could not create the ARMSX2-nx data directories.");
  if(!updateRecoveryOk)
    modalMessage(uiText("Update recovery failed").c_str(),{updateRecoveryError,uiText("The installed launcher was left unchanged.")});

  struct stat configStat{};
  bool firstRun=stat(LAUNCHER_INI,&configStat)!=0;
  storeLoad(g_global,LAUNCHER_INI);
  const bool settingsNormalized=normalizeLauncherTheme(g_global) | normalizeRetroAchievementsStore(g_global);
  storeLoad(g_titles,TITLES_INI);
  storeLoad(g_recent,RECENT_INI);
  storeLoad(g_library,LIBRARY_INI);
  loadLibraryIdentities();
  loadLibraryOrganization();
  if(firstRun){
    g_active=&g_global;
    saveGameSources({DEF_GAMEDIR});
    storeSet(g_global,"Wrapper/SteamGridDBKey","");
    storeSet(g_global,"Wrapper/Language","system");
    storeSet(g_global,"Wrapper/UiSounds","true");
    storeSet(g_global,"Wrapper/Theme","animated");
    storeSet(g_global,"Wrapper/GridColumns","6");
    storeSet(g_global,"Wrapper/GridRows","2");
    storeSet(g_global,"Wrapper/ShowGameTitles","true");
    storeSet(g_global,"Wrapper/ShowRegionFlags","true");
    storeSet(g_global,"Wrapper/ShowCustomSettingsBadges","true");
    storeSet(g_global,"Wrapper/UiAnimations","true");
    storeSet(g_global,"Wrapper/CheckUpdatesAtBoot","true");
    storeSet(g_global,"Wrapper/InstalledReleaseTag",LauncherUpdate_BuiltReleaseTag());
    commitAll();
    if(!storeSave(g_global,LAUNCHER_INI)) return startupFailure("Could not create launcher.ini.");
    launcherLog("first run: launcher.ini created");
  } else {
    bool changed=settingsNormalized;
    if(!storeHas(g_global,"Wrapper/GamePathCount")){ saveGameSources({DEF_GAMEDIR}); changed=true; }
    int columns=atoi(storeGet(g_global,"Wrapper/GridColumns","6"));
    int rows=atoi(storeGet(g_global,"Wrapper/GridRows","2"));
    if(columns<3||columns>8){ storeSet(g_global,"Wrapper/GridColumns","6"); changed=true; }
    if(rows<1||rows>3){ storeSet(g_global,"Wrapper/GridRows","2"); changed=true; }
    if(changed&&!storeSave(g_global,LAUNCHER_INI)) return startupFailure("Could not update launcher.ini.");
  }
  setLauncherLanguage(storeGet(g_global,"Wrapper/Language","system"));
  applyLauncherAppearance();
  uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  startCoverDecodeWorker();
  std::vector<std::string> gamePaths=loadGameSources();
  for(const auto &path:gamePaths) launcherLog("game folder: %s",path.c_str());
  LauncherUpdate_SetWakeCallback(updaterWake,nullptr);
  scanGames(gamePaths);

  if(!biosPresent()) modalMessage(uiText("No PS2 BIOS found").c_str(),{uiText("Copy a PS2 BIOS dump into:"),toEmu(BIOS_DIR),"",uiText("Games will not boot until you add one.")});
  if(FILE *errorFile=fopen(LAUNCH_ERROR,"r")){
    std::vector<std::string> lines;
    char line[1024];
    while(lines.size()<12&&fgets(line,sizeof(line),errorFile)){
      std::string text=line;
      while(!text.empty()&&(text.back()=='\n'||text.back()=='\r')) text.pop_back();
      lines.push_back(text);
    }
    fclose(errorFile);
    remove(LAUNCH_ERROR);
    launcherLog("the last game did not start: %s",lines.empty()?"?":lines.front().c_str());
    lines.push_back("");
    lines.push_back(uiText("The emulator's log: logs/emulog.txt"));
    modalMessage(uiText("The game could not start").c_str(),lines);
  }

  int sel=0,top=0,rows=1;
  // Until the user picks something, an incoming scan batch is free to re-sort
  // under the cursor; once they have, the selection follows its stable key.
  bool keepLibrarySelection=false;
  bool running=true,launch=false,userExit=false;
  std::string launchKey,launchPathKey,launchLegacyKey,launchPath;
  bool launchLegacyUnique=false;
  auto selectGame=[&](Game &game){
    recordPlayed(game);
    launchKey=game.key;
    launchPathKey=game.pathKey;
    launchLegacyKey=game.legacyKey;
    launchLegacyUnique=game.legacyUnique;
    launchPath=game.path;
    launch=true;
    running=false;
  };

  if(g_griddbReady&&strcmp(storeGet(g_global,"Wrapper/CheckUpdatesAtBoot","true"),"false")!=0)
    LauncherUpdate_StartCheck(installedReleaseTag());
  auto changeLibraryTab=[&](int tab){
    selectLibraryTab(tab,sel);
    keepLibrarySelection=true;
    top=0;
  };
  auto openLibrarySettings=[&](){
    const std::vector<std::string> oldPaths=gamePaths;
    const std::string selected=visibleGame(sel)?visibleGame(sel)->key:std::string{};
    g_active=&g_global;
    runSettingsRoot(nullptr);
    storeSave(g_global,LAUNCHER_INI);
    gamePaths=loadGameSources();
    if(gamePaths!=oldPaths||g_rescanAfterSettings){
      scanGames(gamePaths);
      sel=top=0;
      keepLibrarySelection=false;
      g_rescanAfterSettings=false;
    } else sel=visibleIndexForKey(selected);
    g_recentPosition=-1;
    beginScreenFx();
  };
  auto activateLibraryEmpty=[&](){
    if(g_libraryView==LibraryView::All) openLibrarySettings();
    else changeLibraryTab(1);
    g_libraryHeaderFocused=false;
    g_recentPosition=-1;
    beginScreenFx();
  };
  auto activateLibraryHeader=[&](int item){
    if(item<libraryTabCount()){
      changeLibraryTab(item);
      g_libraryHeaderFocused=false;
    } else openLibrarySettings();
  };
  auto pageLibrary=[&](int direction){
    const GLay layout=gridLayout();
    sel=gridPage(sel,direction,layout.cols,layout.rows,(int)g_visibleGames.size());
    keepLibrarySelection=true;
    g_libraryHeaderFocused=false;
  };
libraryLoop:
  while(running&&beginUiFrame()){
    if(g_libraryScan){
      std::string selectedKey;
      if(keepLibrarySelection) if(Game *selected=visibleGame(sel)) selectedKey=selected->key;
      pumpGameScan();
      sel=visibleIndexForKey(selectedKey);
    }
    SDL_Event event; navRepeat();
    while(running&&pollUiEvent(event)){
      pumpStick(event);
      const GLay layout=gridLayout();
      const int cols=layout.cols;
      rows=layout.rows;
      const int count=(int)g_visibleGames.size();
      sel=count?std::clamp(sel,0,count-1):0;
      top=count?(sel/(cols*rows))*rows:0;
      if(event.type==SDL_CONTROLLERAXISMOTION&&
         (event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT||event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT)){
        const size_t trigger=event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT?0:1;
        const bool pressed=event.caxis.value>18000;
        if(pressed&&!g_libraryTriggers[trigger]) pageLibrary(trigger?1:-1);
        g_libraryTriggers[trigger]=pressed;
        continue;
      }
      int tx=0,ty=0;
      const TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SWIPE_L||touch==TOUCH_SWIPE_R){
        const int direction=touch==TOUCH_SWIPE_L?1:-1;
        if(g_touch.y0<88) changeLibraryTab((libraryTabIndex()+direction+libraryTabCount())%libraryTabCount());
        else pageLibrary(direction);
        continue;
      }
      if(touch==TOUCH_SCROLL_UP||touch==TOUCH_SCROLL_DOWN){
        const int step=(touch==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
        if(count) sel=std::clamp(sel+step*(g_libraryView==LibraryView::Recent?1:cols),0,count-1);
        g_libraryHeaderFocused=false; keepLibrarySelection=true;
        continue;
      }
      if(touch==TOUCH_TAP){
        keepLibrarySelection=true;
        const int header=libraryHeaderHit(tx,ty);
        if(header>=0){ activateLibraryHeader(header); continue; }
        {
          SDL_Point point{tx,ty};
          const SDL_Rect emptyAction=libraryEmptyActionRect();
          if(!count&&SDL_PointInRect(&point,&emptyAction)){ activateLibraryEmpty(); continue; }
          const int hit=gridHitTest(tx,ty,top,sel);
          if(hit>=0){
            if(hit==sel&&!g_libraryHeaderFocused) selectGame(*visibleGame(sel));
            else sel=hit;
            g_libraryHeaderFocused=false;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      keepLibrarySelection=true;
      switch(event.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
          const int direction=event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT?-1:1;
          if(g_libraryHeaderFocused)
            g_libraryHeaderSelection=(g_libraryHeaderSelection+direction+libraryTabCount()+1)%(libraryTabCount()+1);
          else sel=gridNav(sel,direction,0,cols,rows,count);
          break;
        }
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
          if(!g_libraryHeaderFocused&&(!count||sel%(cols*rows)<cols)){
            g_libraryHeaderFocused=true; g_libraryHeaderSelection=libraryTabIndex();
          } else if(!g_libraryHeaderFocused) sel=gridNav(sel,0,-1,cols,rows,count);
          break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
          if(g_libraryHeaderFocused) g_libraryHeaderFocused=false;
          else sel=gridNav(sel,0,1,cols,rows,count);
          break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
          changeLibraryTab((libraryTabIndex()+libraryTabCount()-1)%libraryTabCount());
          break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          changeLibraryTab((libraryTabIndex()+1)%libraryTabCount());
          break;
        case BTN_CONFIRM:
          if(g_libraryHeaderFocused) activateLibraryHeader(g_libraryHeaderSelection);
          else if(count) selectGame(*visibleGame(sel));
          else activateLibraryEmpty();
          break;
        case BTN_Y:
          if(!g_libraryHeaderFocused) if(Game *game=visibleGame(sel)){
            const std::string selectedKey=game->key;
            const int result=perGameMenu(*game);
            if(result==1) selectGame(*game);
            else if(result==2){ scanGames(gamePaths); sel=top=0; keepLibrarySelection=false; }
            else { sortLibraryGames(); sel=visibleIndexForKey(selectedKey); }
            g_recentPosition=-1;
            beginScreenFx();
          }
          break;
        case BTN_CANCEL:
          if(g_libraryHeaderFocused) g_libraryHeaderFocused=false;
          else if(confirmBox(tr("Exit ARMSX2-nx?"),
                              {tr("Active scans and network operations will be cancelled safely."),tr("Return to the Homebrew Menu?")})){
            userExit=true; running=false;
          }
          break;
      }
    }
    const GLay layout=gridLayout();
    const int count=(int)g_visibleGames.size();
    sel=count?std::clamp(sel,0,count-1):0;
    top=count?(sel/(layout.cols*layout.rows))*layout.rows:0;
    pollUpdateNotification();
    renderGrid(sel,top);
    waitForNextFrame(g_libraryScan!=nullptr);
  }
  if(userExit&&g_ren){
    clearUiBackground();
    int pw=std::min(SW-96,960),ph=260,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    drawTextC(g_font_big,SW/2,py+58,tr("Closing ARMSX2-nx..."),COL_VAL);
    drawWrapped(g_font_sm,px+60,py+142,pw-120,34,2,
                tr("Finishing background operations safely."),COL_TXT);
    SDL_RenderPresent(g_ren);
  }
  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global,LAUNCHER_INI);
  storeSave(g_recent,RECENT_INI);

  bool willChain=false;
  std::string emulatorNro;
  if(launch&&envHasNextLoad())
    willChain=prepareLaunch(launchKey,launchPathKey,launchLegacyKey,launchLegacyUnique,launchPath,emulatorNro);
  else if(launch)
    modalMessageStatic("Launch failed",{"This loader cannot start another program: open ARMSX2-nx from the Homebrew Menu."});
  if(launch&&!willChain){
    // Back to the library: the message above said why the game could not start.
    nx2SetCpuBoost(false);
    launch=false;
    running=true;
    goto libraryLoop;
  }

  cleanupLauncher();
  if(willChain){
    launcherLog("chainloading %s",emulatorNro.c_str());
    envSetNextLoad(emulatorNro.c_str(),emulatorNro.c_str());
  } else launcherLog(userExit?"exit":"no launch");
  closeLauncherLog();
  return 0;
}

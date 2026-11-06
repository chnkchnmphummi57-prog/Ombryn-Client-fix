// Ombryn v5 - personal, singleplayer-only 64-bit DLL client.
//
// WORKS WITHOUT GAME HOOKS (needs Bedrock on DX11): menu, FPS/CPS/clock/keystrokes,
//   motion blur, chroma UI, toggle sprint/sneak, borderless, always-on-top, profiles.
// NEEDS HOOKS (find them in Ghidra/IDA): everything marked "needs hook" in the menu.
//   The logic and drawing for those modules is written; your hooks call the small
//   API in the "HOOK API" section below, and a module switches to "active" by itself
//   the first time its API function is called.

#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <cstdarg>
#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "version.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// =====================================================================
// Modules
// =====================================================================
enum ModId {
    // Performance
    M_ENTITY_CULL, M_UNDERGROUND_CULL, M_PARTICLE_CULL, M_FPS_UNLOCK,
    // Combat
    M_HITBOX, M_JAVA_ANIM, M_NO_HURTCAM, M_SHORT_SWORDS, M_HIT_COLOR, M_PARTICLE_MULT,
    M_HITBOX_CHROMA, M_COMBO, M_REACH, M_TARGET_HUD,
    // HUD
    M_CPS, M_KEYSTROKES, M_FPS, M_CLOCK, M_TOTEM, M_ARROW, M_BLOCKS, M_ARMOR_HUD,
    M_SPEEDO, M_COMPASS, M_SERVER_STATS,
    // Visuals
    M_FULLBRIGHT, M_ZOOM, M_COSMETICS, M_GLINT, M_HURT_SOUND, M_MOTION_BLUR,
    M_TIME_CHANGER, M_CHROMA_UI, M_CHAT_TWEAKS, M_DYN_FOV,
    // Utilities
    M_OFFHAND, M_TOGGLE_SPRINT, M_TOGGLE_SNEAK, M_SCROLL_KEYS, M_AUTO_GG,
    M_COUNT
};

struct Module {
    const char* name = "";
    const char* cat = "";
    bool needsHook = false;   // true = needs a game hook to do anything
    bool hooked = false;      // set when the hook's API is first called
    bool defOn = false;
    std::atomic<bool> enabled{false};
};
static Module g_modules[M_COUNT];
static inline bool ModOn(ModId id) { return g_modules[id].enabled.load(); }
static inline bool ModActive(ModId id) {
    return g_modules[id].enabled.load() && (!g_modules[id].needsHook || g_modules[id].hooked);
}
static inline void MarkHooked(ModId id) { g_modules[id].hooked = true; }

static std::atomic<bool>& g_entityCulling = g_modules[M_ENTITY_CULL].enabled;
static std::atomic<bool>& g_undergroundCulling = g_modules[M_UNDERGROUND_CULL].enabled;

static void InitModules() {
    struct Def { ModId id; const char* name; const char* cat; bool needsHook; bool on; };
    static const Def defs[] = {
        {M_ENTITY_CULL, "Entity Culling", "Performance", true, true},
        {M_UNDERGROUND_CULL, "Underground Culling", "Performance", true, true},
        {M_PARTICLE_CULL, "Particle Culler", "Performance", true, false},
        {M_FPS_UNLOCK, "FPS Unlocker", "Performance", false, false},
        {M_HITBOX, "Hitbox Viewer", "Combat", true, false},
        {M_JAVA_ANIM, "1.8 Java Animations", "Combat", true, false},
        {M_NO_HURTCAM, "NoHurtCam", "Combat", true, false},
        {M_SHORT_SWORDS, "Short Swords / Low Fire", "Combat", true, false},
        {M_HIT_COLOR, "Hit Color Customizer", "Combat", true, false},
        {M_PARTICLE_MULT, "Particle Multiplier", "Combat", true, false},
        {M_HITBOX_CHROMA, "Hitbox Chroma / RGB", "Combat", true, false},
        {M_COMBO, "Combo Counter", "Combat", true, false},
        {M_REACH, "Reach Display", "Combat", true, false},
        {M_TARGET_HUD, "Target HUD", "Combat", true, false},
        {M_CPS, "CPS Counter", "HUD", false, true},
        {M_KEYSTROKES, "Keystrokes", "HUD", false, true},
        {M_FPS, "FPS Counter", "HUD", false, true},
        {M_CLOCK, "Clock / Real Time HUD", "HUD", false, false},
        {M_TOTEM, "Totem Counter", "HUD", true, false},
        {M_ARROW, "Arrow Counter", "HUD", true, false},
        {M_BLOCKS, "Block Counter", "HUD", true, false},
        {M_ARMOR_HUD, "Armor & Status HUD", "HUD", true, false},
        {M_SPEEDO, "Speedometer / Speed HUD", "HUD", true, false},
        {M_COMPASS, "Direction HUD / Compass", "HUD", true, false},
        {M_SERVER_STATS, "Match Stats Tracker", "HUD", true, false},
        {M_FULLBRIGHT, "Fullbright", "Visuals", true, false},
        {M_ZOOM, "Smooth Zoom", "Visuals", true, false},
        {M_COSMETICS, "Custom Cosmetics & Capes", "Visuals", true, false},
        {M_GLINT, "Glint Color Customizer", "Visuals", true, false},
        {M_HURT_SOUND, "Custom Hurt Sound", "Visuals", true, false},
        {M_MOTION_BLUR, "Motion Blur", "Visuals", false, false},
        {M_TIME_CHANGER, "Time Changer", "Visuals", true, false},
        {M_CHROMA_UI, "Chroma / RGB UI", "Visuals", false, false},
        {M_CHAT_TWEAKS, "Custom Chat Tweaks", "Visuals", true, false},
        {M_DYN_FOV, "Dynamic FOV Toggles", "Visuals", true, false},
        {M_OFFHAND, "Off-Hand Keybind", "Utilities", true, false},
        {M_TOGGLE_SPRINT, "Toggle Sprint", "Utilities", false, false},
        {M_TOGGLE_SNEAK, "Toggle Sneak", "Utilities", false, false},
        {M_SCROLL_KEYS, "Scroll Hotkeys", "Utilities", true, false},
        {M_AUTO_GG, "Auto-GG / Auto-Vote", "Utilities", true, false},
    };
    for (const Def& d : defs) {
        Module& m = g_modules[d.id];
        m.name = d.name; m.cat = d.cat; m.needsHook = d.needsHook;
        m.hooked = !d.needsHook; m.defOn = d.on; m.enabled = d.on;
    }
}

// =====================================================================
// Config
// =====================================================================
struct Config {
    // culling
    float entityMaxDistance = 64.0f;
    float entityAlwaysShow  = 6.0f;
    int   losMaxSteps       = 96;
    int   surfaceMargin     = 2;
    float particleMaxDistance = 24.0f;
    int   fpsLimit = 0;                 // 0 = unlimited (FPS Unlocker)
    // visuals
    float blurStrength = 0.45f;
    float chromaSpeed  = 0.5f;
    float zoomFov      = 30.0f;
    float particleMult = 2.0f;
    float hitColor[3]   = {0.2f, 0.5f, 1.0f};
    float glintColor[3] = {0.2f, 0.8f, 1.0f};
    int   timeOfDay = 0;                // 0 day, 1 sunset, 2 night
    char  hurtSound[260] = "";
    char  autoMsg[64] = "gg";
    // hud
    bool  hud = true;
    bool  titleStatus = true;
    bool  beepOnToggle = true;
    float keysPosX = 0.02f, keysPosY = 0.60f, keysScale = 1.0f;
    // hotkeys
    int keyMenu = VK_INSERT, keyEntity = VK_F6, keyUnderground = VK_F7, keyReload = VK_F8;
    int keyBorderless = VK_F9, keyTopmost = VK_F10, keyEject = VK_END;
    int keyToggleSprint = VK_PRIOR, keyToggleSneak = VK_NEXT;
    int sprintKey = VK_LCONTROL, sneakKey = VK_LSHIFT, zoomKey = 'C';
};
static Config g_cfg;  // written on reload/UI, read by hooks (benign race)
static std::atomic<uint32_t> g_culledEntities{0};
static std::atomic<uint32_t> g_culledSections{0};
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_menuOpen{false};
static std::atomic<int>  g_uiStatus{0};  // 0 waiting, 1 ok, 2 not DX11, 3 hook failed
static std::string g_iniPath;
static std::string g_logPath;

static void Log(const char* fmt, ...) {
    char line[600];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    FILE* f = g_logPath.empty() ? nullptr : fopen(g_logPath.c_str(), "ab");
    if (!f) {
        char tmp[MAX_PATH] = {};
        if (GetTempPathA(MAX_PATH, tmp)) f = fopen((std::string(tmp) + "ombryn.log").c_str(), "ab");
    }
    if (f) { fprintf(f, "%s\r\n", line); fclose(f); }
    OutputDebugStringA(line);
}
static DWORD WINAPI MsgThread(LPVOID p) {
    char* t = (char*)p;
    MessageBoxA(nullptr, t, "Ombryn", MB_OK | MB_TOPMOST | MB_SETFOREGROUND | MB_ICONINFORMATION);
    free(t);
    return 0;
}
static void ShowMsg(const char* text) {
    char* c = _strdup(text);
    HANDLE h = CreateThread(nullptr, 0, MsgThread, c, 0, nullptr);
    if (h) CloseHandle(h);
}

struct Vec3 { float x, y, z; };

// World access for culling (you wire these up with your hooks).
using IsSolidFn  = bool (*)(int x, int y, int z);
using SurfaceYFn = int  (*)(int x, int z);
static IsSolidFn  g_isSolid  = nullptr;
static SurfaceYFn g_surfaceY = nullptr;

// =====================================================================
// Game state fed by your hooks (all HUD modules draw from this)
// =====================================================================
struct PotionInfo { char name[24]; float secondsLeft; };
struct GameState {
    int totems = -1, arrows = -1, blocks = -1;      // -1 = unknown
    float armorDur[4] = {-1, -1, -1, -1};           // helmet, chest, legs, boots (0..1)
    PotionInfo potions[6]; int potionCount = 0;
    int combo = 0; float lastReach = -1.0f;
    char targetName[32] = ""; float targetHp = -1, targetMaxHp = 20, targetArmor = -1;
    ULONGLONG targetTick = 0;
    float x = 0, y = 0, z = 0, yaw = 0; bool hasPos = false; float bps = 0; ULONGLONG posTick = 0;
    int wins = 0, losses = 0, streak = 0;
};
static GameState g_state;

// =====================================================================
// Line of sight + culling decisions
// =====================================================================
static bool HasLineOfSight(const Vec3& a, const Vec3& b, IsSolidFn isSolid, int maxSteps) {
    if (!isSolid) return true;
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return true;
    dx /= len; dy /= len; dz /= len;
    int x = (int)std::floor(a.x), y = (int)std::floor(a.y), z = (int)std::floor(a.z);
    int ex = (int)std::floor(b.x), ey = (int)std::floor(b.y), ez = (int)std::floor(b.z);
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    const float INF = 1e30f;
    float tDx = dx != 0 ? std::fabs(1.0f / dx) : INF;
    float tDy = dy != 0 ? std::fabs(1.0f / dy) : INF;
    float tDz = dz != 0 ? std::fabs(1.0f / dz) : INF;
    float tMx = dx != 0 ? ((dx > 0 ? (x + 1 - a.x) : (a.x - x)) * tDx) : INF;
    float tMy = dy != 0 ? ((dy > 0 ? (y + 1 - a.y) : (a.y - y)) * tDy) : INF;
    float tMz = dz != 0 ? ((dz > 0 ? (z + 1 - a.z) : (a.z - z)) * tDz) : INF;
    for (int i = 0; i < maxSteps; ++i) {
        if (x == ex && y == ey && z == ez) return true;
        if (tMx < tMy && tMx < tMz) { x += sx; tMx += tDx; }
        else if (tMy < tMz)         { y += sy; tMy += tDy; }
        else                        { z += sz; tMz += tDz; }
        if (!(x == ex && y == ey && z == ez) && isSolid(x, y, z)) return false;
    }
    return true;
}

// =====================================================================
// HOOK API: call these from your detours. Each one marks its module as hooked.
// =====================================================================
// true = skip drawing this entity.
bool ShouldCullEntity(const Vec3& cam, const Vec3& entityCenter) {
    MarkHooked(M_ENTITY_CULL);
    if (!g_entityCulling) return false;
    float dx = entityCenter.x - cam.x, dy = entityCenter.y - cam.y, dz = entityCenter.z - cam.z;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < g_cfg.entityAlwaysShow * g_cfg.entityAlwaysShow) return false;
    bool cull = d2 > g_cfg.entityMaxDistance * g_cfg.entityMaxDistance ||
                !HasLineOfSight(cam, entityCenter, g_isSolid, g_cfg.losMaxSteps);
    if (cull) ++g_culledEntities;
    return cull;
}
// true = skip drawing this chunk section (world Y range [yMin, yMax]).
bool ShouldCullSubsurfaceSection(const Vec3& cam, int blockX, int blockZ, int yMin, int yMax) {
    (void)yMin;
    MarkHooked(M_UNDERGROUND_CULL);
    if (!g_undergroundCulling || !g_surfaceY) return false;
    int camSurface = g_surfaceY((int)std::floor(cam.x), (int)std::floor(cam.z));
    if (cam.y < (float)(camSurface - g_cfg.surfaceMargin)) return false;
    bool cull = yMax < g_surfaceY(blockX, blockZ) - g_cfg.surfaceMargin;
    if (cull) ++g_culledSections;
    return cull;
}
// true = skip this particle.
bool ShouldCullParticle(const Vec3& cam, const Vec3& p) {
    MarkHooked(M_PARTICLE_CULL);
    if (!ModOn(M_PARTICLE_CULL)) return false;
    float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    return dx * dx + dy * dy + dz * dz > g_cfg.particleMaxDistance * g_cfg.particleMaxDistance;
}
// Smooth zoom: pass the game's base FOV, use the returned FOV. Hold zoomKey to zoom.
float SmoothZoom(float baseFov) {
    MarkHooked(M_ZOOM);
    static float cur = 1.0f;
    bool held = ModOn(M_ZOOM) && (GetAsyncKeyState(g_cfg.zoomKey) & 0x8000) && !g_menuOpen;
    float target = held ? (g_cfg.zoomFov / baseFov) : 1.0f;
    cur += (target - cur) * 0.2f;
    return baseFov * cur;
}
// Colors for your render hooks (RGB 0..1). Return false if the module is off.
bool HitColor(float out[3]) {
    MarkHooked(M_HIT_COLOR);
    if (!ModOn(M_HIT_COLOR)) return false;
    memcpy(out, g_cfg.hitColor, sizeof(float) * 3); return true;
}
bool GlintColor(float out[3]) {
    MarkHooked(M_GLINT);
    if (!ModOn(M_GLINT)) return false;
    memcpy(out, g_cfg.glintColor, sizeof(float) * 3); return true;
}
// Time changer: returns world time in ticks to force, or -1 if off.
int ForcedTimeTicks() {
    MarkHooked(M_TIME_CHANGER);
    if (!ModOn(M_TIME_CHANGER)) return -1;
    static const int ticks[3] = {6000, 12000, 18000};
    return ticks[g_cfg.timeOfDay < 0 ? 0 : (g_cfg.timeOfDay > 2 ? 2 : g_cfg.timeOfDay)];
}
float ParticleMultiplier() {
    MarkHooked(M_PARTICLE_MULT);
    return ModOn(M_PARTICLE_MULT) ? g_cfg.particleMult : 1.0f;
}
// Simple yes/no checks for the other hook-based modules, e.g. in your hurt-camera hook:
//   if (ModOn(M_NO_HURTCAM)) return;   // skip the shake
// Others: M_HITBOX, M_JAVA_ANIM, M_SHORT_SWORDS, M_HITBOX_CHROMA, M_FULLBRIGHT,
//   M_COSMETICS, M_CHAT_TWEAKS, M_DYN_FOV, M_OFFHAND, M_SCROLL_KEYS. Call MarkHooked(id) once installed.

// Data feeds for HUD modules.
void FeedInventory(int totems, int arrows, int blocks) {
    g_state.totems = totems; g_state.arrows = arrows; g_state.blocks = blocks;
    MarkHooked(M_TOTEM); MarkHooked(M_ARROW); MarkHooked(M_BLOCKS);
}
void FeedArmor(const float dur[4]) {
    memcpy(g_state.armorDur, dur, sizeof(float) * 4); MarkHooked(M_ARMOR_HUD);
}
void FeedPotions(const PotionInfo* p, int n) {
    n = n > 6 ? 6 : (n < 0 ? 0 : n);
    for (int i = 0; i < n; ++i) g_state.potions[i] = p[i];
    g_state.potionCount = n; MarkHooked(M_ARMOR_HUD);
}
void FeedPosition(float x, float y, float z, float yawDeg) {
    ULONGLONG now = GetTickCount64();
    if (g_state.hasPos && now > g_state.posTick) {
        float dt = (now - g_state.posTick) / 1000.0f;
        float inst = std::hypot(x - g_state.x, z - g_state.z) / dt;
        g_state.bps = g_state.bps * 0.8f + inst * 0.2f;
    }
    g_state.x = x; g_state.y = y; g_state.z = z; g_state.yaw = yawDeg;
    g_state.hasPos = true; g_state.posTick = now;
    MarkHooked(M_SPEEDO); MarkHooked(M_COMPASS);
}
void OnPlayerHurt();  // defined below
void OnPlayerHit(float reach, const char* targetName, float hp, float maxHp, float armor) {
    ++g_state.combo; g_state.lastReach = reach;
    strncpy(g_state.targetName, targetName ? targetName : "", sizeof g_state.targetName - 1);
    g_state.targetName[sizeof g_state.targetName - 1] = 0;
    g_state.targetHp = hp; g_state.targetMaxHp = maxHp; g_state.targetArmor = armor;
    g_state.targetTick = GetTickCount64();
    MarkHooked(M_COMBO); MarkHooked(M_REACH); MarkHooked(M_TARGET_HUD);
}
void OnPlayerHurt() {   // call when YOU take damage: resets combo, plays custom hurt sound
    g_state.combo = 0;
    MarkHooked(M_HURT_SOUND);
    if (ModOn(M_HURT_SOUND) && g_cfg.hurtSound[0])
        PlaySoundA(g_cfg.hurtSound, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}
void OnMatchResult(bool win) {
    if (win) { ++g_state.wins; g_state.streak = g_state.streak > 0 ? g_state.streak + 1 : 1; }
    else     { ++g_state.losses; g_state.streak = 0; }
    MarkHooked(M_SERVER_STATS);
}

// =====================================================================
// Config file (ombryn.ini next to the DLL)
// =====================================================================
static const char* kDefaultIni =
    "[culling]\r\n"
    "entity_max_distance=64\r\n"
    "entity_always_show=6\r\n"
    "los_max_steps=96\r\n"
    "surface_margin=2\r\n"
    "particle_max_distance=24\r\n"
    "\r\n"
    "[hotkeys]\r\n"
    "; names: F1-F24, A-Z, 0-9, END, HOME, INSERT, DELETE, PAGEUP, PAGEDOWN, PAUSE, SPACE, TAB, LCTRL, LSHIFT\r\n"
    "menu=INSERT\r\n"
    "toggle_entity=F6\r\n"
    "toggle_underground=F7\r\n"
    "reload_config=F8\r\n"
    "toggle_borderless=F9\r\n"
    "toggle_always_on_top=F10\r\n"
    "toggle_sprint=PAGEUP\r\n"
    "toggle_sneak=PAGEDOWN\r\n"
    "zoom=C\r\n"
    "eject=END\r\n"
    "\r\n"
    "[game_keys]\r\n"
    "; the game's own sprint/sneak keys that Toggle Sprint/Sneak will hold down\r\n"
    "sprint=LCTRL\r\n"
    "sneak=LSHIFT\r\n"
    "\r\n"
    "[misc]\r\n"
    "title_status=1\r\n"
    "beep=1\r\n"
    "hud=1\r\n";

static int ParseKey(const char* s, int fallback) {
    char buf[32]; size_t n = 0;
    for (; *s && n < sizeof(buf) - 1; ++s)
        if (*s != ' ') buf[n++] = (char)toupper((unsigned char)*s);
    buf[n] = 0;
    if (n == 0) return fallback;
    if (buf[0] == 'F' && n >= 2 && n <= 3) {
        int f = atoi(buf + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    if (n == 1 && ((buf[0] >= 'A' && buf[0] <= 'Z') || (buf[0] >= '0' && buf[0] <= '9'))) return buf[0];
    struct { const char* name; int vk; } names[] = {
        {"END", VK_END}, {"HOME", VK_HOME}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"PAUSE", VK_PAUSE}, {"SPACE", VK_SPACE},
        {"TAB", VK_TAB}, {"LCTRL", VK_LCONTROL}, {"CTRL", VK_LCONTROL},
        {"LSHIFT", VK_LSHIFT}, {"SHIFT", VK_LSHIFT}};
    for (auto& e : names) if (!strcmp(buf, e.name)) return e.vk;
    return fallback;
}

static float IniFloat(const char* sec, const char* key, float def) {
    char b[64];
    GetPrivateProfileStringA(sec, key, "", b, sizeof b, g_iniPath.c_str());
    return b[0] ? (float)atof(b) : def;
}
static int IniInt(const char* sec, const char* key, int def) {
    return (int)GetPrivateProfileIntA(sec, key, def, g_iniPath.c_str());
}
static int IniKey(const char* sec, const char* key, int def) {
    char b[32];
    GetPrivateProfileStringA(sec, key, "", b, sizeof b, g_iniPath.c_str());
    return ParseKey(b, def);
}
static void IniSet(const char* sec, const char* key, const char* val) {
    WritePrivateProfileStringA(sec, key, val, g_iniPath.c_str());
}
static void IniSetNum(const char* sec, const char* key, double v) {
    char b[32]; snprintf(b, sizeof b, "%g", v); IniSet(sec, key, b);
}

static bool IsLatchModule(int i) { return i == M_TOGGLE_SPRINT || i == M_TOGGLE_SNEAK; }

// keepKeys = true: leave hotkeys / game keys as they are. Profile files only hold module
// toggles and settings (SaveConfig never writes keys into them), so without this a profile
// load would reset every hotkey you customised in ombryn.ini back to its default.
static void LoadConfig(bool keepKeys = false) {
    Config c;  // defaults
    if (keepKeys) {
        c.keyMenu = g_cfg.keyMenu;           c.keyEntity = g_cfg.keyEntity;
        c.keyUnderground = g_cfg.keyUnderground; c.keyReload = g_cfg.keyReload;
        c.keyBorderless = g_cfg.keyBorderless;   c.keyTopmost = g_cfg.keyTopmost;
        c.keyEject = g_cfg.keyEject;         c.keyToggleSprint = g_cfg.keyToggleSprint;
        c.keyToggleSneak = g_cfg.keyToggleSneak; c.sprintKey = g_cfg.sprintKey;
        c.sneakKey = g_cfg.sneakKey;         c.zoomKey = g_cfg.zoomKey;
    }
    c.entityMaxDistance = IniFloat("culling", "entity_max_distance", c.entityMaxDistance);
    c.entityAlwaysShow  = IniFloat("culling", "entity_always_show", c.entityAlwaysShow);
    c.losMaxSteps       = IniInt("culling", "los_max_steps", c.losMaxSteps);
    c.surfaceMargin     = IniInt("culling", "surface_margin", c.surfaceMargin);
    c.particleMaxDistance = IniFloat("culling", "particle_max_distance", c.particleMaxDistance);
    c.fpsLimit = IniInt("performance", "fps_limit", c.fpsLimit);
    c.blurStrength = IniFloat("visual", "blur_strength", c.blurStrength);
    c.chromaSpeed  = IniFloat("visual", "chroma_speed", c.chromaSpeed);
    c.zoomFov      = IniFloat("visual", "zoom_fov", c.zoomFov);
    c.particleMult = IniFloat("visual", "particle_multiplier", c.particleMult);
    c.hitColor[0] = IniFloat("visual", "hit_r", c.hitColor[0]);
    c.hitColor[1] = IniFloat("visual", "hit_g", c.hitColor[1]);
    c.hitColor[2] = IniFloat("visual", "hit_b", c.hitColor[2]);
    c.glintColor[0] = IniFloat("visual", "glint_r", c.glintColor[0]);
    c.glintColor[1] = IniFloat("visual", "glint_g", c.glintColor[1]);
    c.glintColor[2] = IniFloat("visual", "glint_b", c.glintColor[2]);
    c.timeOfDay = IniInt("visual", "time_of_day", c.timeOfDay);
    GetPrivateProfileStringA("visual", "hurt_sound", "", c.hurtSound, sizeof c.hurtSound, g_iniPath.c_str());
    GetPrivateProfileStringA("utility", "auto_message", "gg", c.autoMsg, sizeof c.autoMsg, g_iniPath.c_str());
    c.keysPosX  = IniFloat("pvp", "keys_pos_x", c.keysPosX);
    c.keysPosY  = IniFloat("pvp", "keys_pos_y", c.keysPosY);
    c.keysScale = IniFloat("pvp", "keys_scale", c.keysScale);
    c.keyMenu = IniKey("hotkeys", "menu", c.keyMenu);
    c.keyEntity = IniKey("hotkeys", "toggle_entity", c.keyEntity);
    c.keyUnderground = IniKey("hotkeys", "toggle_underground", c.keyUnderground);
    c.keyReload = IniKey("hotkeys", "reload_config", c.keyReload);
    c.keyBorderless = IniKey("hotkeys", "toggle_borderless", c.keyBorderless);
    c.keyTopmost = IniKey("hotkeys", "toggle_always_on_top", c.keyTopmost);
    c.keyToggleSprint = IniKey("hotkeys", "toggle_sprint", c.keyToggleSprint);
    c.keyToggleSneak = IniKey("hotkeys", "toggle_sneak", c.keyToggleSneak);
    c.zoomKey = IniKey("hotkeys", "zoom", c.zoomKey);
    c.keyEject = IniKey("hotkeys", "eject", c.keyEject);
    c.sprintKey = IniKey("game_keys", "sprint", c.sprintKey);
    c.sneakKey = IniKey("game_keys", "sneak", c.sneakKey);
    c.titleStatus = IniInt("misc", "title_status", 1) != 0;
    c.beepOnToggle = IniInt("misc", "beep", 1) != 0;
    c.hud = IniInt("misc", "hud", 1) != 0;
    g_cfg = c;
    for (int i = 0; i < M_COUNT; ++i) {
        if (IsLatchModule(i)) continue;  // sprint/sneak latches always start off
        g_modules[i].enabled = IniInt("modules", g_modules[i].name, g_modules[i].defOn ? 1 : 0) != 0;
    }
}

static void SaveConfig() {
    IniSetNum("culling", "entity_max_distance", g_cfg.entityMaxDistance);
    IniSetNum("culling", "entity_always_show", g_cfg.entityAlwaysShow);
    IniSetNum("culling", "los_max_steps", g_cfg.losMaxSteps);
    IniSetNum("culling", "surface_margin", g_cfg.surfaceMargin);
    IniSetNum("culling", "particle_max_distance", g_cfg.particleMaxDistance);
    IniSetNum("performance", "fps_limit", g_cfg.fpsLimit);
    IniSetNum("visual", "blur_strength", g_cfg.blurStrength);
    IniSetNum("visual", "chroma_speed", g_cfg.chromaSpeed);
    IniSetNum("visual", "zoom_fov", g_cfg.zoomFov);
    IniSetNum("visual", "particle_multiplier", g_cfg.particleMult);
    IniSetNum("visual", "hit_r", g_cfg.hitColor[0]);
    IniSetNum("visual", "hit_g", g_cfg.hitColor[1]);
    IniSetNum("visual", "hit_b", g_cfg.hitColor[2]);
    IniSetNum("visual", "glint_r", g_cfg.glintColor[0]);
    IniSetNum("visual", "glint_g", g_cfg.glintColor[1]);
    IniSetNum("visual", "glint_b", g_cfg.glintColor[2]);
    IniSetNum("visual", "time_of_day", g_cfg.timeOfDay);
    IniSet("visual", "hurt_sound", g_cfg.hurtSound);
    IniSet("utility", "auto_message", g_cfg.autoMsg);
    IniSetNum("pvp", "keys_pos_x", g_cfg.keysPosX);
    IniSetNum("pvp", "keys_pos_y", g_cfg.keysPosY);
    IniSetNum("pvp", "keys_scale", g_cfg.keysScale);
    IniSetNum("misc", "title_status", g_cfg.titleStatus ? 1 : 0);
    IniSetNum("misc", "beep", g_cfg.beepOnToggle ? 1 : 0);
    IniSetNum("misc", "hud", g_cfg.hud ? 1 : 0);
    for (int i = 0; i < M_COUNT; ++i) {
        if (IsLatchModule(i)) continue;
        IniSetNum("modules", g_modules[i].name, g_modules[i].enabled ? 1 : 0);
    }
}

// ---- Profiles: profile_<name>.ini files next to the DLL ----
struct IniScope {
    std::string saved;
    explicit IniScope(const std::string& p) : saved(g_iniPath) { g_iniPath = p; }
    ~IniScope() { g_iniPath = saved; }
};
static std::string DirOf(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}
static std::string CleanName(const char* in) {
    std::string out;
    for (; *in && out.size() < 24; ++in)
        if (isalnum((unsigned char)*in) || *in == '_' || *in == '-') out += *in;
    return out.empty() ? std::string("default") : out;
}
static std::string ProfilePath(const std::string& name) {
    return DirOf(g_iniPath) + "profile_" + name + ".ini";
}
static void SaveProfile(const char* name) {
    std::string p = ProfilePath(CleanName(name));
    IniScope scope(p);
    SaveConfig();
}
static void LoadProfile(const std::string& name) {
    std::string p = ProfilePath(name);
    if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    IniScope scope(p);
    LoadConfig(true);
}
static std::vector<std::string> ListProfiles() {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((DirOf(g_iniPath) + "profile_*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string f = fd.cFileName;  // profile_<name>.ini
        if (f.size() > 12) out.push_back(f.substr(8, f.size() - 12));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// =====================================================================
// Window + input helpers
// =====================================================================
struct FindCtx { DWORD pid; HWND best; long area; };
static BOOL CALLBACK EnumCb(HWND h, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetWindowRect(h, &r);
    long a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}
static HWND FindGameWindow() {
    FindCtx c{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(EnumCb, (LPARAM)&c);
    return c.best;
}

static bool g_borderless = false;
static LONG_PTR g_savedStyle = 0;
static RECT g_savedRect{};
static void SetBorderless(HWND w, bool on) {
    if (!w || on == g_borderless) return;
    if (on) {
        g_savedStyle = GetWindowLongPtrW(w, GWL_STYLE);
        GetWindowRect(w, &g_savedRect);
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        LONG_PTR style = (g_savedStyle & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                          WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
        SetWindowLongPtrW(w, GWL_STYLE, style);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(w, GWL_STYLE, g_savedStyle);
        SetWindowPos(w, HWND_TOP, g_savedRect.left, g_savedRect.top,
                     g_savedRect.right - g_savedRect.left, g_savedRect.bottom - g_savedRect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    g_borderless = on;
}
static bool g_topmost = false;
static void SetTopmost(HWND w, bool on) {
    if (!w) return;
    g_topmost = on;
    SetWindowPos(w, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static void Feedback(bool on) {
    if (g_cfg.beepOnToggle) Beep(on ? 1000 : 500, 60);
}

// Toggle sprint / toggle sneak: holds the game's own sprint/sneak key for you.
static void SendKey(int vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}
static bool g_sprintHeld = false, g_sneakHeld = false;
static void UpdateHeldKeys(bool active) {
    bool wantSneak = active && ModOn(M_TOGGLE_SNEAK);
    bool wantSprint = active && ModOn(M_TOGGLE_SPRINT) && (GetAsyncKeyState('W') & 0x8000);
    if (wantSneak != g_sneakHeld) { SendKey(g_cfg.sneakKey, wantSneak); g_sneakHeld = wantSneak; }
    if (wantSprint != g_sprintHeld) { SendKey(g_cfg.sprintKey, wantSprint); g_sprintHeld = wantSprint; }
}

// Auto-GG: types a chat message with the keyboard. Call OnMatchEnd() from a match-end hook.
static DWORD WINAPI ChatThread(LPVOID p) {
    std::string* m = (std::string*)p;
    SendKey('T', true); Sleep(30); SendKey('T', false);  // open chat (game's default key)
    Sleep(250);
    for (char ch : *m) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = (WORD)(unsigned char)ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(15);
    }
    Sleep(50);
    SendKey(VK_RETURN, true); Sleep(30); SendKey(VK_RETURN, false);
    delete m;
    return 0;
}
void OnMatchEnd() {
    MarkHooked(M_AUTO_GG);
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!ModOn(M_AUTO_GG) || now - last < 8000 || !g_cfg.autoMsg[0]) return;
    last = now;
    HANDLE h = CreateThread(nullptr, 0, ChatThread, new std::string(g_cfg.autoMsg), 0, nullptr);
    if (h) CloseHandle(h);
}

// =====================================================================
// CPS tracking
// =====================================================================
static std::atomic<uint64_t> g_frames{0};
static std::mutex g_cpsMtx;
static std::vector<ULONGLONG> g_lmbClicks, g_rmbClicks;
static void SampleClicks(bool gameFocused) {
    static bool pl = false, pr = false;
    bool l = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool r = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_cpsMtx);
    if (gameFocused && !g_menuOpen) {
        if (l && !pl) g_lmbClicks.push_back(now);
        if (r && !pr) g_rmbClicks.push_back(now);
    }
    pl = l; pr = r;
    auto prune = [&](std::vector<ULONGLONG>& v) {
        size_t k = 0;
        while (k < v.size() && now - v[k] > 1000) ++k;
        v.erase(v.begin(), v.begin() + k);
    };
    prune(g_lmbClicks); prune(g_rmbClicks);
}
static int CurrentCps(bool left) {
    std::lock_guard<std::mutex> lock(g_cpsMtx);
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (ULONGLONG t : (left ? g_lmbClicks : g_rmbClicks)) if (now - t <= 1000) ++n;
    return n;
}

// =====================================================================
// In-game UI (ImGui over DX11)
// =====================================================================
static ID3D11Device*        g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static HWND    g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool    g_imguiInit = false;
static ImFont* g_logoFont = nullptr;

// True when the Minecraft window (or its Store frame window) has focus.
static bool IsGameFocused() {
    HWND f = GetForegroundWindow();
    if (!f) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    if (pid == GetCurrentProcessId()) return true;
    if (g_hwnd && (f == g_hwnd || GetAncestor(g_hwnd, GA_ROOT) == f)) return true;
    // UWP builds: the foreground window is an ApplicationFrameWindow owned by another process.
    // Only that class may match by title, otherwise a browser tab called "Minecraft Wiki"
    // would count as the game (hotkeys firing, clicks counted).
    wchar_t cls[64] = {};
    GetClassNameW(f, cls, 64);
    if (wcscmp(cls, L"ApplicationFrameWindow") != 0) return false;
    wchar_t t[128] = {};
    GetWindowTextW(f, t, 128);
    return wcsstr(t, L"Minecraft") != nullptr;
}

// DirectX 12 state (used only when the game renders with DX12)
static ID3D12Device*              g_dev12 = nullptr;
static ID3D12CommandQueue*        g_queue12 = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap12 = nullptr;
static ID3D12DescriptorHeap*      g_srvHeap12 = nullptr;
static ID3D12GraphicsCommandList* g_cmd12 = nullptr;
static ID3D12Fence*               g_fence12 = nullptr;
static HANDLE   g_fenceEvent12 = nullptr;
static UINT64   g_fenceVal12 = 0;
struct Frame12 { ID3D12CommandAllocator* alloc = nullptr; UINT64 fence = 0; };
static std::vector<Frame12> g_frames12;
static UINT g_rtvInc12 = 0;
static DXGI_FORMAT g_rtFmt12 = DXGI_FORMAT_R8G8B8A8_UNORM;
static bool g_dx12Init = false;
static int  g_api = 0;  // 0 unknown, 11, 12, -1 unsupported

// Motion blur: previous frame kept as a texture, drawn over the new frame with some alpha.
static ID3D11Texture2D*          g_prevTex = nullptr;
static ID3D11ShaderResourceView* g_prevSrv = nullptr;
static UINT g_prevW = 0, g_prevH = 0;
static DXGI_FORMAT g_prevFmt = DXGI_FORMAT_UNKNOWN;
static bool g_prevValid = false, g_blurFailed = false;
static ImDrawList* g_blurList = nullptr;   // blur layer, rendered separately from the HUD (DX11 only)

static void ReleasePrev() {
    if (g_prevSrv) { g_prevSrv->Release(); g_prevSrv = nullptr; }
    if (g_prevTex) { g_prevTex->Release(); g_prevTex = nullptr; }
    g_prevValid = false; g_prevW = g_prevH = 0; g_prevFmt = DXGI_FORMAT_UNKNOWN;
}
static void EnsurePrev(ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    if (g_prevTex && d.Width == g_prevW && d.Height == g_prevH && d.Format == g_prevFmt) return;
    ReleasePrev();
    if (d.SampleDesc.Count > 1) { g_blurFailed = true; return; }
    UINT w = d.Width, h = d.Height; DXGI_FORMAT f = d.Format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_prevTex))) { g_prevTex = nullptr; g_blurFailed = true; return; }
    if (FAILED(g_dev->CreateShaderResourceView(g_prevTex, nullptr, &g_prevSrv))) { ReleasePrev(); g_blurFailed = true; return; }
    g_prevW = w; g_prevH = h; g_prevFmt = f;
}

static LRESULT CALLBACK HookedWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_menuOpen) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        switch (m) {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_INPUT:
                return 0;  // menu eats input; the game never sees it
        }
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

static void StyleUI() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8; s.FrameRounding = 5; s.GrabRounding = 5; s.TabRounding = 5;
    s.WindowPadding = ImVec2(12, 12); s.ItemSpacing = ImVec2(8, 8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.08f, 0.10f, 0.96f);
    c[ImGuiCol_TitleBgActive]  = ImVec4(0.10f, 0.35f, 0.55f, 1.00f);
    c[ImGuiCol_CheckMark]      = ImVec4(0.30f, 0.80f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]     = ImVec4(0.30f, 0.70f, 0.95f, 1.00f);
    c[ImGuiCol_Tab]            = ImVec4(0.12f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_TabActive]      = ImVec4(0.15f, 0.45f, 0.70f, 1.00f);
    c[ImGuiCol_TabHovered]     = ImVec4(0.20f, 0.55f, 0.80f, 1.00f);
    c[ImGuiCol_Button]         = ImVec4(0.15f, 0.40f, 0.62f, 1.00f);
}

static ImU32 ChromaCol(float off) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod((float)ImGui::GetTime() * g_cfg.chromaSpeed + off, 1.0f),
                                0.75f, 1.0f, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}
static ImU32 TextCol(float off) {
    return ModOn(M_CHROMA_UI) ? ChromaCol(off) : IM_COL32(255, 255, 255, 235);
}

struct RightStack { ImDrawList* dl; float x; float y; };
static void RLine(RightStack& s, const char* text, ImU32 col) {
    ImVec2 sz = ImGui::CalcTextSize(text);
    float x = s.x - sz.x;
    s.dl->AddRectFilled(ImVec2(x - 6, s.y - 2), ImVec2(x + sz.x + 6, s.y + sz.y + 2), IM_COL32(0, 0, 0, 110), 4.0f);
    s.dl->AddText(ImVec2(x, s.y), col, text);
    s.y += sz.y + 6;
}

static void DrawRightHud() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    RightStack s{dl, ImGui::GetIO().DisplaySize.x - 14.0f, 10.0f};
    float hue = 0.0f;
    auto col = [&]() { hue += 0.08f; return TextCol(hue); };

    // FPS | CPS | clock
    static ULONGLONG lastT = 0; static uint64_t lastF = 0; static int fps = 0;
    ULONGLONG now = GetTickCount64();
    if (lastT == 0) { lastT = now; lastF = g_frames; }
    if (now - lastT >= 500) {
        fps = (int)((g_frames - lastF) * 1000 / (now - lastT));
        lastT = now; lastF = g_frames;
    }
    char buf[160]; buf[0] = 0; char t[64];
    auto add = [&](const char* piece) {
        if (buf[0]) strncat(buf, "  |  ", sizeof buf - strlen(buf) - 1);
        strncat(buf, piece, sizeof buf - strlen(buf) - 1);
    };
    if (ModOn(M_FPS)) { snprintf(t, sizeof t, "%d FPS", fps); add(t); }
    if (ModOn(M_CPS)) { snprintf(t, sizeof t, "%d | %d CPS", CurrentCps(true), CurrentCps(false)); add(t); }
    if (ModOn(M_CLOCK)) { SYSTEMTIME st; GetLocalTime(&st); snprintf(t, sizeof t, "%02d:%02d", st.wHour, st.wMinute); add(t); }
    if (buf[0]) RLine(s, buf, col());

    // counters
    if (ModActive(M_TOTEM) && g_state.totems >= 0) { snprintf(t, sizeof t, "Totems: %d", g_state.totems); RLine(s, t, col()); }
    if (ModActive(M_ARROW) && g_state.arrows >= 0) { snprintf(t, sizeof t, "Arrows: %d", g_state.arrows); RLine(s, t, col()); }
    if (ModActive(M_BLOCKS) && g_state.blocks >= 0) { snprintf(t, sizeof t, "Blocks: %d", g_state.blocks); RLine(s, t, col()); }
    if (ModActive(M_ARMOR_HUD)) {
        const char* nm[4] = {"H", "C", "L", "B"};
        char a[128]; a[0] = 0;
        for (int i = 0; i < 4; ++i) {
            if (g_state.armorDur[i] < 0) continue;
            snprintf(t, sizeof t, "%s%s %d%%", a[0] ? "  " : "", nm[i], (int)(g_state.armorDur[i] * 100));
            strncat(a, t, sizeof a - strlen(a) - 1);
        }
        if (a[0]) RLine(s, a, col());
        for (int i = 0; i < g_state.potionCount; ++i) {
            int sec = (int)g_state.potions[i].secondsLeft;
            snprintf(t, sizeof t, "%s %d:%02d", g_state.potions[i].name, sec / 60, sec % 60);
            RLine(s, t, col());
        }
    }
    // combat
    if (ModActive(M_COMBO)) { snprintf(t, sizeof t, "Combo: %d", g_state.combo); RLine(s, t, col()); }
    if (ModActive(M_REACH) && g_state.lastReach >= 0) { snprintf(t, sizeof t, "Reach: %.2f", g_state.lastReach); RLine(s, t, col()); }
    if (ModActive(M_TARGET_HUD) && g_state.targetName[0] && GetTickCount64() - g_state.targetTick < 5000) {
        snprintf(t, sizeof t, "%s  HP %.0f/%.0f", g_state.targetName, g_state.targetHp, g_state.targetMaxHp);
        if (g_state.targetArmor >= 0) {
            char u[32]; snprintf(u, sizeof u, "  Armor %.0f", g_state.targetArmor);
            strncat(t, u, sizeof t - strlen(t) - 1);
        }
        RLine(s, t, col());
    }
    if (ModActive(M_SPEEDO) && g_state.hasPos) { snprintf(t, sizeof t, "%.2f BPS", g_state.bps); RLine(s, t, col()); }
    if (ModActive(M_SERVER_STATS)) {
        snprintf(t, sizeof t, "W %d  L %d  Streak %d", g_state.wins, g_state.losses, g_state.streak);
        RLine(s, t, col());
    }

    // compass, top centre
    if (ModActive(M_COMPASS) && g_state.hasPos) {
        static const char* dirs[8] = {"S", "SW", "W", "NW", "N", "NE", "E", "SE"};
        float yw = std::fmod(g_state.yaw, 360.0f);
        if (yw < 0) yw += 360.0f;
        int idx = (int)((yw + 22.5f) / 45.0f) % 8;
        snprintf(t, sizeof t, "%s   %d  %d  %d", dirs[idx], (int)std::floor(g_state.x), (int)std::floor(g_state.y), (int)std::floor(g_state.z));
        ImVec2 sz = ImGui::CalcTextSize(t);
        float x = (ImGui::GetIO().DisplaySize.x - sz.x) * 0.5f;
        dl->AddRectFilled(ImVec2(x - 8, 8), ImVec2(x + sz.x + 8, 12 + sz.y), IM_COL32(0, 0, 0, 110), 4.0f);
        dl->AddText(ImVec2(x, 10), TextCol(0.5f), t);
    }
}

static void DrawKeystrokes() {
    bool fg = IsGameFocused();
    auto down = [&](int vk) { return fg && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float u = 36.0f * g_cfg.keysScale, gap = 4.0f * g_cfg.keysScale;
    float x0 = ds.x * g_cfg.keysPosX, y0 = ds.y * g_cfg.keysPosY;
    auto box = [&](float x, float y, float w, float h, const char* label, bool on, const char* sub) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                          on ? IM_COL32(255, 255, 255, 175) : IM_COL32(0, 0, 0, 115), 5.0f);
        ImU32 c = on ? IM_COL32(0, 0, 0, 255) : TextCol(x / 400.0f);
        ImVec2 ts = ImGui::CalcTextSize(label);
        float ty = sub ? y + h * 0.5f - ts.y : y + (h - ts.y) * 0.5f;
        dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, ty), c, label);
        if (sub) {
            ImVec2 ss = ImGui::CalcTextSize(sub);
            dl->AddText(ImVec2(x + (w - ss.x) * 0.5f, ty + ts.y + 1), c, sub);
        }
    };
    float full = 3 * u + 2 * gap;
    box(x0 + u + gap, y0, u, u, "W", down('W'), nullptr);
    box(x0, y0 + u + gap, u, u, "A", down('A'), nullptr);
    box(x0 + u + gap, y0 + u + gap, u, u, "S", down('S'), nullptr);
    box(x0 + 2 * (u + gap), y0 + u + gap, u, u, "D", down('D'), nullptr);
    float mw = (full - gap) * 0.5f, my = y0 + 2 * (u + gap);
    char l[16], r[16];
    snprintf(l, sizeof l, "%d CPS", CurrentCps(true));
    snprintf(r, sizeof r, "%d CPS", CurrentCps(false));
    box(x0, my, mw, u, "LMB", down(VK_LBUTTON), l);
    box(x0 + mw + gap, my, mw, u, "RMB", down(VK_RBUTTON), r);
    box(x0, my + u + gap, full, u * 0.6f, "SPACE", down(VK_SPACE), nullptr);
}

// "Ombryn" with a big first O. Returns (width, height). draw=false only measures.
static ImVec2 DrawLogo(ImDrawList* dl, ImVec2 pos, ImU32 col, float scale = 1.0f, bool draw = true) {
    ImFont* smallF = ImGui::GetFont();
    ImFont* bigF = g_logoFont ? g_logoFont : smallF;
    float bigSize = (g_logoFont ? g_logoFont->FontSize : ImGui::GetFontSize() * 2.2f) * scale;
    float smallSize = ImGui::GetFontSize() * 1.35f * scale;
    ImVec2 oSz = bigF->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, "O");
    ImVec2 rSz = smallF->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, "mbryn");
    if (draw) {
        ImVec2 p2(pos.x + oSz.x, pos.y + (oSz.y - rSz.y));  // bottom-aligned with the big O
        dl->AddText(bigF, bigSize, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 170), "O");
        dl->AddText(bigF, bigSize, pos, col, "O");
        dl->AddText(smallF, smallSize, ImVec2(p2.x + 1, p2.y + 1), IM_COL32(0, 0, 0, 170), "mbryn");
        dl->AddText(smallF, smallSize, p2, col, "mbryn");
    }
    return ImVec2(oSz.x + rSz.x, oSz.y);
}

static void DrawModuleList() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float y = 10.0f, hue = 0.0f;
    auto line = [&](const char* t, ImU32 col) {
        dl->AddText(ImVec2(11, y + 1), IM_COL32(0, 0, 0, 160), t);
        dl->AddText(ImVec2(10, y), col, t);
        y += ImGui::GetFontSize() + 3;
    };
    y += DrawLogo(dl, ImVec2(10, y), ModOn(M_CHROMA_UI) ? ChromaCol(0) : IM_COL32(90, 200, 255, 255)).y + 4.0f;
    for (int i = 0; i < M_COUNT; ++i) {
        if (!ModActive((ModId)i)) continue;
        if (i == M_FPS || i == M_CPS || i == M_CLOCK || i == M_KEYSTROKES) continue;  // shown elsewhere
        if (i == M_MOTION_BLUR && (g_api == 12 || g_blurFailed)) continue;  // can't run, don't list it
        hue += 0.06f;
        line(g_modules[i].name, TextCol(hue));
    }
    if (g_borderless) line("Borderless", TextCol(hue + 0.06f));
    if (g_topmost) line("Always On Top", TextCol(hue + 0.12f));
}

// =====================================================================
// Hook Finder: tools to locate game functions from inside the running game.
//   - "Find code that uses this text": finds a string in the game's memory, then every
//     place in the code that references it, with the function start and a signature.
//   - "Scan for signature": checks how many times a byte pattern appears (1 = good).
// =====================================================================
struct MemRegion { const uint8_t* p; size_t n; bool exec; };

static std::vector<MemRegion> ReadableRegions(HMODULE mod = nullptr) {
    std::vector<MemRegion> out;
    const uint8_t* base = (const uint8_t*)(mod ? mod : GetModuleHandleW(nullptr));
    if (!base) return out;
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const uint8_t* end = base + nt->OptionalHeader.SizeOfImage;
    const uint8_t* p = base;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        const uint8_t* rb = (const uint8_t*)mi.BaseAddress;
        const uint8_t* re = rb + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const uint8_t* s0 = rb < base ? base : rb;
            const uint8_t* e0 = re > end ? end : re;
            bool ex = (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
            if (e0 > s0) out.push_back({s0, (size_t)(e0 - s0), ex});
        }
        p = re > p ? re : p + 0x1000;
    }
    return out;
}

static std::string GameBuildInfo() {
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return "Game info unavailable.";
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    std::string ver = "unknown";
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        DWORD h = 0, sz = GetFileVersionInfoSizeW(exe, &h);
        if (sz) {
            std::vector<char> data(sz);
            VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
            if (GetFileVersionInfoW(exe, 0, sz, data.data()) &&
                VerQueryValueW(data.data(), L"\\", (LPVOID*)&fi, &len) && fi) {
                char v[64];
                snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                         HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
                ver = v;
            }
        }
    }
    char buf[256];
    snprintf(buf, sizeof buf, "Game version: %s\nTimestamp: 0x%08X   Image size: 0x%X",
             ver.c_str(), (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.SizeOfImage);
    return buf;
}

static std::atomic<bool> g_hfBusy{false};
static std::mutex g_hfMtx;
static std::string g_hfOut = "Nothing yet. Pick a tool and press its button.";
static void HfSet(const std::string& t) { std::lock_guard<std::mutex> l(g_hfMtx); g_hfOut = t; }
static std::string HfGet() { std::lock_guard<std::mutex> l(g_hfMtx); return g_hfOut; }

// First bytes of a function as a signature. Call and jump targets and RIP-relative
// addresses become '?' so it survives small changes. Approximate; always test it.
static std::string MakeSignature(const uint8_t* p, size_t avail) {
    std::string out;
    size_t n = avail < 28 ? avail : 28, i = 0;
    auto add = [&](const char* t) { if (!out.empty()) out += ' '; out += t; };
    char t[8];
    while (i < n) {
        uint8_t b = p[i];
        if ((b == 0xE8 || b == 0xE9) && i + 5 <= n) {
            snprintf(t, sizeof t, "%02X", b); add(t);
            for (int k = 0; k < 4; ++k) add("?");
            i += 5; continue;
        }
        size_t o = ((b & 0xF0) == 0x40 && i + 1 < n) ? i + 1 : i;
        uint8_t op = p[o];
        bool ripOp = op == 0x8B || op == 0x8D || op == 0x89 || op == 0x3B || op == 0x39 ||
                     op == 0x03 || op == 0x2B || op == 0x63;
        if (ripOp && o + 6 <= n && (p[o + 1] & 0xC7) == 0x05) {
            for (size_t k = i; k <= o + 1; ++k) { snprintf(t, sizeof t, "%02X", p[k]); add(t); }
            for (int k = 0; k < 4; ++k) add("?");
            i = o + 6; continue;
        }
        snprintf(t, sizeof t, "%02X", b); add(t);
        ++i;
    }
    return out;
}

static void FindRefsJob(std::string text, bool utf16) {
    std::vector<uint8_t> needle;
    for (char c : text) { needle.push_back((uint8_t)c); if (utf16) needle.push_back(0); }
    if (needle.empty()) { HfSet("Type some text first."); g_hfBusy = false; return; }
    auto regs = ReadableRegions();
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);

    // 1) find the text in data
    std::vector<const uint8_t*> hits;
    for (const MemRegion& r : regs) {
        if (r.exec || r.n < needle.size()) continue;
        size_t i = 0;
        while (i + needle.size() <= r.n && hits.size() < 16) {
            const void* f = memchr(r.p + i, needle[0], r.n - i - needle.size() + 1);
            if (!f) break;
            i = (size_t)((const uint8_t*)f - r.p);
            bool startOk = utf16 ? (i < 2 || (r.p[i - 1] == 0 && r.p[i - 2] == 0)) : (i == 0 || r.p[i - 1] == 0);
            if (startOk && !memcmp(r.p + i, needle.data(), needle.size())) hits.push_back(r.p + i);
            ++i;
        }
    }
    char line[256];
    std::string out;
    snprintf(line, sizeof line, "Text \"%s\"%s found %u time(s) as the start of a string.\n",
             text.c_str(), utf16 ? " (UTF-16)" : "", (unsigned)hits.size());
    out += line;
    if (hits.empty()) {
        out += "Try a shorter or different piece of text, or toggle UTF-16.";
        HfSet(out); g_hfBusy = false; return;
    }
    // 2) find code that loads those addresses (LEA reg, [rip+disp32])
    struct Ref { const uint8_t* instr; const uint8_t* target; const MemRegion* reg; };
    std::vector<Ref> refs;
    for (const MemRegion& r : regs) {
        if (!r.exec) continue;
        for (size_t i = 0; i + 7 <= r.n && refs.size() < 24; ++i) {
            uint8_t b = r.p[i];
            if ((b == 0x48 || b == 0x4C) && r.p[i + 1] == 0x8D && (r.p[i + 2] & 0xC7) == 0x05) {
                int32_t d; memcpy(&d, r.p + i + 3, 4);
                const uint8_t* tgt = r.p + i + 7 + d;
                for (const uint8_t* h : hits) if (h == tgt) { refs.push_back({r.p + i, tgt, &r}); break; }
            }
        }
    }
    snprintf(line, sizeof line, "Found %u place(s) in the code that use it.\n\n", (unsigned)refs.size());
    out += line;
    int num = 0;
    for (const Ref& rf : refs) {
        const uint8_t* lo = rf.instr - 0x3000 < rf.reg->p ? rf.reg->p : rf.instr - 0x3000;
        const uint8_t* fs = nullptr;
        for (const uint8_t* q = rf.instr; q > lo; --q)
            if (q[-1] == 0xCC && q[0] != 0xCC) { fs = q; break; }
        snprintf(line, sizeof line, "#%d  used at RVA 0x%llX   (string at RVA 0x%llX)\n", ++num,
                 (unsigned long long)(rf.instr - base), (unsigned long long)(rf.target - base));
        out += line;
        if (fs) {
            size_t avail = (size_t)((rf.reg->p + rf.reg->n) - fs);
            snprintf(line, sizeof line, "    function probably starts at RVA 0x%llX (%d bytes before)\n    signature: ",
                     (unsigned long long)(fs - base), (int)(rf.instr - fs));
            out += line;
            out += MakeSignature(fs, avail) + "\n";
        } else {
            out += "    function start not found nearby\n";
        }
    }
    Log("%s", out.c_str());
    HfSet(out);
    g_hfBusy = false;
}

static void SigTestJob(std::string sig) {
    std::vector<int> pat;
    size_t i = 0;
    while (i < sig.size()) {
        while (i < sig.size() && sig[i] == ' ') ++i;
        size_t j = i;
        while (j < sig.size() && sig[j] != ' ') ++j;
        if (j > i) {
            std::string tok = sig.substr(i, j - i);
            pat.push_back(tok[0] == '?' ? -1 : (int)strtoul(tok.c_str(), nullptr, 16));
        }
        i = j;
    }
    size_t k = 0;
    while (k < pat.size() && pat[k] < 0) ++k;
    if (pat.empty() || k == pat.size()) { HfSet("Enter a signature like: 48 89 5C 24 ? 57 48 83 EC"); g_hfBusy = false; return; }

    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    auto regs = ReadableRegions();
    unsigned count = 0;
    std::string firsts;
    for (const MemRegion& r : regs) {
        if (r.n < pat.size()) continue;
        size_t pos0 = 0, last = r.n - pat.size();
        while (pos0 <= last) {
            const void* f = memchr(r.p + pos0 + k, pat[k], last - pos0 + 1);
            if (!f) break;
            size_t pos = (size_t)((const uint8_t*)f - (r.p + k));
            bool ok = true;
            for (size_t j = 0; j < pat.size(); ++j)
                if (pat[j] >= 0 && r.p[pos + j] != (uint8_t)pat[j]) { ok = false; break; }
            if (ok) {
                if (++count <= 8) {
                    char t[40]; snprintf(t, sizeof t, "  RVA 0x%llX\n", (unsigned long long)(r.p + pos - base));
                    firsts += t;
                }
            }
            pos0 = pos + 1;
        }
    }
    char head[160];
    snprintf(head, sizeof head, "Signature found %u time(s). %s\n", count,
             count == 1 ? "Exactly one match: usable." : (count == 0 ? "No match." : "More than one match: make it longer."));
    std::string out = head + firsts;
    Log("%s", out.c_str());
    HfSet(out);
    g_hfBusy = false;
}

// Big Ombryn watermark + dimmed screen while the menu is open.
static void DrawMenuWatermark() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, 80));
    ImU32 col = ModOn(M_CHROMA_UI) ? ChromaCol(0.3f) : IM_COL32(90, 200, 255, 255);
    const float scale = 1.8f;
    ImVec2 m = DrawLogo(dl, ImVec2(0, 0), col, scale, false);
    DrawLogo(dl, ImVec2(ds.x - m.x - 28.0f, ds.y - m.y - 58.0f), col, scale, true);
    const char* sub = "Client  |  Insert to close";
    ImVec2 ss = ImGui::CalcTextSize(sub);
    dl->AddText(ImVec2(ds.x - ss.x - 28.0f, ds.y - ss.y - 28.0f), IM_COL32(255, 255, 255, 210), sub);
}

static void DrawUI() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_menuOpen;

    // (motion blur is drawn as its own layer in RenderOverlayDx11, underneath everything here)

    // chroma accents for the menu
    static bool wasChroma = false;
    bool chroma = ModOn(M_CHROMA_UI);
    if (chroma) {
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(ChromaCol(0));
        ImVec4 d(a.x * 0.6f, a.y * 0.6f, a.z * 0.6f, 1.0f);
        ImVec4* c = ImGui::GetStyle().Colors;
        c[ImGuiCol_CheckMark] = a; c[ImGuiCol_SliderGrab] = a;
        c[ImGuiCol_TabActive] = d; c[ImGuiCol_Button] = d; c[ImGuiCol_TitleBgActive] = d;
    } else if (wasChroma) {
        StyleUI();
    }
    wasChroma = chroma;

    if (g_cfg.hud) DrawModuleList();
    DrawRightHud();
    if (ModOn(M_KEYSTROKES)) DrawKeystrokes();
    if (!g_menuOpen) return;
    DrawMenuWatermark();
    ClipCursor(nullptr);

    ImGui::SetNextWindowSize(ImVec2(540, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Ombryn", nullptr, ImGuiWindowFlags_NoCollapse)) {
        {
            float lh = DrawLogo(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(),
                                ModOn(M_CHROMA_UI) ? ChromaCol(0) : IM_COL32(90, 200, 255, 255)).y;
            ImGui::Dummy(ImVec2(0, lh + 4.0f));
        }
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Modules")) {
                ImGui::BeginChild("mods", ImVec2(0, 0), false);
                static const char* cats[] = {"Performance", "Combat", "HUD", "Visuals", "Utilities"};
                for (const char* cat : cats) {
                    if (!ImGui::CollapsingHeader(cat, ImGuiTreeNodeFlags_DefaultOpen)) continue;
                    for (int i = 0; i < M_COUNT; ++i) {
                        Module& m = g_modules[i];
                        if (strcmp(m.cat, cat) != 0) continue;
                        bool on = m.enabled;
                        if (ImGui::Checkbox(m.name, &on)) m.enabled = on;
                        if (m.needsHook && !m.hooked) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "(needs hook)");
                        }
                        if (i == M_MOTION_BLUR && (g_api == 12 || g_blurFailed)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                                               g_api == 12 ? "(DX11 only)" : "(unsupported backbuffer)");
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                ImGui::BeginChild("settings", ImVec2(0, 0), false);
                ImGui::SeparatorText("Culling");
                ImGui::SliderFloat("Entity max distance", &g_cfg.entityMaxDistance, 16.0f, 256.0f, "%.0f");
                ImGui::SliderFloat("Always show within", &g_cfg.entityAlwaysShow, 0.0f, 20.0f, "%.0f");
                ImGui::SliderInt("Line-of-sight steps", &g_cfg.losMaxSteps, 16, 256);
                ImGui::SliderInt("Surface margin", &g_cfg.surfaceMargin, 0, 10);
                ImGui::SliderFloat("Particle max distance", &g_cfg.particleMaxDistance, 4.0f, 64.0f, "%.0f");
                ImGui::Text("Entities culled: %u   Sections culled: %u", g_culledEntities.load(), g_culledSections.load());
                ImGui::SeparatorText("Performance");
                ImGui::SliderInt("FPS limit (0 = unlimited)", &g_cfg.fpsLimit, 0, 1000);
                ImGui::TextDisabled("Turn on the FPS Unlocker module for this to apply.");
                ImGui::SeparatorText("Visuals");
                ImGui::SliderFloat("Motion blur strength", &g_cfg.blurStrength, 0.05f, 0.9f, "%.2f");
                ImGui::SliderFloat("Chroma speed", &g_cfg.chromaSpeed, 0.1f, 3.0f, "%.1f");
                ImGui::SliderFloat("Zoom FOV", &g_cfg.zoomFov, 10.0f, 70.0f, "%.0f");
                ImGui::SliderFloat("Particle multiplier", &g_cfg.particleMult, 1.0f, 8.0f, "%.1f");
                ImGui::ColorEdit3("Hit color", g_cfg.hitColor);
                ImGui::ColorEdit3("Glint color", g_cfg.glintColor);
                ImGui::Combo("Time of day", &g_cfg.timeOfDay, "Day\0Sunset\0Night\0");
                ImGui::InputText("Hurt sound (.wav path)", g_cfg.hurtSound, sizeof g_cfg.hurtSound);
                ImGui::SeparatorText("HUD");
                ImGui::Checkbox("Show module list", &g_cfg.hud);
                ImGui::SliderFloat("Keystrokes X", &g_cfg.keysPosX, 0.0f, 0.95f, "%.2f");
                ImGui::SliderFloat("Keystrokes Y", &g_cfg.keysPosY, 0.0f, 0.90f, "%.2f");
                ImGui::SliderFloat("Keystrokes size", &g_cfg.keysScale, 0.6f, 2.0f, "%.1f");
                ImGui::SeparatorText("Utilities");
                ImGui::InputText("Auto-GG message", g_cfg.autoMsg, sizeof g_cfg.autoMsg);
                ImGui::SeparatorText("Window");
                bool b = g_borderless;
                if (ImGui::Checkbox("Borderless fullscreen", &b)) SetBorderless(g_hwnd, b);
                bool tp = g_topmost;
                if (ImGui::Checkbox("Always on top", &tp)) SetTopmost(g_hwnd, tp);
                ImGui::Checkbox("Title-bar status", &g_cfg.titleStatus);
                ImGui::Checkbox("Beep on hotkey toggle", &g_cfg.beepOnToggle);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hook Finder")) {
                static std::string build = GameBuildInfo();
                ImGui::TextWrapped("%s", build.c_str());
                ImGui::Separator();
                static char text[128] = "gfx_field_of_view";
                static bool utf16 = false;
                ImGui::InputText("Text in the game", text, sizeof text);
                ImGui::Checkbox("UTF-16 text", &utf16);
                ImGui::BeginDisabled(g_hfBusy.load());
                if (ImGui::Button("Find code that uses this text")) {
                    g_hfBusy = true; HfSet("Searching... (can take a few seconds)");
                    std::string t = text; bool u = utf16;
                    std::thread([t, u]() { FindRefsJob(t, u); }).detach();
                }
                static char sigBuf[256] = "";
                ImGui::InputText("Signature", sigBuf, sizeof sigBuf);
                if (ImGui::Button("Scan for signature")) {
                    g_hfBusy = true; HfSet("Scanning...");
                    std::string sg = sigBuf;
                    std::thread([sg]() { SigTestJob(sg); }).detach();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Copy result")) ImGui::SetClipboardText(HfGet().c_str());
                static char view[16384];
                strncpy(view, HfGet().c_str(), sizeof view - 1);
                view[sizeof view - 1] = 0;
                ImGui::InputTextMultiline("##hfout", view, sizeof view, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Profiles")) {
                static char nameBuf[32] = "default";
                static std::vector<std::string> profs;
                static bool refresh = true;
                if (refresh) { profs = ListProfiles(); refresh = false; }
                ImGui::TextWrapped("A profile saves every module toggle and setting. Load one with a click.");
                ImGui::InputText("Name", nameBuf, sizeof nameBuf);
                if (ImGui::Button("Save profile")) { SaveProfile(nameBuf); refresh = true; }
                ImGui::SameLine();
                if (ImGui::Button("Refresh list")) refresh = true;
                ImGui::Separator();
                if (profs.empty()) ImGui::TextDisabled("No profiles yet.");
                for (const std::string& p : profs) {
                    std::string label = "Load: " + p;
                    if (ImGui::Button(label.c_str())) LoadProfile(p);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Config")) {
                if (ImGui::Button("Save to ombryn.ini")) SaveConfig();
                ImGui::SameLine();
                if (ImGui::Button("Reload from file")) LoadConfig();
                ImGui::Spacing();
                if (ImGui::Button("Eject client")) g_running = false;
                ImGui::TextWrapped("Hotkeys (change them in ombryn.ini):\n"
                                   "Insert: menu\n"
                                   "F6/F7: entity/underground culling\n"
                                   "F8: reload  F9: borderless  F10: on top\n"
                                   "PageUp/PageDown: toggle sprint/sneak\n"
                                   "C (hold): zoom   End: eject");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

static void SetupImGuiCommon() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    char winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryA(winDir, MAX_PATH)) {
        std::string fp = std::string(winDir) + "\\Fonts\\segoeuib.ttf";
        if (GetFileAttributesA(fp.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ImFont* base = io.Fonts->AddFontFromFileTTF(fp.c_str(), 17.0f);
            g_logoFont = io.Fonts->AddFontFromFileTTF(fp.c_str(), 38.0f);
            if (base) io.FontDefault = base;
        }
    }
    StyleUI();
}
static void HookWndProc() {
    if (g_hwnd) g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
}
static void OnUiReady(int api) {
    Log("UI ready on DirectX %d, hwnd %p", api, (void*)g_hwnd);
    Beep(1000, 80); Beep(1400, 80);   // two beeps = the menu is hooked
}

// ---- DirectX 11 ----
static void RenderOverlayDx11(IDXGISwapChain* sc) {
    if (!g_imguiInit) {
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        g_hwnd = d.OutputWindow ? d.OutputWindow : FindGameWindow();
        if (!g_hwnd) { g_uiStatus = 3; Log("no window handle"); ShowMsg("Ombryn: couldn't find the game window."); return; }
        SetupImGuiCommon();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_dev, g_ctx);
        HookWndProc();
        g_imguiInit = true;
        g_uiStatus = 1;
        OnUiReady(11);
    }
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (!bb) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
    if (!rtv) { bb->Release(); return; }

    bool blur = ModOn(M_MOTION_BLUR) && !g_blurFailed;
    if (blur) EnsurePrev(bb);
    else if (g_prevTex) ReleasePrev();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);

    // Motion blur. Layer 1: last frame's snapshot blended over the new game frame, drawn alone.
    // Then the snapshot for the NEXT frame is taken, before the HUD/menu go on top. (Copying
    // after the HUD, as before, fed the HUD back into the blur: FPS/CPS digits and keystroke
    // boxes smeared and the HUD looked doubled.)  Still one ImGui frame per Present.
    if (blur && g_prevSrv && g_prevValid) {
        if (!g_blurList) g_blurList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
        int a = (int)(std::fmax(0.0f, std::fmin(0.9f, g_cfg.blurStrength)) * 255.0f);
        g_blurList->_ResetForNewFrame();
        g_blurList->PushClipRectFullScreen();
        g_blurList->AddImage((ImTextureID)(uintptr_t)g_prevSrv, ImVec2(0, 0), dd->DisplaySize,
                             ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
        ImDrawData layer;
        layer.Valid = true;
        layer.DisplayPos = dd->DisplayPos; layer.DisplaySize = dd->DisplaySize;
        layer.FramebufferScale = dd->FramebufferScale; layer.OwnerViewport = dd->OwnerViewport;
        layer.AddDrawList(g_blurList);
        ImGui_ImplDX11_RenderDrawData(&layer);
    }
    if (blur && g_prevTex) { g_ctx->CopyResource(g_prevTex, bb); g_prevValid = true; }
    ImGui_ImplDX11_RenderDrawData(dd);  // HUD + menu
    rtv->Release();
    bb->Release();
}

// ---- DirectX 12 ----
static DXGI_FORMAT ConcreteFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
    }
}
static bool InitDx12(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    g_hwnd = d.OutputWindow ? d.OutputWindow : FindGameWindow();
    if (!g_hwnd) { Log("DX12: no window handle"); return false; }
    UINT n = d.BufferCount < 2 ? 2 : d.BufferCount;
    g_rtFmt12 = ConcreteFormat(d.BufferDesc.Format);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = n; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap12)))) return false;
    g_rtvInc12 = g_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap12)))) return false;

    g_frames12.assign(n, Frame12());
    for (Frame12& f : g_frames12)
        if (FAILED(g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
    if (FAILED(g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames12[0].alloc, nullptr,
                                          IID_PPV_ARGS(&g_cmd12)))) return false;
    g_cmd12->Close();
    if (FAILED(g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence12)))) return false;
    g_fenceEvent12 = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    SetupImGuiCommon();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_Init(g_dev12, (int)n, g_rtFmt12, g_srvHeap12,
                        g_srvHeap12->GetCPUDescriptorHandleForHeapStart(),
                        g_srvHeap12->GetGPUDescriptorHandleForHeapStart());
    HookWndProc();
    return true;
}
static void RenderOverlayDx12(IDXGISwapChain* sc) {
    if (!g_queue12) return;  // waiting to see the game's command queue
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) return;
    if (!g_dx12Init) {
        if (!InitDx12(sc)) {
            g_uiStatus = 3; g_api = -1;
            Log("DX12 init failed");
            ShowMsg("Ombryn: DirectX 12 setup failed. See ombryn.log.");
            sc3->Release();
            return;
        }
        g_dx12Init = true; g_imguiInit = true; g_uiStatus = 1;
        OnUiReady(12);
    }
    UINT idx = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (idx >= g_frames12.size() || FAILED(sc3->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&bb)) || !bb) {
        sc3->Release();
        return;
    }
    Frame12& fc = g_frames12[idx];
    if (g_fence12->GetCompletedValue() < fc.fence) {
        g_fence12->SetEventOnCompletion(fc.fence, g_fenceEvent12);
        WaitForSingleObject(g_fenceEvent12, 1000);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap12->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g_rtvInc12;
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = g_rtFmt12; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_dev12->CreateRenderTargetView(bb, &rd, rtv);

    fc.alloc->Reset();
    g_cmd12->Reset(fc.alloc, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = bb;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmd12->ResourceBarrier(1, &bar);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    g_cmd12->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_srvHeap12};
    g_cmd12->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd12);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd12->ResourceBarrier(1, &bar);
    g_cmd12->Close();
    ID3D12CommandList* lists[] = {g_cmd12};
    g_queue12->ExecuteCommandLists(1, lists);
    g_queue12->Signal(g_fence12, ++g_fenceVal12);
    fc.fence = g_fenceVal12;
    bb->Release();
    sc3->Release();
}

// ---- picks DX11 or DX12 the first time a frame is presented ----
static void RenderOverlay(IDXGISwapChain* sc) {
    if (g_api == -1 || g_uiStatus == 3) return;
    if (g_api == 0) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&g_dev12)) && g_dev12) {
            g_api = 12;
        } else if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) && g_dev) {
            g_dev->GetImmediateContext(&g_ctx);
            g_api = 11;
        } else {
            g_api = -1; g_uiStatus = 2;
            Log("the game's swapchain is neither DX11 nor DX12");
            ShowMsg("Ombryn: this game isn't using DirectX 11 or 12, so the menu can't draw.");
            return;
        }
        Log("graphics API detected: DirectX %d", g_api);
    }
    if (g_api == 12) RenderOverlayDx12(sc); else RenderOverlayDx11(sc);
}

// Present hooks (vtable index 8 = Present, 22 = Present1). Not version-specific.
using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PresentFn  g_origPresent  = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static thread_local int t_presentDepth = 0;

// FPS unlocker: turns VSync off when the game presents a frame, and optionally caps FPS itself.
static void LimitFps() {
    if (g_cfg.fpsLimit <= 0) return;
    static LARGE_INTEGER freq = {}, last = {};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); return; }
    const double target = 1.0 / (double)g_cfg.fpsLimit;
    for (;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        if (dt >= target) { last = now; break; }
        if (target - dt > 0.002) Sleep(1); else YieldProcessor();
    }
}
static void PrepPresent(IDXGISwapChain* sc, UINT& syncInterval, UINT& flags) {
    if (!ModOn(M_FPS_UNLOCK)) return;
    LimitFps();
    if (flags & DXGI_PRESENT_TEST) return;
    syncInterval = 0;  // no VSync
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d)) && (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) && d.Windowed)
        flags |= DXGI_PRESENT_ALLOW_TEARING;  // lets frames go past the monitor's refresh rate
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT si, UINT fl) {
    if (t_presentDepth == 0) { ++g_frames; RenderOverlay(sc); PrepPresent(sc, si, fl); }
    ++t_presentDepth;
    HRESULT hr = g_origPresent(sc, si, fl);
    --t_presentDepth;
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_presentDepth == 0) { ++g_frames; RenderOverlay(sc); PrepPresent(sc, si, fl); }
    ++t_presentDepth;
    HRESULT hr = g_origPresent1(sc, si, fl, p);
    --t_presentDepth;
    return hr;
}

// Second set, used only if the DX12 dummy swapchain has different Present addresses.
static PresentFn  g_origPresentB  = nullptr;
static Present1Fn g_origPresent1B = nullptr;
static HRESULT STDMETHODCALLTYPE HookedPresentB(IDXGISwapChain* sc, UINT si, UINT fl) {
    if (t_presentDepth == 0) { ++g_frames; RenderOverlay(sc); PrepPresent(sc, si, fl); }
    ++t_presentDepth;
    HRESULT hr = g_origPresentB(sc, si, fl);
    --t_presentDepth;
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookedPresent1B(IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_presentDepth == 0) { ++g_frames; RenderOverlay(sc); PrepPresent(sc, si, fl); }
    ++t_presentDepth;
    HRESULT hr = g_origPresent1B(sc, si, fl, p);
    --t_presentDepth;
    return hr;
}
// Catches the game's DX12 command queue so we can draw on it.
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static ExecFn g_origExec = nullptr;
static void STDMETHODCALLTYPE HookedExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!g_queue12 && q) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { g_queue12 = q; q->AddRef(); }
    }
    g_origExec(q, n, lists);
}

// Build a throwaway swapchain just to read the Present addresses from its vtable.
static bool GetPresentAddrs(void** present, void** present1) {
    WNDCLASSEXA wc{}; wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ombryn_dummy";
    RegisterClassExA(&wc);
    HWND hw = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) {
        Log("DX11 probe: CreateWindowExA failed with Win32 error %lu", GetLastError());
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100; sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = SUCCEEDED(hr) && sc;
    Log("DX11 probe: D3D11CreateDeviceAndSwapChain hr=0x%08X swapchain=%p", (unsigned)hr, sc);
    if (ok) {
        void** vt = *(void***)sc;
        *present = vt[8];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            *present1 = (*(void***)sc1)[22];
            sc1->Release();
        }
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hw);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

// =====================================================================
// Game hooks (YOU fill these in, per game version)
// =====================================================================
// Signature scanner: pattern like "48 8B ? ? 89", '?' = wildcard.
// Scans only the module's EXECUTABLE pages that are really readable (the old version walked
// the whole image blindly and could fault on a NOACCESS page), and only returns a match that
// is UNIQUE: hooking the first of several matches would hook the wrong function.
static std::vector<int> ParsePattern(const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { bytes.push_back(-1); while (*p == '?') ++p; }
        else {
            char* end = nullptr;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p || v > 0xFF) return {};   // not a hex byte: reject the whole pattern
            bytes.push_back((int)v);
            p = end;
        }
    }
    return bytes;
}
// Counts matches of pat (-1 = wildcard) inside [p, p+n); remembers the first one.
static void ScanBuffer(const uint8_t* p, size_t n, const std::vector<int>& pat,
                       const uint8_t** first, unsigned* hits) {
    size_t k = 0;
    while (k < pat.size() && pat[k] < 0) ++k;
    if (k == pat.size() || n < pat.size()) return;   // empty / all wildcards / too short
    size_t last = n - pat.size(), pos0 = 0;
    while (pos0 <= last) {
        const void* f = memchr(p + pos0 + k, pat[k], last - pos0 + 1);
        if (!f) break;
        size_t pos = (size_t)((const uint8_t*)f - (p + k));
        bool ok = true;
        for (size_t j = 0; j < pat.size(); ++j)
            if (pat[j] >= 0 && p[pos + j] != (uint8_t)pat[j]) { ok = false; break; }
        if (ok) { if (!*first) *first = p + pos; ++*hits; }
        pos0 = pos + 1;
    }
}
static uint8_t* FindPattern(HMODULE mod, const char* pattern) {
    std::vector<int> pat = ParsePattern(pattern);
    if (pat.empty()) { Log("FindPattern: bad pattern \"%s\"", pattern); return nullptr; }
    const uint8_t* first = nullptr;
    unsigned hits = 0;
    for (const MemRegion& r : ReadableRegions(mod))
        if (r.exec) ScanBuffer(r.p, r.n, pat, &first, &hits);
    if (hits != 1) {
        Log("FindPattern: \"%s\" matched %u times (need exactly 1)", pattern, hits);
        return nullptr;
    }
    return (uint8_t*)first;
}
// Finds a function by signature and hooks it. The module flips to active the first
// time your detour calls its API function above (e.g. ShouldCullEntity).
static bool InstallHook(const char* pattern, void* detour, void** original) {
    uint8_t* target = FindPattern(GetModuleHandleA(nullptr), pattern);
    if (!target) return false;
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) { Log("InstallHook: MH_CreateHook %p -> %s", (void*)target, MH_StatusToString(st)); return false; }
    st = MH_EnableHook(target);
    Log("InstallHook: %p -> %s", (void*)target, MH_StatusToString(st));
    return st == MH_OK;
}

// Example detour (copy the real signature from the function you found):
//
//   using RenderEntityFn = void (*)(void* renderer, void* entity, void* ctx);
//   static RenderEntityFn g_origRenderEntity = nullptr;
//   void HookedRenderEntity(void* renderer, void* entity, void* ctx) {
//       Vec3 cam = /* read camera pos */;
//       Vec3 pos = /* read entity pos  */;
//       if (ShouldCullEntity(cam, pos)) return;
//       g_origRenderEntity(renderer, entity, ctx);
//   }
// ...and in InitHooks():
//   InstallHook("YOUR ? SIGNATURE ? HERE", (void*)&HookedRenderEntity, (void**)&g_origRenderEntity);
// Also set g_isSolid and g_surfaceY to functions that read the game's block data.

// Same trick for DirectX 12: dummy device + queue + swapchain, read the vtable addresses.
static bool GetDx12Addrs(void** present, void** present1, void** exec) {
    ID3D12Device* dev = nullptr;
    HRESULT deviceHr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(deviceHr) || !dev) {
        Log("DX12 probe: D3D12CreateDevice hr=0x%08X", (unsigned)deviceHr);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    IDXGIFactory1* fac = nullptr;
    IDXGISwapChain* sc = nullptr;
    bool ok = false;
    WNDCLASSEXA wc{}; wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ombryn_dummy12";
    RegisterClassExA(&wc);
    HWND hw = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) {
        Log("DX12 probe: CreateWindowExA failed with Win32 error %lu", GetLastError());
    }
    HRESULT queueHr = hw ? dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)) : E_FAIL;
    HRESULT factoryHr = (SUCCEEDED(queueHr) && q) ? CreateDXGIFactory1(IID_PPV_ARGS(&fac)) : E_FAIL;
    if (FAILED(queueHr) || !q) Log("DX12 probe: CreateCommandQueue hr=0x%08X", (unsigned)queueHr);
    if (FAILED(factoryHr) || !fac) Log("DX12 probe: CreateDXGIFactory1 hr=0x%08X", (unsigned)factoryHr);
    if (SUCCEEDED(queueHr) && q && SUCCEEDED(factoryHr) && fac) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 100; sd.BufferDesc.Height = 100;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hw;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT swapHr = fac->CreateSwapChain(q, &sd, &sc);
        if (FAILED(swapHr) || !sc) Log("DX12 probe: CreateSwapChain hr=0x%08X", (unsigned)swapHr);
        if (SUCCEEDED(swapHr) && sc) {
            *present = (*(void***)sc)[8];
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
                *present1 = (*(void***)sc1)[22];
                sc1->Release();
            }
            *exec = (*(void***)q)[10];  // ID3D12CommandQueue::ExecuteCommandLists
            ok = true;
        }
    }
    if (sc) sc->Release();
    if (fac) fac->Release();
    if (q) q->Release();
    dev->Release();
    DestroyWindow(hw);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

static bool InitHooks() {
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return false; }
    void *pr = nullptr, *pr1 = nullptr, *pr12 = nullptr, *pr112 = nullptr, *ex12 = nullptr;
    bool ok11 = GetPresentAddrs(&pr, &pr1);
    bool ok12 = GetDx12Addrs(&pr12, &pr112, &ex12);
    Log("dummy swapchains: dx11=%d (%p %p)  dx12=%d (%p %p exec %p)", ok11, pr, pr1, ok12, pr12, pr112, ex12);
    int presentHooks = 0;
    auto hook = [&presentHooks](const char* name, void* target, void* detour, void** original) {
        if (!target) { Log("hook %s unavailable (no target address)", name); return false; }
        MH_STATUS st = MH_CreateHook(target, detour, original);
        Log("hook %s at %p -> %s", name, target, MH_StatusToString(st));
        if (st == MH_OK && (strstr(name, "Present") != nullptr)) ++presentHooks;
        return st == MH_OK;
    };
    hook("DX11 Present", pr, (void*)&HookedPresent, (void**)&g_origPresent);
    hook("DX11 Present1", pr1, (void*)&HookedPresent1, (void**)&g_origPresent1);
    if (pr12 && pr12 != pr) hook("DX12 Present", pr12, (void*)&HookedPresentB, (void**)&g_origPresentB);
    if (pr112 && pr112 != pr1) hook("DX12 Present1", pr112, (void*)&HookedPresent1B, (void**)&g_origPresent1B);
    hook("DX12 ExecuteCommandLists", ex12, (void*)&HookedExec, (void**)&g_origExec);
    if (presentHooks == 0) {
        g_uiStatus = 3;
        Log("hook setup failed: no DXGI Present entry point could be hooked");
        MH_Uninitialize();
        return false;
    }
    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    Log("MH_EnableHook(all) -> %s", MH_StatusToString(en));
    if (en != MH_OK) {
        g_uiStatus = 3;
        Log("hook setup failed while enabling DXGI hooks");
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return false;
    }
    // --- your game hooks go here (see the example above) ---
    (void)FindPattern; (void)InstallHook;
    return true;
}

// =====================================================================
// Worker thread: hotkeys, toggle keys, title status, eject
// =====================================================================
static bool EdgePressed(int vk, bool& prev, bool fg) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !prev;
    prev = down;
    return pressed && fg;
}
static void FlipModule(ModId id) {
    bool now = !g_modules[id].enabled.load();
    g_modules[id].enabled = now;
    Feedback(now);
}

static DWORD WINAPI Worker(LPVOID mod) {
    Sleep(2000);  // let the game finish loading
    Log("worker started");
    timeBeginPeriod(1);  // accurate Sleep(1) for the FPS limiter
    if (!InitHooks()) {
        ShowMsg("Ombryn: hook setup failed. See ombryn.log.");
        g_running = false;
    } else {
        Beep(880, 120);  // one beep = at least one Present hook is enabled
    }
    ULONGLONG startTick = GetTickCount64();
    bool warned = false;

    HWND game = nullptr;
    std::wstring baseTitle, lastTitle;
    bool pE = false, pU = false, pR = false, pB = false, pT = false, pX = false, pM = false, pS = false, pN = false;
    DWORD lastStatus = GetTickCount(), lastRefresh = 0;
    uint32_t lastEnt = 0;

    while (g_running) {
        DWORD now = GetTickCount();
        if (!game || !IsWindow(game) || now - lastRefresh > 2000) {
            HWND found = FindGameWindow();
            if (found && found != game) {
                game = found;
                wchar_t t[256] = {};
                GetWindowTextW(game, t, 256);
                baseTitle = t;
            }
            lastRefresh = now;
        }
        bool fg = IsGameFocused();
        SampleClicks(fg);
        UpdateHeldKeys(fg && !g_menuOpen);

        if (EdgePressed(g_cfg.keyMenu, pM, fg)) g_menuOpen = !g_menuOpen;
        if (EdgePressed(g_cfg.keyEntity, pE, fg)) FlipModule(M_ENTITY_CULL);
        if (EdgePressed(g_cfg.keyUnderground, pU, fg)) FlipModule(M_UNDERGROUND_CULL);
        if (EdgePressed(g_cfg.keyToggleSprint, pS, fg)) FlipModule(M_TOGGLE_SPRINT);
        if (EdgePressed(g_cfg.keyToggleSneak, pN, fg)) FlipModule(M_TOGGLE_SNEAK);
        if (EdgePressed(g_cfg.keyReload, pR, fg)) { LoadConfig(); Feedback(true); }
        if (EdgePressed(g_cfg.keyBorderless, pB, fg) && game) { SetBorderless(game, !g_borderless); Feedback(g_borderless); }
        if (EdgePressed(g_cfg.keyTopmost, pT, fg) && game) { SetTopmost(game, !g_topmost); Feedback(g_topmost); }
        if (EdgePressed(g_cfg.keyEject, pX, fg)) { Feedback(false); g_running = false; }

        if (g_cfg.titleStatus && game && now - lastStatus >= 500) {
            uint32_t ent = g_culledEntities.load();
            uint32_t perSec = (ent - lastEnt) * 1000u / (now - lastStatus);
            lastEnt = ent;
            lastStatus = now;
            wchar_t buf[320];
            swprintf(buf, 320, L"%ls | Cull E:%ls U:%ls | %u culled/s%ls",
                     baseTitle.c_str(), g_entityCulling ? L"on" : L"off",
                     g_undergroundCulling ? L"on" : L"off", perSec,
                     g_uiStatus == 2 ? L" | UI: needs DX11" : (g_uiStatus == 3 ? L" | UI hook failed" : L""));
            if (lastTitle != buf) { SetWindowTextW(game, buf); lastTitle = buf; }
        }
        if (!warned && GetTickCount64() - startTick > 20000) {
            if (g_frames == 0) {
                warned = true;
                Log("watchdog: Present was never called");
                ShowMsg("Ombryn is loaded, but the game's graphics never called our hook, so no menu can draw.");
            } else if (g_api == 12 && !g_queue12) {
                warned = true;
                Log("watchdog: DX12 command queue never captured");
                ShowMsg("Ombryn: DirectX 12 detected but the command queue wasn't captured.");
            }
        }
        Sleep(5);
    }

    // ---- cleanup / eject ----
    UpdateHeldKeys(false);  // release sprint/sneak if we were holding them
    if (game && IsWindow(game)) {
        if (g_borderless) SetBorderless(game, false);
        if (g_topmost) SetTopmost(game, false);
        if (!baseTitle.empty()) SetWindowTextW(game, baseTitle.c_str());
    }
    g_menuOpen = false;
    MH_DisableHook(MH_ALL_HOOKS);
    if (g_imguiInit && g_hwnd && IsWindow(g_hwnd) &&
        GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) == (LONG_PTR)HookedWndProc)
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    Sleep(300);  // let in-flight hooked calls finish
    for (int w = 0; w < 50 && g_hfBusy.load(); ++w) Sleep(100);  // let a running Hook Finder scan end
    ReleasePrev();
    if (g_imguiInit) {
        if (g_api == 12) {
            if (g_queue12 && g_fence12) {  // wait for the GPU to finish our last frame
                g_queue12->Signal(g_fence12, ++g_fenceVal12);
                if (g_fence12->GetCompletedValue() < g_fenceVal12) {
                    g_fence12->SetEventOnCompletion(g_fenceVal12, g_fenceEvent12);
                    WaitForSingleObject(g_fenceEvent12, 2000);
                }
            }
            ImGui_ImplDX12_Shutdown();
        } else {
            ImGui_ImplDX11_Shutdown();
        }
        ImGui_ImplWin32_Shutdown();
        if (g_blurList) { IM_DELETE(g_blurList); g_blurList = nullptr; }
        ImGui::DestroyContext();
        g_imguiInit = false;
    }
    for (Frame12& f : g_frames12) if (f.alloc) f.alloc->Release();
    g_frames12.clear();
    if (g_cmd12) { g_cmd12->Release(); g_cmd12 = nullptr; }
    if (g_fence12) { g_fence12->Release(); g_fence12 = nullptr; }
    if (g_rtvHeap12) { g_rtvHeap12->Release(); g_rtvHeap12 = nullptr; }
    if (g_srvHeap12) { g_srvHeap12->Release(); g_srvHeap12 = nullptr; }
    if (g_fenceEvent12) { CloseHandle(g_fenceEvent12); g_fenceEvent12 = nullptr; }
    if (g_queue12) { g_queue12->Release(); g_queue12 = nullptr; }
    if (g_dev12) { g_dev12->Release(); g_dev12 = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    timeEndPeriod(1);
    MH_Uninitialize();
    FreeLibraryAndExitThread((HMODULE)mod, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        g_iniPath = DirOf(path) + "ombryn.ini";
        g_logPath = DirOf(path) + "ombryn.log";
        Log("---- Ombryn v5 attached ----");
        InitModules();
        if (GetFileAttributesA(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            FILE* f = fopen(g_iniPath.c_str(), "wb");
            if (f) { fputs(kDefaultIni, f); fclose(f); }
        }
        LoadConfig();
        CreateThread(nullptr, 0, Worker, mod, 0, nullptr);
    }
    return TRUE;
}

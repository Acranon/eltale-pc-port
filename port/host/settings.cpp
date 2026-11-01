// The data folder and settings.json (host.h). settings.json is versioned: version 2 (R0) has the
// picture's own switches and a preset; version 1 (no "version", eltale_rt64's menu) kept RT64's
// UserConfiguration under "graphics", which is read once into the switches it maps to.
#include "host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cJSON.h"

namespace host {

Settings settings;

const char *const preset_names[PRESET_COUNT] = { "Classic", "Classic + visuals", "Everything on", "Custom" };
static const char *const preset_keys[PRESET_COUNT] = { "classic", "classic_visuals", "everything", "custom" };

bool Graphics::operator==(const Graphics &o) const {
    return resolution == o.resolution && (resolution != 2 || resolution_scale == o.resolution_scale) &&
           msaa == o.msaa && frame_rate == o.frame_rate && (frame_rate != 2 || frame_rate_target == o.frame_rate_target) &&
           three_point == o.three_point && scaling == o.scaling && vsync == o.vsync && widescreen == o.widescreen &&
           border == o.border && draw_distance == o.draw_distance && n64_fog == o.n64_fog &&
           n64_dither == o.n64_dither && shader_warmup == o.shader_warmup;
}

// The owner's three presets (docs/renderer/PLAN.md, decision 3)
Graphics preset_graphics(int preset) {
    Graphics g; // Everything on: 240 fps, widescreen, the lot
    if (preset == PRESET_CLASSIC) {
        // like the N64: 30 fps, 4:3, the border, three-point filtering, N64 fog and dither
        g.resolution = 0;
        g.msaa = 1;
        g.frame_rate = 0;
        g.three_point = true;
        g.scaling = 0;
        g.widescreen = 0;
        g.border = true;
        g.draw_distance = 0;
        g.n64_fog = true;
        g.n64_dither = true;
    } else if (preset == PRESET_CLASSIC_VISUALS) {
        // a better picture (resolution, MSAA, filtering, draw distance), the game's look and timing kept
        g.resolution = 1;
        g.msaa = 4;
        g.frame_rate = 0;
        g.three_point = true;
        g.scaling = 2;
        g.widescreen = 0;
        g.border = false;
        g.draw_distance = 1;
        g.n64_fog = true;
        g.n64_dither = false;
    }
    return g;
}

int matching_preset(const Graphics &g) {
    for (int p = 0; p < PRESET_CUSTOM; p++)
        if (preset_graphics(p) == g) return p;
    return PRESET_CUSTOM;
}

const std::string &data_dir() {
    static std::string dir;
    if (dir.empty()) {
        namespace fs = std::filesystem;
        fs::path path;
        const char *env = std::getenv("ELTALE_DATA_DIR");
#ifdef _WIN32
        const char *base = std::getenv("APPDATA");
        if (env && *env) path = fs::u8path(env);
        else if (base && *base) path = fs::u8path(base) / "eltale";
        else path = "eltale-data";
#else
        const char *xdg = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
        if (env && *env) path = env;
        else if (xdg && *xdg) path = fs::path(xdg) / "eltale";
        else if (home && *home) path = fs::path(home) / ".local/share/eltale";
        else path = "eltale-data";
#endif
        std::error_code ec;
        fs::create_directories(path, ec);
        dir = path.u8string();
    }
    return dir;
}

static std::filesystem::path settings_path() { return std::filesystem::u8path(data_dir()) / "settings.json"; }

// ---- Reading ------------------------------------------------------------------------------------
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const cJSON *item(const cJSON *j, const char *a, const char *b = nullptr) {
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, a);
    return b && x ? cJSON_GetObjectItemCaseSensitive(x, b) : x;
}
static void read_int(const cJSON *x, int &v, int lo, int hi) {
    if (cJSON_IsNumber(x)) v = clampi((int)x->valuedouble, lo, hi);
}
static void read_bool(const cJSON *x, bool &v) {
    if (cJSON_IsBool(x)) v = cJSON_IsTrue(x);
}
// a string from a list: its index
static void read_choice(const cJSON *x, int &v, std::initializer_list<const char *> names) {
    if (!cJSON_IsString(x)) return;
    int i = 0;
    for (const char *n : names) {
        if (!std::strcmp(x->valuestring, n)) { v = i; return; }
        i++;
    }
}

static const std::initializer_list<const char *> resolution_keys = { "original", "window", "multiple" };
static const std::initializer_list<const char *> frame_rate_keys = { "original", "display", "fixed" };
static const std::initializer_list<const char *> scaling_keys = { "sharp", "smooth", "smooth_pixel_edges" };
static const std::initializer_list<const char *> distance_keys = { "original", "far", "farther" };
static const std::initializer_list<const char *> widescreen_keys = { "off", "16:9", "window" };

static void read_graphics(const cJSON *j, Graphics &g) {
    read_choice(item(j, "resolution"), g.resolution, resolution_keys);
    read_int(item(j, "resolution_scale"), g.resolution_scale, 1, 8);
    read_int(item(j, "msaa"), g.msaa, 1, 8);
    g.msaa = g.msaa >= 8 ? 8 : g.msaa >= 4 ? 4 : g.msaa >= 2 ? 2 : 1;
    read_choice(item(j, "frame_rate"), g.frame_rate, frame_rate_keys);
    read_int(item(j, "frame_rate_target"), g.frame_rate_target, 30, 240);
    read_bool(item(j, "three_point"), g.three_point);
    read_choice(item(j, "scaling"), g.scaling, scaling_keys);
    read_bool(item(j, "vsync"), g.vsync);
    // the picture's shape: "off", "16:9", "window" (a file from before R7 has true or false: 16:9 or off)
    const cJSON *wide = item(j, "widescreen");
    if (cJSON_IsBool(wide)) g.widescreen = cJSON_IsTrue(wide) ? 1 : 0;
    read_choice(wide, g.widescreen, widescreen_keys);
    read_bool(item(j, "border"), g.border);
    read_choice(item(j, "draw_distance"), g.draw_distance, distance_keys);
    read_bool(item(j, "n64_fog"), g.n64_fog);
    read_bool(item(j, "n64_dither"), g.n64_dither);
    read_bool(item(j, "shader_warmup"), g.shader_warmup);
}

// Version 1's "graphics": RT64's UserConfiguration (its JSON names); the switches it had, the rest
// as Everything on
static void read_rt64_graphics(const cJSON *j, Graphics &g) {
    read_choice(item(j, "resolution"), g.resolution, { "Original", "WindowIntegerScale", "Manual" });
    if (cJSON_IsNumber(item(j, "resolutionMultiplier")))
        g.resolution_scale = clampi((int)(item(j, "resolutionMultiplier")->valuedouble + 0.5), 1, 8);
    int aa = -1;
    read_choice(item(j, "antialiasing"), aa, { "None", "MSAA2X", "MSAA4X", "MSAA8X" });
    if (aa >= 0) g.msaa = aa == 0 ? 1 : 1 << aa;
    read_choice(item(j, "filtering"), g.scaling, { "Nearest", "Linear", "AntiAliasedPixelScaling" });
    read_bool(item(j, "threePointFiltering"), g.three_point);
    read_choice(item(j, "refreshRate"), g.frame_rate, { "Original", "Display", "Manual" });
    read_int(item(j, "refreshRateTarget"), g.frame_rate_target, 30, 240);
}

// "controls": per action (input.cpp's names) a list of three binding names ("" none: keyboard/mouse, keyboard/mouse,
// controller); an action left out keeps its defaults
static void read_controls(const cJSON *j, Controls &c) {
    read_int(item(j, "mouse_sensitivity"), c.mouse_sensitivity, 10, 400);
    read_bool(item(j, "invert_y"), c.invert_y);
    const cJSON *b = item(j, "bindings");
    if (!cJSON_IsObject(b)) return;
    for (int a = 0; a < ACT_COUNT; a++) {
        const cJSON *list = item(b, action_key(a));
        if (!cJSON_IsArray(list)) continue;
        for (int s = 0; s < BIND_SLOTS; s++) {
            const cJSON *x = cJSON_GetArrayItem(list, s);
            c.bind[a][s] = cJSON_IsString(x) ? binding_from_name(x->valuestring) : 0;
        }
    }
}

void load_settings() {
    std::ifstream f(settings_path(), std::ios::binary);
    if (!f) return;
    std::stringstream text;
    text << f.rdbuf();
    cJSON *j = cJSON_Parse(text.str().c_str());
    if (!j) {
        std::fprintf(stderr, "[host] ignoring %s: not JSON\n", settings_path().u8string().c_str());
        return;
    }
    Settings &s = settings;
    read_int(item(j, "audio", "volume"), s.volume, 0, 100);
    read_bool(item(j, "audio", "mute_in_background"), s.mute_in_background);
    read_bool(item(j, "window", "fullscreen"), s.fullscreen);
    const cJSON *lang = item(j, "game", "language");
    if (cJSON_IsString(lang)) s.english = std::strcmp(lang->valuestring, "jp") != 0;
    read_int(item(j, "game", "font"), s.font, 0, 5);
    if (cJSON_IsObject(item(j, "controls"))) read_controls(item(j, "controls"), s.controls);
    int version = 1;
    read_int(item(j, "version"), version, 1, 1000);
    const cJSON *g = item(j, "graphics");
    if (version >= 2 && cJSON_IsObject(g)) {
        s.graphics = preset_graphics(PRESET_EVERYTHING);
        read_graphics(g, s.graphics);
        int preset = PRESET_CUSTOM;
        read_choice(item(g, "preset"), preset, { preset_keys[0], preset_keys[1], preset_keys[2], preset_keys[3] });
        // a preset is only kept while the switches are still its own
        s.preset = preset != PRESET_CUSTOM && preset_graphics(preset) == s.graphics ? preset : matching_preset(s.graphics);
    } else if (cJSON_IsObject(g)) {
        s.graphics = preset_graphics(PRESET_EVERYTHING);
        read_rt64_graphics(g, s.graphics);
        s.preset = matching_preset(s.graphics);
        std::fprintf(stderr, "[host] settings.json: RT64's graphics options taken over (%s)\n", preset_names[s.preset]);
    }
    cJSON_Delete(j);
}

// ---- Writing ------------------------------------------------------------------------------------
static const char *key(std::initializer_list<const char *> names, int i) {
    return names.begin()[clampi(i, 0, (int)names.size() - 1)];
}

void save_settings() {
    const Settings &s = settings;
    const Graphics &g = s.graphics;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "version", 2);
    cJSON *audio = cJSON_AddObjectToObject(j, "audio");
    cJSON_AddNumberToObject(audio, "volume", s.volume);
    cJSON_AddBoolToObject(audio, "mute_in_background", s.mute_in_background);
    cJSON *win = cJSON_AddObjectToObject(j, "window");
    cJSON_AddBoolToObject(win, "fullscreen", s.fullscreen);
    cJSON *game = cJSON_AddObjectToObject(j, "game");
    cJSON_AddStringToObject(game, "language", s.english ? "en" : "jp");
    cJSON_AddNumberToObject(game, "font", s.font);
    cJSON *controls = cJSON_AddObjectToObject(j, "controls");
    cJSON_AddNumberToObject(controls, "mouse_sensitivity", s.controls.mouse_sensitivity);
    cJSON_AddBoolToObject(controls, "invert_y", s.controls.invert_y);
    cJSON *bindings = cJSON_AddObjectToObject(controls, "bindings");
    for (int a = 0; a < ACT_COUNT; a++) {
        cJSON *list = cJSON_AddArrayToObject(bindings, action_key(a));
        for (int b = 0; b < BIND_SLOTS; b++) cJSON_AddItemToArray(list, cJSON_CreateString(binding_name(s.controls.bind[a][b]).c_str()));
    }
    cJSON *gr = cJSON_AddObjectToObject(j, "graphics");
    cJSON_AddStringToObject(gr, "preset", preset_keys[clampi(s.preset, 0, PRESET_CUSTOM)]);
    cJSON_AddStringToObject(gr, "resolution", key(resolution_keys, g.resolution));
    cJSON_AddNumberToObject(gr, "resolution_scale", g.resolution_scale);
    cJSON_AddNumberToObject(gr, "msaa", g.msaa);
    cJSON_AddStringToObject(gr, "frame_rate", key(frame_rate_keys, g.frame_rate));
    cJSON_AddNumberToObject(gr, "frame_rate_target", g.frame_rate_target);
    cJSON_AddBoolToObject(gr, "three_point", g.three_point);
    cJSON_AddStringToObject(gr, "scaling", key(scaling_keys, g.scaling));
    cJSON_AddBoolToObject(gr, "vsync", g.vsync);
    cJSON_AddStringToObject(gr, "widescreen", widescreen_keys.begin()[clampi(g.widescreen, 0, 2)]);
    cJSON_AddBoolToObject(gr, "border", g.border);
    cJSON_AddStringToObject(gr, "draw_distance", key(distance_keys, g.draw_distance));
    cJSON_AddBoolToObject(gr, "n64_fog", g.n64_fog);
    cJSON_AddBoolToObject(gr, "n64_dither", g.n64_dither);
    cJSON_AddBoolToObject(gr, "shader_warmup", g.shader_warmup);
    char *text = cJSON_Print(j);
    std::ofstream f(settings_path(), std::ios::binary);
    if (f && text) f << text << "\n";
    cJSON_free(text);
    cJSON_Delete(j);
}

} // namespace host

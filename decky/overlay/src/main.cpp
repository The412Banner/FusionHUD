// FusionHUD overlay for gamescope sessions (DroidDeck): draws the FusionHUD sizes over games as
// gamescope's untagged external overlay, started and configured by the FusionHUD Decky plugin.
//
// Window properties and the frame-time message queue follow MangoHud's mangoapp (MIT,
// https://github.com/flightlessmango/MangoHud, src/app/main.cpp). Layout, colours and config keys
// follow FusionHUD's FusionHudView.kt / HudMetrics.java (GPL-3.0, The412Banner).
//
// Usage: fusionhud-overlay <config file>
// The config file holds FusionHUD's key=value settings (comma or newline separated); it is re-read
// whenever it changes.

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <dirent.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <regex>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_quit{false};
std::string g_renderer;  // GL_RENDERER of the overlay's own context, the GPU name source

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

std::string read_line(const std::string &path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    return s;
}

bool read_long(const std::string &path, long long &out) {
    std::string s = read_line(path);
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return false;
    out = v;
    return true;
}

// ---- Config (FusionHUD KeyValueSet keys) ----------------------------------------------------------

struct Config {
    std::map<std::string, std::string> kv;
    std::string get(const std::string &k, const std::string &def) const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    bool on(const std::string &k, bool def) const { return get(k, def ? "1" : "0") == "1"; }
    int num(const std::string &k, int def, int lo, int hi) const {
        std::string v = get(k, "");
        char *end = nullptr;
        long n = strtol(v.c_str(), &end, 10);
        if (v.empty() || end == v.c_str()) return def;
        return int(std::clamp<long>(n, lo, hi));
    }
};

Config parse_config(const std::string &text) {
    Config c;
    std::string item;
    auto flush = [&]() {
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = item.substr(0, eq), v = item.substr(eq + 1);
            auto trim = [](std::string &s) {
                s.erase(0, s.find_first_not_of(" \t\r"));
                s.erase(s.find_last_not_of(" \t\r") + 1);
            };
            trim(k);
            trim(v);
            if (!k.empty()) c.kv[k] = v;
        }
        item.clear();
    };
    for (char ch : text) {
        if (ch == ',' || ch == '\n') flush();
        else item += ch;
    }
    flush();
    return c;
}

// ---- Frame times from gamescope (mangoapp's message queue, untagged stream) -----------------------

struct mangoapp_msg_header {
    long msg_type;
    uint32_t version;
} __attribute__((packed));

struct mangoapp_msg_v1 {
    struct mangoapp_msg_header hdr;
    uint32_t pid;
    uint64_t app_frametime_ns;
    uint8_t fsrUpscale;
    uint8_t fsrSharpness;
    uint64_t visible_frametime_ns;
    uint64_t latency_ns;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint16_t displayRefresh;
    bool bAppWantsHDR : 1;
    bool bSteamFocused : 1;
    char engineName[40];
} __attribute__((packed));

struct Frames {
    std::mutex m;
    std::deque<std::pair<uint64_t, uint64_t>> times;  // (received, app frame time), last 10 s
    std::string engine;
    uint32_t outW = 0, outH = 0;
    uint16_t refresh = 0;
    uint64_t sessionStart = 0;
} g_frames;

void frame_reader() {
    // The untagged (legacy) stream, type 1: gamescope sends it while an untagged external overlay exists.
    // Same key as gamescope and mangoapp: ftok of a relative "mangoapp" (usually absent, so -1).
    const key_t key = ftok("mangoapp", 65);
    int msgid = msgget(key, 0666 | IPC_CREAT);
    fprintf(stderr, "fusionhud-overlay: frame queue key %d id %d\n", int(key), msgid);
    if (msgid < 0) {
        fprintf(stderr, "fusionhud-overlay: msgget failed: %s\n", strerror(errno));
        return;
    }
    struct {
        long mtype;
        unsigned char data[1016];
    } buf;
    while (!g_quit) {
        ssize_t n = msgrcv(msgid, &buf, sizeof(buf.data), 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "fusionhud-overlay: msgrcv failed: %s\n", strerror(errno));
            sleep(1);
            continue;
        }
        const size_t bytes = size_t(n) + sizeof(long);
        const auto *msg = reinterpret_cast<const mangoapp_msg_v1 *>(&buf);
        uint32_t version = 0;
        memcpy(&version, reinterpret_cast<const char *>(&buf) + offsetof(mangoapp_msg_header, version), sizeof(version));
        if (version != 1) continue;

        uint64_t ft = 0;
        if (bytes >= offsetof(mangoapp_msg_v1, app_frametime_ns) + sizeof(uint64_t))
            memcpy(&ft, reinterpret_cast<const char *>(msg) + offsetof(mangoapp_msg_v1, app_frametime_ns), sizeof(ft));

        std::lock_guard<std::mutex> lock(g_frames.m);
        if (bytes >= offsetof(mangoapp_msg_v1, displayRefresh) + sizeof(uint16_t)) {
            memcpy(&g_frames.outW, reinterpret_cast<const char *>(msg) + offsetof(mangoapp_msg_v1, outputWidth), 4);
            memcpy(&g_frames.outH, reinterpret_cast<const char *>(msg) + offsetof(mangoapp_msg_v1, outputHeight), 4);
            memcpy(&g_frames.refresh, reinterpret_cast<const char *>(msg) + offsetof(mangoapp_msg_v1, displayRefresh), 2);
        }
        if (bytes >= offsetof(mangoapp_msg_v1, engineName) + sizeof(msg->engineName)) {
            char name[41] = {};
            memcpy(name, reinterpret_cast<const char *>(msg) + offsetof(mangoapp_msg_v1, engineName), 40);
            g_frames.engine = name;
        }
        // ~0 marks a message without an app frame (visible-frame updates); skip pauses over a second.
        if (ft == 0 || ft == ~0ull || ft > 1000000000ull) continue;
        const uint64_t t = now_ns();
        if (!g_frames.sessionStart) g_frames.sessionStart = t;
        g_frames.times.emplace_back(t, ft);
        while (!g_frames.times.empty() && t - g_frames.times.front().first > 10000000000ull)
            g_frames.times.pop_front();
    }
}

struct FpsNow {
    float fps = 0, avg = 0, low1 = 0, low01 = 0, low001 = 0;
    std::vector<float> graph;  // frame times in ms, oldest first
    std::string engine;
    uint32_t outW = 0, outH = 0;
    uint16_t refresh = 0;
    float sessionSec = 0;
};

FpsNow fps_now() {
    FpsNow r;
    std::lock_guard<std::mutex> lock(g_frames.m);
    const uint64_t t = now_ns();
    r.engine = g_frames.engine;
    r.outW = g_frames.outW;
    r.outH = g_frames.outH;
    r.refresh = g_frames.refresh;
    if (g_frames.sessionStart) r.sessionSec = float(t - g_frames.sessionStart) / 1e9f;
    double sum1 = 0, sumAll = 0;
    size_t n1 = 0;
    std::vector<uint64_t> all;
    all.reserve(g_frames.times.size());
    for (const auto &p : g_frames.times) {
        all.push_back(p.second);
        sumAll += double(p.second);
        if (t - p.first <= 1000000000ull) {
            sum1 += double(p.second);
            n1++;
        }
    }
    if (n1) r.fps = float(1e9 / (sum1 / double(n1)));
    if (!all.empty()) {
        r.avg = float(1e9 / (sumAll / double(all.size())));
        const size_t start = all.size() > 120 ? all.size() - 120 : 0;
        for (size_t i = start; i < all.size(); i++) r.graph.push_back(float(all[i]) / 1e6f);
        std::sort(all.begin(), all.end(), std::greater<uint64_t>());
        auto low = [&](double frac) {
            size_t i = std::min(all.size() - 1, size_t(double(all.size()) * frac));
            return float(1e9 / double(all[i]));
        };
        r.low1 = low(0.01);
        r.low01 = low(0.001);
        r.low001 = low(0.0001);
    }
    return r;
}

// ---- System metrics (FusionHUD HudMetrics, the subset these sizes draw) ---------------------------

struct Snapshot {
    std::string gpuName;
    int cpu = -1, gpu = -1, gpuMhz = -1;
    float ramPct = -1, ramUsedGiB = -1, vramGiB = -1;
    int bat = -1;
    float batTemp = NAN, watts = 0, cpuTemp = NAN, gpuTemp = NAN;
    bool charging = false;
};

class Metrics {
public:
    Snapshot snapshot() {
        Snapshot s;
        s.gpuName = gpu_name();
        s.cpu = cpu_percent();
        s.gpu = gpu_percent();
        long long mhz;
        if (read_long("/sys/class/kgsl/kgsl-3d0/gpuclk", mhz) && mhz > 0) s.gpuMhz = int(mhz / 1000000);
        ram(s);
        long long v;
        if (read_long("/sys/class/kgsl/kgsl-3d0/page_alloc", v) && v > 0) s.vramGiB = float(double(v) / (1024.0 * 1024.0 * 1024.0));
        battery(s);
        temps(s);
        return s;
    }

private:
    unsigned long long prevIdle = 0, prevTotal = 0;
    bool zonesFound = false;
    std::string cpuZone, gpuZone;

    static std::string gpu_name() {
        // As FusionHUD's GPUInformation.extractModelName: "zink Vulkan 1.4(Turnip Adreno (TM) 750 ...)" → "Adreno 750".
        static const std::string cached = [] {
            std::smatch m;
            std::string r = g_renderer;
            for (const char *tm : {"(TM)", "(R)"})
                for (size_t p; (p = r.find(tm)) != std::string::npos;) r.erase(p, strlen(tm));
            if (std::regex_search(r, m, std::regex("Adreno\\s*(\\d+)", std::regex::icase))) return "Adreno " + m[1].str();
            if (std::regex_search(r, m, std::regex("(Immortalis|Mali)[\\s-]*([A-Za-z]?\\d+[A-Za-z0-9]*)", std::regex::icase)))
                return m[1].str() + "-" + m[2].str();
            if (std::regex_search(r, m, std::regex("Xclipse\\s*(\\d+)", std::regex::icase))) return "Xclipse " + m[1].str();
            // The kernel's own name as a last resort (KGSL: "Adreno33v2").
            return read_line("/sys/class/kgsl/kgsl-3d0/gpu_model");
        }();
        return cached;
    }

    int cpu_percent() {
        std::string l = read_line("/proc/stat");
        unsigned long long u, n, sy, id, io = 0, irq = 0, sirq = 0, st = 0;
        if (sscanf(l.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq, &st) < 4) return -1;
        const unsigned long long idle = id + io, total = u + n + sy + id + io + irq + sirq + st;
        int pct = -1;
        if (prevTotal && total > prevTotal) pct = int(std::lround(100.0 * double((total - prevTotal) - (idle - prevIdle)) / double(total - prevTotal)));
        prevIdle = idle;
        prevTotal = total;
        return pct < 0 ? -1 : std::clamp(pct, 0, 100);
    }

    static int gpu_percent() {
        long long v;
        if (read_long("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", v)) return int(std::clamp<long long>(v, 0, 100));
        std::string l = read_line("/sys/class/kgsl/kgsl-3d0/gpubusy");
        long long busy, total;
        if (sscanf(l.c_str(), "%lld %lld", &busy, &total) == 2 && total > 0) return int(std::clamp<long long>(busy * 100 / total, 0, 100));
        if (read_long("/sys/class/devfreq/gpu/load", v)) return int(std::clamp<long long>(v, 0, 100));
        return -1;
    }

    static void ram(Snapshot &s) {
        std::ifstream f("/proc/meminfo");
        std::string key;
        long long val;
        std::string unit;
        long long total = 0, avail = 0;
        while (f >> key >> val >> unit) {
            if (key == "MemTotal:") total = val;
            else if (key == "MemAvailable:") avail = val;
        }
        if (total > 0) {
            s.ramPct = float(100.0 * double(total - avail) / double(total));
            s.ramUsedGiB = float(double(total - avail) / (1024.0 * 1024.0));
        }
    }

    static void battery(Snapshot &s) {
        const std::string b = "/sys/class/power_supply/battery/";
        long long v;
        if (read_long(b + "capacity", v)) s.bat = int(v);
        if (read_long(b + "temp", v)) s.batTemp = float(v) / 10.0f;
        long long cur, volt;
        if (read_long(b + "current_now", cur) && read_long(b + "voltage_now", volt)) {
            double i = double(std::llabs(cur));
            if (i > 0 && i < 20000) i *= 1000.0;  // mA on some kernels, µA on most
            s.watts = float(i * double(volt) / 1e12);
        }
        std::string st = read_line(b + "status");
        s.charging = st == "Charging" || st == "Full";
    }

    static float read_zone(const std::string &zone) {
        long long v;
        if (zone.empty() || !read_long(zone + "/temp", v)) return NAN;
        float c = v > 1000 ? float(v) / 1000.0f : float(v);
        return (c >= 1.0f && c <= 150.0f) ? c : NAN;
    }

    void find_zones() {
        zonesFound = true;
        int cpuRank = 99, gpuRank = 99;
        DIR *d = opendir("/sys/class/thermal");
        if (!d) return;
        while (dirent *e = readdir(d)) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            const std::string zone = std::string("/sys/class/thermal/") + e->d_name;
            std::string type = read_line(zone + "/type");
            for (auto &ch : type) ch = char(tolower(ch));
            int c = 99, g = 99;
            if (type == "cpu-silicon") c = 0;
            else if (type.rfind("cpu-0", 0) == 0) c = 1;
            else if (type.find("cpu") != std::string::npos && type.find("gpu") == std::string::npos) c = 2;
            else if (type.find("soc") != std::string::npos) c = 3;
            if (type == "gpu-silicon") g = 0;
            else if (type.find("gpuss") != std::string::npos) g = 1;
            else if (type.find("gpu") != std::string::npos) g = 2;
            if (c < cpuRank && !std::isnan(read_zone(zone))) { cpuRank = c; cpuZone = zone; }
            if (g < gpuRank && !std::isnan(read_zone(zone))) { gpuRank = g; gpuZone = zone; }
        }
        closedir(d);
    }

    void temps(Snapshot &s) {
        if (!zonesFound) find_zones();
        s.cpuTemp = read_zone(cpuZone);
        s.gpuTemp = read_zone(gpuZone);
    }
};

// ---- Drawing --------------------------------------------------------------------------------------

constexpr ImU32 argb(uint32_t c) {  // 0xAARRGGBB → ImGui ABGR
    return ((c >> 24) & 0xFF) << 24 | (c & 0xFF) << 16 | ((c >> 8) & 0xFF) << 8 | ((c >> 16) & 0xFF);
}

const uint32_t COL_GPU = 0xFF5EE08A, COL_CPU = 0xFF58A6FF, COL_VRAM = 0xFFC98BFF, COL_RAM = 0xFFFF7BC0,
               COL_BAT = 0xFFFFAB5E, COL_FPS = 0xFFFF6B6B, COL_GRAPH = 0xFF5EE08A, COL_VALUE = 0xFFF2F5F9,
               COL_DIM = 0xFF9AA4B2, COL_TEMP_OK = 0xFF66BB6A, COL_TEMP_AMBER = 0xFFFFB74D, COL_TEMP_RED = 0xFFEF5350;

struct Span {
    std::string text;
    uint32_t col;
    float px;
};
using Run = std::vector<Span>;

struct Glyph {
    float x, baseline;
    std::string text;
    uint32_t col;
    float px;
};

class Hud {
public:
    ImFont *font = nullptr;
    float density = 2.156f;  // Pocket FIT: override density 345 / 160

    void apply(const Config &c) {
        cfg = c;
        size = c.get("hudSize", "pill");
        scale = float(c.num("hudScale", 100, 50, 200)) / 100.0f;
        bgOpacity = float(c.num("hudOpacity", 80, 0, 100)) / 100.0f;
        const std::string col = c.get("hudColor", "vivid");
        intensity = col == "soft" ? 0.72f : col == "mid" ? 0.88f : 1.0f;
        outline = float(c.num("hudOutline", 40, 0, 100)) / 100.0f;
        outlineAccent = c.on("hudOutlineAccent", true);
        accent = 0xFF000000u | uint32_t(strtoul(c.get("hudAccent", "A374FF").c_str(), nullptr, 16) & 0xFFFFFF);
        position = c.get("hudPosition", "top-left");
        fahrenheit = c.get("tempUnit", "c") == "f";
        bands = c.get("tempBands", "1") != "0";
        auto_ = c.get("tempAuto", "1") != "0";
        redCpu = c.num("tempRedCpu", 90, 30, 150);
        redGpu = c.num("tempRedGpu", 90, 30, 150);
        redBat = c.num("tempRedBat", 48, 20, 100);
    }

    // Lays out the current size; draw() then paints it at the configured corner of a w x h screen.
    void build(const Snapshot &s, const FpsNow &f, float screenScale) {
        k = density * scale * screenScale;
        glyphs.clear();
        graphRect = false;
        capsule = false;
        contentW = contentH = 0;
        fps = f;
        if (size == "minimal") build_minimal(f);
        else build_pill(s, f);
    }

    void draw(ImDrawList *dl, float screenW, float screenH) const {
        if (contentW <= 0 || contentH <= 0) return;
        const float margin = sp(12.0f);
        float ox = margin, oy = margin;
        if (position.find("right") != std::string::npos) ox = screenW - contentW - margin;
        if (position.find("bottom") != std::string::npos) oy = screenH - contentH - margin;
        if (position.find("center") != std::string::npos && position.find("left") == std::string::npos &&
            position.find("right") == std::string::npos)
            ox = (screenW - contentW) / 2.0f;

        const float radius = capsule ? contentH / 2.0f : sp(8.0f);
        dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + contentW, oy + contentH),
                          IM_COL32(0, 0, 0, int(std::lround(bgOpacity * 255.0f))), radius);
        const float strokeW = outline > 0 ? outline * sp(3.5f) : 0;
        if (strokeW > 0) {
            const float h = strokeW / 2.0f;
            dl->AddRect(ImVec2(ox + h, oy + h), ImVec2(ox + contentW - h, oy + contentH - h),
                        argb(outlineAccent ? accent : 0xFFC8C8C8u), std::max(0.0f, radius - h), 0, strokeW);
        }
        for (const auto &g : glyphs)
            dl->AddText(font, g.px, ImVec2(ox + g.x, oy + g.baseline - ascent(g.px)), argb(g.col), g.text.c_str());
        if (graphRect && fps.graph.size() >= 2) {
            float peak = 1.0f;
            for (float v : fps.graph) peak = std::max(peak, v);
            const float step = (gx1 - gx0) / float(fps.graph.size() - 1);
            std::vector<ImVec2> pts;
            for (size_t i = 0; i < fps.graph.size(); i++)
                pts.emplace_back(ox + gx0 + float(i) * step, oy + gy1 - std::clamp(fps.graph[i] / peak, 0.0f, 1.0f) * (gy1 - gy0));
            dl->AddPolyline(pts.data(), int(pts.size()), argb(blend(COL_GRAPH)), 0, sp(1.6f));
        }
    }

private:
    Config cfg;
    std::string size = "pill", position = "top-left";
    float scale = 1, bgOpacity = 0.8f, intensity = 1, outline = 0.4f, k = 2;
    bool outlineAccent = true, fahrenheit = false, bands = true, auto_ = true;
    int redCpu = 90, redGpu = 90, redBat = 48;
    uint32_t accent = 0xFFA374FF;
    std::vector<Glyph> glyphs;
    float contentW = 0, contentH = 0;
    bool capsule = false, graphRect = false;
    float gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
    FpsNow fps;

    float sp(float v) const { return v * k; }
    float measure(const std::string &t, float px) const { return font->CalcTextSizeA(px, FLT_MAX, 0, t.c_str()).x; }
    float ascent(float px) const { return font->Ascent * px / font->FontSize; }
    float lineH(float px) const { return (font->Ascent - font->Descent) * px / font->FontSize; }
    float runWidth(const Run &r) const {
        float w = 0;
        for (const auto &s : r) w += measure(s.text, s.px);
        return w;
    }
    uint32_t blend(uint32_t c) const {
        if (intensity >= 0.999f) return c;
        // Desaturate by the intensity, as FusionHUD scales the HSV saturation.
        float r = float((c >> 16) & 0xFF) / 255.0f, g = float((c >> 8) & 0xFF) / 255.0f, b = float(c & 0xFF) / 255.0f;
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
        ImGui::ColorConvertHSVtoRGB(h, s * intensity, v, r, g, b);
        return (c & 0xFF000000u) | uint32_t(std::lround(r * 255)) << 16 | uint32_t(std::lround(g * 255)) << 8 | uint32_t(std::lround(b * 255));
    }
    float placeRun(float x, float baseline, const Run &r) {
        for (const auto &s : r) {
            glyphs.push_back({x, baseline, s.text, blend(s.col), s.px});
            x += measure(s.text, s.px);
        }
        return x;
    }
    std::string fmt1(float v) const {
        char b[32];
        snprintf(b, sizeof b, "%.1f", v);
        return b;
    }
    std::string fpsText(float v) const {
        char b[32];
        if (cfg.on("fpsDecimal", true)) snprintf(b, sizeof b, "%.1f", v);
        else snprintf(b, sizeof b, "%d", int(std::lround(v)));
        return b;
    }
    std::string lowText(float v) const { return v <= 0 ? "—" : fmt1(v); }
    std::string clockText() const {
        char b[32];
        time_t t = time(nullptr);
        tm lt;
        localtime_r(&t, &lt);
        strftime(b, sizeof b, "%I:%M %p", &lt);
        return b[0] == '0' ? std::string(b + 1) : std::string(b);
    }
    std::string apiLabel(const FpsNow &f) const {
        if (!cfg.on("showEngine", true) || f.engine.empty() || f.engine == "gamescope") return "FPS";
        return f.engine;
    }
    Run tempSpans(float c, int red, float numPx, float unitPx) const {
        if (std::isnan(c)) return {};
        uint32_t col = COL_VALUE;
        if (bands) {
            const int r = red, amber = int(std::lround(r * 0.88f));
            col = c >= float(r) ? COL_TEMP_RED : c >= float(amber) ? COL_TEMP_AMBER : COL_TEMP_OK;
        }
        const float v = fahrenheit ? c * 9.0f / 5.0f + 32.0f : c;
        return {{std::to_string(int(std::lround(v))), col, numPx}, {fahrenheit ? "°F" : "°C", COL_DIM, unitPx}};
    }

    void addSubtleClock(float pad, float centerX = -1) {
        if (!cfg.on("showClock", true)) return;
        const float px = sp(9.5f);
        const std::string t = clockText();
        const float w = measure(t, px);
        const float footerTop = contentH - pad;
        float x = centerX >= 0 ? std::clamp(centerX - w / 2.0f, pad, std::max(pad, contentW - pad - w))
                               : std::max(pad, contentW - pad - w);
        glyphs.push_back({x, footerTop + ascent(px), t, blend(COL_DIM), px});
        contentH = footerTop + lineH(px) + pad * 0.5f;
        contentW = std::max(contentW, pad + w + pad);
    }

    void build_pill(const Snapshot &s, const FpsNow &f) {
        capsule = true;
        const float bigPx = sp(30), bigUnitPx = bigPx * 0.36f, stkPx = sp(11.5f);
        const float pad = sp(10), midGap = sp(12), stkLineGap = sp(3);

        Run left = {{fpsText(f.fps), COL_VALUE, bigPx}, {"fps", COL_DIM, bigUnitPx}};

        std::vector<Run> stack;
        if (cfg.on("showGpuModel", true) && !s.gpuName.empty()) stack.push_back({{s.gpuName, COL_DIM, stkPx}});
        {
            Run l;
            if (cfg.on("showGPULoad", true)) l.push_back({"GPU " + (s.gpu >= 0 ? std::to_string(s.gpu) : std::string("—")) + "%", COL_GPU, stkPx});
            if (cfg.on("showCPUUsage", cfg.on("showCPULoad", true))) {
                if (!l.empty()) l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({"CPU " + (s.cpu >= 0 ? std::to_string(s.cpu) : std::string("—")) + "%", COL_CPU, stkPx});
            }
            if (!l.empty()) stack.push_back(l);
        }
        if (cfg.on("showRAM", true) && s.ramPct >= 0) {
            Run l = {{"RAM " + std::to_string(int(std::lround(s.ramPct))) + "%", COL_RAM, stkPx}};
            if (cfg.on("showBatteryTemp", false) && !std::isnan(s.batTemp)) {
                l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({"BAT ", COL_BAT, stkPx});
                for (auto &sp_ : tempSpans(s.batTemp, redBat, stkPx, stkPx * 0.62f)) l.push_back(sp_);
            }
            stack.push_back(l);
        }
        if (cfg.on("showBattery", true) || cfg.on("showPower", true)) {
            Run l;
            bool any = false;
            if (cfg.on("showBattery", true) && s.bat >= 0) { l.push_back({"BAT " + std::to_string(s.bat) + "%", COL_BAT, stkPx}); any = true; }
            if (cfg.on("showPower", true) && s.watts > 0) {
                if (any) l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({fmt1(s.watts) + "W", COL_DIM, stkPx});
                any = true;
            }
            if (any) stack.push_back(l);
        }
        {
            Run l = {{fmt1(1000.0f / std::max(f.fps, 1.0f)) + "ms", COL_DIM, stkPx}};
            if (cfg.on("showVram", true) && s.vramGiB >= 0) {
                l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({fmt1(s.vramGiB) + "G vram", COL_DIM, stkPx});
            }
            stack.push_back(l);
        }

        const std::string api = apiLabel(f);
        const bool hasApi = api != "FPS";
        const float apiH = hasApi ? lineH(stkPx) : 0, apiGap = hasApi ? stkLineGap : 0;
        const float leftW = runWidth(left), leftH = lineH(bigPx);
        const float apiW = hasApi ? measure(api, stkPx) : 0;
        const float leftBlockW = std::max(leftW, apiW);
        const float leftColH = apiH + apiGap + leftH;
        const float stkH = lineH(stkPx);
        float stackW = 0;
        for (const auto &l : stack) stackW = std::max(stackW, runWidth(l));
        const float stackTotalH = float(stack.size()) * stkH + float(std::max<int>(0, int(stack.size()) - 1)) * stkLineGap;
        const float innerH = std::max(leftColH, stackTotalH);
        contentW = pad + leftBlockW + midGap + stackW + pad;
        contentH = pad + innerH + pad;

        float ly = pad + (innerH - leftColH) / 2.0f;
        if (hasApi) {
            placeRun(pad + (leftBlockW - apiW) / 2.0f, ly + ascent(stkPx), {{api, COL_FPS, stkPx}});
            ly += apiH + apiGap;
        }
        placeRun(pad + (leftBlockW - leftW) / 2.0f, ly + ascent(bigPx), left);
        float sy = pad + (innerH - stackTotalH) / 2.0f;
        for (const auto &l : stack) {
            placeRun(pad + leftBlockW + midGap, sy + ascent(stkPx), l);
            sy += stkH + stkLineGap;
        }
        addSubtleClock(pad, pad + leftBlockW / 2.0f);
        fitCapsule();
    }

    // The capsule's ends curve in (radius = height / 2), so a line near the top or bottom can fit the box
    // and still cross the outline. Shift everything right and widen until every line clears both ends by
    // FusionHUD's capsule margin (outline width + 2sp).
    void fitCapsule() {
        const float r = contentH / 2.0f;
        const float margin = (outline > 0 ? outline * sp(3.5f) : 0.0f) + sp(2.0f);
        auto curveIn = [&](float top, float bottom) {
            const float d = std::min(top, contentH - bottom);  // closest the line gets to the top or bottom
            if (d >= r) return 0.0f;
            const float dy = r - std::max(d, 0.0f);
            return r - std::sqrt(std::max(0.0f, r * r - dy * dy));
        };
        float shift = 0;
        for (const auto &g : glyphs) {
            const float top = g.baseline - ascent(g.px), bottom = top + lineH(g.px);
            shift = std::max(shift, curveIn(top, bottom) + margin - g.x);
        }
        if (shift > 0) {
            for (auto &g : glyphs) g.x += shift;
            contentW += shift;
        }
        for (const auto &g : glyphs) {
            const float top = g.baseline - ascent(g.px), bottom = top + lineH(g.px);
            contentW = std::max(contentW, g.x + measure(g.text, g.px) + curveIn(top, bottom) + margin);
        }
    }

    void build_minimal(const FpsNow &f) {
        const float bigPx = sp(34), bigUnitPx = bigPx * 0.32f, subPx = sp(11.5f);
        const float pad = sp(10), lineGap = sp(6);
        Run big = {{fpsText(f.fps), COL_VALUE, bigPx}, {"fps", COL_DIM, bigUnitPx}};
        Run sub = {{"1% ", COL_DIM, subPx}, {lowText(f.low1), COL_FPS, subPx},
                   {"  ·  0.1% ", COL_DIM, subPx}, {lowText(f.low01), COL_FPS, subPx}};
        if (cfg.on("showLow001", true)) {
            sub.push_back({"  ·  0.01% ", COL_DIM, subPx});
            sub.push_back({lowText(f.low001), COL_FPS, subPx});
        }
        const bool graph = cfg.on("showFPSGraph", false);
        const float bigW = runWidth(big), bigH = lineH(bigPx), subW = runWidth(sub), subH = lineH(subPx);
        const float gw = sp(120), gh = sp(22);
        const float inner = std::max(std::max(bigW, subW), graph ? gw : 0.0f);
        contentW = inner + pad * 2;
        float y = pad;
        placeRun(pad + (inner - bigW) / 2.0f, y + ascent(bigPx), big);
        y += bigH + lineGap;
        placeRun(pad + (inner - subW) / 2.0f, y + ascent(subPx), sub);
        y += subH + lineGap;
        if (graph) {
            graphRect = true;
            gx0 = pad + (inner - gw) / 2.0f;
            gy0 = y;
            gx1 = gx0 + gw;
            gy1 = y + gh;
            y += gh;
        }
        contentH = y + pad;
        std::string footer = cfg.on("showEngine", true) && apiLabel(f) != "FPS" ? apiLabel(f) : "";
        if (!footer.empty()) {
            const float apiPx = sp(9.5f);
            const float footerTop = contentH - pad;
            glyphs.push_back({pad, footerTop + ascent(apiPx), footer, blend(COL_DIM), apiPx});
            const float clockW = cfg.on("showClock", true) ? measure(clockText(), apiPx) : 0;
            contentW = std::max(contentW, pad + measure(footer, apiPx) + sp(12) + clockW + pad);
            if (!cfg.on("showClock", true)) contentH = footerTop + lineH(apiPx) + pad * 0.5f;
        }
        addSubtleClock(pad);
    }
};

int x_io_error(Display *) {
    g_quit = true;
    _exit(0);
}

void on_signal(int) { g_quit = true; }

void set_cardinal(Display *dpy, Window win, const char *name, uint32_t value) {
    Atom atom = XInternAtom(dpy, name, False);
    XChangeProperty(dpy, win, atom, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&value), 1);
}

ImFont *load_font(ImGuiIO &io) {
    const char *candidates[] = {"/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
                                "/usr/share/fonts/TTF/DejaVuSansMono.ttf"};
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2014, 0x2014, 0x2022, 0x2022, 0x2192, 0x2192, 0};
    for (const char *p : candidates) {
        if (access(p, R_OK) == 0) {
            ImFontConfig fc;
            fc.OversampleH = 2;
            if (ImFont *f = io.Fonts->AddFontFromFileTTF(p, 64.0f, &fc, ranges)) return f;
        }
    }
    fprintf(stderr, "fusionhud-overlay: DejaVu Sans Mono not found, using ImGui's built-in font\n");
    return io.Fonts->AddFontDefault();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <config file>\n", argv[0]);
        return 2;
    }
    const std::string configPath = argv[1];
    prctl(PR_SET_PDEATHSIG, SIGTERM);  // the plugin backend going away takes the overlay with it
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    XSetIOErrorHandler(x_io_error);

    if (!glfwInit()) {
        fprintf(stderr, "fusionhud-overlay: glfwInit failed (DISPLAY=%s)\n", getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, 1);
    glfwWindowHint(GLFW_DECORATED, 0);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, 1);
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);

    int screenW = 1280, screenH = 800;
    if (const GLFWvidmode *mode = glfwGetVideoMode(glfwGetPrimaryMonitor())) {
        screenW = mode->width;
        screenH = mode->height;
    }
    GLFWwindow *window = glfwCreateWindow(screenW, screenH, "FusionHUD overlay", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "fusionhud-overlay: could not create the window\n");
        glfwTerminate();
        return 1;
    }
    Display *dpy = glfwGetX11Display();
    Window xwin = glfwGetX11Window(window);
    // Untagged external overlay, like a mangoapp without MANGOAPP_MSG_TYPE; never takes focus.
    set_cardinal(dpy, xwin, "GAMESCOPE_EXTERNAL_OVERLAY", 1);
    set_cardinal(dpy, xwin, "GAMESCOPE_NO_FOCUS", 1);
    XSync(dpy, False);
    fprintf(stderr, "fusionhud-overlay: window %lux%d on DISPLAY=%s\n", (unsigned long)screenW, screenH,
            getenv("DISPLAY") ? getenv("DISPLAY") : "");

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    // libGL is loaded at run time (no GL link), so the three calls made here go through GLFW too.
    auto pViewport = reinterpret_cast<void (*)(GLint, GLint, GLsizei, GLsizei)>(glfwGetProcAddress("glViewport"));
    auto pClearColor = reinterpret_cast<void (*)(GLfloat, GLfloat, GLfloat, GLfloat)>(glfwGetProcAddress("glClearColor"));
    auto pClear = reinterpret_cast<void (*)(GLbitfield)>(glfwGetProcAddress("glClear"));
    auto pGetString = reinterpret_cast<const unsigned char *(*)(GLenum)>(glfwGetProcAddress("glGetString"));
    if (pGetString)
        if (const unsigned char *r = pGetString(GL_RENDERER)) g_renderer = reinterpret_cast<const char *>(r);
    fprintf(stderr, "fusionhud-overlay: GL renderer %s\n", g_renderer.c_str());
    if (!pViewport || !pClearColor || !pClear) {
        fprintf(stderr, "fusionhud-overlay: OpenGL functions not found\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    Hud hud;
    hud.font = load_font(io);
    ImGui_ImplGlfw_InitForOpenGL(window, false);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::thread(frame_reader).detach();

    Metrics metrics;
    Snapshot snap = metrics.snapshot();
    uint64_t lastMetrics = 0, lastConfigCheck = 0;
    time_t configMtime = -1;

    while (!g_quit && !glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
        const uint64_t t = now_ns();
        if (t - lastConfigCheck > 500000000ull) {
            lastConfigCheck = t;
            struct stat st;
            if (stat(configPath.c_str(), &st) == 0 && st.st_mtime != configMtime) {
                configMtime = st.st_mtime;
                std::ifstream f(configPath);
                std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                hud.apply(parse_config(text));
            }
        }
        if (t - lastMetrics > 1000000000ull) {
            lastMetrics = t;
            snap = metrics.snapshot();
        }

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        const float screenScale = float(std::max(fbH, 1)) / 1080.0f;
        hud.build(snap, fps_now(), screenScale);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        hud.draw(ImGui::GetForegroundDrawList(), float(fbW), float(fbH));
        ImGui::Render();
        pViewport(0, 0, fbW, fbH);
        pClearColor(0, 0, 0, 0);
        pClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

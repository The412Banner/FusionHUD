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
    uint32_t outW = 0, outH = 0, pid = 0;
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
        if (bytes >= offsetof(mangoapp_msg_v1, pid) + sizeof(uint32_t))
            memcpy(&g_frames.pid, reinterpret_cast<const char *>(msg) + offsetof(mangoapp_msg_v1, pid), 4);
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
    float fps = 0, avg = 0, low1 = 0, low01 = 0, low001 = 0, minMs = 0, maxMs = 0;
    uint32_t pid = 0;
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
    r.pid = g_frames.pid;
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
        r.maxMs = float(all.front()) / 1e6f;
        r.minMs = float(all.back()) / 1e6f;
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
    std::string gpuName, proton;
    int cpu = -1, gpu = -1, gpuMhz = -1, cpuMhz = -1;
    std::vector<int> corePct, coreMhz;
    long long ramUsed = -1, ramTotal = -1, vram = -1, swapUsed = -1, swapTotal = -1, netDown = -1, netUp = -1;
    float ramPct = -1, ramUsedGiB = -1, vramGiB = -1;
    int bat = -1;
    float batTemp = NAN, watts = 0, cpuTemp = NAN, gpuTemp = NAN;
    bool charging = false;
};

// DroidDeck's guest gets its own copies of these (session parts in the app: BatteryComponent writes
// power_supply/BAT1, HwmonComponent hwmon/*, GpuStatsComponent kgsl-3d0/{gpu_busy_percentage,clock_mhz,temp},
// GpuMemComponent /run/droiddeck-hud/gpu-mem, CpuStatComponent /proc/stat). Android's own paths are the
// fallbacks, so the same build also reads a plain Android/Linux system.
class Metrics {
public:
    Snapshot snapshot(uint32_t gamePid) {
        Snapshot s;
        s.gpuName = gpu_name();
        cpu_stat(s);
        s.gpu = gpu_percent();
        s.gpuMhz = gpu_mhz();
        core_clocks(s);
        ram(s);
        s.vram = vram_bytes();
        if (s.vram > 0) s.vramGiB = float(double(s.vram) / (1024.0 * 1024.0 * 1024.0));
        net(s);
        battery(s);
        temps(s);
        s.proton = proton_of(gamePid);
        return s;
    }

private:
    unsigned long long prevIdle = 0, prevTotal = 0;
    std::vector<unsigned long long> prevCoreIdle, prevCoreTotal;
    long long prevRx = -1, prevTx = -1;
    uint64_t prevNetNs = 0;
    uint32_t protonPid = 0;
    std::string protonName;
    bool tempsFound = false;
    std::string cpuTempFile, gpuTempFile, batteryDir;

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

    // Aggregate and per-core load from /proc/stat deltas (DroidDeck binds its own CpuStatComponent copy).
    void cpu_stat(Snapshot &s) {
        std::ifstream f("/proc/stat");
        std::string line;
        while (std::getline(f, line) && line.rfind("cpu", 0) == 0) {
            unsigned long long u, n, sy, id, io = 0, irq = 0, sirq = 0, st = 0;
            char name[16];
            if (sscanf(line.c_str(), "%15s %llu %llu %llu %llu %llu %llu %llu %llu", name, &u, &n, &sy, &id, &io, &irq, &sirq, &st) < 5) continue;
            const unsigned long long idle = id + io, total = u + n + sy + id + io + irq + sirq + st;
            if (strcmp(name, "cpu") == 0) {
                if (prevTotal && total > prevTotal)
                    s.cpu = std::clamp(int(std::lround(100.0 * double((total - prevTotal) - (idle - prevIdle)) / double(total - prevTotal))), 0, 100);
                prevIdle = idle;
                prevTotal = total;
                continue;
            }
            const int core = atoi(name + 3);
            if (core < 0 || core > 255) continue;
            if (int(prevCoreTotal.size()) <= core) {
                prevCoreTotal.resize(core + 1, 0);
                prevCoreIdle.resize(core + 1, 0);
            }
            if (int(s.corePct.size()) <= core) s.corePct.resize(core + 1, -1);
            if (prevCoreTotal[core] && total > prevCoreTotal[core])
                s.corePct[core] = std::clamp(int(std::lround(100.0 * double((total - prevCoreTotal[core]) - (idle - prevCoreIdle[core])) /
                                                             double(total - prevCoreTotal[core]))), 0, 100);
            prevCoreIdle[core] = idle;
            prevCoreTotal[core] = total;
        }
    }

    static void core_clocks(Snapshot &s) {
        const long n = sysconf(_SC_NPROCESSORS_CONF);
        for (long i = 0; i < n && i < 256; i++) {
            long long khz;
            const int mhz = read_long("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq", khz) && khz > 0 ? int(khz / 1000) : 0;
            s.coreMhz.push_back(mhz);
            s.cpuMhz = std::max(s.cpuMhz, mhz > 0 ? mhz : -1);
        }
    }

    static int gpu_percent() {
        long long v;
        // "  3 %" in both Android's file and DroidDeck's copy; strtoll skips the spaces and stops at '%'.
        if (read_long("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", v)) return int(std::clamp<long long>(v, 0, 100));
        std::string l = read_line("/sys/class/kgsl/kgsl-3d0/gpubusy");
        long long busy, total;
        if (sscanf(l.c_str(), "%lld %lld", &busy, &total) == 2 && total > 0) return int(std::clamp<long long>(busy * 100 / total, 0, 100));
        if (read_long("/sys/class/devfreq/gpu/load", v)) return int(std::clamp<long long>(v, 0, 100));
        return -1;
    }

    static int gpu_mhz() {
        long long v;
        if (read_long("/sys/class/kgsl/kgsl-3d0/clock_mhz", v) && v > 0) return int(v);
        if (read_long("/sys/class/kgsl/kgsl-3d0/gpuclk", v) && v > 0) return int(v / 1000000);
        if (read_long("/sys/class/kgsl/kgsl-3d0/cur_freq", v) && v > 0) return int(v > 100000 ? v / 1000000 : v);
        return -1;
    }

    static long long vram_bytes() {
        long long v;
        if (read_long("/run/droiddeck-hud/gpu-mem", v) && v > 0) return v;
        if (read_long("/sys/class/kgsl/kgsl/page_alloc", v) && v > 0) return v;
        return -1;
    }

    // Received and sent bytes per second over every interface but lo (/proc/net/dev).
    void net(Snapshot &s) {
        std::ifstream f("/proc/net/dev");
        std::string line;
        long long rx = 0, tx = 0;
        bool any = false;
        while (std::getline(f, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string iface = line.substr(0, colon);
            iface.erase(0, iface.find_first_not_of(' '));
            if (iface.empty() || iface == "lo") continue;
            long long v[9];
            if (sscanf(line.c_str() + colon + 1, "%lld %lld %lld %lld %lld %lld %lld %lld %lld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) != 9) continue;
            rx += v[0];
            tx += v[8];
            any = true;
        }
        if (!any) return;
        const uint64_t t = now_ns();
        if (prevRx >= 0 && t > prevNetNs) {
            const double sec = double(t - prevNetNs) / 1e9;
            s.netDown = (long long)(double(std::max(0LL, rx - prevRx)) / sec);
            s.netUp = (long long)(double(std::max(0LL, tx - prevTx)) / sec);
        }
        prevRx = rx;
        prevTx = tx;
        prevNetNs = t;
    }

    // The Proton build running the focused game: the first STEAM_COMPAT_TOOL_PATHS entry in its environment.
    std::string proton_of(uint32_t pid) {
        if (pid == protonPid) return protonName;
        protonPid = pid;
        protonName.clear();
        if (!pid) return protonName;
        std::ifstream f("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
        std::string var;
        while (std::getline(f, var, '\0')) {
            if (var.rfind("STEAM_COMPAT_TOOL_PATHS=", 0) != 0) continue;
            std::string first = var.substr(24, var.find(':', 24) == std::string::npos ? std::string::npos : var.find(':', 24) - 24);
            while (!first.empty() && first.back() == '/') first.pop_back();
            const size_t slash = first.rfind('/');
            protonName = slash == std::string::npos ? first : first.substr(slash + 1);
            break;
        }
        return protonName;
    }

    static void ram(Snapshot &s) {
        std::ifstream f("/proc/meminfo");
        std::string key;
        long long val;
        std::string unit;
        long long total = 0, avail = 0, swapTotal = -1, swapFree = -1;
        while (f >> key >> val >> unit) {
            if (key == "MemTotal:") total = val;
            else if (key == "MemAvailable:") avail = val;
            else if (key == "SwapTotal:") swapTotal = val;
            else if (key == "SwapFree:") swapFree = val;
        }
        if (total > 0) {
            s.ramPct = float(100.0 * double(total - avail) / double(total));
            s.ramUsedGiB = float(double(total - avail) / (1024.0 * 1024.0));
            s.ramUsed = (total - avail) * 1024;
            s.ramTotal = total * 1024;
        }
        if (swapTotal > 0) {
            s.swapTotal = swapTotal * 1024;
            s.swapUsed = std::max(0LL, swapTotal - std::max(0LL, swapFree)) * 1024;
        }
    }

    // The first power supply whose type is Battery: BAT1 in DroidDeck, "battery" on Android.
    std::string find_battery() {
        DIR *d = opendir("/sys/class/power_supply");
        if (!d) return "";
        std::string found;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string dir = std::string("/sys/class/power_supply/") + e->d_name + "/";
            if (read_line(dir + "type") == "Battery") { found = dir; break; }
        }
        closedir(d);
        return found;
    }

    void battery(Snapshot &s) {
        long long v;
        if (batteryDir.empty() || !read_long(batteryDir + "capacity", v)) batteryDir = find_battery();
        if (batteryDir.empty()) return;
        const std::string &b = batteryDir;
        if (read_long(b + "capacity", v)) s.bat = int(v);
        if (read_long(b + "temp", v)) s.batTemp = float(v) / 10.0f;
        // current × voltage, as FusionHUD: Android's own power_now is not in µW on every kernel (the Pocket FIT
        // reports 63702476 at about 8 W). DroidDeck's BAT1 power_now is this same product.
        long long cur, volt;
        if (read_long(b + "current_now", cur) && read_long(b + "voltage_now", volt) && cur != 0) {
            double i = double(std::llabs(cur));
            if (i < 20000) i *= 1000.0;  // mA on some kernels, µA on most
            s.watts = float(i * double(volt) / 1e12);
        } else if (read_long(b + "power_now", v) && v != 0) {
            s.watts = float(double(std::llabs(v)) / 1e6);
        }
        std::string st = read_line(b + "status");
        s.charging = st == "Charging" || st == "Full";
    }

    static float read_milli(const std::string &file) {
        long long v;
        if (file.empty() || !read_long(file, v)) return NAN;
        float c = v > 1000 ? float(v) / 1000.0f : float(v);
        return (c >= 1.0f && c <= 150.0f) ? c : NAN;
    }

    // hwmon first (DroidDeck: cpu0_thermal, gpuss0_thermal), then KGSL's own temp, then thermal zones ranked
    // as FusionHUD's HudMetrics does.
    void find_temps() {
        tempsFound = true;
        int cpuRank = 99, gpuRank = 99;
        auto offer = [&](const std::string &type, const std::string &file) {
            std::string t = type;
            for (auto &ch : t) ch = char(tolower(ch));
            int c = 99, g = 99;
            if (t == "cpu-silicon") c = 0;
            else if (t.rfind("cpu-0", 0) == 0 || t.rfind("cpu0", 0) == 0) c = 1;
            else if (t.find("cpu") != std::string::npos && t.find("gpu") == std::string::npos) c = 2;
            else if (t.find("soc") != std::string::npos) c = 3;
            if (t == "gpu-silicon") g = 0;
            else if (t.find("gpuss") != std::string::npos) g = 1;
            else if (t.find("gpu") != std::string::npos) g = 2;
            if (c < cpuRank && !std::isnan(read_milli(file))) { cpuRank = c; cpuTempFile = file; }
            if (g < gpuRank && !std::isnan(read_milli(file))) { gpuRank = g; gpuTempFile = file; }
        };
        if (DIR *d = opendir("/sys/class/hwmon")) {
            while (dirent *e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                const std::string dir = std::string("/sys/class/hwmon/") + e->d_name;
                offer(read_line(dir + "/name"), dir + "/temp1_input");
            }
            closedir(d);
        }
        if (gpuRank > 1 && !std::isnan(read_milli("/sys/class/kgsl/kgsl-3d0/temp"))) {
            gpuRank = 1;
            gpuTempFile = "/sys/class/kgsl/kgsl-3d0/temp";
        }
        if (DIR *d = opendir("/sys/class/thermal")) {
            while (dirent *e = readdir(d)) {
                if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
                const std::string zone = std::string("/sys/class/thermal/") + e->d_name;
                offer(read_line(zone + "/type"), zone + "/temp");
            }
            closedir(d);
        }
    }

    void temps(Snapshot &s) {
        if (!tempsFound) find_temps();
        s.cpuTemp = read_milli(cpuTempFile);
        s.gpuTemp = read_milli(gpuTempFile);
    }
};

// ---- Drawing --------------------------------------------------------------------------------------

constexpr ImU32 argb(uint32_t c) {  // 0xAARRGGBB → ImGui ABGR
    return ((c >> 24) & 0xFF) << 24 | (c & 0xFF) << 16 | ((c >> 8) & 0xFF) << 8 | ((c >> 16) & 0xFF);
}

const uint32_t COL_GPU = 0xFF5EE08A, COL_CPU = 0xFF58A6FF, COL_VRAM = 0xFFC98BFF, COL_RAM = 0xFFFF7BC0,
               COL_BAT = 0xFFFFAB5E, COL_FPS = 0xFFFF6B6B, COL_GRAPH = 0xFF5EE08A, COL_VALUE = 0xFFF2F5F9,
               COL_DIM = 0xFF9AA4B2, COL_LO = 0xFFE4E8EE, COL_TEMP_OK = 0xFF66BB6A, COL_TEMP_AMBER = 0xFFFFB74D, COL_TEMP_RED = 0xFFEF5350;

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
        redCpu = c.num("tempRedCpu", 90, 30, 150);
        redGpu = c.num("tempRedGpu", 90, 30, 150);
        redBat = c.num("tempRedBat", 48, 20, 100);
        // FusionHudView.applyConfig flags.
        showFPS = c.on("showFPS", true);
        showEngine = c.on("showEngine", true);
        showGpuModel = c.on("showGpuModel", true);
        showCPU = c.on("showCPUUsage", c.on("showCPULoad", true));
        showGPU = c.on("showGPULoad", true);
        showCpuTemp = c.on("showTemp", true);
        showGpuTemp = c.on("showGpuTemp", false);
        showVram = c.on("showVram", true);
        showRAM = c.on("showRAM", true);
        showBattery = c.on("showBattery", true);
        showPower = c.on("showPower", true);
        showBatteryTemp = c.on("showBatteryTemp", false);
        showGraph = c.on("showFPSGraph", false);
        showLow001 = c.on("showLow001", true);
        fpsDecimal = c.on("fpsDecimal", true);
        showClock = c.on("showClock", true);
        showPerCore = c.on("showPerCore", true);
        showSwap = c.on("showSwap", true);
        showNet = c.on("showNet", true);
        showResolution = c.on("showResolution", true);
        showProton = c.on("showProton", true);
        showSession = c.on("showSession", true);
    }

    // Lays out the current size; draw() then paints it at the configured corner of a w x h screen.
    void build(const Snapshot &s, const FpsNow &f, float screenScale, float sessionSec) {
        k = density * scale * screenScale;
        glyphs.clear();
        tiles.clear();
        graphRect = false;
        capsule = false;
        contentW = contentH = 0;
        fps = f;
        session = sessionSec;
        if (size == "minimal") build_minimal(f);
        else if (size == "full") build_full(s, f);
        else if (size == "tiles") build_tiles(s, f);
        else if (size == "mega") build_mega(s, f);
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
        if (!tiles.empty()) {
            const int a = std::clamp(int(std::lround(14.0f * bgOpacity)), 8, 40);
            for (const auto &t : tiles)
                dl->AddRectFilled(ImVec2(ox + t.x, oy + t.y), ImVec2(ox + t.z, oy + t.w), IM_COL32(255, 255, 255, a), sp(6.0f));
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
    struct HudRow {
        Span label;
        Run vals;
    };

    Config cfg;
    std::string size = "pill", position = "top-left";
    float scale = 1, bgOpacity = 0.8f, intensity = 1, outline = 0.4f, k = 2, session = 0;
    bool outlineAccent = true, fahrenheit = false, bands = true;
    bool showFPS = true, showEngine = true, showGpuModel = true, showCPU = true, showGPU = true, showCpuTemp = true,
         showGpuTemp = false, showVram = true, showRAM = true, showBattery = true, showPower = true, showBatteryTemp = false,
         showGraph = false, showLow001 = true, fpsDecimal = true, showClock = true, showPerCore = true, showSwap = true,
         showNet = true, showResolution = true, showProton = true, showSession = true;
    int redCpu = 90, redGpu = 90, redBat = 48;
    uint32_t accent = 0xFFA374FF;
    std::vector<Glyph> glyphs;
    std::vector<ImVec4> tiles;  // x0, y0, x1, y1
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
    static void append(Run &to, const Run &from) { to.insert(to.end(), from.begin(), from.end()); }
    std::string fmt1(float v) const {
        char b[32];
        snprintf(b, sizeof b, "%.1f", v);
        return b;
    }
    std::string fpsText(float v) const {
        char b[32];
        if (fpsDecimal) snprintf(b, sizeof b, "%.1f", v);
        else snprintf(b, sizeof b, "%d", int(std::lround(v)));
        return b;
    }
    std::string lowText(float v) const { return v <= 0 ? "—" : fmt1(v); }
    static std::string bytesGb(long long bytes) {  // HudMetrics.formatBytesGb
        char b[32];
        const double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) snprintf(b, sizeof b, "%.1fGiB", gb);
        else snprintf(b, sizeof b, "%lldMiB", bytes / (1024 * 1024));
        return b;
    }
    static std::string vramText(long long bytes) {  // HudMetrics.formatGiB
        char b[32];
        const double gib = double(bytes) / (1024.0 * 1024.0 * 1024.0);
        if (gib >= 0.1) snprintf(b, sizeof b, "%.1fGiB", gib);
        else snprintf(b, sizeof b, "%lldMiB", bytes / (1024 * 1024));
        return b;
    }
    std::string clockText() const {
        char b[32];
        time_t t = time(nullptr);
        tm lt;
        localtime_r(&t, &lt);
        strftime(b, sizeof b, "%I:%M %p", &lt);
        return b[0] == '0' ? std::string(b + 1) : std::string(b);
    }
    std::string elapsedText() const {
        const int sec = std::max(0, int(session));
        char b[32];
        if (sec >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
        else snprintf(b, sizeof b, "%02d:%02d", (sec % 3600) / 60, sec % 60);
        return b;
    }
    std::string resolutionText(const FpsNow &f) const {
        if (!f.outW || !f.outH) return "";
        std::string r = std::to_string(f.outW) + "x" + std::to_string(f.outH);
        if (f.refresh) r += " @ " + std::to_string(f.refresh) + "Hz";
        return r;
    }
    std::string apiLabel(const FpsNow &f) const {
        if (!showEngine || f.engine.empty() || f.engine == "gamescope") return "FPS";
        return f.engine;
    }
    Run numUnit(const std::string &num, const std::string &unit, float numPx, float unitPx) const {
        if (num.empty()) return {{"—", COL_DIM, numPx}, {unit, COL_DIM, unitPx}};
        return {{num, COL_VALUE, numPx}, {unit, COL_DIM, unitPx}};
    }
    static std::string num(int v) { return v < 0 ? "" : std::to_string(v); }
    Run valueUnit(const std::string &text, float numPx, float unitPx) const {
        size_t i = 0;
        while (i < text.size() && (isdigit((unsigned char)text[i]) || text[i] == '.' || text[i] == '-')) i++;
        if (i == 0 || i == text.size()) return {{text, COL_VALUE, numPx}};
        return {{text.substr(0, i), COL_VALUE, numPx}, {text.substr(i), COL_DIM, unitPx}};
    }
    Span gap(float unitPx) const { return {"  ", COL_DIM, unitPx}; }
    Run tempSpans(float c, int red, float numPx, float unitPx) const {
        if (std::isnan(c)) return {};
        uint32_t col = COL_VALUE;
        if (bands) {
            const int amber = int(std::lround(red * 0.88f));
            col = c >= float(red) ? COL_TEMP_RED : c >= float(amber) ? COL_TEMP_AMBER : COL_TEMP_OK;
        }
        const float v = fahrenheit ? c * 9.0f / 5.0f + 32.0f : c;
        return {{std::to_string(int(std::lround(v))), col, numPx}, {fahrenheit ? "°F" : "°C", COL_DIM, unitPx}};
    }
    Run gpuValues(const Snapshot &s, float px, float unitPx) const {
        Run v = numUnit(num(s.gpu), "%", px, unitPx);
        if (showGpuTemp) {
            Run t = tempSpans(s.gpuTemp, redGpu, px, unitPx);
            if (!t.empty()) { v.push_back(gap(unitPx)); append(v, t); }
        }
        v.push_back(gap(unitPx));
        append(v, numUnit(num(s.gpuMhz), "MHz", px, unitPx));
        return v;
    }
    Run cpuValues(const Snapshot &s, float px, float unitPx) const {
        Run v = numUnit(num(s.cpu), "%", px, unitPx);
        if (showCpuTemp) {
            Run t = tempSpans(s.cpuTemp, redCpu, px, unitPx);
            if (!t.empty()) { v.push_back(gap(unitPx)); append(v, t); }
        }
        v.push_back(gap(unitPx));
        append(v, numUnit(num(s.cpuMhz), "MHz", px, unitPx));
        return v;
    }
    // BAT row of Full/Mega: percent, battery temperature, watts.
    bool batteryValues(const Snapshot &s, float px, float unitPx, Run &v) const {
        bool any = false;
        if (showBattery && s.bat >= 0) { append(v, numUnit(std::to_string(s.bat), "%", px, unitPx)); any = true; }
        if (showBatteryTemp) {
            Run t = tempSpans(s.batTemp, redBat, px, unitPx);
            if (!t.empty()) { if (any) v.push_back(gap(unitPx)); append(v, t); any = true; }
        }
        if (showPower && s.watts > 0) { if (any) v.push_back(gap(unitPx)); append(v, numUnit(fmt1(s.watts), "W", px, unitPx)); any = true; }
        return any;
    }
    void fpsRows(const FpsNow &f, float px, float unitPx, std::vector<HudRow> &rows) const {
        Run v = numUnit(fpsText(f.fps), "FPS", px, unitPx);
        v.push_back(gap(unitPx));
        append(v, numUnit(fmt1(1000.0f / std::max(f.fps, 1.0f)), "ms", px, unitPx));
        rows.push_back({{apiLabel(f), COL_FPS, px}, v});
        rows.push_back({{"AVG", COL_LO, px}, numUnit(fmt1(f.avg), "FPS", px, unitPx)});
        rows.push_back({{"1%", COL_LO, px}, numUnit(lowText(f.low1), "FPS", px, unitPx)});
        rows.push_back({{"0.1%", COL_LO, px}, numUnit(lowText(f.low01), "FPS", px, unitPx)});
        if (showLow001) rows.push_back({{"0.01%", COL_LO, px}, numUnit(lowText(f.low001), "FPS", px, unitPx)});
    }

    // FusionHudView.layoutColumn: a label column then values; returns (bottom, right).
    std::pair<float, float> layoutColumn(const std::vector<HudRow> &rows, float x, float y0, float px, float lineGap, float lvGap) {
        if (rows.empty()) return {y0, x};
        float labelCol = 0;
        for (const auto &r : rows) labelCol = std::max(labelCol, measure(r.label.text, px));
        const float h = lineH(px);
        float y = y0, right = x;
        for (const auto &r : rows) {
            const float baseline = y + ascent(px);
            placeRun(x, baseline, {r.label});
            right = std::max(right, placeRun(x + labelCol + lvGap, baseline, r.vals));
            y += h + lineGap;
        }
        return {y - lineGap, right};
    }

    // FusionHudView.layoutWrap: " · "-separated fragments wrapped at maxWidth; returns (bottom, right).
    std::pair<float, float> layoutWrap(const std::vector<Run> &frags, float x, float y0, float maxWidth, float px, float lineGap) {
        if (frags.empty()) return {y0, x};
        const float h = lineH(px), sepW = measure(" · ", px);
        float cx = x, y = y0, right = x;
        bool first = true;
        for (const auto &frag : frags) {
            const float w = runWidth(frag);
            if (!first && (cx + sepW + w - x) > maxWidth) { y += h + lineGap; cx = x; first = true; }
            if (!first) { placeRun(cx, y + ascent(px), {{" · ", COL_DIM, px}}); cx += sepW; }
            cx = placeRun(cx, y + ascent(px), frag);
            right = std::max(right, cx);
            first = false;
        }
        return {y + h, right};
    }

    void addSubtleClock(float pad, float centerX = -1) {
        if (!showClock) return;
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

    void build_full(const Snapshot &s, const FpsNow &f) {
        const float rowPx = sp(12), unitPx = rowPx * 0.62f, pad = sp(10), lineGap = sp(4), lvGap = sp(8);
        std::vector<HudRow> rows;
        if (showGpuModel && !s.gpuName.empty()) rows.push_back({{"GPU", COL_GPU, rowPx}, {{s.gpuName, COL_VALUE, rowPx}}});
        if (showGPU) rows.push_back({{"GPU", COL_GPU, rowPx}, gpuValues(s, rowPx, unitPx)});
        if (showCPU) rows.push_back({{"CPU", COL_CPU, rowPx}, cpuValues(s, rowPx, unitPx)});
        if (showVram && s.vram > 0) rows.push_back({{"VRAM", COL_VRAM, rowPx}, valueUnit(vramText(s.vram), rowPx, unitPx)});
        if (showRAM && s.ramUsed >= 0) {
            Run v = valueUnit(bytesGb(s.ramUsed), rowPx, unitPx);
            v.push_back(gap(unitPx));
            append(v, numUnit(std::to_string(int(std::lround(s.ramPct))), "%", rowPx, unitPx));
            rows.push_back({{"RAM", COL_RAM, rowPx}, v});
        }
        {
            Run v;
            if (batteryValues(s, rowPx, unitPx, v)) rows.push_back({{"BAT", COL_BAT, rowPx}, v});
        }
        if (showFPS) fpsRows(f, rowPx, unitPx, rows);

        float labelCol = 0;
        for (const auto &r : rows) labelCol = std::max(labelCol, measure(r.label.text, rowPx));
        auto [y, maxRight] = layoutColumn(rows, pad, pad, rowPx, lineGap, lvGap);
        if (!rows.empty()) y += lineGap;
        if (showFPS && showGraph) {
            const float ftPx = unitPx * 1.15f, h = lineH(rowPx);
            placeRun(pad, y + ascent(rowPx), {{"Frametime", COL_FPS, ftPx}});
            const std::string stat = "min:" + fmt1(f.minMs) + " max:" + fmt1(f.maxMs);
            const float statX = pad + std::max(labelCol, measure("Frametime", ftPx)) + lvGap;
            maxRight = std::max(maxRight, placeRun(statX, y + ascent(rowPx), {{stat, COL_DIM, unitPx}}));
            y += h + lineGap;
            const float gh = sp(22), right = std::max(maxRight, pad + sp(160));
            graphRect = true;
            gx0 = pad; gy0 = y; gx1 = right; gy1 = y + gh;
            maxRight = std::max(maxRight, right);
            y += gh;
        }
        contentW = maxRight + pad;
        contentH = y + pad;
        addSubtleClock(pad);
    }

    void build_tiles(const Snapshot &s, const FpsNow &f) {
        const float keyPx = sp(10), valPx = sp(18), subPx = sp(10), unitPx = sp(11);
        const float pad = sp(9), innerPad = sp(8), tileGap = sp(6), lineGap = sp(4);
        struct Tile {
            std::string key;
            uint32_t keyColor;
            Run value;
            std::string sub;
            bool wide;
        };
        std::vector<Tile> list;
        if (showFPS) {
            list.push_back({"FPS", COL_FPS, {{fpsText(f.fps), COL_VALUE, valPx}}, fmt1(f.avg) + " avg · " + lowText(f.low1) + " 1%", false});
            list.push_back({"FRAME", COL_DIM, numUnit(fmt1(1000.0f / std::max(f.fps, 1.0f)), "ms", valPx, unitPx),
                            fmt1(f.minMs) + " – " + fmt1(f.maxMs), false});
        }
        auto deg = [&](float c) {
            const float v = fahrenheit ? c * 9.0f / 5.0f + 32.0f : c;
            return std::to_string(int(std::lround(v))) + (fahrenheit ? "°F" : "°C");
        };
        if (showGPU)
            list.push_back({"GPU", COL_GPU, numUnit(num(s.gpu), "%", valPx, unitPx),
                            showGpuTemp && !std::isnan(s.gpuTemp) ? deg(s.gpuTemp) : (s.gpuMhz > 0 ? std::to_string(s.gpuMhz) + "MHz" : ""), false});
        if (showCPU) {
            std::string sub;
            if (showCpuTemp && !std::isnan(s.cpuTemp)) sub = deg(s.cpuTemp);
            if (s.cpuMhz > 0) sub += (sub.empty() ? "" : " · ") + std::to_string(s.cpuMhz);
            list.push_back({"CPU", COL_CPU, numUnit(num(s.cpu), "%", valPx, unitPx), sub, false});
        }
        if (showVram && s.vram > 0) list.push_back({"VRAM", COL_VRAM, valueUnit(vramText(s.vram), valPx, unitPx), "", false});
        if (showRAM && s.ramUsed >= 0)
            list.push_back({"RAM", COL_RAM, numUnit(std::to_string(int(std::lround(s.ramPct))), "%", valPx, unitPx),
                            bytesGb(s.ramUsed) + " / " + bytesGb(s.ramTotal), false});
        if (apiLabel(f) != "FPS") list.push_back({"API", COL_FPS, {{f.engine, COL_VALUE, valPx}}, "", false});
        if (showGpuModel && !s.gpuName.empty()) list.push_back({"GPU", COL_GPU, {{s.gpuName, COL_VALUE, valPx}}, "", true});
        {
            Run parts;
            bool any = false;
            if (showBattery && s.bat >= 0) { append(parts, numUnit(std::to_string(s.bat), "%", valPx, unitPx)); any = true; }
            if (showBatteryTemp && !std::isnan(s.batTemp)) {
                if (any) parts.push_back({" · ", COL_DIM, subPx});
                append(parts, tempSpans(s.batTemp, redBat, valPx, unitPx));
                any = true;
            }
            if (showPower && s.watts > 0) {
                if (any) parts.push_back({" · ", COL_DIM, subPx});
                append(parts, numUnit(fmt1(s.watts), "W", valPx, unitPx));
                any = true;
            }
            if (any) list.push_back({"BAT", COL_BAT, parts, "", true});
        }
        if (list.empty()) return;

        const float keyH = lineH(keyPx), valH = lineH(valPx), subH = lineH(subPx);
        auto tileW = [&](const Tile &t) {
            return std::max(std::max(measure(t.key, keyPx), runWidth(t.value)), t.sub.empty() ? 0.0f : measure(t.sub, subPx)) + innerPad * 2;
        };
        float normalW = 0;
        for (const auto &t : list) if (!t.wide) normalW = std::max(normalW, tileW(t));
        if (normalW == 0) for (const auto &t : list) normalW = std::max(normalW, tileW(t) / 2.0f);
        for (const auto &t : list) if (t.wide) normalW = std::max(normalW, (tileW(t) - tileGap) / 2.0f);
        const float tileH = innerPad * 2 + keyH + lineGap + valH + lineGap + subH;
        const float fullW = normalW * 2 + tileGap;
        auto place = [&](const Tile &t, float tx, float ty, float tw) {
            tiles.push_back(ImVec4(tx, ty, tx + tw, ty + tileH));
            placeRun(tx + innerPad, ty + innerPad + ascent(keyPx), {{t.key, t.keyColor, keyPx}});
            placeRun(tx + innerPad, ty + innerPad + keyH + lineGap + ascent(valPx), t.value);
            if (!t.sub.empty()) placeRun(tx + innerPad, ty + innerPad + keyH + lineGap + valH + lineGap + ascent(subPx), {{t.sub, COL_DIM, subPx}});
        };
        float x = pad, y = pad;
        int col = 0;
        for (const auto &t : list) {
            if (t.wide) {
                if (col != 0) { y += tileH + tileGap; col = 0; x = pad; }
                place(t, pad, y, fullW);
                y += tileH + tileGap;
            } else {
                place(t, x, y, normalW);
                col++;
                x += normalW + tileGap;
                if (col == 2) { col = 0; x = pad; y += tileH + tileGap; }
            }
        }
        if (col != 0) y += tileH + tileGap;
        contentW = pad + fullW + pad;
        contentH = y + (pad - tileGap);
        addSubtleClock(pad);
    }

    void build_mega(const Snapshot &s, const FpsNow &f) {
        const float rowPx = sp(11.5f), unitPx = rowPx * 0.62f, bandPx = sp(9.5f);
        const float pad = sp(10), lineGap = sp(3.5f), lvGap = sp(7), gutter = sp(16);
        std::vector<HudRow> left, right;
        if (showGpuModel && !s.gpuName.empty()) left.push_back({{"GPU", COL_GPU, rowPx}, {{s.gpuName, COL_VALUE, rowPx}}});
        if (showGPU) left.push_back({{"GPU", COL_GPU, rowPx}, gpuValues(s, rowPx, unitPx)});
        if (showCPU) left.push_back({{"CPU", COL_CPU, rowPx}, cpuValues(s, rowPx, unitPx)});
        if (showPerCore) {
            const size_t n = std::max(s.corePct.size(), s.coreMhz.size());
            for (size_t i = 0; i < n; i++) {
                Run v = numUnit(i < s.corePct.size() ? num(s.corePct[i]) : "", "%", rowPx, unitPx);
                v.push_back(gap(unitPx));
                append(v, numUnit(i < s.coreMhz.size() && s.coreMhz[i] > 0 ? std::to_string(s.coreMhz[i]) : "", "MHz", rowPx, unitPx));
                left.push_back({{"C" + std::to_string(i), COL_CPU, rowPx}, v});
            }
        }
        if (showVram && s.vram > 0) right.push_back({{"VRAM", COL_VRAM, rowPx}, valueUnit(vramText(s.vram), rowPx, unitPx)});
        if (showRAM && s.ramUsed >= 0) {
            Run v = valueUnit(bytesGb(s.ramUsed), rowPx, unitPx);
            v.push_back({"/", COL_DIM, unitPx});
            append(v, valueUnit(bytesGb(s.ramTotal), rowPx, unitPx));
            v.push_back(gap(unitPx));
            append(v, numUnit(std::to_string(int(std::lround(s.ramPct))), "%", rowPx, unitPx));
            right.push_back({{"RAM", COL_RAM, rowPx}, v});
        }
        if (showSwap && s.swapUsed >= 0) {
            Run v = valueUnit(bytesGb(s.swapUsed), rowPx, unitPx);
            v.push_back({"/", COL_DIM, unitPx});
            append(v, valueUnit(bytesGb(s.swapTotal), rowPx, unitPx));
            right.push_back({{"SWP", COL_RAM, rowPx}, v});
        }
        if (showNet && s.netDown >= 0) {
            right.push_back({{"NET", COL_FPS, rowPx},
                             {{"↓", COL_DIM, unitPx}, {std::to_string(s.netDown / 1024), COL_VALUE, rowPx},
                              {" ↑", COL_DIM, unitPx}, {std::to_string(s.netUp / 1024), COL_VALUE, rowPx}, {"KB/s", COL_DIM, unitPx}}});
        }
        {
            Run v;
            if (batteryValues(s, rowPx, unitPx, v)) right.push_back({{"BAT", COL_BAT, rowPx}, v});
        }
        if (showFPS) fpsRows(f, rowPx, unitPx, right);

        auto [leftBottom, leftRight] = layoutColumn(left, pad, pad, rowPx, lineGap, lvGap);
        const float rightX = left.empty() ? pad : leftRight + gutter;
        auto [rightBottom, rightRight] = layoutColumn(right, rightX, pad, rowPx, lineGap, lvGap);
        float y = std::max(leftBottom, rightBottom), maxRight = std::max(leftRight, rightRight);

        std::vector<Run> band;
        const std::string res = resolutionText(f);
        if (showResolution && !res.empty()) band.push_back({{"RES ", COL_DIM, bandPx}, {res, COL_VALUE, bandPx}});
        if (showProton && !s.proton.empty()) band.push_back({{s.proton, COL_VRAM, bandPx}});
        if (showSession) band.push_back({{"elapsed ", COL_DIM, bandPx}, {elapsedText(), COL_VALUE, bandPx}});
        if (!band.empty()) {
            y += sp(4);
            auto [bBottom, bRight] = layoutWrap(band, pad, y, std::max(maxRight - pad, sp(180)), bandPx, lineGap);
            y = bBottom;
            maxRight = std::max(maxRight, bRight);
        }
        if (showGraph) {
            y += sp(3);
            const float gh = sp(22), gr = std::max(maxRight, pad + sp(220));
            graphRect = true;
            gx0 = pad; gy0 = y; gx1 = gr; gy1 = y + gh;
            maxRight = std::max(maxRight, gr);
            y += gh;
        }
        contentW = maxRight + pad;
        contentH = y + pad;
        addSubtleClock(pad);
    }

    void build_pill(const Snapshot &s, const FpsNow &f) {
        capsule = true;
        const float bigPx = sp(30), bigUnitPx = bigPx * 0.36f, stkPx = sp(11.5f);
        const float pad = sp(10), midGap = sp(12), stkLineGap = sp(3);

        Run left = {{fpsText(f.fps), COL_VALUE, bigPx}, {"fps", COL_DIM, bigUnitPx}};

        std::vector<Run> stack;
        if (showGpuModel && !s.gpuName.empty()) stack.push_back({{s.gpuName, COL_DIM, stkPx}});
        {
            Run l;
            if (showGPU) l.push_back({"GPU " + (s.gpu >= 0 ? std::to_string(s.gpu) : std::string("—")) + "%", COL_GPU, stkPx});
            if (showCPU) {
                if (!l.empty()) l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({"CPU " + (s.cpu >= 0 ? std::to_string(s.cpu) : std::string("—")) + "%", COL_CPU, stkPx});
            }
            if (!l.empty()) stack.push_back(l);
        }
        if (showRAM && s.ramPct >= 0) {
            Run l = {{"RAM " + std::to_string(int(std::lround(s.ramPct))) + "%", COL_RAM, stkPx}};
            if (showBatteryTemp && !std::isnan(s.batTemp)) {
                l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({"BAT ", COL_BAT, stkPx});
                append(l, tempSpans(s.batTemp, redBat, stkPx, stkPx * 0.62f));
            }
            stack.push_back(l);
        }
        if (showBattery || showPower) {
            Run l;
            bool any = false;
            if (showBattery && s.bat >= 0) { l.push_back({"BAT " + std::to_string(s.bat) + "%", COL_BAT, stkPx}); any = true; }
            if (showPower && s.watts > 0) {
                if (any) l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({fmt1(s.watts) + "W", COL_DIM, stkPx});
                any = true;
            }
            if (any) stack.push_back(l);
        }
        {
            Run l = {{fmt1(1000.0f / std::max(f.fps, 1.0f)) + "ms", COL_DIM, stkPx}};
            if (showVram && s.vram > 0) {
                l.push_back({" · ", COL_DIM, stkPx});
                l.push_back({vramText(s.vram) + " vram", COL_DIM, stkPx});
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
        if (showLow001) {
            sub.push_back({"  ·  0.01% ", COL_DIM, subPx});
            sub.push_back({lowText(f.low001), COL_FPS, subPx});
        }
        const float bigW = runWidth(big), bigH = lineH(bigPx), subW = runWidth(sub), subH = lineH(subPx);
        const float gw = sp(120), gh = sp(22);
        const float inner = std::max(std::max(bigW, subW), showGraph ? gw : 0.0f);
        contentW = inner + pad * 2;
        float y = pad;
        placeRun(pad + (inner - bigW) / 2.0f, y + ascent(bigPx), big);
        y += bigH + lineGap;
        placeRun(pad + (inner - subW) / 2.0f, y + ascent(subPx), sub);
        y += subH + lineGap;
        if (showGraph) {
            graphRect = true;
            gx0 = pad + (inner - gw) / 2.0f;
            gy0 = y;
            gx1 = gx0 + gw;
            gy1 = y + gh;
            y += gh;
        }
        contentH = y + pad;
        const std::string footer = apiLabel(f) != "FPS" ? apiLabel(f) : "";
        if (!footer.empty()) {
            const float apiPx = sp(9.5f);
            const float footerTop = contentH - pad;
            glyphs.push_back({pad, footerTop + ascent(apiPx), footer, blend(COL_DIM), apiPx});
            const float clockW = showClock ? measure(clockText(), apiPx) : 0;
            contentW = std::max(contentW, pad + measure(footer, apiPx) + sp(12) + clockW + pad);
            if (!showClock) contentH = footerTop + lineH(apiPx) + pad * 0.5f;
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
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0};  // Latin-1, dashes and bullets, arrows
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
    Snapshot snap = metrics.snapshot(0);
    uint64_t lastMetrics = 0, lastConfigCheck = 0;
    const uint64_t startNs = now_ns();
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
            snap = metrics.snapshot(fps_now().pid);
        }

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        const float screenScale = float(std::max(fbH, 1)) / 1080.0f;
        hud.build(snap, fps_now(), screenScale, float(t - startNs) / 1e9f);

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

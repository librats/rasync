#include "app/terminal.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rasync::term {
namespace {

bool g_color = false;

bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

std::string wrap(const std::string& s, const char* code) {
    if (!g_color) return s;
    return std::string("\x1b[") + code + "m" + s + "\x1b[0m";
}

} // namespace

void init(bool force_no_color) {
#ifdef _WIN32
    // UTF-8 output + enable ANSI escape processing on the console.
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/);
#endif
    const char* no_color = std::getenv("NO_COLOR");
    g_color = !force_no_color && !no_color && stdout_is_tty();
}

bool colors_enabled() { return g_color; }

std::string bold(const std::string& s)    { return wrap(s, "1"); }
std::string dim(const std::string& s)     { return wrap(s, "2"); }
std::string red(const std::string& s)     { return wrap(s, "31"); }
std::string green(const std::string& s)   { return wrap(s, "32"); }
std::string yellow(const std::string& s)  { return wrap(s, "33"); }
std::string blue(const std::string& s)    { return wrap(s, "34"); }
std::string magenta(const std::string& s) { return wrap(s, "35"); }
std::string cyan(const std::string& s)    { return wrap(s, "36"); }
std::string gray(const std::string& s)    { return wrap(s, "90"); }

std::string bytes(uint64_t n) {
    static const std::array<const char*, 6> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(n);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < units.size()) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(n), units[u]);
    else        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

std::string rate(double bytes_sec) {
    if (bytes_sec < 1.0) return "0 B/s";
    return bytes(static_cast<uint64_t>(bytes_sec)) + "/s";
}

std::string duration(uint64_t secs) {
    char buf[32];
    if (secs < 60)        std::snprintf(buf, sizeof(buf), "%llus", static_cast<unsigned long long>(secs));
    else if (secs < 3600) std::snprintf(buf, sizeof(buf), "%llum %02llus",
                                        static_cast<unsigned long long>(secs / 60),
                                        static_cast<unsigned long long>(secs % 60));
    else                  std::snprintf(buf, sizeof(buf), "%lluh %02llum",
                                        static_cast<unsigned long long>(secs / 3600),
                                        static_cast<unsigned long long>((secs % 3600) / 60));
    return buf;
}

std::string percent_bar(double pct, int width) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = static_cast<int>(std::lround(pct / 100.0 * width));
    std::string bar = "[";
    for (int i = 0; i < width; ++i) bar += (i < filled) ? '#' : '-';
    char tail[16];
    std::snprintf(tail, sizeof(tail), "] %3d%%", static_cast<int>(std::lround(pct)));
    return bar + tail;
}

} // namespace rasync::term

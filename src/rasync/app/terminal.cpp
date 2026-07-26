#include "app/terminal.h"

#include <array>
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

std::string bold(const std::string& s)    { return wrap(s, "1"); }
std::string dim(const std::string& s)     { return wrap(s, "2"); }
std::string red(const std::string& s)     { return wrap(s, "31"); }
std::string green(const std::string& s)   { return wrap(s, "32"); }
std::string yellow(const std::string& s)  { return wrap(s, "33"); }
std::string blue(const std::string& s)    { return wrap(s, "34"); }
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

} // namespace rasync::term

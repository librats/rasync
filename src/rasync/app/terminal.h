#pragma once

/**
 * @file terminal.h
 * @brief Small, dependency-free terminal styling and human-readable formatting.
 *
 * Everything the CLI prints goes through here so colour can be switched off in one
 * place (a non-TTY, `--no-color`, or the `NO_COLOR` convention) and so sizes/rates
 * render consistently. On Windows, `init()` flips the console into UTF-8 + VT mode
 * so the same ANSI codes and glyphs work as on POSIX.
 */

#include <cstdint>
#include <string>

namespace rasync::term {

/// Prepare the console (UTF-8 + ANSI on Windows) and auto-detect colour support.
/// `force_no_color` wins regardless of TTY detection.
void init(bool force_no_color = false);

// — styling (no-ops when colour is disabled) —
std::string bold(const std::string& s);
std::string dim(const std::string& s);
std::string red(const std::string& s);
std::string green(const std::string& s);
std::string yellow(const std::string& s);
std::string blue(const std::string& s);
std::string cyan(const std::string& s);
std::string gray(const std::string& s);

// — human-readable formatting —
std::string bytes(uint64_t n);       ///< e.g. "4.0 MiB"

} // namespace rasync::term

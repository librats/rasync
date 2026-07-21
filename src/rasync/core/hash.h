#pragma once

/**
 * @file hash.h
 * @brief Content hashing (SHA-256) for files and byte buffers.
 *
 * A file's identity in rasync is the SHA-256 of its contents. Two files with the
 * same hash are considered identical, which is what lets the diff engine skip
 * transfers, detect renames, and recognise already-converged files. Hashing a
 * file streams it in blocks so an arbitrarily large file costs O(1) memory.
 *
 * The implementation reuses librats' vendored SHA-256 (the same primitive its
 * FileTransfer subsystem verifies transfers with), so rasync adds no crypto code.
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace rasync {

using Hash = std::array<uint8_t, 32>;

/// Lowercase hex (64 chars) of a hash.
std::string to_hex(const Hash& h);

/// Parse 64 hex chars into a hash; nullopt if malformed.
std::optional<Hash> hash_from_hex(const std::string& hex);

/// SHA-256 of an in-memory buffer.
Hash sha256(const void* data, size_t size);
inline Hash sha256(const std::string& s) { return sha256(s.data(), s.size()); }

/// SHA-256 of a file's contents, streamed. nullopt if the file cannot be read.
std::optional<Hash> sha256_file(const std::string& path);

/// The canonical hash of an empty input (handy sentinel / test vector).
Hash empty_hash();

} // namespace rasync

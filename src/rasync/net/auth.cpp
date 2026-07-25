#include "net/auth.h"

#include "core/hash.h"
#include "version.h"

namespace rasync {
namespace {

/// 128-bit tag over `label`-then-key. The separator is a NUL because a label is
/// never allowed to contain one, so no (label, key) pair can be re-split into a
/// different pair with the same bytes — that is what keeps the two derivations
/// from ever colliding.
std::string tag(const char* label, const std::string& key) {
    std::string input(label);
    input.push_back('\0');
    input += key;
    return to_hex(sha256(input)).substr(0, 32);
}

} // namespace

std::string protocol_id(const std::string& key) {
    if (key.empty()) return kProtocolName;
    return std::string(kProtocolName) + "+" + tag("rasync-psk", key);
}

std::string discovery_id(const std::string& key) {
    if (key.empty()) return kProtocolName;
    return "rasync:" + tag("rasync-discovery", key);
}

} // namespace rasync

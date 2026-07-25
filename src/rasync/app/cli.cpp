#include "app/cli.h"

#include "app/terminal.h"
#include "version.h"

#include <cctype>
#include <sstream>

namespace rasync {
namespace {

bool parse_uint(const std::string& s, unsigned long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtoul(s.c_str(), &end, 10);
    return end && *end == '\0';
}

/// A peer id as printed in the banner: the 64 hex chars of a SHA-256. Checked
/// here so a typo is a startup error instead of a peer that silently never
/// matches the allow-list.
bool is_peer_id(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

} // namespace

std::string help_text(const std::string& prog) {
    using namespace term;
    std::ostringstream o;
    o << bold(cyan("rasync")) << " " << kVersion
      << " — real-time two-way directory sync over P2P\n\n";
    o << bold("USAGE\n");
    o << "  " << prog << " [options] " << bold("<directory>") << "\n\n";
    o << bold("CONNECTION\n");
    o << "  " << green("-p, --port") << " <n>         listen on TCP port <n> (default: ephemeral)\n";
    o << "  " << green("    --peer") << " <host:port>  dial a specific peer (repeatable)\n";
    o << "  " << green("    --key") << " <secret>      shared secret: peers must match it to connect\n";
    o << "                            (also keys auto-discovery over DHT + mDNS)\n";
    o << "  " << green("    --allow") << " <peer-id>    only sync with this peer id (64 hex, repeatable)\n";
    o << "  " << green("    --discover") << "           enable DHT + mDNS discovery without a key\n";
    o << "  " << green("    --lan") << "                discover on the local network only (mDNS)\n\n";
    o << bold("SYNC BEHAVIOUR\n");
    o << "  " << green("    --mirror") << "             one-way mirror instead of two-way merge\n";
    o << "  " << green("    --source") << " / " << green("--replica") << "  mirror role (source is authoritative)\n";
    o << "  " << green("    --conflict") << " <policy>  newer|larger|local|remote (default: newer)\n";
    o << "                            (peers must pair: newer/newer, larger/larger,\n";
    o << "                             local/remote — a mismatch is refused)\n";
    o << "  " << green("    --no-delete") << "          never delete files (additive sync)\n";
    o << "  " << green("    --no-delta") << "           send whole files instead of rsync deltas\n";
    o << "  " << green("    --ignore") << " <pattern>   exclude paths (gitignore syntax, repeatable)\n";
    o << "  " << green("    --ignore-file") << " <path> load ignore patterns from a file\n";
    o << "                            (default: <directory>/.rasyncignore)\n\n";
    o << bold("RUN MODE\n");
    o << "  " << green("    --interval") << " <secs>    rescan cadence for change detection (default: 3)\n";
    o << "  " << green("    --once") << "               reconcile once with connected peers, then exit\n";
    o << "  " << green("    --data-dir") << " <path>    state directory (default: <directory>/.rasync)\n\n";
    o << bold("OUTPUT\n");
    o << "  " << green("-v, --verbose") << "            show every file operation\n";
    o << "  " << green("-q, --quiet") << "              only warnings and errors\n";
    o << "  " << green("    --no-color") << "           disable coloured output\n";
    o << "  " << green("-h, --help") << "               show this help\n";
    o << "  " << green("    --version") << "            show version\n\n";
    o << bold("EXAMPLES\n");
    o << dim("  # Host A: listen and share ./data\n");
    o << "  " << prog << " -p 9000 ./data\n";
    o << dim("  # Host B: connect to A and keep ./data identical\n");
    o << "  " << prog << " --peer a.example.com:9000 ./data\n";
    o << dim("  # Both hosts: find each other automatically, and only each other,\n");
    o << dim("  # by a shared secret (a peer without it cannot even handshake)\n");
    o << "  " << prog << " --key s3cr3t-passphrase ./data\n";
    o << dim("  # One-way backup mirror (source pushes, replica follows)\n");
    o << "  " << prog << " --mirror --source -p 9000 ./data      " << dim("(on the source)") << "\n";
    o << "  " << prog << " --mirror --replica --peer host:9000 ./backup\n";
    return o.str();
}

ParseResult parse_args(int argc, char** argv) {
    ParseResult res;
    Options& o = res.options;

    auto fail = [&](const std::string& msg) {
        res.action = ParseResult::Error;
        res.error = msg;
        return res;
    };

    std::vector<std::string> positionals;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Split --opt=value.
        std::string value;
        bool has_inline = false;
        if (arg.rfind("--", 0) == 0) {
            auto eq = arg.find('=');
            if (eq != std::string::npos) { value = arg.substr(eq + 1); arg = arg.substr(0, eq); has_inline = true; }
        }
        auto next = [&](const std::string& name) -> std::string {
            if (has_inline) return value;
            if (i + 1 >= argc) { fail("option " + name + " needs a value"); return ""; }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help")        { res.action = ParseResult::Help; return res; }
        else if (arg == "--version")               { res.action = ParseResult::Version; return res; }
        else if (arg == "-p" || arg == "--port") {
            unsigned long p = 0;
            if (!parse_uint(next(arg), p) || p > 65535) return fail("invalid --port");
            o.port = static_cast<uint16_t>(p);
        }
        else if (arg == "--peer")                  o.peers.push_back(next(arg));
        else if (arg == "--key")                 { o.key = next(arg); o.discover = true; }
        else if (arg == "--allow") {
            std::string id = next(arg);
            if (res.action == ParseResult::Error) return res;
            if (!is_peer_id(id)) return fail("--allow expects a 64-character hex peer id, got: " + id);
            o.allow.push_back(id);
        }
        else if (arg == "--discover")              o.discover = true;
        else if (arg == "--lan")                 { o.lan_only = true; o.discover = true; }
        else if (arg == "--mirror")                o.mode = SyncMode::Mirror;
        else if (arg == "--source")                o.source = true;
        else if (arg == "--replica")               o.replica = true;
        else if (arg == "--conflict") {
            std::string c = next(arg);
            if (c == "newer") o.conflict = ConflictPolicy::Newer;
            else if (c == "larger") o.conflict = ConflictPolicy::Larger;
            else if (c == "local") o.conflict = ConflictPolicy::PreferLocal;
            else if (c == "remote") o.conflict = ConflictPolicy::PreferRemote;
            else return fail("unknown --conflict policy: " + c);
        }
        else if (arg == "--no-delete")             o.no_delete = true;
        else if (arg == "--no-delta")              o.no_delta = true;
        else if (arg == "--ignore")                o.ignores.push_back(next(arg));
        else if (arg == "--ignore-file")           o.ignore_file = next(arg);
        else if (arg == "--interval") {
            unsigned long s = 0;
            if (!parse_uint(next(arg), s) || s == 0) return fail("invalid --interval");
            o.interval = static_cast<int>(s);
        }
        else if (arg == "--once")                  o.once = true;
        else if (arg == "--data-dir")              o.data_dir = next(arg);
        else if (arg == "--no-color")              o.no_color = true;
        else if (arg == "-v" || arg == "--verbose") o.verbosity = 2;
        else if (arg == "-q" || arg == "--quiet")   o.verbosity = 0;
        else if (!arg.empty() && arg[0] == '-')     return fail("unknown option: " + arg);
        else                                        positionals.push_back(arg);

        if (res.action == ParseResult::Error) return res;
    }

    if (positionals.empty()) return fail("missing <directory> to sync");
    if (positionals.size() > 1) return fail("expected a single <directory>, got " +
                                            std::to_string(positionals.size()));
    o.directory = positionals.front();

    // Validate mirror role coherence.
    if (o.mode == SyncMode::Mirror && !o.source && !o.replica)
        return fail("--mirror requires --source or --replica");
    if (o.source && o.replica)
        return fail("--source and --replica are mutually exclusive");
    if (o.mode == SyncMode::TwoWay && (o.source || o.replica))
        return fail("--source/--replica only apply with --mirror");

    return res;
}

} // namespace rasync

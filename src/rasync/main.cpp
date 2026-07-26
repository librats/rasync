#include "app/cli.h"
#include "app/daemon.h"
#include "app/terminal.h"
#include "version.h"

#include <csignal>
#include <iostream>
#include <string>

namespace {

void on_signal(int) { rasync::request_stop(); }

std::string basename_of(const char* path) {
    std::string p = path ? path : "rasync";
    auto slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
    using namespace rasync;
    const std::string prog = basename_of(argc > 0 ? argv[0] : "rasync");

    ParseResult pr = parse_args(argc, argv);
    switch (pr.action) {
        case ParseResult::Help:
            term::init(false);
            std::cout << help_text(prog);
            return 0;
        case ParseResult::Version:
            // The describe string is what a bug report needs — it names the exact
            // tree, dirty included — so it goes here rather than in the banner.
            std::cout << "rasync " << kVersion;
            if (std::string(kGitDescribe) != "unknown") std::cout << " (" << kGitDescribe << ")";
            std::cout << "\n";
            return 0;
        case ParseResult::Error:
            term::init(false);
            std::cerr << term::red("error: ") << pr.error << "\n\n"
                      << "Run '" << prog << " --help' for usage.\n";
            return 2;
        case ParseResult::Run:
            break;
    }

    std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    std::signal(SIGTERM, on_signal);
#endif

    return run_daemon(pr.options);
}

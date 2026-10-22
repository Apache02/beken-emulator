// The emulator executable: builds a machine from a profile, loads images,
// connects UART1/UART2 to host endpoints and runs until stopped.

#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>

#include "app/cli.h"
#include "app/endpoints.h"
#include "app/registry.h"
#include "core/loader.h"
#include "core/log.h"
#include "core/machine.h"
#include "core/profile_loader.h"
#include "periph/uart.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void request_stop(int) { g_stop = 1; }

// Installs the stop handlers for SIGINT and SIGTERM and puts the previous ones
// back when destroyed.
class StopSignals {
  public:
    StopSignals() {
        struct sigaction sa {};
        sa.sa_handler = request_stop;
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, &old_int_);
        ::sigaction(SIGTERM, &sa, &old_term_);
    }
    ~StopSignals() {
        ::sigaction(SIGINT, &old_int_, nullptr);
        ::sigaction(SIGTERM, &old_term_, nullptr);
    }
    StopSignals(const StopSignals &)            = delete;
    StopSignals &operator=(const StopSignals &) = delete;

  private:
    struct sigaction old_int_ {
    }, old_term_{};
};

std::filesystem::path executable_path(const char *argv0) {
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe;
    return std::filesystem::absolute(argv0, ec);
}

int run(int argc, char **argv) {
    bk::CliOptions options = bk::parse_cli(argc, argv);
    if (options.help) {
        std::cout << bk::usage() << std::flush;
        return 0;
    }
    bk::Logger::configure(
        std::cerr,
        bk::colour_enabled(::isatty(STDERR_FILENO) != 0, std::getenv("NO_COLOR"), std::getenv("FORCE_COLOR")));
    for (const std::string &d : options.debug) bk::Logger::set_debug(d);

    bk::Profile                 profile = bk::load_profile(options.profile, bk::profile_dir(executable_path(argv[0])));
    bk::Machine                 machine(profile, bk::builtin_registry());
    std::vector<bk::LoadAction> loads = bk::plan_loads(machine, options.loads);
    bk::apply(loads);

    // A write to a closed stdout must not kill the emulator.
    std::signal(SIGPIPE, SIG_IGN);
    // Installed before the endpoints exist, so a stop request can never leave a
    // terminal in raw mode; the run loop below checks the flag before each step.
    StopSignals stop_signals;

    // Declared after the machine: destroyed (and terminals restored) first.
    std::map<std::string, std::unique_ptr<bk::ByteEndpoint>> endpoints;
    for (const auto &[name, spec] : {
             std::pair{std::string("UART1"), options.uart1},
             std::pair{std::string("UART2"), options.uart2}
    }) {
        auto *uart = dynamic_cast<bk::Uart *>(machine.module(name));
        if (!uart) continue;
        auto endpoint = bk::make_endpoint(spec);
        uart->attach(endpoint.get());
        std::cout << name << ": " << endpoint->description() << std::endl;
        endpoints[name] = std::move(endpoint);
    }

    machine.cpu().reset(0);
    machine.scheduler().run_forever([] { return g_stop != 0; });

    // Detach before the endpoints go away.
    for (const auto &[name, endpoint] : endpoints)
        if (auto *uart = dynamic_cast<bk::Uart *>(machine.module(name))) uart->attach(nullptr);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::cout.flush();
        std::cerr << "error: " << e.what() << std::endl;
        return 2;
    }
}

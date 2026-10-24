// The emulator executable: builds a machine from a profile, loads images,
// attaches the requested I2C devices and the WiFi network, connects every UART
// to a host endpoint and runs until stopped.

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
#include "i2c/catalog.h"
#include "net/network.h"
#include "periph/i2c1.h"
#include "periph/uart.h"
#include "periph/wifi/mac.h"

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
    for (const auto &[name, level] : options.log_levels) bk::Logger::set_level(name, level);

    bk::Profile profile = bk::load_profile(options.profile, bk::profile_dir(executable_path(argv[0])));
    // The devices outlive the machine that talks to them.
    std::vector<std::pair<std::string, std::unique_ptr<bk::I2cDevice>>> i2c_devices;
    for (const auto &[bus, spec] : options.i2c) i2c_devices.emplace_back(bus, bk::parse_device_spec(spec));
    bk::Machine                 machine(profile, bk::builtin_registry());
    std::vector<bk::LoadAction> loads = bk::plan_loads(machine, options.loads);
    bk::apply(loads);
    for (const auto &[bus, device] : i2c_devices) {
        auto *controller = dynamic_cast<bk::I2c1 *>(machine.module(bus));
        if (!controller) throw std::invalid_argument("no I2C bus named " + bus + " in profile " + profile.name);
        controller->attach(device.get());
    }

    // Declared after the machine, so it goes first; the MAC is detached from it
    // before that.
    std::unique_ptr<bk::Network> network;
    bk::WifiMac                 *mac = nullptr;
    if (!options.publish.empty() || options.ssid) {
        mac = dynamic_cast<bk::WifiMac *>(machine.module("MAC"));
        if (!mac)
            throw std::invalid_argument(std::string(options.publish.empty() ? "--ssid" : "--publish") +
                                        " needs a profile with a WiFi MAC module named MAC");
        network = std::make_unique<bk::Network>(options.publish, bk::Network::kDefaultMac, options.ssid);
        mac->attach(network.get());
        for (const bk::Publish &p : network->addresses()) std::cout << bk::describe(p) << std::endl;
    }

    // A write to a closed stdout must not kill the emulator.
    std::signal(SIGPIPE, SIG_IGN);
    // Installed before the endpoints exist, so a stop request can never leave a
    // terminal in raw mode; the run loop below checks the flag before each step.
    StopSignals stop_signals;

    // Declared after the machine: destroyed (and terminals restored) first.
    std::map<std::string, std::unique_ptr<bk::ByteEndpoint>> endpoints;
    for (const auto &[name, spec] : options.uart) {
        if (!dynamic_cast<bk::Uart *>(machine.module(name)))
            throw std::invalid_argument("no UART named " + name + " in profile " + profile.name);
    }
    for (const bk::ModuleSpec &m : profile.modules) {
        auto *uart = dynamic_cast<bk::Uart *>(machine.module(m.name));
        if (!uart) continue;
        const std::string &name     = m.name;
        auto               given    = options.uart.find(name);
        auto               endpoint = bk::make_endpoint(given == options.uart.end() ? "pty" : given->second);
        uart->attach(endpoint.get());
        std::cout << name << ": " << endpoint->description() << std::endl;
        endpoints[name] = std::move(endpoint);
    }

    machine.cpu().reset(0);
    machine.scheduler().run_forever([] { return g_stop != 0; });

    if (network) {
        mac->attach(nullptr);
        network->close();
    }
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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/net/ZmqBroker.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitRuntimeError = 3;

std::atomic<bool> g_stop{false};

void HandleSignal(int) {
    g_stop.store(true);
}

struct Options {
    std::string frontend = "tcp://*:5555";
    std::string backend = "tcp://*:5556";
    int hwm = 10'000;
    int durationSeconds = 0;
    int statsIntervalSeconds = 10;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT ZeroMQ broker " << tmcmt::kVersion << "\n\n"
              << "XSUB/XPUB forwarder between Edge publishers and Master subscribers.\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --frontend <endpoint>  Publishers connect here (default: tcp://*:5555)\n"
              << "  --backend <endpoint>   Subscribers connect here (default: tcp://*:5556)\n"
              << "  --hwm <messages>       Send/receive high-water mark (default: 10000)\n"
              << "  --duration <seconds>   Stop after this many seconds (0 = until Ctrl+C)\n"
              << "  --stats-interval <s>   Print statistics every N seconds (default: 10)\n"
              << "  --version              Print version and exit\n"
              << "  -h, --help             Print this help and exit\n";
}

std::optional<Options> ParseArgs(int argc, char** argv, std::string& error) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextValue = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--frontend") {
            auto value = nextValue("--frontend");
            if (!value) {
                return std::nullopt;
            }
            options.frontend = std::move(*value);
        } else if (arg == "--backend") {
            auto value = nextValue("--backend");
            if (!value) {
                return std::nullopt;
            }
            options.backend = std::move(*value);
        } else if (arg == "--hwm") {
            auto value = nextValue("--hwm");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.hwm = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid --hwm value '" + *value + "'";
                return std::nullopt;
            }
        } else if (arg == "--duration") {
            auto value = nextValue("--duration");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.durationSeconds = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid --duration value '" + *value + "'";
                return std::nullopt;
            }
        } else if (arg == "--stats-interval") {
            auto value = nextValue("--stats-interval");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.statsIntervalSeconds = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid --stats-interval value '" + *value + "'";
                return std::nullopt;
            }
        } else {
            error = "unknown option '" + arg + "'";
            return std::nullopt;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    auto options = ParseArgs(argc, argv, parseError);
    if (!options) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argv[0]);
        return kExitUsageError;
    }
    if (options->showHelp) {
        PrintUsage(argv[0]);
        return kExitOk;
    }
    if (options->showVersion) {
        std::cout << tmcmt::kVersion << '\n';
        return kExitOk;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    tmcmt::net::ZmqBrokerOptions brokerOptions;
    brokerOptions.frontend_endpoint = options->frontend;
    brokerOptions.backend_endpoint = options->backend;
    brokerOptions.hwm = options->hwm;

    tmcmt::net::ZmqBroker broker;
    const auto status = broker.Start(brokerOptions);
    if (!status.ok()) {
        std::cerr << "error: " << status.ToString() << '\n';
        return kExitRuntimeError;
    }

    std::cout << "broker: publishers -> " << broker.frontend_endpoint() << '\n'
              << "broker: subscribers -> " << broker.backend_endpoint() << '\n'
              << std::flush;

    const auto start = std::chrono::steady_clock::now();
    const auto statsInterval = std::chrono::seconds(options->statsIntervalSeconds > 0
                                                        ? options->statsIntervalSeconds
                                                        : 10);
    auto nextStats = start + statsInterval;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStats) {
            const auto& stats = broker.stats();
            std::cout << "broker: forwarded=" << stats.forwarded_to_subscribers
                      << " subscriptions=" << stats.forwarded_to_publishers
                      << " bytes=" << stats.bytes_forwarded << " errors=" << stats.errors << '\n'
                      << std::flush;
            nextStats = now + statsInterval;
        }
        if (options->durationSeconds > 0 &&
            now - start >= std::chrono::seconds(options->durationSeconds)) {
            break;
        }
    }

    const tmcmt::net::ZmqBrokerStats finalStats = broker.stats();
    broker.Stop();
    std::cout << "broker: stopped after " << std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << " s, forwarded=" << finalStats.forwarded_to_subscribers
              << " subscriptions=" << finalStats.forwarded_to_publishers << " bytes=" << finalStats.bytes_forwarded
              << " errors=" << finalStats.errors << '\n';
    return kExitOk;
}

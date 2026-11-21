#include "logging.h"

#include <iostream>
#include <print>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dup_filter_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sstream>

#include "../zdon_record.h"
#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <vector>
#if defined(YATAIDON_HAS_CPPTRACE)
#include <cpptrace/cpptrace.hpp>
#endif

#include "../../platform/platform.h"

static void log_stacktrace() {
#if defined(YATAIDON_HAS_CPPTRACE)
    try {
        const cpptrace::raw_trace raw = cpptrace::generate_raw_trace(1);
        std::ostringstream oss;
        raw.resolve().print(oss, false);
        // Z-DON: each frame as module + address too. A release exe has no symbol table (stripped when published,
        // 10 MB less) so the names above are missing; these addresses (the image's own, relocation undone) still
        // point into the unstripped copy the server keeps (zdon_data/symbols/<version>): look them up with
        // `nm -C --numeric-sort YataiDON.exe`.
        oss << "\nobject addresses:\n";
        for (const cpptrace::object_frame& f : raw.resolve_object_trace().frames) {
            const std::string_view path = f.object_path;
            const size_t slash = path.find_last_of("/\\");
            oss << std::format("  {} 0x{:x}\n", slash == std::string_view::npos ? path : path.substr(slash + 1),
                               f.object_address);
        }
        spdlog::critical("Stack trace:\n{}", oss.str());
        spdlog::default_logger()->flush();
    } catch (...) {
    }
#endif
}

void handle_exception() {
    try {
        auto exception_ptr = std::current_exception();
        if (exception_ptr) {
            std::rethrow_exception(exception_ptr);
        }
    } catch (const std::exception& e) {
        spdlog::critical("Uncaught exception: {}", e.what());
    } catch (...) {
        spdlog::critical("Uncaught exception of unknown type");
    }
    log_stacktrace();
    std::_Exit(1);
}

static void install_crash_handlers() {
    std::set_terminate(handle_exception);
    platform_install_crash_handlers();
}

static spdlog::level::level_enum parse_log_level(const std::string& log_level_str) {
    if (log_level_str == "off") {
        return spdlog::level::off;
    }
    if (log_level_str == "debug") {
        return spdlog::level::debug;
    }
    if (log_level_str == "info") {
        return spdlog::level::info;
    }
    if (log_level_str == "warning") {
        return spdlog::level::warn;
    }
    if (log_level_str == "error") {
        return spdlog::level::err;
    }
    if (log_level_str == "critical") {
        return spdlog::level::critical;
    }
    std::println(stderr, "Unknown log level '{}'; using info", log_level_str);
    return spdlog::level::info;
}

static void apply_flush_policy() {
    spdlog::flush_on(spdlog::level::critical);
    // Without a periodic flush the file trails several seconds behind
    // the game, which reads like a freeze wherever the log happens to
    // stop mid-line.
    spdlog::flush_every(std::chrono::seconds(1));
}

static void install_logger(const std::shared_ptr<spdlog::logger>& logger, const std::string& log_level_str) {
    logger->set_level(parse_log_level(log_level_str));
    spdlog::set_default_logger(logger);
    apply_flush_policy();

    install_crash_handlers();
}

static void setup_fallback_logging(const std::string& log_level_str) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%^%l%$] %n: %v");
    install_logger(std::make_shared<spdlog::logger>("", console_sink), log_level_str);
}

void setup_logging(const std::string& log_level_str) {
    if (log_level_str == "off") {
        auto logger = std::make_shared<spdlog::logger>("", std::make_shared<spdlog::sinks::null_sink_mt>());
        logger->set_level(spdlog::level::off);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(0));
        install_crash_handlers();
        return;
    }
    try {
#ifndef __ANDROID__
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%^%l%$] %n: %v");
#endif

        // Z-DON: a recording (--record, often a child of the running game) keeps the game's latest.log
        const std::string log_name = !zdon_record::active() ? "latest.log"
                                     : zdon_record::part() >= 0
                                         ? "record_part" + std::to_string(zdon_record::part()) + ".log"
                                         : "record.log";
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_name, true);
        file_sink->set_pattern("[%H:%M:%S.%e] [%l] %n: %v");

        auto dup_filter = std::make_shared<spdlog::sinks::dup_filter_sink_mt>(std::chrono::seconds(5));
        dup_filter->add_sink(file_sink);

#ifdef __ANDROID__
        auto android_sink = std::make_shared<spdlog::sinks::android_sink_mt>("YataiDON");
        std::vector<spdlog::sink_ptr> sinks{android_sink, dup_filter};
#else
        std::vector<spdlog::sink_ptr> sinks{console_sink, dup_filter};
#endif
        install_logger(std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end()), log_level_str);

    } catch (const std::exception& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << " -- falling back to console-only logging"
                  << std::endl;
        setup_fallback_logging(log_level_str);
    } catch (...) {
        std::cerr << "Log initialization failed with unknown exception -- falling back to console-only logging"
                  << std::endl;
        setup_fallback_logging(log_level_str);
    }
}

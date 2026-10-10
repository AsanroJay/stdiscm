// Variant 2 - WAIT UNTIL ALL THREADS ARE DONE, THEN PRINT + STRAIGHT RANGE DIVISION
//
// Build:  g++ -O2 -o primes main.cpp -pthread
// Run:    ./primes            (Windows: primes.exe) - reads config.txt
//         ./primes other.txt  - reads another config file
//
// Same division of work as Variant 1: [1, y] is cut into x contiguous blocks.
// The difference is that worker threads never touch the console. Each thread
// records every prime it finds (plus the moment it found it) in its OWN vector,
// so no lock is needed while searching. After every thread has been joined,
// the main thread prints all results in one pass.
//
// The timestamps printed are the times the primes were FOUND, which lets you
// compare discovery order with the (sorted) print order.

// ===========================================================================
// Shared helpers (identical in all four variants): config file, timestamps,
// primality test, thread-id formatting, start/end banners.
// ===========================================================================
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace common {

// ---------------------------------------------------------------------------
// Configuration (read from config.txt, "key = value", '#' starts a comment)
// ---------------------------------------------------------------------------
struct Config {
    int           threads     = 4;        // x : number of worker threads
    std::uint64_t limit       = 1000;     // y : search for primes in [1, y]
    bool          sync_output = true;     // immediate variants: lock around each printed line
    std::string   mode        = "spawn";  // divisibility variants: "spawn" or "pool"
};

inline std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline bool parse_bool(const std::string& v) {
    std::string s = lower(v);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    throw std::runtime_error("expected true/false, got '" + v + "'");
}

inline Config load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file '" + path + "'");

    Config cfg;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error(path + ":" + std::to_string(lineno) + ": expected key = value");
        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        try {
            if (key == "threads")          cfg.threads = std::stoi(val);
            else if (key == "limit")       cfg.limit = std::stoull(val);
            else if (key == "sync_output") cfg.sync_output = parse_bool(val);
            else if (key == "mode")        cfg.mode = lower(val);
            else std::cerr << "warning: " << path << ":" << lineno
                           << ": unknown key '" << key << "' ignored\n";
        } catch (const std::invalid_argument&) {
            throw std::runtime_error(path + ":" + std::to_string(lineno) + ": bad value for '" + key + "'");
        }
    }

    if (cfg.threads < 1 || cfg.threads > 4096)
        throw std::runtime_error("threads must be between 1 and 4096");
    if (cfg.limit < 2)
        throw std::runtime_error("limit must be at least 2");
    if (cfg.mode != "spawn" && cfg.mode != "pool")
        throw std::runtime_error("mode must be 'spawn' or 'pool'");
    return cfg;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------
using WallClock   = std::chrono::system_clock;   // for human-readable timestamps
using SteadyClock = std::chrono::steady_clock;   // for measuring elapsed time

// Formats a wall-clock instant as "HH:MM:SS.uuuuuu" (or with the date prepended).
inline std::string format_time(WallClock::time_point tp, bool with_date = false) {
    std::time_t t = WallClock::to_time_t(tp);
    long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                       tp.time_since_epoch()).count() % 1000000;
    // Converting to local time (std::localtime) is slow and not thread-safe, so
    // each thread converts at most once per second and reuses the "HH:MM:SS"
    // text for every timestamp within that second.
    thread_local std::time_t cached_sec = -1;
    thread_local bool cached_with_date = false;
    thread_local char base[32];
    if (t != cached_sec || with_date != cached_with_date) {
        static std::mutex localtime_mutex;  // std::localtime uses shared storage
        std::tm tm;
        {
            std::lock_guard<std::mutex> lock(localtime_mutex);
            tm = *std::localtime(&t);
        }
        std::strftime(base, sizeof base, with_date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &tm);
        cached_sec = t;
        cached_with_date = with_date;
    }
    char out[48];
    std::snprintf(out, sizeof out, "%s.%06lld", base, us);
    return out;
}

inline std::string now_str(bool with_date = false) {
    return format_time(WallClock::now(), with_date);
}

inline double ms_since(SteadyClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

// Milliseconds with three decimals, e.g. "12.345".
inline std::string fmt_ms(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", ms);
    return buf;
}

// ---------------------------------------------------------------------------
// Thread identity
// ---------------------------------------------------------------------------
inline std::string tid_str(std::thread::id id = std::this_thread::get_id()) {
    std::ostringstream os;
    os << id;
    return os.str();
}

// ---------------------------------------------------------------------------
// Primality by trial division (used by the range-split variants).
// `divisions` counts how many modulo operations were performed, so the
// total amount of work can be compared between schemes.
// ---------------------------------------------------------------------------
inline bool is_prime(std::uint64_t n, std::uint64_t& divisions) {
    if (n < 2) return false;
    for (std::uint64_t d = 2; d * d <= n; ++d) {
        ++divisions;
        if (n % d == 0) return false;
    }
    return true;
}

// Integer square root (floor), safe for 64-bit values.
inline std::uint64_t isqrt(std::uint64_t n) {
    std::uint64_t r = static_cast<std::uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (r * r > n) --r;
    while ((r + 1) * (r + 1) <= n) ++r;
    return r;
}

// ---------------------------------------------------------------------------
// Run banners: every run prints a start and an end timestamp.
// ---------------------------------------------------------------------------
inline void print_run_start(const std::string& variant, const Config& cfg,
                            const std::string& extra = "") {
    std::cout << "==============================================================\n"
              << variant << "\n"
              << "threads (x) = " << cfg.threads << ",  limit (y) = " << cfg.limit;
    if (!extra.empty()) std::cout << ",  " << extra;
    std::cout << "\n"
              << "RUN START : " << now_str(true) << "\n"
              << "==============================================================\n";
}

inline void print_run_end(std::uint64_t primes, double elapsed_ms) {
    std::cout << "==============================================================\n"
              << "Primes found : " << primes << "\n"
              << "Elapsed      : " << fmt_ms(elapsed_ms) << " ms\n"
              << "RUN END   : " << now_str(true) << "\n"
              << "==============================================================\n";
    std::cout.flush();
}

}  // namespace common

// ===========================================================================
// Variant-specific code
// ===========================================================================

#include <iomanip>
#include <vector>

namespace {

struct Block {
    std::uint64_t lo = 1, hi = 0;  // inclusive; empty when lo > hi
};

struct Found {
    std::uint64_t n;
    common::WallClock::time_point when;
};

struct ThreadResult {
    std::thread::id id;
    std::vector<Found> primes;  // written only by its own thread
    std::uint64_t divisions = 0;
    double ms = 0.0;
};

std::vector<Block> split_range(std::uint64_t limit, int parts) {
    std::vector<Block> blocks(parts);
    std::uint64_t base = limit / parts, rem = limit % parts, next = 1;
    for (int i = 0; i < parts; ++i) {
        std::uint64_t size = base + (static_cast<std::uint64_t>(i) < rem ? 1 : 0);
        blocks[i].lo = next;
        blocks[i].hi = next + size - 1;
        next += size;
    }
    return blocks;
}

void worker(Block block, ThreadResult& out) {
    const auto t0 = common::SteadyClock::now();
    out.id = std::this_thread::get_id();
    for (std::uint64_t n = block.lo; n <= block.hi; ++n) {
        if (common::is_prime(n, out.divisions))
            out.primes.push_back({n, common::WallClock::now()});
    }
    out.ms = common::ms_since(t0);
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "config.txt";
    common::Config cfg;
    try {
        cfg = common::load_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 1;
    }

    common::print_run_start("Variant 2: print after all threads finish | straight range division", cfg);
    const auto t_start = common::SteadyClock::now();

    const auto blocks = split_range(cfg.limit, cfg.threads);
    for (int i = 0; i < cfg.threads; ++i)
        std::cout << "Thread " << i << " -> [" << blocks[i].lo << ", " << blocks[i].hi << "]\n";
    std::cout << "--------------------------------------------------------------\n"
              << "[" << common::now_str() << "] searching... (no output until all threads join)\n";
    std::cout.flush();

    // ---- search phase -----------------------------------------------------
    std::vector<ThreadResult> results(cfg.threads);
    std::vector<std::thread> pool;
    pool.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; ++i)
        pool.emplace_back(worker, blocks[i], std::ref(results[i]));
    for (auto& t : pool) t.join();
    const double search_ms = common::ms_since(t_start);
    std::cout << "[" << common::now_str() << "] all " << cfg.threads
              << " threads joined, printing results\n"
              << "--------------------------------------------------------------\n";

    // ---- print phase (main thread only, no locking needed) -----------------
    const auto t_print = common::SteadyClock::now();
    std::uint64_t total = 0;
    for (int i = 0; i < cfg.threads; ++i) {
        const std::string tid = common::tid_str(results[i].id);
        for (const auto& f : results[i].primes) {
            std::cout << "[" << common::format_time(f.when) << "] [Thread " << i
                      << " | id " << tid << "] prime: " << f.n << "\n";
        }
        total += results[i].primes.size();
    }
    const double print_ms = common::ms_since(t_print);

    std::uint64_t divisions = 0;
    std::cout << "--------------------------------------------------------------\n"
              << "thread   range                      primes    divisions    time(ms)\n";
    for (int i = 0; i < cfg.threads; ++i) {
        std::ostringstream range;
        range << "[" << blocks[i].lo << ", " << blocks[i].hi << "]";
        std::cout << std::left << std::setw(9) << i << std::setw(27) << range.str()
                  << std::right << std::setw(6) << results[i].primes.size()
                  << std::setw(13) << results[i].divisions
                  << std::setw(12) << common::fmt_ms(results[i].ms) << "\n";
        divisions += results[i].divisions;
    }
    std::cout << "total divisions: " << divisions << "\n"
              << "search phase   : " << common::fmt_ms(search_ms) << " ms\n"
              << "print phase    : " << common::fmt_ms(print_ms) << " ms\n";

    common::print_run_end(total, common::ms_since(t_start));
    return 0;
}

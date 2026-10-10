// Variant 4 - WAIT UNTIL ALL THREADS ARE DONE, THEN PRINT + THREADS SPLIT THE DIVISIBILITY TEST
//
// Build:  g++ -O2 -o primes main.cpp -pthread
// Run:    ./primes            (Windows: primes.exe) - reads config.txt
//         ./primes other.txt  - reads another config file
//
// Same division of work as Variant 3: the search is linear over 2..y, and for
// each number the candidate divisors 2..floor(sqrt(n)) are split across x
// threads. The last thread to finish a number records the verdict; primes are
// stored (with the finding thread and time) instead of printed. Only after the
// whole search is over and every thread has been joined does the main thread
// print the results.
//
// Config key `mode`:
//   spawn - create and join x threads for every number (y * x creations)
//   pool  - x persistent threads that meet at two barriers per number

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

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <vector>

namespace {

class Barrier {
public:
    explicit Barrier(int count) : threshold_(count), waiting_(count) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(m_);
        const unsigned long gen = generation_;
        if (--waiting_ == 0) {
            ++generation_;
            waiting_ = threshold_;
            cv_.notify_all();
        } else {
            cv_.wait(lock, [&] { return gen != generation_; });
        }
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    const int threshold_;
    int waiting_;
    unsigned long generation_ = 0;
};

struct Round {
    std::uint64_t n = 0;
    std::uint64_t root = 0;
    std::atomic<bool> composite{false};
    std::atomic<int> remaining{0};
};

struct SlotStats {
    std::uint64_t divisions = 0;
    std::uint64_t reported = 0;
};

struct Found {
    std::uint64_t n;
    common::WallClock::time_point when;
    int slot;
    std::thread::id id;
};

// Exactly one thread appends per number and numbers are separated by a join or
// barrier, so contention is nil; the mutex just keeps the code obviously safe.
std::mutex g_results_mutex;
std::vector<Found> g_results;

void divisor_chunk(std::uint64_t root, int slot, int parts, std::uint64_t& lo, std::uint64_t& hi) {
    lo = 2; hi = 1;
    if (root < 2) return;
    const std::uint64_t count = root - 1;
    const std::uint64_t base = count / parts, rem = count % parts;
    const std::uint64_t s = static_cast<std::uint64_t>(slot);
    lo = 2 + s * base + std::min(s, rem);
    hi = lo + base + (s < rem ? 1 : 0) - 1;
}

void check_chunk(int slot, Round& round, int threads, SlotStats& stats) {
    std::uint64_t lo, hi;
    divisor_chunk(round.root, slot, threads, lo, hi);
    for (std::uint64_t d = lo; d <= hi; ++d) {
        if (round.composite.load(std::memory_order_relaxed)) break;
        ++stats.divisions;
        if (round.n % d == 0) {
            round.composite.store(true, std::memory_order_relaxed);
            break;
        }
    }
    if (round.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        !round.composite.load(std::memory_order_relaxed)) {
        ++stats.reported;
        std::lock_guard<std::mutex> lock(g_results_mutex);
        g_results.push_back({round.n, common::WallClock::now(), slot, std::this_thread::get_id()});
    }
}

void prepare(Round& round, std::uint64_t n, int threads) {
    round.n = n;
    round.root = common::isqrt(n);
    round.composite.store(false, std::memory_order_relaxed);
    round.remaining.store(threads, std::memory_order_relaxed);
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

    common::print_run_start("Variant 4: print after all threads finish | threads split the divisibility test",
                            cfg, "mode = " + cfg.mode);
    const auto t_start = common::SteadyClock::now();

    std::vector<SlotStats> stats(cfg.threads);
    Round round;
    std::uint64_t threads_created = 0, sync_rounds = 0;

    std::cout << "Linear search 2.." << cfg.limit << "; for each n, divisors 2..sqrt(n) are split across "
              << cfg.threads << " threads\n"
              << "--------------------------------------------------------------\n"
              << "[" << common::now_str() << "] searching... (no output until all threads join)\n";
    std::cout.flush();

    // ---- search phase -----------------------------------------------------
    if (cfg.mode == "spawn") {
        std::vector<std::thread> workers;
        workers.reserve(cfg.threads);
        for (std::uint64_t n = 2; n <= cfg.limit; ++n) {
            prepare(round, n, cfg.threads);
            workers.clear();
            for (int i = 0; i < cfg.threads; ++i)
                workers.emplace_back(check_chunk, i, std::ref(round), cfg.threads, std::ref(stats[i]));
            for (auto& t : workers) t.join();
            threads_created += cfg.threads;
        }
    } else {
        Barrier start(cfg.threads + 1), done(cfg.threads + 1);
        bool stop = false;
        std::vector<std::thread> workers;
        for (int i = 0; i < cfg.threads; ++i) {
            workers.emplace_back([&, i] {
                for (;;) {
                    start.arrive_and_wait();
                    if (stop) return;
                    check_chunk(i, round, cfg.threads, stats[i]);
                    done.arrive_and_wait();
                }
            });
        }
        threads_created = cfg.threads;
        for (std::uint64_t n = 2; n <= cfg.limit; ++n) {
            prepare(round, n, cfg.threads);
            start.arrive_and_wait();
            done.arrive_and_wait();
            ++sync_rounds;
        }
        stop = true;
        start.arrive_and_wait();
        for (auto& t : workers) t.join();
    }
    const double search_ms = common::ms_since(t_start);
    std::cout << "[" << common::now_str() << "] all threads joined, printing results\n"
              << "--------------------------------------------------------------\n";

    // ---- print phase ---------------------------------------------------------
    const auto t_print = common::SteadyClock::now();
    for (const auto& f : g_results) {
        std::cout << "[" << common::format_time(f.when) << "] [Thread " << f.slot
                  << " | id " << common::tid_str(f.id) << "] prime: " << f.n << "\n";
    }
    const double print_ms = common::ms_since(t_print);

    std::uint64_t divisions = 0;
    std::cout << "--------------------------------------------------------------\n"
              << "thread   divisions    primes reported\n";
    for (int i = 0; i < cfg.threads; ++i) {
        std::cout << std::left << std::setw(9) << i << std::right << std::setw(9)
                  << stats[i].divisions << std::setw(19) << stats[i].reported << "\n";
        divisions += stats[i].divisions;
    }
    std::cout << "numbers tested : " << (cfg.limit - 1) << "\n"
              << "threads created: " << threads_created << "\n";
    if (cfg.mode == "pool") std::cout << "barrier rounds : " << sync_rounds << " (x2 barriers)\n";
    std::cout << "total divisions: " << divisions << "\n"
              << "search phase   : " << common::fmt_ms(search_ms) << " ms\n"
              << "print phase    : " << common::fmt_ms(print_ms) << " ms\n";

    common::print_run_end(g_results.size(), common::ms_since(t_start));
    return 0;
}

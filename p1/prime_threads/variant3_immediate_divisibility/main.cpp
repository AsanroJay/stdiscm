// Variant 3 - PRINT IMMEDIATELY + THREADS SPLIT THE DIVISIBILITY TEST
//
// Build:  g++ -O2 -o primes main.cpp -pthread
// Run:    ./primes            (Windows: primes.exe) - reads config.txt
//         ./primes other.txt  - reads another config file
//
// The search itself is linear: numbers 2, 3, 4, ..., y are checked one after
// another. The threads cooperate on ONE number at a time: the candidate
// divisors 2..floor(sqrt(n)) are split into x chunks and each thread tests one
// chunk. As soon as any thread finds a divisor it raises a shared flag and the
// others stop early. The LAST thread to finish a number announces the verdict;
// if no divisor was found, that thread prints the prime immediately with its
// thread index, thread id and a timestamp.
//
// Two ways to run the threads are provided (config key `mode`):
//   spawn - the literal scheme: for every number create x threads, then join
//           them all before moving to the next number. y * x thread creations.
//   pool  - create x threads once; they meet at two barriers per number
//           (start / done). Removes creation cost but not the per-number
//           synchronisation.

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

std::mutex g_out_mutex;

// Reusable barrier (C++17-friendly; std::barrier would need C++20).
class Barrier {
public:
    explicit Barrier(int count) : threshold_(count), waiting_(count) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(m_);
        const unsigned long gen = generation_;
        if (--waiting_ == 0) {          // last to arrive releases everyone
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

// State for the number currently being tested.
struct Round {
    std::uint64_t n = 0;
    std::uint64_t root = 0;              // floor(sqrt(n))
    std::atomic<bool> composite{false};  // set by whichever thread finds a divisor
    std::atomic<int> remaining{0};       // threads still working on n
};

struct SlotStats {
    std::uint64_t divisions = 0;  // modulo operations done by this slot
    std::uint64_t reported = 0;   // primes this slot announced
};

struct Settings {
    int threads;
    bool sync_output;
};

std::atomic<std::uint64_t> g_primes{0};

// Divisors 2..root split into `parts` contiguous chunks; returns slot's chunk.
void divisor_chunk(std::uint64_t root, int slot, int parts, std::uint64_t& lo, std::uint64_t& hi) {
    lo = 2; hi = 1;  // empty
    if (root < 2) return;
    const std::uint64_t count = root - 1;
    const std::uint64_t base = count / parts, rem = count % parts;
    const std::uint64_t s = static_cast<std::uint64_t>(slot);
    lo = 2 + s * base + std::min(s, rem);
    hi = lo + base + (s < rem ? 1 : 0) - 1;
}

void report_prime(int slot, std::uint64_t n, bool sync_output) {
    if (sync_output) {
        std::string line = "[" + common::now_str() + "] [Thread " + std::to_string(slot) +
                           " | id " + common::tid_str() + "] prime: " + std::to_string(n) + "\n";
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << line;
    } else {
        std::cout << "[" << common::now_str() << "] [Thread " << slot
                  << " | id " << common::tid_str() << "] prime: " << n << "\n";
    }
}

// One thread's share of the divisibility test for round.n.
void check_chunk(int slot, Round& round, const Settings& set, SlotStats& stats) {
    std::uint64_t lo, hi;
    divisor_chunk(round.root, slot, set.threads, lo, hi);
    for (std::uint64_t d = lo; d <= hi; ++d) {
        if (round.composite.load(std::memory_order_relaxed)) break;  // someone already proved it
        ++stats.divisions;
        if (round.n % d == 0) {
            round.composite.store(true, std::memory_order_relaxed);
            break;
        }
    }
    // The last thread out decides. acq_rel makes every other thread's flag
    // write visible here before we read it.
    if (round.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        !round.composite.load(std::memory_order_relaxed)) {
        ++stats.reported;
        g_primes.fetch_add(1, std::memory_order_relaxed);
        report_prime(slot, round.n, set.sync_output);
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

    common::print_run_start("Variant 3: print immediately | threads split the divisibility test", cfg,
                            "mode = " + cfg.mode + ", sync_output = " + (cfg.sync_output ? "true" : "false"));
    const auto t_start = common::SteadyClock::now();

    const Settings set{cfg.threads, cfg.sync_output};
    std::vector<SlotStats> stats(cfg.threads);
    Round round;
    std::uint64_t threads_created = 0, sync_rounds = 0;

    std::cout << "Linear search 2.." << cfg.limit << "; for each n, divisors 2..sqrt(n) are split across "
              << cfg.threads << " threads\n"
              << "--------------------------------------------------------------\n";

    if (cfg.mode == "spawn") {
        // Create and join x threads for every single number.
        std::vector<std::thread> workers;
        workers.reserve(cfg.threads);
        for (std::uint64_t n = 2; n <= cfg.limit; ++n) {
            prepare(round, n, cfg.threads);
            workers.clear();
            for (int i = 0; i < cfg.threads; ++i)
                workers.emplace_back(check_chunk, i, std::ref(round), std::cref(set), std::ref(stats[i]));
            for (auto& t : workers) t.join();
            threads_created += cfg.threads;
        }
    } else {
        // Persistent workers synchronised by two barriers per number.
        Barrier start(cfg.threads + 1), done(cfg.threads + 1);
        bool stop = false;  // written by main before `start`, read by workers after it
        std::vector<std::thread> workers;
        for (int i = 0; i < cfg.threads; ++i) {
            workers.emplace_back([&, i] {
                for (;;) {
                    start.arrive_and_wait();
                    if (stop) return;
                    check_chunk(i, round, set, stats[i]);
                    done.arrive_and_wait();
                }
            });
        }
        threads_created = cfg.threads;
        for (std::uint64_t n = 2; n <= cfg.limit; ++n) {
            prepare(round, n, cfg.threads);
            start.arrive_and_wait();  // release workers on n
            done.arrive_and_wait();   // wait for all of them to finish n
            ++sync_rounds;
        }
        stop = true;
        start.arrive_and_wait();
        for (auto& t : workers) t.join();
    }

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
    std::cout << "total divisions: " << divisions << "\n";

    common::print_run_end(g_primes.load(), common::ms_since(t_start));
    return 0;
}

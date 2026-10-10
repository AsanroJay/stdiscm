// bench.cpp - runs all four variants over a range of thread counts and
// records how long each run takes.
//
// Build and run from inside the benchmark folder, after building all four
// variants (each variant folder must contain its primes / primes.exe):
//
//     g++ -O2 -o bench bench.cpp
//     ./bench          full run (takes several minutes)
//     ./bench quick    smaller limits, 1 repetition each
//
// For every configuration the benchmark writes a temporary config file, runs
// the variant with its output redirected to a temporary file (printing to a
// terminal would be much slower), and reads the timings that the program
// prints in its own summary. With 3 repetitions the median is reported.
// Results are printed as a table and saved to results.csv.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32
const char* SEP = "\\";
const char* EXE = "primes.exe";
#else
const char* SEP = "/";
const char* EXE = "primes";
#endif

const char* VARIANT_DIR[] = {
    "",  // variants are numbered from 1
    "variant1_immediate_range",
    "variant2_deferred_range",
    "variant3_immediate_divisibility",
    "variant4_deferred_divisibility",
};

const char* CONFIG_FILE = "bench_config.txt";
const char* OUTPUT_FILE = "bench_out.txt";

struct Job {
    std::string experiment;
    int variant;
    std::string mode;  // "-" for the range variants
    int threads;
    unsigned long long limit;
};

// Values read back from one run's output.
struct Sample {
    double elapsed_ms = -1, search_ms = -1, print_ms = -1;
    long long primes = -1, divisions = -1, threads_created = -1;
    std::string per_thread_divisions;  // space-separated, one per thread
};

std::string exe_path(int variant) {
    return std::string("..") + SEP + VARIANT_DIR[variant] + SEP + EXE;
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// Returns the text after the first ':' on the line.
std::string after_colon(const std::string& line) {
    std::string::size_type p = line.find(':');
    return p == std::string::npos ? "" : line.substr(p + 1);
}

double to_double(const std::string& s) { return std::atof(s.c_str()); }
long long to_ll(const std::string& s) { return std::atoll(s.c_str()); }

Sample parse_output(const std::string& path, bool range_variant) {
    Sample s;
    std::ifstream in(path);
    std::string line;
    bool in_table = false;
    std::ostringstream per_thread;

    while (std::getline(in, line)) {
        if (starts_with(line, "Elapsed"))              s.elapsed_ms = to_double(after_colon(line));
        else if (starts_with(line, "search phase"))    s.search_ms = to_double(after_colon(line));
        else if (starts_with(line, "print phase"))     s.print_ms = to_double(after_colon(line));
        else if (starts_with(line, "Primes found"))    s.primes = to_ll(after_colon(line));
        else if (starts_with(line, "total divisions")) s.divisions = to_ll(after_colon(line));
        else if (starts_with(line, "threads created")) s.threads_created = to_ll(after_colon(line));

        // Per-thread summary table: starts after the "thread ..." header and
        // ends at the first line that does not begin with a digit.
        if (starts_with(line, "thread ")) { in_table = true; continue; }
        if (in_table) {
            if (line.empty() || line[0] < '0' || line[0] > '9') { in_table = false; continue; }
            std::istringstream row(line);
            std::vector<std::string> tok;
            std::string t;
            while (row >> t) tok.push_back(t);
            // range:        idx  [lo,  hi]  primes  divisions  time
            // divisibility: idx  divisions  reported
            std::size_t col = range_variant ? 4 : 1;
            if (tok.size() > col) {
                if (!per_thread.str().empty()) per_thread << ' ';
                per_thread << tok[col];
            }
        }
    }
    s.per_thread_divisions = per_thread.str();
    return s;
}

bool run_once(const Job& job, Sample& out) {
    {
        std::ofstream cfg(CONFIG_FILE);
        cfg << "threads = " << job.threads << "\n"
            << "limit = " << job.limit << "\n"
            << "mode = " << (job.mode == "-" ? "spawn" : job.mode) << "\n"
            << "sync_output = true\n";
    }
    std::string cmd = exe_path(job.variant) + " " + CONFIG_FILE + " > " + OUTPUT_FILE;
    if (std::system(cmd.c_str()) != 0) return false;
    out = parse_output(OUTPUT_FILE, job.variant <= 2);
    return out.elapsed_ms >= 0;
}

std::string num_or_blank(double v, int decimals) {
    if (v < 0) return "";
    std::ostringstream os;
    os << std::fixed << std::setprecision(decimals) << v;
    return os.str();
}

std::string num_or_blank(long long v) { return v < 0 ? "" : std::to_string(v); }

}  // namespace

int main(int argc, char* argv[]) {
    const bool quick = argc > 1 && std::string(argv[1]) == "quick";
    const int reps = quick ? 1 : 3;
    const unsigned long long range_limit = quick ? 1000000ULL : 5000000ULL;
    const unsigned long long div_limit = quick ? 5000ULL : 20000ULL;
    const int range_threads[] = {1, 2, 3, 4, 8, 16, 32, 64};
    const int div_threads[] = {1, 2, 4, 8, 16};

    for (int v = 1; v <= 4; ++v) {
        if (!file_exists(exe_path(v))) {
            std::cerr << "missing " << exe_path(v) << "\n"
                      << "Build every variant first, e.g. in " << VARIANT_DIR[v] << ":\n"
                      << "    g++ -O2 -o primes main.cpp -pthread\n"
                      << "and run bench from inside the benchmark folder.\n";
            return 1;
        }
    }

    // Experiment A: range division at a large limit, many thread counts.
    // Experiment B: all four variants (both modes for 3 and 4) at a small limit.
    std::vector<Job> plan;
    for (int v = 1; v <= 2; ++v)
        for (int t : range_threads) plan.push_back({"A_range_scaling", v, "-", t, range_limit});
    for (int t : div_threads) {
        for (int v = 1; v <= 2; ++v) plan.push_back({"B_same_limit", v, "-", t, div_limit});
        for (int v = 3; v <= 4; ++v) {
            plan.push_back({"B_same_limit", v, "spawn", t, div_limit});
            plan.push_back({"B_same_limit", v, "pool", t, div_limit});
        }
    }

    std::ofstream csv("results.csv");
    csv << "experiment,variant,mode,threads,limit,reps,elapsed_ms_median,elapsed_ms_all,"
           "search_ms,print_ms,primes,divisions,threads_created,per_thread_divisions\n";

    std::cout << "Benchmark: " << plan.size() << " configurations x " << reps << " run(s)\n"
              << "logical CPUs reported by the system: " << std::thread::hardware_concurrency() << "\n\n"
              << std::left << std::setw(6) << "run" << std::setw(18) << "experiment"
              << std::setw(9) << "variant" << std::setw(8) << "mode" << std::right
              << std::setw(8) << "threads" << std::setw(10) << "limit" << std::setw(14) << "median ms" << "\n";

    for (std::size_t i = 0; i < plan.size(); ++i) {
        const Job& job = plan[i];
        std::vector<Sample> samples;
        for (int r = 0; r < reps; ++r) {
            Sample s;
            if (!run_once(job, s)) {
                std::cerr << "run failed: variant " << job.variant << ", threads " << job.threads << "\n";
                return 1;
            }
            samples.push_back(s);
        }

        // Median of the elapsed times; the other columns come from that same run.
        std::vector<Sample> sorted = samples;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Sample& a, const Sample& b) { return a.elapsed_ms < b.elapsed_ms; });
        const Sample& med = sorted[sorted.size() / 2];

        std::ostringstream all;
        for (std::size_t k = 0; k < samples.size(); ++k)
            all << (k ? " " : "") << num_or_blank(samples[k].elapsed_ms, 3);

        csv << job.experiment << ',' << job.variant << ',' << job.mode << ',' << job.threads << ','
            << job.limit << ',' << reps << ',' << num_or_blank(med.elapsed_ms, 3) << ',' << all.str() << ','
            << num_or_blank(med.search_ms, 3) << ',' << num_or_blank(med.print_ms, 3) << ','
            << num_or_blank(med.primes) << ',' << num_or_blank(med.divisions) << ','
            << num_or_blank(med.threads_created) << ',' << med.per_thread_divisions << '\n';
        csv.flush();

        std::ostringstream run;
        run << (i + 1) << "/" << plan.size();
        std::cout << std::left << std::setw(6) << run.str() << std::setw(18) << job.experiment
                  << std::setw(9) << job.variant << std::setw(8) << job.mode << std::right
                  << std::setw(8) << job.threads << std::setw(10) << job.limit
                  << std::setw(14) << num_or_blank(med.elapsed_ms, 1) << std::endl;
    }

    std::remove(CONFIG_FILE);
    std::remove(OUTPUT_FILE);
    std::cout << "\nwrote results.csv\n";
    return 0;
}

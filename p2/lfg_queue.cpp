// =============================================================================
//  LFG (Looking For Group) Dungeon Queue Manager
//
//  Model
//  -----
//  * All players arrive at the same time, so every possible party
//    (1 tank + 1 healer + 3 DPS) is formed up front, all-or-nothing.
//    Leftover players that cannot complete a party are reported, never queued.
//  * Each of the n dungeon instances is a worker thread.
//  * The dispatcher (main thread) hands parties out in FIFO order to the
//    longest-idle free instance. If none is free it waits on `instanceFree_`.
//  * An instance waits on its own condition variable for a party, keeps it for
//    a random t in [t1, t2] seconds (waiting with the mutex released), then
//    marks itself empty, rejoins the back of the free-instance queue and
//    signals the dispatcher.
//  * A monitor thread redraws a live dashboard in place about 16 times a
//    second (progress bars + a marquee ticker) and turns Ctrl+C into a
//    graceful stop.
//
//  Synchronization
//  ---------------
//  * One mutex (`mtx_`) guards ALL shared state (monitor pattern). With a
//    single lock there is no lock ordering, so circular wait is impossible.
//  * `instanceFree_` : dispatcher waits for a free instance (behaves like a
//                      counting semaphore initialised to n).
//  * Instance::cv    : an instance waits for work, then for its clear time to
//                      pass; wait/wait_until release the mutex while waiting.
//  * `monitorCv_`    : paces the dashboard and wakes it up to stop.
//  * Parties are formed all-or-nothing: no thread holds a partial party.
//  * Both queues are FIFO, so no party and no instance can be starved.
//
//  Build:  g++ -std=c++17 -O2 -pthread lfg_queue.cpp -o lfg_queue
//  Run:    ./lfg_queue                       interactive prompts
//          ./lfg_queue n t h d t1 t2         values as arguments
//          ./lfg_queue --plain [values]      line-by-line log, no animation
//          ./lfg_queue --ascii [values]      live view using ASCII only
// =============================================================================

#ifdef _MSC_VER
#  pragma execution_character_set("utf-8")   // narrow literals as UTF-8
#endif

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

// ----------------------------------------------------------------------------
//  Limits
// ----------------------------------------------------------------------------
static const long long MAX_INSTANCES  = 100;      // one thread per instance
static const long long MAX_PLAYERS    = 1000000;  // per role
static const long long MAX_CLEAR_TIME = 15;       // spec: t2 <= 15
static const int       FRAME_MS       = 60;       // ~16 frames per second
static const int       MIN_LIVE_COLS  = 60;
static const int       MIN_LIVE_ROWS  = 14;

// ----------------------------------------------------------------------------
//  Small string helpers
// ----------------------------------------------------------------------------
static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string rjust(const std::string& s, size_t w) {
    return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

static std::string ljust(const std::string& s, size_t w) {
    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}

static std::string plural(long long n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// ----------------------------------------------------------------------------
//  Terminal: ANSI colours, size, UTF-8 aware width/truncation
// ----------------------------------------------------------------------------
namespace term {

static bool color = false;

// Glyph set. Unicode by default (smooth eighth-block bars); --ascii swaps in
// plain characters for console fonts that lack block elements.
struct Glyphs {
    const char* part[8];
    const char *full, *track, *idle, *cap, *rule, *dot, *diamond, *ok, *bad, *more, *dash, *ticker;
};
// Written as \u escapes so the source is pure ASCII and compiles the same on
// any compiler/code page (UTF-8 output on MSVC comes from the pragma below).
static const Glyphs UNICODE_GLYPHS = {
    {"", "\u258F", "\u258E", "\u258D", "\u258C", "\u258B", "\u258A", "\u2589"},
    "\u2588", "\u2591", "\u00B7", "\u2502", "\u2500", " \u00B7 ", "\u25C6 ", "\u2713", "\u2717", "\u2026", "\u2013", "\u00BB"};
static const Glyphs ASCII_GLYPHS = {
    {"", "", "", "", "", "", "", ""},
    "#", "-", ".", "|", "-", " | ", "* ", "OK", "X", "...", "-", ">>"};
static const Glyphs* G = &UNICODE_GLYPHS;

static const char* reset()  { return color ? "\x1b[0m"  : ""; }
static const char* bold()   { return color ? "\x1b[1m"  : ""; }
static const char* dim()    { return color ? "\x1b[2m"  : ""; }
static const char* green()  { return color ? "\x1b[32m" : ""; }
static const char* red()    { return color ? "\x1b[31m" : ""; }
static const char* yellow() { return color ? "\x1b[33m" : ""; }
static const char* cyan()   { return color ? "\x1b[36m" : ""; }

static bool stdoutIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// Windows 10+ consoles understand ANSI once virtual-terminal mode is on.
static bool enableAnsi() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
    if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return false;
    SetConsoleOutputCP(CP_UTF8);
    return true;
#else
    return true;
#endif
}

static void size(int& cols, int& rows) {
    cols = 80;
    rows = 24;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        cols = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
}

// Async-signal-safe write (used from the Ctrl+C handler).
static void rawWrite(const char* s, size_t n) {
#ifdef _WIN32
    _write(1, s, static_cast<unsigned>(n));
#else
    ssize_t r = write(STDOUT_FILENO, s, n);
    (void)r;
#endif
}

static size_t u8len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// Index just past an ANSI escape sequence starting at s[i].
static size_t skipEscape(const std::string& s, size_t i) {
    size_t j = i + 1;
    if (j < s.size() && s[j] == '[') {
        ++j;
        while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) ++j;
        if (j < s.size()) ++j;
    } else if (j < s.size()) {
        ++j;
    }
    return j;
}

// Visible width in terminal columns (every glyph used here is 1 column).
static int width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') { i = skipEscape(s, i); continue; }
        i += u8len(static_cast<unsigned char>(s[i]));
        ++w;
    }
    return w;
}

// Truncate or pad to exactly `w` visible columns. Lines never wrap, which keeps
// the cursor arithmetic of the in-place redraw exact.
static std::string fit(const std::string& s, int w) {
    std::string out;
    int used = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') {
            size_t j = skipEscape(s, i);
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (used == w) break;
        size_t n = u8len(static_cast<unsigned char>(s[i]));
        out.append(s, i, n);
        i += n;
        ++used;
    }
    out += reset();
    if (used < w) out.append(static_cast<size_t>(w - used), ' ');
    return out;
}

static std::vector<std::string> glyphs(const std::string& s) {
    std::vector<std::string> g;
    for (size_t i = 0; i < s.size();) {
        size_t n = u8len(static_cast<unsigned char>(s[i]));
        g.push_back(s.substr(i, n));
        i += n;
    }
    return g;
}

static std::string repeat(const char* g, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += g;
    return s;
}

// Smooth progress bar: full blocks plus an eighth-block for the partial cell.
static std::string bar(double frac, int w, bool active) {
    std::string s;
    if (!active) {
        s += dim();
        s += repeat(G->idle, w);
        s += reset();
        return s;
    }
    frac = std::max(0.0, std::min(1.0, frac));
    double cells = frac * w;
    int full = std::min(w, static_cast<int>(cells));
    int eighth = static_cast<int>((cells - full) * 8);
    s += green();
    s += repeat(G->full, full);
    int used = full;
    if (used < w && eighth > 0 && *G->part[eighth]) {
        s += G->part[eighth];
        ++used;
    }
    s += reset();
    s += dim();
    s += repeat(G->track, w - used);
    s += reset();
    return s;
}

}  // namespace term

// ----------------------------------------------------------------------------
//  Ctrl+C: first press = graceful stop, second press = quit immediately
// ----------------------------------------------------------------------------
// Lock-free atomics: safe to touch from a signal handler AND from other threads
// (a plain volatile flag would be a data race between threads).
static std::atomic<bool> g_interrupt{false};
static std::atomic<bool> g_liveActive{false};

extern "C" void onSigint(int) {
    if (g_interrupt) {
        if (g_liveActive) {
            static const char restore[] = "\x1b[0m\x1b[?25h\n";
            term::rawWrite(restore, sizeof(restore) - 1);
        }
        std::_Exit(130);
    }
    g_interrupt = true;
    std::signal(SIGINT, onSigint);  // Windows resets the handler after each call
}

// ----------------------------------------------------------------------------
//  Shared state (the "monitor")
// ----------------------------------------------------------------------------
struct Instance {
    int id = 0;

    // --- everything below is protected by DungeonManager::mtx_ ---
    bool active = false;          // a party is inside
    bool hasJob = false;          // dispatcher assigned a party that has not finished
    long long partyId = -1;
    int clearTime = 0;            // seconds for the current run
    Clock::time_point startedAt;
    long long partiesServed = 0;
    long long totalTimeServed = 0;

    std::condition_variable cv;   // party assigned / shutdown / abort
};

struct Event {
    double at;
    std::string text;
};

class DungeonManager {
public:
    DungeonManager(int n, int t1, int t2, long long parties, long long leftT,
                   long long leftH, long long leftD, bool live)
        : n_(n), t1_(t1), t2_(t2), totalParties_(parties), leftT_(leftT),
          leftH_(leftH), leftD_(leftD), live_(live),
          rng_(std::random_device{}()), dist_(t1, t2) {
        instances_.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            auto inst = std::make_unique<Instance>();
            inst->id = i + 1;
            instances_.push_back(std::move(inst));
            freeInstances_.push_back(i);   // every instance starts empty
        }
    }

    void run() {
        start_ = Clock::now();
        if (live_) {
            g_liveActive = true;
            std::cout << "\x1b[?25l" << std::flush;   // hide cursor
        }
        for (auto& inst : instances_)
            workers_.emplace_back(&DungeonManager::instanceLoop, this, inst.get());
        monitor_ = std::thread(&DungeonManager::monitorLoop, this);

        dispatch();

        // No more parties: let idle instances exit; busy ones finish first.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_ = true;
            for (auto& inst : instances_) inst->cv.notify_all();
        }
        for (auto& w : workers_) w.join();

        {
            std::lock_guard<std::mutex> lk(mtx_);
            end_ = Clock::now();
            if (!abort_) {
                logEventLocked(totalParties_ == 0
                                   ? "Nothing to run: no full party could be formed"
                                   : "All " + plural(totalParties_, "party", "parties") +
                                         " served");
            }
            monitorStop_ = true;
        }
        monitorCv_.notify_all();
        monitor_.join();

        if (live_) {
            std::cout << "\x1b[?25h" << std::flush;   // show cursor again
            g_liveActive = false;
        }
    }

    bool interrupted() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return abort_;
    }

    void printSummary() const {
        using namespace term;
        std::lock_guard<std::mutex> lk(mtx_);
        double wall = std::chrono::duration<double>(end_ - start_).count();

        long long maxServed = 0;
        for (const auto& inst : instances_) maxServed = std::max(maxServed, inst->partiesServed);

        const std::string rule(66, '-');
        std::ostringstream o;
        o << "\n" << bold() << "==================== SUMMARY ====================" << reset() << "\n";
        o << bold() << ljust("Instance", 12) << ljust("Parties served", 18)
          << ljust("Total time served", 20) << "Share" << reset() << "\n";
        o << rule << "\n";
        long long sumParties = 0, sumTime = 0;
        for (const auto& inst : instances_) {
            int len = maxServed == 0 ? 0
                      : static_cast<int>(16.0 * inst->partiesServed / maxServed + 0.5);
            o << ljust(std::to_string(inst->id), 12)
              << ljust(std::to_string(inst->partiesServed), 18)
              << ljust(std::to_string(inst->totalTimeServed) + "s", 20)
              << green() << (live_ ? repeat(G->full, len) : std::string(static_cast<size_t>(len), '#'))
              << reset() << "\n";
            sumParties += inst->partiesServed;
            sumTime += inst->totalTimeServed;
        }
        o << rule << "\n";
        o << bold() << ljust("TOTAL", 12) << ljust(std::to_string(sumParties), 18)
          << sumTime << "s" << reset() << "\n\n";

        o << "Parties formed            : " << totalParties_ << "\n";
        o << "Parties served            : " << sumParties << "\n";
        if (abort_) {
            o << yellow() << "Stopped early (Ctrl+C)    : " << cutShort_
              << " cut short inside a dungeon, " << (totalParties_ - dispatched_)
              << " never dispatched" << reset() << "\n";
        }
        o << "Elapsed wall-clock time   : " << std::fixed << std::setprecision(1) << wall << "s\n";
        o << "Players left in queue     : " << leftT_ << " tank(s), " << leftH_
          << " healer(s), " << leftD_ << " DPS"
          << (leftT_ + leftH_ + leftD_ > 0 ? "  (can't form a full party)" : "")
          << "\n";
        o << bold() << "=================================================" << reset() << "\n";
        std::cout << o.str() << std::flush;
    }

private:
    // ---------------------------------------------------------------- threads
    // Dispatcher (main thread): FIFO parties -> longest-idle free instance.
    void dispatch() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            logEventLocked(totalParties_ == 0
                               ? std::string("Queue opened: no full party can be formed")
                               : "Queue opened: " + plural(totalParties_, "party", "parties") +
                                     " waiting");
        }
        for (long long p = 1; p <= totalParties_; ++p) {
            std::unique_lock<std::mutex> lk(mtx_);
            // Sleep (no busy-waiting) until an instance is free or we must stop.
            instanceFree_.wait(lk, [this] { return !freeInstances_.empty() || abort_; });
            if (abort_) break;

            int idx = freeInstances_.front();   // longest-idle instance first
            freeInstances_.pop_front();

            Instance& inst = *instances_[static_cast<size_t>(idx)];
            inst.partyId = p;
            inst.clearTime = dist_(rng_);       // only the dispatcher touches rng_
            inst.startedAt = Clock::now();
            inst.hasJob = true;
            inst.active = true;
            dispatched_ = p;

            logEventLocked(fmt("Party %lld entered Instance %d (clear time %ds)", p, inst.id,
                               inst.clearTime));
            inst.cv.notify_one();               // wake exactly that instance
        }
    }

    // One thread per dungeon instance.
    void instanceLoop(Instance* inst) {
        std::unique_lock<std::mutex> lk(mtx_);
        while (true) {
            inst->cv.wait(lk, [&] { return inst->hasJob || shutdown_ || abort_; });
            if (!inst->hasJob) return;   // shutdown/abort while idle

            // The party clears the dungeon. wait_until RELEASES the mutex while
            // waiting, so other instances run in parallel; it only wakes early
            // if the run is aborted.
            const auto finishAt = inst->startedAt + std::chrono::seconds(inst->clearTime);
            if (inst->cv.wait_until(lk, finishAt, [this] { return abort_; })) {
                long long party = inst->partyId;
                inst->active = false;
                inst->hasJob = false;
                inst->partyId = -1;
                ++cutShort_;
                logEventLocked(fmt("Party %lld stopped early in Instance %d", party, inst->id));
                return;
            }

            long long finished = inst->partyId;
            inst->partiesServed++;
            inst->totalTimeServed += inst->clearTime;
            inst->active = false;
            inst->hasJob = false;
            inst->partyId = -1;
            ++completed_;
            freeInstances_.push_back(inst->id - 1);   // back of the line

            logEventLocked(fmt("Party %lld cleared Instance %d in %ds", finished, inst->id,
                               inst->clearTime));
            instanceFree_.notify_one();               // a slot opened up
        }
    }

    // Draws the dashboard (live mode) and turns Ctrl+C into a graceful stop.
    void monitorLoop() {
        while (true) {
            bool stop;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (g_interrupt && !abort_ && !monitorStop_) requestAbortLocked();
                stop = monitorStop_;
            }
            if (live_) render(stop);
            if (stop) return;
            std::unique_lock<std::mutex> lk(mtx_);
            monitorCv_.wait_for(lk, std::chrono::milliseconds(live_ ? FRAME_MS : 50),
                                [this] { return monitorStop_; });
        }
    }

    void requestAbortLocked() {
        abort_ = true;
        logEventLocked("Ctrl+C: stopping, no new parties will be dispatched");
        for (auto& inst : instances_) inst->cv.notify_all();
        instanceFree_.notify_all();
    }

    // ---------------------------------------------------------------- events
    double secondsSince(Clock::time_point t) const {
        return std::chrono::duration<double>(t - start_).count();
    }

    // Caller holds mtx_.
    void logEventLocked(const std::string& text) {
        Event e{secondsSince(Clock::now()), text};
        events_.push_back(e);
        if (events_.size() > 64) events_.pop_front();
        if (live_)
            appendTickerLocked(text);
        else
            printPlainLocked(e);
    }

    // Plain mode: one block per event with every instance's status.
    void printPlainLocked(const Event& e) const {
        std::ostringstream out;
        out << "\n[" << std::fixed << std::setprecision(1) << std::setw(5) << e.at << "s] "
            << e.text << "\n";
        for (const auto& inst : instances_) {
            out << "   Instance " << std::left << std::setw(4) << inst->id << ": ";
            if (inst->active)
                out << "active  (Party " << inst->partyId << ")";
            else
                out << "empty";
            out << "\n";
        }
        std::cout << out.str() << std::flush;
    }

    // ---------------------------------------------------------------- marquee
    // The ticker is a tape of glyphs scrolling right-to-left one column per
    // frame. New text is padded so it always enters from the right edge.
    void appendTickerLocked(const std::string& text) {
        size_t unread = tape_.size() > tickerPos_ ? tape_.size() - tickerPos_ : 0;
        size_t tw = static_cast<size_t>(tickerWidth_);
        if (unread < tw)
            tape_.insert(tape_.end(), tw - unread, " ");
        else
            tape_.insert(tape_.end(), 5, " ");
        for (auto& g : term::glyphs(std::string(term::G->diamond) + text)) tape_.push_back(g);
    }

    std::string tickerWindowLocked(int tw, bool advance) {
        tickerWidth_ = tw;
        size_t len = tape_.size();
        if (advance && tickerPos_ < len) {
            // One column per frame normally; speed up in proportion to the
            // text still waiting off-screen so the ticker never lags far
            // behind a burst of events.
            size_t shownEnd = tickerPos_ + static_cast<size_t>(tw);
            size_t backlog = len > shownEnd ? len - shownEnd : 0;
            size_t step = 1 + backlog / 12;
            tickerPos_ = std::min(len, tickerPos_ + step);
        }
        std::string s;
        for (size_t i = tickerPos_; i < tickerPos_ + static_cast<size_t>(tw); ++i)
            s += i < len ? tape_[i] : " ";
        if (tickerPos_ > 4096) {
            tape_.erase(tape_.begin(), tape_.begin() + static_cast<long>(tickerPos_));
            tickerPos_ = 0;
        }
        return s;
    }

    // ---------------------------------------------------------------- render
    struct View {
        int id;
        bool active;
        long long party;
        double elapsed;
        int clear;
    };

    struct Layout {
        bool grid;
        int logLines;
        int gridCols;
        int gridRows;
        bool truncated;
    };

    // Fixed lines: title, rule, overall, counts, blank, column header,
    // rule, ticker = 8, plus the event log and the instance block.
    Layout computeLayout(int W, int rows) const {
        const int n = n_;
        auto avail = [&](int logLines) { return rows - 1 - (8 + logLines); };
        for (int L : {4, 2})
            if (n <= avail(L)) return {false, L, 1, n, false};
        const int cellW = 25;
        int gc = std::max(1, (W - 2) / cellW);
        int gr = (n + gc - 1) / gc;
        for (int L : {2, 0})
            if (gr <= avail(L)) return {true, L, gc, gr, false};
        return {true, 0, gc, std::max(1, avail(0) - 1), true};
    }

    void render(bool final) {
        using namespace term;
        int cols, rows;
        term::size(cols, rows);
        const int W = std::max(20, std::min(cols - 1, 100));
        const Layout lay = computeLayout(W, rows);
        const int tickerW = std::max(10, W - 5);

        // ---- snapshot under the lock (short), draw outside it ----
        std::vector<View> views;
        views.reserve(instances_.size());
        long long completed, dispatched, cut;
        bool aborted;
        double now;
        std::vector<Event> recent;
        std::string ticker;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto tnow = final ? end_ : Clock::now();
            for (const auto& inst : instances_) {
                double el = inst->active
                                ? std::chrono::duration<double>(tnow - inst->startedAt).count()
                                : 0.0;
                views.push_back({inst->id, inst->active, inst->partyId, el, inst->clearTime});
            }
            completed = completed_;
            dispatched = dispatched_;
            cut = cutShort_;
            aborted = abort_;
            now = secondsSince(tnow);
            size_t k = std::min(events_.size(), static_cast<size_t>(lay.logLines));
            recent.assign(events_.end() - static_cast<long>(k), events_.end());
            ticker = final && !events_.empty() ? G->diamond + events_.back().text
                                               : tickerWindowLocked(tickerW, true);
        }

        int activeCount = 0;
        double partial = 0;
        for (const auto& v : views) {
            if (!v.active) continue;
            ++activeCount;
            partial += std::min(1.0, v.elapsed / std::max(1, v.clear));
        }

        std::vector<std::string> lines;
        const std::string rule = std::string(dim()) + repeat(G->rule, W) + reset();

        // Title
        static const char* SPIN[4] = {"|", "/", "-", "\\"};
        std::string mark = final ? (aborted ? std::string(red()) + G->bad : std::string(green()) + G->ok)
                                 : std::string(green()) + SPIN[(frame_++ / 2) % 4];
        std::string state = final ? (aborted ? "  stopped" : "  complete") : "";
        int mins = static_cast<int>(now / 60);
        double secs = now - mins * 60;
        std::string left = std::string(" ") + mark + reset() + " " + bold() + "LFG DUNGEON QUEUE" +
                           reset() + dim() + state + reset();
        std::string right = std::string(dim()) + fmt("%d instances%sclear %d%s%ds%s", n_, G->dot, t1_, G->dash, t2_, G->dot) +
                            reset() + fmt("elapsed %02d:%04.1f ", mins, secs);
        lines.push_back(fit(left, W - width(right)) + right);
        lines.push_back(rule);

        // Overall progress (smooth: includes the fraction of every active run)
        if (totalParties_ == 0) {
            lines.push_back(std::string("  ") + yellow() +
                            "No full party could be formed (need 1 tank, 1 healer, 3 DPS)" + reset());
        } else {
            double frac = (completed + partial) / static_cast<double>(totalParties_);
            if (final && !aborted) frac = 1.0;
            int bw = std::max(8, W - 18);
            lines.push_back(std::string("  Overall  ") + G->cap + bar(frac, bw, true) + G->cap +
                            fmt("%4d%%", static_cast<int>(frac * 100)));
        }
        std::string counts = std::string("  ") + green() + "cleared " + std::to_string(completed) +
                             reset() + dim() + G->dot + reset() + "in dungeon " +
                             std::to_string(activeCount) + dim() + G->dot + reset() + "waiting " +
                             std::to_string(totalParties_ - dispatched);
        if (cut > 0) counts += std::string(dim()) + G->dot + reset() + red() + "cut short " +
                               std::to_string(cut) + reset();
        counts += std::string(dim()) + "   leftover " + reset() +
                  fmt("%lldT %lldH %lldD", leftT_, leftH_, leftD_);
        lines.push_back(counts);
        lines.push_back("");

        // Instances
        if (!lay.grid) {
            int bw = std::max(8, std::min(60, W - 40));
            lines.push_back(std::string(dim()) + "  " + rjust("INST", 4) + "  STATUS  " +
                            ljust("PARTY", 7) + "  " + ljust("PROGRESS", static_cast<size_t>(bw + 2)) +
                            " " + rjust("TIME", 12) + reset());
            for (const auto& v : views) {
                std::string st = v.active ? std::string(green()) + bold() + "active" + reset()
                                          : std::string(dim()) + "empty " + reset();
                std::string party = v.active ? "P" + std::to_string(v.party) : "-";
                std::string tm = v.active ? fmt("%5.1fs / %2ds", std::min<double>(v.elapsed, v.clear),
                                                v.clear)
                                          : rjust("-", 12);
                lines.push_back("  " + rjust(std::to_string(v.id), 4) + "  " + st + "  " +
                                ljust(party, 7) + "  " + G->cap +
                                bar(v.active ? v.elapsed / std::max(1, v.clear) : 0, bw, v.active) +
                                G->cap + " " + tm);
            }
        } else {
            lines.push_back(std::string(dim()) +
                            "  Compact view: enlarge the terminal for party numbers and timers" +
                            reset());
            size_t shown = 0;
            for (int r = 0; r < lay.gridRows; ++r) {
                std::string line = " ";
                for (int c = 0; c < lay.gridCols; ++c) {
                    size_t i = static_cast<size_t>(r * lay.gridCols + c);
                    if (i >= views.size()) break;
                    const View& v = views[i];
                    std::string st = v.active ? std::string(green()) + "active" + reset()
                                              : std::string(dim()) + "empty " + reset();
                    line += " " + rjust(std::to_string(v.id), 3) + " " + st + " " + G->cap +
                            bar(v.active ? v.elapsed / std::max(1, v.clear) : 0, 10, v.active) +
                            G->cap + " ";
                    ++shown;
                }
                lines.push_back(line);
            }
            if (lay.truncated && shown < views.size()) {
                int a = 0;
                for (size_t i = shown; i < views.size(); ++i) a += views[i].active ? 1 : 0;
                int e = static_cast<int>(views.size() - shown) - a;
                lines.push_back(std::string(yellow()) +
                                fmt("  %s %d more not shown (%d active, %d empty): enlarge the terminal",
                                    G->more, static_cast<int>(views.size() - shown), a, e) +
                                reset());
            }
        }
        lines.push_back(rule);

        // Recent events (newest last), fixed height so the frame never jumps
        for (int i = 0; i < lay.logLines; ++i) {
            if (i < static_cast<int>(recent.size()))
                lines.push_back(std::string("  ") + dim() + fmt("[%5.1fs] ", recent[i].at) + reset() +
                                recent[i].text);
            else
                lines.push_back("");
        }

        // Marquee ticker
        lines.push_back(std::string("  ") + dim() + G->ticker + reset() + "  " + cyan() + ticker + reset());

        // ---- emit the frame in one write ----
        std::string out = "\x1b[?2026h";   // begin synchronized update (ignored if unsupported)
        if (cols != lastCols_ || rows != lastRows_) {
            if (lastHeight_ > 0) out += "\x1b[H\x1b[2J";   // resized: start clean at the top
            lastCols_ = cols;
            lastRows_ = rows;
        } else if (lastHeight_ > 0) {
            out += "\x1b[" + std::to_string(lastHeight_) + "A\r";   // back to the frame's top
        }
        for (const auto& l : lines) out += fit(l, W) + "\x1b[K\n";
        out += "\x1b[J\x1b[?2026l";
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        lastHeight_ = static_cast<int>(lines.size());
    }

    // ---------------------------------------------------------------- data
    const int n_, t1_, t2_;
    const long long totalParties_, leftT_, leftH_, leftD_;
    const bool live_;

    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist_;

    mutable std::mutex mtx_;
    std::condition_variable instanceFree_;
    std::condition_variable monitorCv_;
    std::deque<int> freeInstances_;                  // FIFO of free instance indices
    std::vector<std::unique_ptr<Instance>> instances_;
    std::vector<std::thread> workers_;
    std::thread monitor_;

    bool shutdown_ = false;
    bool abort_ = false;
    bool monitorStop_ = false;
    long long dispatched_ = 0;
    long long completed_ = 0;
    long long cutShort_ = 0;
    std::deque<Event> events_;
    std::vector<std::string> tape_;
    size_t tickerPos_ = 0;
    int tickerWidth_ = 60;

    Clock::time_point start_, end_;

    // monitor-thread-only drawing state
    unsigned long frame_ = 0;
    int lastHeight_ = 0, lastCols_ = 0, lastRows_ = 0;
};

// ----------------------------------------------------------------------------
//  Input validation
// ----------------------------------------------------------------------------
struct FieldSpec {
    const char* key;
    const char* prompt;
    long long lo, hi;
};

static const FieldSpec FIELDS[6] = {
    {"n",  "Max concurrent instances",        1, MAX_INSTANCES},
    {"t",  "Tank players in queue",           0, MAX_PLAYERS},
    {"h",  "Healer players in queue",         0, MAX_PLAYERS},
    {"d",  "DPS players in queue",            0, MAX_PLAYERS},
    {"t1", "Fastest clear time in seconds",   1, MAX_CLEAR_TIME},
    {"t2", "Slowest clear time in seconds",   1, MAX_CLEAR_TIME},
};

static std::string clip(const std::string& s) {
    std::string t = trim(s);
    return t.size() > 24 ? t.substr(0, 21) + "..." : t;
}

// Parses one whole number and explains exactly what is wrong if it can't.
static bool checkField(const FieldSpec& f, const std::string& raw, long long& out,
                       std::string& err) {
    const std::string s = trim(raw);
    const std::string key = f.key;
    const bool isTime = key == "t1" || key == "t2";

    if (s.empty()) {
        err = "no value entered";
        return false;
    }
    if (s.find_first_of(" \t") != std::string::npos) {
        err = "enter one value only (got \"" + clip(s) + "\")";
        return false;
    }

    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        i = 1;
    }
    bool digits = false, dot = false, other = false;
    for (size_t j = i; j < s.size(); ++j) {
        char c = s[j];
        if (std::isdigit(static_cast<unsigned char>(c))) digits = true;
        else if (c == '.' && !dot) dot = true;
        else other = true;
    }
    if (other || !digits) {
        err = "\"" + clip(s) + "\" is not a whole number (use digits only, e.g. 5)";
        return false;
    }
    if (dot) {
        err = "\"" + clip(s) + "\" is a decimal; enter a whole number" +
              (isTime ? std::string(" of seconds") : std::string());
        return false;
    }

    // Accumulate with an overflow guard (anything this big is out of range anyway).
    unsigned long long v = 0;
    for (size_t j = i; j < s.size(); ++j) {
        v = v * 10 + static_cast<unsigned>(s[j] - '0');
        if (v > 1000000000000000000ULL) {
            err = "\"" + clip(s) + "\" is far too " + (neg ? "small" : "large") +
                  "; allowed range is " + std::to_string(f.lo) + " to " + std::to_string(f.hi);
            return false;
        }
    }
    long long val = neg ? -static_cast<long long>(v) : static_cast<long long>(v);

    if (val < f.lo) {
        if (key == "n")
            err = "at least 1 instance is needed";
        else if (isTime)
            err = "clear time must be at least 1 second";
        else
            err = "a player count cannot be negative";
        return false;
    }
    if (val > f.hi) {
        if (key == "n")
            err = "at most " + std::to_string(f.hi) + " instances are supported";
        else if (isTime)
            err = "clear time can be at most " + std::to_string(f.hi) + " seconds (spec: t2 <= 15)";
        else
            err = "at most " + std::to_string(f.hi) + " players per role are supported";
        return false;
    }
    out = val;
    return true;
}

static void printUsage(std::ostream& os, const char* prog) {
    os << "Usage:\n"
       << "  " << prog << "                     prompt for each value\n"
       << "  " << prog << " n t h d t1 t2       pass all six values\n"
       << "  " << prog << " --plain [values]    line-by-line log instead of the live view\n"
       << "  " << prog << " --ascii [values]    live view drawn with plain ASCII characters\n\n"
       << "  n   max concurrent instances   1 to " << MAX_INSTANCES << "\n"
       << "  t   tank players in queue      0 to " << MAX_PLAYERS << "\n"
       << "  h   healer players in queue    0 to " << MAX_PLAYERS << "\n"
       << "  d   DPS players in queue       0 to " << MAX_PLAYERS << "\n"
       << "  t1  fastest clear time (s)     1 to " << MAX_CLEAR_TIME << "\n"
       << "  t2  slowest clear time (s)     t1 to " << MAX_CLEAR_TIME << "\n";
}

struct Config {
    long long v[6];   // n, t, h, d, t1, t2
    long long& n()  { return v[0]; }
    long long& t()  { return v[1]; }
    long long& h()  { return v[2]; }
    long long& d()  { return v[3]; }
    long long& t1() { return v[4]; }
    long long& t2() { return v[5]; }
};

static bool loadFromArgs(const std::vector<std::string>& a, Config& c, const char* prog) {
    std::vector<std::string> errors;
    bool ok[6];
    for (int i = 0; i < 6; ++i) {
        std::string err;
        ok[i] = checkField(FIELDS[i], a[static_cast<size_t>(i)], c.v[i], err);
        if (!ok[i]) errors.push_back(std::string(FIELDS[i].key) + ": " + err);
    }
    if (ok[4] && ok[5] && c.t2() < c.t1())
        errors.push_back(fmt("t2: slowest clear time (%lld) must be >= fastest clear time t1 (%lld)",
                             c.t2(), c.t1()));
    if (errors.empty()) return true;
    std::cerr << "Invalid input:\n";
    for (const auto& e : errors) std::cerr << "  - " << e << "\n";
    std::cerr << "\n";
    printUsage(std::cerr, prog);
    return false;
}

static long long promptField(const FieldSpec& f, long long minValue) {
    using namespace term;
    const long long lo = std::max(f.lo, minValue);
    while (true) {
        std::cout << "  " << f.prompt << " (" << f.key << ") " << dim() << "[" << lo << "-" << f.hi
                  << "]" << reset() << ": " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            std::cerr << "Input ended before all values were entered. Exiting.\n";
            std::exit(1);
        }
        const std::string cmd = lower(trim(line));
        if (cmd == "q" || cmd == "quit" || cmd == "exit") {
            std::cout << "Cancelled.\n";
            std::exit(0);
        }
        long long v;
        std::string err;
        if (!checkField(f, line, v, err)) {
            std::cout << "    " << red() << G->bad << " " << err << reset() << "\n";
            continue;
        }
        if (v < lo) {   // only t2 has a dynamic minimum (t1)
            std::cout << "    " << red() << G->bad << " must be at least t1 = " << lo
                      << " (the slowest clear time can't be faster than the fastest)" << reset()
                      << "\n";
            continue;
        }
        return v;
    }
}

// ----------------------------------------------------------------------------
//  main
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* prog = argc > 0 ? argv[0] : "lfg_queue";
    bool forcePlain = false;
    std::vector<std::string> values;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--plain") {
            forcePlain = true;
        } else if (a == "--ascii") {
            term::G = &term::ASCII_GLYPHS;
        } else if (a == "-h" || a == "--help") {
            printUsage(std::cout, prog);
            return 0;
        } else if (a.size() > 1 && a[0] == '-' && !std::isdigit(static_cast<unsigned char>(a[1]))) {
            std::cerr << "Unknown option \"" << a << "\"\n\n";
            printUsage(std::cerr, prog);
            return 1;
        } else {
            values.push_back(a);
        }
    }

    const bool tty = term::stdoutIsTerminal();
    const bool ansi = tty && term::enableAnsi();
    term::color = ansi && std::getenv("NO_COLOR") == nullptr;
    using namespace term;

    Config cfg{};
    if (values.size() == 6) {
        if (!loadFromArgs(values, cfg, prog)) return 1;
    } else if (values.empty()) {
        std::cout << bold() << "=== LFG Dungeon Queue ===" << reset() << "\n"
                  << dim() << "Enter whole numbers. Type q to quit." << reset() << "\n";
        for (int i = 0; i < 5; ++i) cfg.v[i] = promptField(FIELDS[i], LLONG_MIN);
        cfg.t2() = promptField(FIELDS[5], cfg.t1());
    } else {
        std::cerr << "Expected 6 values (n t h d t1 t2) but got " << values.size() << ".\n\n";
        printUsage(std::cerr, prog);
        return 1;
    }

    // All-or-nothing party formation: a party exists only if all five roles do.
    const long long parties = std::min({cfg.t(), cfg.h(), cfg.d() / 3});
    const long long leftT = cfg.t() - parties;
    const long long leftH = cfg.h() - parties;
    const long long leftD = cfg.d() - parties * 3;
    const long long waves = cfg.n() > 0 ? (parties + cfg.n() - 1) / cfg.n() : 0;

    int cols, rows;
    term::size(cols, rows);
    bool live = ansi && !forcePlain;
    std::string modeNote;
    if (live && (cols < MIN_LIVE_COLS || rows < MIN_LIVE_ROWS)) {
        live = false;
        modeNote = fmt("terminal is %dx%d, the live view needs %dx%d; using the line-by-line log",
                       cols, rows, MIN_LIVE_COLS, MIN_LIVE_ROWS);
    }

    // Configuration summary and warnings
    std::cout << "\n" << bold() << "Configuration" << reset() << "\n"
              << "  Instances (n)          " << cfg.n() << "\n"
              << "  Tanks / Healers / DPS  " << cfg.t() << " / " << cfg.h() << " / " << cfg.d() << "\n"
              << "  Clear time             " << cfg.t1() << "-" << cfg.t2() << "s\n"
              << "  Full parties           " << parties << "\n";
    if (parties > 0)
        std::cout << "  Estimated run time     " << waves * cfg.t1() << "-" << waves * cfg.t2() << "s\n";
    std::cout << "  Display                "
              << (live ? "live view (use --plain for a line-by-line log)" : "line-by-line log") << "\n";

    std::vector<std::string> warnings;
    if (parties == 0) {
        std::vector<std::string> missing;
        if (cfg.t() == 0) missing.push_back("a tank");
        if (cfg.h() == 0) missing.push_back("a healer");
        if (cfg.d() < 3) missing.push_back(fmt("3 DPS (have %lld)", cfg.d()));
        std::string m;
        for (size_t i = 0; i < missing.size(); ++i) m += (i ? ", " : "") + missing[i];
        warnings.push_back("No full party can be formed: missing " + m +
                           ". Every instance will stay empty.");
    } else {
        if (cfg.n() > parties)
            warnings.push_back("Only " + plural(parties, "party", "parties") + " can be formed, so " +
                               plural(cfg.n() - parties, "instance", "instances") +
                               " will stay empty the whole run.");
        if (leftT + leftH + leftD > 0)
            warnings.push_back(fmt("Leftover: %lld tank(s), %lld healer(s), %lld DPS can't form a party.",
                                   leftT, leftH, leftD));
        if (waves * cfg.t2() > 300)
            warnings.push_back(fmt("Long run: up to %llds. Press Ctrl+C any time to stop early and "
                                   "see the summary.",
                                   waves * cfg.t2()));
    }
    if (!modeNote.empty()) warnings.push_back("Display: " + modeNote + ".");
    for (const auto& w : warnings) std::cout << "  " << yellow() << "! " << w << reset() << "\n";
    std::cout << "\n" << std::flush;

    std::signal(SIGINT, onSigint);   // installed after input so Ctrl+C at a prompt still quits

    DungeonManager manager(static_cast<int>(cfg.n()), static_cast<int>(cfg.t1()),
                           static_cast<int>(cfg.t2()), parties, leftT, leftH, leftD, live);
    manager.run();
    manager.printSummary();
    return manager.interrupted() ? 130 : 0;
}

# LFG Dungeon Queue: Process Synchronization

A C++17 multithreaded simulation of an MMORPG "Looking for Group" dungeon queue.
Up to `n` dungeon instances run at the same time. Parties of 1 tank + 1 healer + 3 DPS
are matched and sent in, with no deadlock and no starvation. A live console dashboard
redraws in place, with smooth progress bars and a scrolling marquee of events.

## Files

| File | Purpose |
|---|---|
| `lfg_queue.cpp` | Complete source code (single file) |
| `README.md` | Build steps, usage, input rules and demo script |

## Requirements

- A C++17 compiler with thread support
  - Linux / macOS: `g++` 7+ or `clang++` 5+
  - Windows: Visual Studio 2017+ or MinGW-w64 `g++` (POSIX-threads build, e.g. from MSYS2)
- For the live view on Windows: Windows 10 or newer. Windows Terminal is recommended.

## Build

**Linux / macOS**
```bash
g++ -std=c++17 -O2 -pthread lfg_queue.cpp -o lfg_queue
```

**Windows: MinGW-w64 / MSYS2**
```bash
g++ -std=c++17 -O2 -pthread lfg_queue.cpp -o lfg_queue.exe
```

**Windows: Visual Studio (Developer Command Prompt)**
```bat
cl /std:c++17 /EHsc /O2 lfg_queue.cpp
```
In the Visual Studio IDE, set *Project → Properties → C/C++ → Language → C++ Language
Standard* to ISO C++17 or later. The source is pure ASCII (special characters are
written as `\u` escapes), so no encoding flags are needed.

## Run

```bash
./lfg_queue                         # prompts for each value
./lfg_queue  n  t  h  d  t1  t2     # all six values as arguments
./lfg_queue  3  5  5  15  2  6      # example
./lfg_queue --plain 3 5 5 15 2 6    # line-by-line log instead of the live view
./lfg_queue --ascii 3 5 5 15 2 6    # live view drawn with plain ASCII characters
./lfg_queue --help
```

| Input | Meaning | Allowed values |
|---|---|---|
| `n`  | max concurrent instances | 1 – 100 |
| `t`  | tank players in queue | 0 – 1,000,000 |
| `h`  | healer players in queue | 0 – 1,000,000 |
| `d`  | DPS players in queue | 0 – 1,000,000 |
| `t1` | fastest clear time (seconds) | 1 – 15 |
| `t2` | slowest clear time (seconds) | t1 – 15 |

Press **Ctrl+C** during a run to stop cleanly. No new parties are sent in, parties
still inside are marked as cut short, and the summary still prints. Press Ctrl+C a
second time to quit immediately.

## Output

**Live view** (default when the output is a terminal). The dashboard redraws in place
about 16 times a second instead of printing new lines:

- **Overall bar:** fills smoothly, counting the share of every run already completed.
- **One row per instance:** `active` or `empty`, the party inside, a progress bar and an
  `elapsed / clear time` timer.
- **Counts:** parties cleared, in a dungeon, still waiting, and leftover players.
- **Event log:** the last four enters and clears.
- **Marquee ticker:** every event scrolls past from right to left.

The view adapts to the terminal. With many instances or a short window it switches to a
compact grid that still shows each instance as `active` or `empty`. Resizing the window
redraws the view. If the terminal is smaller than 60×14, or the output is redirected to a
file, the program uses the line-by-line log instead. Colours can be turned off with the
`NO_COLOR` environment variable.

**Line-by-line log** (`--plain`). Every event prints a timestamped line followed by the
status of every instance.

**Summary** (both modes). For each instance it shows parties served, total time served
and a share bar, then totals, wall-clock time, and any leftover players.

## Input validation

Every value is checked before the simulation starts. Interactive mode re-asks until the
value is valid, and `q` quits. Argument mode lists **all** problems at once and exits
with code 1.

| Input | Example | Message |
|---|---|---|
| Nothing entered | `` | no value entered |
| Not a number | `abc`, `1e3`, `1,000`, `+` | "abc" is not a whole number (use digits only, e.g. 5) |
| Decimal | `2.5`, `2.0` | "2.5" is a decimal; enter a whole number |
| More than one value | `3 4` | enter one value only |
| Negative player count | `-1` | a player count cannot be negative |
| No instances | `0`, `-3` for n | at least 1 instance is needed |
| Clear time below 1 | `0` for t1 | clear time must be at least 1 second |
| Above the limit | `101` for n, `16` for t2 | at most 100 instances / clear time can be at most 15 seconds |
| Huge number | `99999999999999999999` | is far too large; allowed range is ... |
| t2 smaller than t1 | t1 `5`, t2 `2` | slowest clear time (2) must be >= fastest clear time t1 (5) |
| Wrong number of arguments | 5 or 7 values | Expected 6 values (n t h d t1 t2) but got 5 |
| Unknown option | `--fast` | Unknown option "--fast" |
| Input ends early | Ctrl+D / end of piped input | Input ended before all values were entered |

Leading/trailing spaces, Windows line endings, a leading `+` and leading zeros (`007`)
are accepted.

Valid but unusual inputs run normally and print a warning first:

- **No full party possible** (e.g. 0 healers, or fewer than 3 DPS): the warning says which
  role is missing. Every instance stays empty and the program ends cleanly.
- **More instances than parties:** the warning says how many instances will stay empty.
- **Leftover players:** the warning lists who can't be placed in a full party.
- **Long runs** (estimated over 5 minutes): the warning says Ctrl+C stops the run early.

## How it works

1. **Party formation (atomic, all-or-nothing).** All players arrive at once, so the
   number of parties is `min(t, h, ⌊d/3⌋)`. A party exists only when all five roles are
   available, so no thread ever holds a partial party.
2. **Instances are worker threads.** Each of the `n` instances waits on its own condition
   variable until it is handed a party.
3. **The dispatcher is the main thread.** It takes parties in FIFO order. If no instance
   is free, it blocks on the `instanceFree_` condition variable, which acts as a counting
   semaphore initialised to `n`. Otherwise it gives the party to the longest-idle
   instance (front of a FIFO queue of free instances).
4. **Clearing.** The instance calls `wait_until(start + t)`, which **releases the mutex**
   while the party is in the dungeon. It then marks itself empty, records its stats,
   rejoins the back of the free queue and signals the dispatcher.
5. **Monitor thread.** About 16 times a second it copies a snapshot of the shared state
   under the mutex and draws the frame *outside* the lock, so drawing never stalls the
   workers. It also turns Ctrl+C (a lock-free atomic flag set by the signal handler) into
   a clean stop.
6. **Shutdown.** After the last party is dispatched, a `shutdown_` flag wakes idle
   instances so they exit. Busy instances finish first. The main thread joins every
   thread, then prints the summary.

### Why no deadlock
- Only **one** mutex exists, so circular wait between locks is impossible.
- Parties are formed atomically, so there is no hold-and-wait on players.
- Every wait (`wait`, `wait_until`) releases the mutex while sleeping.
- Leftover players never wait on anything, so the program always ends.

### Why no starvation
- Parties are served strictly in FIFO order (party 1, 2, 3 …).
- Free instances are reused in FIFO order, so no instance sits idle while others take
  all the work.
- The queue is finite and every clear time is at most `t2`, so the k-th party is
  guaranteed to finish within `⌈k / n⌉ · t2` seconds.

## Demo script (for the video)

| # | Command | What it shows |
|---|---|---|
| 1 | `./lfg_queue 3 6 6 19 2 5` | Live view: bars fill, instances free up and refill; 1 leftover DPS |
| 2 | `./lfg_queue 3 10 10 30 2 5` | More parties than instances: the dispatcher waits for a free slot |
| 3 | `./lfg_queue 5 2 2 6 1 2` | More instances than parties: extra instances stay `empty` (with a warning) |
| 4 | `./lfg_queue 3 5 0 15 1 2` | No healers: warning, zero parties, clean exit (no deadlock) |
| 5 | `./lfg_queue 4 40 40 120 1 1` | Fairness: each instance serves exactly 10 parties |
| 6 | `./lfg_queue 2 10 10 30 3 3`, then Ctrl+C | Clean stop and partial summary |
| 7 | `./lfg_queue` then `abc`, ` `, `2.5`, `-1`, `0`, `500`, `3 4`, and t2 < t1 | Each bad value is rejected with its own message |
| 8 | `./lfg_queue 0 1 1 3 5 2` | Argument mode lists every problem at once |

## Verification

Checked with ThreadSanitizer (no data races) in line-by-line mode, in live mode inside a
pseudo-terminal, and with Ctrl+C during a run:
```bash
g++ -std=c++17 -g -fsanitize=thread -pthread lfg_queue.cpp -o lfg_tsan
./lfg_tsan 4 40 40 120 1 1
```
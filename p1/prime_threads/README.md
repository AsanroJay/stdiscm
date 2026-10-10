# Basic Threading — Multithreaded Prime Search (4 variants)

A C++ program that creates **x** threads to find every prime from 1 to **y**,
written four ways: two printing strategies × two ways of dividing the work.
`x` and `y` are read from `config.txt` in each variant folder.

| Folder | Printing | Task division |
|---|---|---|
| `variant1_immediate_range/` | Print immediately (thread id + timestamp) | Straight division of the search range |
| `variant2_deferred_range/` | Wait for all threads, then print everything | Straight division of the search range |
| `variant3_immediate_divisibility/` | Print immediately (thread id + timestamp) | Linear search; threads split the divisibility test of each number |
| `variant4_deferred_divisibility/` | Wait for all threads, then print everything | Linear search; threads split the divisibility test of each number |

Every run prints a **RUN START** timestamp first and a **RUN END** timestamp last.

```
prime_threads/
├── README.md                  ← this file
├── all_variants_demo.mp4      ← the four demo videos back to back
├── variant1_immediate_range/
│   ├── main.cpp               ← the whole program, one file
│   ├── config.txt             ← x and y
│   └── demo.mp4               ← video demonstration
├── variant2_deferred_range/          (same three files)
├── variant3_immediate_divisibility/  (same three files)
├── variant4_deferred_divisibility/   (same three files)
└── benchmark/
    ├── bench.cpp              ← runs all four variants and times them
    └── results.csv            ← measurements used in the slides
```

## Build

Each variant is a single `main.cpp`. Open a terminal in the variant's folder and run:

```
g++ -O2 -o primes main.cpp -pthread
```

That produces `primes` (Linux/macOS) or `primes.exe` (Windows). Repeat in each of
the four folders. Only the standard library is used — no other files or tools.

* `-O2` turns on optimisation, so the timings are meaningful. The program also builds without it.
* `-pthread` links the threading library. Recent compilers on Linux and MinGW-w64
  link it automatically, so plain `g++ -o primes main.cpp` usually works too.
* The code compiles as C++11 or newer, so any g++ from version 5 onward works.
* **Windows:** use a MinGW-w64 g++ with POSIX threads (for example from MSYS2 or
  WinLibs). Some old MinGW installs use the "win32" thread model and do not
  support `std::thread`. You can check with `g++ -v`: look for `Thread model: posix`.

## Run

Run from inside the variant folder so the program finds `config.txt`:

```
./primes                  # Windows: primes.exe
./primes my_config.txt    # optional: use a different config file
```

## Configuration (`config.txt`)

One `key = value` per line; `#` starts a comment. Edit and re-run — no rebuild needed.

| Key | Used by | Meaning | Default |
|---|---|---|---|
| `threads` | all | **x** — number of threads | 4 |
| `limit` | all | **y** — search primes in 1..y | 1000 |
| `sync_output` | 1, 3 | `true`: each line is printed under a mutex (whole lines, mixed order). `false`: no lock, so pieces of lines from different threads tear into each other — use it to see raw interleaving | true |
| `mode` | 3, 4 | `spawn`: create and join x threads **for every number** (the literal scheme). `pool`: create x threads once and synchronise them with a barrier for each number | spawn |

## What each variant does

**Straight range division (1, 2).** `[1, y]` is cut into x contiguous blocks whose
sizes differ by at most one (y = 1000, x = 4 → 1–250, 251–500, 501–750, 751–1000).
Each thread tests every number in its block by trial division up to √n.

**Divisibility split (3, 4).** Numbers 2..y are tested one at a time, in order.
For each number n, the candidate divisors 2..⌊√n⌋ are split into x chunks, one per
thread. A thread that finds a divisor sets a shared atomic flag and the others stop
early. The **last thread to finish** that number (tracked with an atomic countdown)
decides the verdict and, if n is prime, reports it. So the thread id on each line
is the thread that confirmed the prime.

**Print immediately (1, 3).** The thread prints the prime the moment it is found:
`[HH:MM:SS.microseconds] [Thread i | id <OS thread id>] prime: n`.

**Deferred printing (2, 4).** Threads store results in memory (variant 2 uses one
vector per thread, so it needs no locking at all). Only after every thread is
joined does the main thread print everything. The printed timestamps are the times
the primes were *found*. These variants also report the search-phase and
print-phase times separately.

After the results, every variant prints a per-thread summary (the work each thread
did, and for variants 3 and 4 the number of threads created), followed by the
elapsed time and the RUN END timestamp.

## Demo videos

Each folder's `demo.mp4` shows that variant's config file, the `g++` build, a full
run (x = 4, y = 1000) and a timing run across several thread counts. The videos
replay real terminal sessions: the commands were run and their output captured,
then rendered frame by frame, with captions pointing out what to look for. Prime
lines are coloured by thread; torn lines (`sync_output = false`) are shown in red.

## Benchmark

Build all four variants first (see Build), then:

```
cd benchmark
g++ -O2 -o bench bench.cpp
./bench           # full run, several minutes; writes results.csv
./bench quick     # faster: smaller limits, one run each
```

Experiment A times variants 1 and 2 at y = 5,000,000 for x = 1 … 64.
Experiment B runs all four variants (both modes for 3 and 4) at y = 20,000 for x = 1 … 16.
Each configuration is run 3 times and the median is kept. The included
`results.csv` was measured on a 2-vCPU Linux VM; on a machine with more cores, the
range-division variants keep speeding up until x reaches the core count.

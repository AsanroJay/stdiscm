# Variant 1 - print immediately, straight range division

Build: `make` (or `g++ -std=c++17 -O2 -pthread main.cpp -o primes`; MSVC: `cl /EHsc /O2 /std:c++17 main.cpp /Fe:primes.exe`; or CMake — see the top-level README).

Run: `./primes` from this folder (reads `config.txt`), or `./primes other_config.txt`.

Edit `config.txt` to change the number of threads (`threads`) and the upper bound (`limit`).
`demo.mp4` is the video demonstration of this variant.

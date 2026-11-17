# Prmd: random-access crops from a single-file image pyramid

`prmd` stores an image and its N-level 2× pyramid in **one file** (`.prmd`) and decodes any
`(level, x, y, w, h)` crop by reading and decoding only the code-blocks that crop depends on.
Coarser levels are not stored separately. Each finer level is stored as **detail bands
relative to the level above it** (integer Haar or 5/3 wavelet), so the whole pyramid costs
about as much as level 0 alone. A JPEG2000 (OpenJPEG) backend and a naive "every level tiled
independently" layout are built in for comparison.

- Modes and their options, with worked examples: [`docs/modes.md`](docs/modes.md)
- Design and interfaces: [`docs/design.md`](docs/design.md)
- Results write-up: [`docs/writeup.md`](docs/writeup.md)
- Benchmark tables (every mode, lossy, RGB): [`docs/benchmarks.md`](docs/benchmarks.md); regenerate them with `prmdbench` (below)
- Raspberry Pi 5 results: [`docs/benchmarks-pi5.md`](docs/benchmarks-pi5.md)
- Per-step pipeline timings: [`docs/pipeline_durations.md`](docs/pipeline_durations.md)

## Build

Requirements: a C++17 compiler, CMake ≥ 3.16, and the development packages for libjpeg(-turbo),
libpng, zstd, OpenJPEG 2.5 and GoogleTest. On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config \
     libjpeg-turbo8-dev libpng-dev libzstd-dev libopenjp2-7-dev libgtest-dev
```

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release        # -DPRMD_NATIVE=OFF to skip -mcpu=native
cmake --build build -j
ctest --test-dir build -j8 --output-on-failure    # 874 tests
```

This produces `build/prmd` (CLI), `build/prmdbench` (benchmark), `build/prmdpipeline`
(per-step pipeline timing), `build/prmd_tests` and `build/prmd_profile_tests`.

### Sanitizer builds

`PRMD_SANITIZE` adds `-fsanitize=...` to every target, with every finding fatal so ctest fails
on it. Use a separate build directory:

```sh
cmake -B build-asan -DPRMD_SANITIZE=address,undefined && cmake --build build-asan -j
ctest --test-dir build-asan -j8 --output-on-failure

CC=clang CXX=clang++ cmake -B build-tsan -DPRMD_SANITIZE=thread && cmake --build build-tsan -j
ctest --test-dir build-tsan -j8 --output-on-failure
```

ThreadSanitizer checks the multithreaded encoder. Build it with clang: GCC 15's libtsan crashes
(SIGILL in `pthread_cond_wait`) on aarch64 with pointer authentication.

### Pipeline timers (off by default)

The library marks each encode and decode step with `PRMD_TIMED("step")` (`include/prmd/profile.h`).
Unless `PRMD_PROFILE` is defined, the macro expands to nothing and the timers are not in the
binary. To compile them in, use a separate build directory:

```sh
cmake -B build-profile -DCMAKE_BUILD_TYPE=Release -DPRMD_PROFILE=ON && cmake --build build-profile -j
build-profile/prmdpipeline test_image.jpg --runs 5 --levels 6 --threads 1     # per-step breakdown, serial
build/prmdpipeline test_image.jpg --runs 5 --levels 6 --threads 1            # totals without timers
build/prmdpipeline test_image.jpg --runs 5 --levels 6                        # totals, automatic threads
```

`prmdpipeline` encodes once and decodes one 256² crop per level (overview, then zooming in) plus
4 pans and jumps at L0: 9 crops with the default `--levels 5` (10 with the `--levels 6` used
in [pipeline_durations.md](docs/pipeline_durations.md)). `--levels`, `--mode`, `--down`,
`--color`, `--quant`, `--entropy`, `--entropy-level`, `--tile`, `--cblk`, `--cache-mib` and
`--threads` (encoder threads, default 0 = automatic) change the scenario; `--runs N` takes the median of N runs, `--file` sets the output `.prmd` path
(default `pipeline.prmd` in the current directory) and `--label` names the run in the report. Results and method:
[`docs/pipeline_durations.md`](docs/pipeline_durations.md).

## Use

```sh
# Encode (input: JPEG or PNG; stored as gray by default)
build/prmd encode test_image.jpg img.prmd --levels 5
build/prmd encode test_image.jpg img_rgb.prmd --levels 5 --color rgb --mode cdf53
build/prmd encode test_image.jpg img_q4.prmd --mode cdf53 --quant 4      # lossy
build/prmd encode test_image.jpg img.j2k --codec j2k --levels 5          # JPEG2000 comparison

# Decode a crop to PNG (coordinates are in that level's pixels)
build/prmd decode img.prmd --level 1 --x 1900 --y 1100 --w 512 --h 512 -o crop.png --stats

# Out-of-bounds requests are rejected (exit code 4); --clamp decodes the intersection instead
build/prmd decode img.prmd --level 3 --x 1000 --y 600 --w 256 --h 256 -o crop.png --clamp

# Inspect a file: levels, blocks, bytes per level
build/prmd info img.prmd
```

`prmd encode` options:

| option | values | default |
|---|---|---|
| `--codec` | `prmd`, `j2k` | `prmd` |
| `--color` | `gray`, `rgb` | `gray` |
| `--mode` | `haar` (exact 2×2 mean), `cdf53` (5/3 wavelet), `laplacian` (any downsampler + residual), `naive` (independent levels) | `haar` |
| `--down` | `box`, `gauss` (for `laplacian` / `naive`) | `box` |
| `--levels` | 1–16 | 5 |
| `--tile`, `--cblk` | tile (reporting unit) and code-block (decode unit) size | 256, 128 |
| `--entropy` | `zstd` (fast decode), `range` (context-adaptive, ~4–5% smaller, ~12× slower decode; ~10× on a Pi 5), `raw` | `zstd` — see [design.md §7.1](docs/design.md) |
| `--entropy-level` | zstd level 1–22. 19 encodes 13–36× slower on one thread and 6–21× with the default threads (mode-dependent); it makes gray lossless files no smaller, but RGB files 2.7–4.1% and lossy files 3–12% smaller ([design.md §7.2](docs/design.md)) | 1 |
| `--quant` | 0 = lossless; Q > 0 = lossy quantizer step (`haar`, `cdf53`, `laplacian`). Laplacian guarantees ±Q/2 per level on gray | 0 |
| `--no-med` | store base-block pixels as they are, without MED prediction (the coarsest level; every level for `naive`). Faster crops, larger file: gray `naive` +23%, crops about 1.5× faster ([benchmarks.md](docs/benchmarks.md)) | MED on |
| `--no-ycocg` | store R, G, B as they are, without YCoCg-R (`rgb` only; ignored for gray) | YCoCg-R on |
| `--threads` | encoder threads: 0 = automatic (the library chooses per image from its size and the cost of the mode and coder, up to all hardware threads; small images run serially with no thread started), 1 = serial, N = at most N. The `.prmd` file is byte-identical for every value; at most one thread per code-block. J2K uses at most one thread per 4 code-blocks of a tile (4 for 256² tiles). A multithreaded encode adds a `threads N` line to the report | 0 |
| `--j2k-tile`, `--j2k-lossy-psnr` | J2K tile size; lossy 9/7 target PSNR | 256; lossless |

Exit codes: 0 ok, 1 usage, 2 I/O error, 3 format error, 4 out of range.

## Library API (C++)

```cpp
#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/image_io.h"

prmd::EncodeOptions opt;            // levels, color, transform, entropy, tile/cblk, ...
opt.threads = 0;                    // default: automatic (by image size, up to all hardware
                                    // threads); 1 = serial, on this thread
prmd::encode_file(prmd::load_image("in.jpg"), "img.prmd", prmd::Codec::Prmd, opt);

auto dec = prmd::open_decoder("img.prmd");        // .prmd or .j2k, chosen by file magic
prmd::DecodeResult r = dec->decode(2, {100, 50, 256, 256});
// r.image (Gray8/RGB8), r.rect, r.stats {tiles_touched, blocks_decoded, bytes_read, read_calls, ...}
```

Encoding is multithreaded by default (`EncodeOptions::threads = 0`, automatic: about
⌊√(serial work / 0.14 ms)⌋ threads, so none for small images and every hardware thread for
large ones; see [design.md §1.4](docs/design.md)), and the file does not depend on the thread
count. A program that already encodes several images in parallel should pass `threads = 1`.
Output files are written to a temporary file next to the target and renamed over it when
complete, so a failed encode never truncates or half-writes an existing file. Decoding is
single-threaded; a decoder must not be shared between threads.

## Benchmark

```sh
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 300 --lossy
build/prmdbench test_image.jpg --size 4096 --iters 100 --color rgb --no-entropy-table \
                --codecs haar,cdf53,laplacian-box,laplacian-gauss,naive,j2k-t256,haar+range,haar+zstd19 \
                --lossy --out build/bench/results_rgb.md
```

Encodes choose their thread count automatically unless `--threads N` is given. The published
tables ran each command twice, as above (automatic) and with `--threads 1` (serial), and show
both encode times; decoding is single-threaded either way. `--scaling 1,2,4,8,0` adds an
encode-scaling table (time per thread count, the best speedup over `N=1`, and a check that
every count writes the same file). On the benchmark machine (14 Apple Silicon cores) a 4096² gray `haar` encode takes
72 ms on one thread and 20 ms with the automatic count; on a Raspberry Pi 5 (4 cores),
see [`docs/benchmarks-pi5.md`](docs/benchmarks-pi5.md).

It reports file sizes (vs raw and vs naive), mean/p50/p95 latency for random 256² and 512² crops
at every level, tiles touched, blocks decoded and bytes read per crop, an entropy-coder comparison
(zstd/range/raw on haar, cdf53 and naive, so "vs naive" is always same-coder),
a cached random-walk trajectory, full-level decode times, and PSNR of each codec's levels against
the plain 2×2-mean pyramid.

All benchmark output goes to the build directory, which is not tracked in git. The default
`--out` is `bench/results.md` in the directory of the `prmdbench` executable, resolved at run time
(here `build/bench/results.md`, whatever the current directory, and still next to the binary
if `build/` is copied to another host), and the samples go next
to the report in `build/bench/samples/`: the centre 256² of level 3, saved **once per level definition**
(`haar`, `box`, `gauss`, `cdf53`, and each JPEG2000 tiling), for comparing aliasing and blur by eye.
Codecs that share a definition, such as `naive` and `laplacian-box`, or a mode under any entropy
coder, decode identical levels. JPEG2000 runs its 5/3 transform per tile, so its levels differ
slightly from whole-image `cdf53` near tile borders, and each tiling gets its own sample.

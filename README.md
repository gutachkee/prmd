# Prmd: random-access crops from a single-file image pyramid

`prmd` stores an image and its N-level 2× pyramid in **one file** (`.prmd`) and decodes any
`(level, x, y, w, h)` crop by reading and decoding only the code-blocks that crop depends on.
Coarser levels are not stored separately. Each finer level is stored as **detail bands
relative to the level above it** (integer Haar or 5/3 wavelet), so the whole pyramid costs
about as much as level 0 alone. A JPEG2000 (OpenJPEG) backend and a naive "every level tiled
independently" layout are built in for comparison.

- Modes and their options, with worked examples: [`docs/modes.md`](docs/modes.md)
- Design and interfaces: [`docs/DESIGN.md`](docs/DESIGN.md)
- Results write-up: [`docs/WRITEUP.md`](docs/WRITEUP.md)
- Benchmark tables (every mode, lossy, RGB): [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md); regenerate them with `prmdbench` (below)
- Raspberry Pi 5 results: [`docs/Pi5_stats.md`](docs/Pi5_stats.md)
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
ctest --test-dir build -j8 --output-on-failure    # 222 tests
```

This produces `build/prmd` (CLI), `build/prmdbench` (benchmark), `build/prmdpipeline`
(per-step pipeline timing) and `build/prmd_tests`.

### Pipeline timers (off by default)

The library marks each encode and decode step with `PRMD_TIMED("step")` (`include/prmd/profile.h`).
Unless `PRMD_PROFILE` is defined, the macro expands to nothing and the timers are not in the
binary. To compile them in, use a separate build directory:

```sh
cmake -B build-profile -DCMAKE_BUILD_TYPE=Release -DPRMD_PROFILE=ON && cmake --build build-profile -j
build-profile/prmdpipeline test_image.jpg --runs 5                # per-step breakdown
build/prmdpipeline test_image.jpg --runs 5                        # totals without timers
```

`prmdpipeline` encodes once and decodes one 256² crop per level (overview, then zooming in) plus
4 pans and jumps at L0: 10 crops with the default `--levels 6`. `--levels`, `--mode`, `--down`,
`--color`, `--quant`, `--entropy`, `--entropy-level`, `--tile`, `--cblk` and `--cache-mib` change
the scenario; `--runs N` takes the median of N runs, `--file` sets the output `.prmd` path
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
| `--entropy` | `zstd` (fast decode), `range` (context-adaptive, ~4–5% smaller, ~12× slower decode; ~10× on a Pi 5), `raw` | `zstd` — see [DESIGN.md §7.1](docs/DESIGN.md) |
| `--entropy-level` | zstd level 1–22. 19 encodes 4–34× slower (mode-dependent); it makes gray lossless files no smaller, but RGB files 2.7–4.1% and lossy files 3–12% smaller ([DESIGN.md §7.2](docs/DESIGN.md)) | 1 |
| `--quant` | 0 = lossless; Q > 0 = lossy quantizer step (`haar`, `cdf53`, `laplacian`). Laplacian guarantees ±Q/2 per level on gray | 0 |
| `--j2k-tile`, `--j2k-lossy-psnr` | J2K tile size; lossy 9/7 target PSNR | 256; lossless |

Exit codes: 0 ok, 1 usage, 2 I/O error, 3 format error, 4 out of range.

## Library API (C++)

```cpp
#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/image_io.h"

prmd::EncodeOptions opt;            // levels, color, transform, entropy, tile/cblk, ...
prmd::encode_file(prmd::load_image("in.jpg"), "img.prmd", prmd::Codec::Prmd, opt);

auto dec = prmd::open_decoder("img.prmd");        // .prmd or .j2k, chosen by file magic
prmd::DecodeResult r = dec->decode(2, {100, 50, 256, 256});
// r.image (Gray8/RGB8), r.rect, r.stats {tiles_touched, blocks_decoded, bytes_read, read_calls, ...}
```

## Benchmark

```sh
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 300 --lossy
build/prmdbench test_image.jpg --size 4096 --iters 100 --color rgb --no-entropy-table \
                --codecs haar,cdf53,naive,j2k-t256,haar+range --out build/bench/results_rgb.md
```

It reports file sizes (vs raw and vs naive), mean/p50/p95 latency for random 256² and 512² crops
at every level, tiles touched, blocks decoded and bytes read per crop, an entropy-coder comparison
(zstd/range/raw on haar, cdf53 and naive, so "vs naive" is always same-coder),
a cached random-walk trajectory, full-level decode times, and PSNR of each codec's levels against
the plain 2×2-mean pyramid.

All benchmark output goes to the build directory, which is not tracked in git. The default
`--out` is `<build dir>/bench/results.md` (here `build/bench/results.md`), and the samples go next
to the report in `build/bench/samples/`: the centre 256² of level 3, saved **once per level definition**
(`haar`, `box`, `gauss`, `cdf53`, and each JPEG2000 tiling), for comparing aliasing and blur by eye.
Codecs that share a definition, such as `naive` and `laplacian-box`, or a mode under any entropy
coder, decode identical levels. JPEG2000 runs its 5/3 transform per tile, so its levels differ
slightly from whole-image `cdf53` near tile borders, and each tiling gets its own sample.

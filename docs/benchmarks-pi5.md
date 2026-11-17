# Prmd on a Raspberry Pi 5

The same `prmdbench` runs as in [benchmarks.md](benchmarks.md), on a Raspberry Pi 5, with the multithreaded encoder: the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd at level 1, 300 random crops per row (100 for RGB), seed 1, file in the page cache. Decoding is single-threaded, so every crop and full-level time is one core. Encode times are given for one thread and for the default automatic thread count, which uses all 4 cores for every 4096² image.

## Setup

| | Raspberry Pi 5 |
|---|---|
| CPU | 4× Cortex-A76 @ 2.4 GHz (`ondemand` governor) |
| RAM | 4 GB |
| OS | Debian 13 (trixie), kernel 6.18 `rpi-2712` |
| compiler | GCC 14.2, Release `-O2 -mcpu=native` |
| zstd · OpenJPEG · libjpeg · libpng | 1.5.7 · 2.5.3 · 2.1.5 · 1.6.48 |
| code | branch `threading`, commit 7b350ea (copied with `git archive`, built in a fresh directory) |
| tests (`ctest`) | 884 / 884 pass (26 s at `-j4`) |
| date | 2026-11-17 |

The Pi had stock cooling and no overclock. It ran at 49–61 °C over the 34 minutes of benchmarks, the clock stayed at 2.4 GHz under load, and `vcgencmd get_throttled` reported `0x0` (no throttling) at every 20 s sample. No package had to be installed.

To reproduce on the Pi:

```sh
sudo apt install build-essential cmake pkg-config \
     libjpeg62-turbo-dev libpng-dev libzstd-dev libopenjp2-7-dev libgtest-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
(cd build && ctest -j4)
# decode tables (each run once with --threads 1 and once without, i.e. automatic)
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 300 --lossy --threads 1
build/prmdbench test_image.jpg --size 4096 --iters 100 --color rgb --no-entropy-table \
                --codecs haar,cdf53,naive,j2k-t256,haar+range --out build/bench/results_rgb.md --threads 1
# encode times and scaling (gray and rgb)
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 20 --color gray --no-entropy-table \
                --codecs haar,cdf53,laplacian-box,laplacian-gauss,naive,j2k-t256,j2k-t1024,haar+zstd19,cdf53+zstd19,naive+zstd19,haar+range \
                --scaling 1,2,3,4,0
```

**Where the encode times come from.** The encode columns below are the N=1 and *auto* columns of the scaling runs (median of 3, 1 above 3 s, work directory in `/tmp`, which is tmpfs on the Pi). The single-shot encode columns of the latency runs agreed for new files, but those runs used a work directory on the SD card (ext4), and on ext4 replacing an existing file makes the kernel write the new file's data out at `rename()`: the second run, which wrote over the first run's files, measured `haar` at 243 ms with 4 threads against 225 ms on one. A full-image `prmd encode` on one thread took 650 ms to a new file or to `/tmp`, and 1050–1080 ms over an existing file on the SD card. The encoder before threading paid the same when it truncated an existing file (890 → 1300 ms), so this is the storage, not the thread pool. Write to a new file or to tmpfs when timing encodes on the Pi.

## Summary

- **The files are portable.** File sizes, bytes per level, blocks, tiles and bytes read per crop, and every PSNR value match [benchmarks.md](benchmarks.md) to the last digit, and the scaling runs wrote the same file at every thread count. The format and the encoder are deterministic across machines, compilers and thread counts.
- **A random 256² gray crop takes 0.66–1.43 ms** at any level with `haar`, and a 512² crop takes 3.4–4.6 ms. The whole 5-level pyramid encodes in 219 ms on one core and **96 ms on all four** (automatic).
- **Threads speed up the encode 2.1–2.7× with zstd 1 and 3.0–3.5× with the slow coders and JPEG2000**, out of 4 cores. The automatic count picks 4 for every 4096² image, the fastest or tied-fastest count in every row (auto and N=4 differ by at most 7%, run-to-run noise at the same count).
- **`.prmd` crops decode 11–20× faster than JPEG2000 crops.**
- **`naive` is slightly faster than `haar` on the Pi:** 6% at 256², 8% at 512², and 16% on a full L0 decode. `haar` still saves 23% of the file.
- **`cdf53` is now close to `haar`:** 1.2× per crop and on a full-L0 decode, and 1.4× its encode. Before its row-major lifting it was 4.8× `haar`'s full-L0 decode (1318 → 321 ms) and 4.4× its encode.

## File size

| codec | file size | vs raw L0 (16.00 MiB) | vs naive |
|---|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | **−25.7%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | −27.9% |

## Crop latency (gray, lossless)

Mean / p95 per crop, from the `--threads 1` run (the automatic-thread run gave the same decode times within 5%). Tiles and bytes per crop are the same as in benchmarks.md.

| crop | level | haar | naive | J2K, 256² tiles | J2K ÷ haar |
|---|---:|---:|---:|---:|---:|
| 256² | L0 | 1.43 / 1.61 ms | 1.34 / 1.38 ms | 23.5 / 26.3 ms | 16.4× |
| 256² | L1 | 1.36 / 1.53 ms | 1.32 / 1.35 ms | 21.5 / 22.9 ms | 15.8× |
| 256² | L2 | 1.30 / 1.59 ms | 1.30 / 1.32 ms | 15.7 / 16.5 ms | 12.1× |
| 256² | L3 | 1.33 / 1.35 ms | 1.30 / 1.32 ms | 14.1 / 14.5 ms | 10.6× |
| 256² | L4 (whole level) | 0.66 / 0.66 ms | 0.66 / 0.66 ms | 13.2 / 13.4 ms | 20.0× |
| 512² | L0 | 4.59 / 4.80 ms | 4.21 / 4.47 ms | 60.6 / 66.5 ms | 13.2× |
| 512² | L1 | 4.50 / 4.82 ms | 4.15 / 4.22 ms | 59.1 / 61.8 ms | 13.1× |
| 512² | L2 | 4.48 / 4.54 ms | 4.10 / 4.15 ms | 50.1 / 51.1 ms | 11.2× |
| 512² | L3 (whole level) | 3.38 / 3.39 ms | 2.83 / 2.84 ms | 43.4 / 43.6 ms | 12.8× |

The Pi's p95 sits close to its mean, at most about 22% above it. Decoding all of L0 and then cropping would cost 271 ms, about **190×** a 256² crop. On the 1000-step random walk, a 64 MiB block cache brings a `haar` crop from 1.40 ms down to 0.60 ms, and a `naive` one from 1.33 ms to 0.17 ms. Every decode figure is within 2% of the previous run at 7b0db91.

## Encode and decode time per mode

| mode | encode: 1 thread / auto (4) | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 219 / 96 ms | 1.43 / 1.61 ms | 4.59 / 4.80 ms | 271 ms | 63.0 ms |
| `cdf53` | 300 / 144 ms | 1.68 / 1.89 ms | 5.45 / 5.83 ms | 321 ms | 74.9 ms |
| `laplacian-box` | 447 / 197 ms | 2.05 / 2.26 ms | 6.86 / 7.06 ms | 412 ms | 94.6 ms |
| `laplacian-gauss` | 592 / 224 ms | 1.89 / 2.08 ms | 6.30 / 6.52 ms | 377 ms | 85.5 ms |
| `naive` | 269 / 101 ms | 1.34 / 1.38 ms | 4.21 / 4.47 ms | 229 ms | 53.5 ms |
| JPEG2000, 256² tiles | 2694 / 823 ms | 23.5 / 26.3 ms | 60.6 / 66.5 ms | 2216 ms | 605 ms |

On one core `haar` and `naive` encode 1.1–1.4× faster than `cdf53` and 1.7–2.7× faster than `laplacian`, and 10–12× faster than JPEG2000. With threads every `.prmd` mode encodes the 4096² image in 0.1–0.22 s. Compared with the encoder before threading on the same Pi (previous version of this page), one-thread encodes are 1.1–4.1× faster: `haar` 283 → 219 ms, `cdf53` 1237 → 300 ms, `laplacian-gauss` 1058 → 592 ms, JPEG2000 unchanged.

## Encode scaling

`prmdbench --scaling 1,2,3,4,0` (commands under [Setup](#setup)): `encode_file` wall time, median of 3 (1 above 3 s). *best vs N=1*: the N=1 time over the fastest column's. Every row wrote the same file at every count.

Gray:

| codec | N=1 | N=2 | N=3 | N=4 | auto | best vs N=1 | identical |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar | 219 ms | 123 ms | 101 ms | 97.3 ms | 96.0 ms (4 used) | 2.28× (auto) | yes |
| cdf53 | 300 ms | 179 ms | 144 ms | 134 ms | 144 ms (4 used) | 2.24× (N=4) | yes |
| laplacian-box | 447 ms | 259 ms | 206 ms | 188 ms | 197 ms (4 used) | 2.37× (N=4) | yes |
| laplacian-gauss | 592 ms | 334 ms | 254 ms | 224 ms | 224 ms (4 used) | 2.64× (N=4) | yes |
| naive | 269 ms | 144 ms | 108 ms | 101 ms | 101 ms (4 used) | 2.66× (auto) | yes |
| j2k-t256 | 2694 ms | 1414 ms | 983 ms | 829 ms | 823 ms (4 used) | 3.27× (auto) | yes |
| j2k-t1024 | 2753 ms | 1464 ms | 1013 ms | 831 ms | 828 ms (4 used) | 3.32× (auto) | yes |
| haar+zstd19 | 2939 ms | 1486 ms | 1042 ms | 881 ms | 882 ms (4 used) | 3.34× (N=4) | yes |
| cdf53+zstd19 | 2899 ms | 1490 ms | 1056 ms | 887 ms | 888 ms (4 used) | 3.27× (N=4) | yes |
| naive+zstd19 | 3792 ms | 1912 ms | 1335 ms | 1129 ms | 1127 ms (4 used) | 3.36× (auto) | yes |
| haar+range | 1069 ms | 550 ms | 385 ms | 305 ms | 306 ms (4 used) | 3.50× (N=4) | yes |

RGB:

| codec | N=1 | N=2 | N=3 | N=4 | auto | best vs N=1 | identical |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar | 827 ms | 487 ms | 402 ms | 391 ms | 393 ms (4 used) | 2.12× (N=4) | yes |
| cdf53 | 1059 ms | 654 ms | 537 ms | 509 ms | 514 ms (4 used) | 2.08× (N=4) | yes |
| laplacian-box | 1630 ms | 929 ms | 732 ms | 703 ms | 701 ms (4 used) | 2.32× (auto) | yes |
| laplacian-gauss | 2080 ms | 1156 ms | 885 ms | 811 ms | 809 ms (4 used) | 2.57× (auto) | yes |
| naive | 988 ms | 572 ms | 451 ms | 445 ms | 443 ms (4 used) | 2.23× (auto) | yes |
| j2k-t256 | 5552 ms | 3013 ms | 2133 ms | 1862 ms | 1844 ms (4 used) | 3.01× (auto) | yes |
| j2k-t1024 | 5802 ms | 3169 ms | 2306 ms | 1917 ms | 1935 ms (4 used) | 3.03× (N=4) | yes |
| haar+zstd19 | 18904 ms | 10042 ms | 7195 ms | 5914 ms | 5880 ms (4 used) | 3.21× (auto) | yes |
| cdf53+zstd19 | 17902 ms | 9562 ms | 6902 ms | 5721 ms | 5727 ms (4 used) | 3.13× (N=4) | yes |
| naive+zstd19 | 25618 ms | 13537 ms | 9560 ms | 7721 ms | 7718 ms (4 used) | 3.32× (auto) | yes |
| haar+range | 2364 ms | 1251 ms | 905 ms | 747 ms | 748 ms (4 used) | 3.16× (N=4) | yes |

- The slow coders (zstd 19, range) and JPEG2000 reach 3.0–3.5× on 4 cores, since block coding is almost all of their time. The zstd-1 encodes reach 2.1–2.7×: the transforms are limited by the Pi's memory bandwidth, and the file write stays serial. The fourth core adds only 1–12% for them.
- *auto* used 4 threads in every row and was within 1% of N=4 in all but two (gray `cdf53` 144 vs 134 ms, `laplacian-box` 197 vs 188 ms; the same thread count, so run-to-run noise; at 7b0db91 both were within 1%): on 4 cores the automatic rule ([design.md §1.4](design.md)) already asks for all of them from about a 720² gray `haar` image (less for the costlier modes and coders), so it never picks a count below the best one here. JPEG2000 with 256² tiles can use 4 threads, the same cap as the core count.
- Full image (`prmdpipeline test_image.jpg --runs 5 --levels 6`, 8824 × 5336, otherwise defaults, file in `/tmp`): encode 647 ms on one thread, 273 ms automatic (2.4×), and the 10 crops took 13.4 ms after either encode. With the new default `--levels 5` (9 crops): 653 / 269 ms, crops 13.2 ms.

## Entropy coders

| coder | file size | 256² L0 mean | 512² L1 mean | encode: 1 thread / auto |
|---|---:|---:|---:|---:|
| haar + zstd 1 (default) | 11.58 MiB | 1.44 ms | 4.51 ms | 219 / 96 ms |
| haar + zstd 19 | 11.59 MiB | 1.45 ms | 4.49 ms | 2939 / 882 ms |
| haar + range coder | 11.13 MiB | 13.6 ms | 32.0 ms | 1069 / 306 ms |
| haar + raw | 32.01 MiB | 1.04 ms | 3.65 ms | |

The range coder decodes about 10× slower than zstd. zstd level 19 decodes at the same speed but encodes 10–14× slower on gray with one thread (`haar` 2.9 s, `cdf53` 2.9 s, `naive` 3.8 s) and 17–26× slower on RGB (`haar` 18.9 s instead of 0.83 s); with 4 threads the gap is 6–11× on gray and 11–17× on RGB (`haar` RGB 5.9 s instead of 0.39 s). It makes gray files no smaller and RGB files 2.7–4.1% smaller.

## Lossy

Sizes and PSNR match [benchmarks.md](benchmarks.md) exactly.

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|
| haar | Q = 4 | 3.66 | 38.6 dB | 12 | 1.60 ms |
| cdf53 | Q = 4 | 3.53 | 41.9 dB | 11 | 1.85 ms |
| laplacian-gauss | Q = 4 | 4.72 | 46.4 dB | 2 | 2.20 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 15.7 ms |

`.prmd` lossy crops decode 7–10× faster than JPEG2000.

## RGB

Decode times from the `--threads 1` run; encode from the RGB scaling run.

| codec | file size | encode: 1 thread / auto | 256² L0 mean | 512² L0 mean | full L0 |
|---|---:|---:|---:|---:|---:|
| haar | 23.91 MiB | 827 / 393 ms | 4.65 ms | 14.9 ms | 828 ms |
| cdf53 | 24.91 MiB | 1059 / 514 ms | 5.32 ms | 17.2 ms | 987 ms |
| naive | 30.84 MiB | 988 / 443 ms | 4.50 ms | 14.3 ms | 738 ms |
| JPEG2000, 256² tiles | 20.98 MiB | 5552 / 1844 ms | 48.8 ms | 126 ms | 4621 ms |
| haar + range coder | 20.52 MiB | 2364 / 748 ms | 29.6 ms | 71.8 ms | 2577 ms |

An RGB `haar` crop costs 3.3× a gray one. JPEG2000 is 8–11× slower per crop. A 64 MiB cache brings an RGB `haar` crop on the random walk from 4.64 to 1.76 ms.

## What this means for an embedded target

- A Pi-class Cortex-A76 serves a random 256² crop of a 4096² gray pyramid in under 1.5 ms on one core, and the 512² crop in under 5 ms. Three cores stay free for other work, or for parallel block decoding later.
- Encoding a 4096² gray pyramid takes about 0.1 s on all four cores (0.22 s on one), so reference images can be rebuilt on the device. The automatic thread count needs no tuning on 4 cores; an application that encodes on a core it shares with time-critical work should pass `threads = 1` or a small N.
- At this class of hardware, the arithmetic-light modes matter most. `haar` and `naive` are the fastest; `cdf53` is now within 1.2× of them on decode.
- The gap to JPEG2000 stays above an order of magnitude for crops.

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
| code | branch `threading`, commit 7b0db91 (copied with `git archive`, built in a fresh directory) |
| tests (`ctest`) | 874 / 874 pass (26 s at `-j4`) |
| date | 2026-11-16 |

The Pi had stock cooling and no overclock. It ran at 50–61 °C over about an hour of benchmarks, the clock stayed at 2.4 GHz under load, and `vcgencmd get_throttled` reported `0x0` (no throttling) at every 20 s sample. No package had to be installed.

To reproduce on the Pi:

```sh
sudo apt install build-essential cmake pkg-config \
     libjpeg62-turbo-dev libpng-dev libzstd-dev libopenjp2-7-dev libgtest-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
ctest --test-dir build -j4
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
- **A random 256² gray crop takes 0.66–1.43 ms** at any level with `haar`, and a 512² crop takes 3.4–4.6 ms. The whole 5-level pyramid encodes in 217 ms on one core and **96 ms on all four** (automatic).
- **Threads speed up the encode 2.1–2.7× with zstd 1 and 3.0–3.5× with the slow coders and JPEG2000**, out of 4 cores. The automatic count picks 4 for every 4096² image, and that was the fastest count or within 1% of it in every row.
- **`.prmd` crops decode 11–20× faster than JPEG2000 crops.**
- **`naive` is slightly faster than `haar` on the Pi:** 6% at 256², 8% at 512², and 15% on a full L0 decode. `haar` still saves 23% of the file.
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
| 256² | L0 | 1.42 / 1.60 ms | 1.33 / 1.36 ms | 23.5 / 26.3 ms | 16.6× |
| 256² | L1 | 1.43 / 1.80 ms | 1.31 / 1.35 ms | 21.5 / 22.9 ms | 15.0× |
| 256² | L2 | 1.30 / 1.58 ms | 1.29 / 1.31 ms | 15.7 / 16.4 ms | 12.1× |
| 256² | L3 | 1.33 / 1.34 ms | 1.28 / 1.30 ms | 14.1 / 14.5 ms | 10.6× |
| 256² | L4 (whole level) | 0.66 / 0.66 ms | 0.66 / 0.66 ms | 13.2 / 13.5 ms | 20.0× |
| 512² | L0 | 4.59 / 4.79 ms | 4.21 / 4.31 ms | 60.7 / 66.4 ms | 13.2× |
| 512² | L1 | 4.50 / 4.82 ms | 4.18 / 4.26 ms | 59.2 / 61.9 ms | 13.1× |
| 512² | L2 | 4.49 / 4.54 ms | 4.11 / 4.16 ms | 50.0 / 51.0 ms | 11.1× |
| 512² | L3 (whole level) | 3.36 / 3.37 ms | 2.89 / 2.91 ms | 43.1 / 43.5 ms | 12.8× |

The Pi's p95 sits close to its mean, at most about 25% above it. Decoding all of L0 and then cropping would cost 270 ms, about **190×** a 256² crop. On the 1000-step random walk, a 64 MiB block cache brings a `haar` crop from 1.39 ms down to 0.60 ms, and a `naive` one from 1.32 ms to 0.17 ms.

## Encode and decode time per mode

| mode | encode: 1 thread / auto (4) | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 217 / 96 ms | 1.42 / 1.60 ms | 4.59 / 4.79 ms | 270 ms | 63.2 ms |
| `cdf53` | 299 / 133 ms | 1.66 / 1.87 ms | 5.42 / 5.79 ms | 321 ms | 75.3 ms |
| `laplacian-box` | 446 / 188 ms | 2.02 / 2.21 ms | 6.92 / 7.16 ms | 415 ms | 95.2 ms |
| `laplacian-gauss` | 561 / 217 ms | 1.87 / 2.05 ms | 6.35 / 6.56 ms | 376 ms | 86.1 ms |
| `naive` | 268 / 100 ms | 1.33 / 1.36 ms | 4.21 / 4.31 ms | 228 ms | 54.0 ms |
| JPEG2000, 256² tiles | 2678 / 831 ms | 23.5 / 26.3 ms | 60.7 / 66.4 ms | 2216 ms | 605 ms |

On one core `haar` and `naive` encode 1.1–1.4× faster than `cdf53` and 1.7–2.6× faster than `laplacian`, and 10–12× faster than JPEG2000. With threads every `.prmd` mode encodes the 4096² image in 0.1–0.22 s. Compared with the encoder before threading on the same Pi (previous version of this page), one-thread encodes are 1.1–4.1× faster: `haar` 283 → 217 ms, `cdf53` 1237 → 299 ms, `laplacian-gauss` 1058 → 561 ms, JPEG2000 unchanged.

## Encode scaling

`prmdbench --scaling 1,2,3,4,0` (commands under [Setup](#setup)): `encode_file` wall time, median of 3 (1 above 3 s). *best vs N=1*: the N=1 time over the fastest column's. Every row wrote the same file at every count.

Gray:

| codec | N=1 | N=2 | N=3 | N=4 | auto | best vs N=1 | identical |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar | 217 ms | 123 ms | 100 ms | 95 ms | 96 ms (4 used) | 2.28× (N=4) | yes |
| cdf53 | 299 ms | 178 ms | 142 ms | 133 ms | 133 ms (4 used) | 2.25× (N=4) | yes |
| laplacian-box | 446 ms | 259 ms | 204 ms | 188 ms | 188 ms (4 used) | 2.37× (auto) | yes |
| laplacian-gauss | 561 ms | 320 ms | 243 ms | 218 ms | 217 ms (4 used) | 2.59× (auto) | yes |
| naive | 268 ms | 143 ms | 107 ms | 100 ms | 100 ms (4 used) | 2.70× (auto) | yes |
| j2k-t256 | 2678 ms | 1414 ms | 986 ms | 825 ms | 831 ms (4 used) | 3.25× (N=4) | yes |
| j2k-t1024 | 2758 ms | 1463 ms | 1010 ms | 829 ms | 830 ms (4 used) | 3.33× (N=4) | yes |
| haar+zstd19 | 2929 ms | 1487 ms | 1048 ms | 887 ms | 882 ms (4 used) | 3.32× (auto) | yes |
| cdf53+zstd19 | 2906 ms | 1486 ms | 1053 ms | 892 ms | 891 ms (4 used) | 3.26× (auto) | yes |
| naive+zstd19 | 3801 ms | 1911 ms | 1339 ms | 1138 ms | 1147 ms (4 used) | 3.34× (N=4) | yes |
| haar+range | 1061 ms | 544 ms | 380 ms | 301 ms | 302 ms (4 used) | 3.53× (N=4) | yes |

RGB:

| codec | N=1 | N=2 | N=3 | N=4 | auto | best vs N=1 | identical |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar | 829 ms | 484 ms | 420 ms | 385 ms | 386 ms (4 used) | 2.15× (N=4) | yes |
| cdf53 | 1060 ms | 652 ms | 537 ms | 506 ms | 505 ms (4 used) | 2.10× (auto) | yes |
| laplacian-box | 1634 ms | 923 ms | 730 ms | 702 ms | 701 ms (4 used) | 2.33× (auto) | yes |
| laplacian-gauss | 1991 ms | 1103 ms | 849 ms | 785 ms | 790 ms (4 used) | 2.54× (N=4) | yes |
| naive | 989 ms | 569 ms | 451 ms | 442 ms | 442 ms (4 used) | 2.24× (N=4) | yes |
| j2k-t256 | 5557 ms | 2992 ms | 2134 ms | 1844 ms | 1847 ms (4 used) | 3.01× (N=4) | yes |
| j2k-t1024 | 5793 ms | 3185 ms | 2325 ms | 1945 ms | 1936 ms (4 used) | 2.99× (auto) | yes |
| haar+zstd19 | 19237 ms | 10020 ms | 7193 ms | 5902 ms | 5957 ms (4 used) | 3.26× (N=4) | yes |
| cdf53+zstd19 | 18218 ms | 9682 ms | 6953 ms | 5739 ms | 5767 ms (4 used) | 3.17× (N=4) | yes |
| naive+zstd19 | 25643 ms | 13521 ms | 9600 ms | 7776 ms | 7737 ms (4 used) | 3.31× (auto) | yes |
| haar+range | 2352 ms | 1257 ms | 911 ms | 752 ms | 751 ms (4 used) | 3.13× (auto) | yes |

- The slow coders (zstd 19, range) and JPEG2000 reach 3.0–3.5× on 4 cores, since block coding is almost all of their time. The zstd-1 encodes reach 2.1–2.7×: the transforms are limited by the Pi's memory bandwidth, and the file write stays serial. The fourth core adds only 2–11% for them.
- *auto* equals N=4 within 1% in every row: on 4 cores the automatic rule ([design.md §1.4](design.md)) already asks for all of them from about a 720² gray `haar` image (less for the costlier modes and coders), so it never picks a count below the best one here. JPEG2000 with 256² tiles can use 4 threads, the same cap as the core count.
- Full image (`prmdpipeline test_image.jpg --runs 5`, 8824 × 5336, 6 levels, defaults, file in `/tmp`): encode 645 ms on one thread, 273 ms automatic (2.4×). The 10 crops took 13.4 ms after either encode.

## Entropy coders

| coder | file size | 256² L0 mean | 512² L1 mean | encode: 1 thread / auto |
|---|---:|---:|---:|---:|
| haar + zstd 1 (default) | 11.58 MiB | 1.42 ms | 4.48 ms | 217 / 96 ms |
| haar + zstd 19 | 11.59 MiB | 1.44 ms | 4.47 ms | 2929 / 882 ms |
| haar + range coder | 11.13 MiB | 13.7 ms | 32.3 ms | 1061 / 302 ms |
| haar + raw | 32.01 MiB | 1.03 ms | 3.64 ms | |

The range coder decodes about 10× slower than zstd. zstd level 19 decodes at the same speed but encodes 10–14× slower on gray with one thread (`haar` 2.9 s, `cdf53` 2.9 s, `naive` 3.8 s) and 17–26× slower on RGB (`haar` 19.2 s instead of 0.83 s); with 4 threads the gap is 7–12× on gray and 11–18× on RGB (`haar` RGB 5.9 s instead of 0.39 s). It makes gray files no smaller and RGB files 2.7–4.1% smaller.

## Lossy

Sizes and PSNR match [benchmarks.md](benchmarks.md) exactly.

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|
| haar | Q = 4 | 3.66 | 38.6 dB | 12 | 1.59 ms |
| cdf53 | Q = 4 | 3.53 | 41.9 dB | 11 | 1.83 ms |
| laplacian-gauss | Q = 4 | 4.72 | 46.4 dB | 2 | 2.18 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 15.7 ms |

`.prmd` lossy crops decode 7–10× faster than JPEG2000.

## RGB

Decode times from the `--threads 1` run; encode from the RGB scaling run.

| codec | file size | encode: 1 thread / auto | 256² L0 mean | 512² L0 mean | full L0 |
|---|---:|---:|---:|---:|---:|
| haar | 23.91 MiB | 829 / 386 ms | 4.64 ms | 14.8 ms | 833 ms |
| cdf53 | 24.91 MiB | 1060 / 505 ms | 5.32 ms | 17.2 ms | 986 ms |
| naive | 30.84 MiB | 989 / 442 ms | 4.51 ms | 14.3 ms | 741 ms |
| JPEG2000, 256² tiles | 20.98 MiB | 5557 / 1847 ms | 48.8 ms | 127 ms | 4645 ms |
| haar + range coder | 20.52 MiB | 2352 / 751 ms | 29.7 ms | 71.9 ms | 2580 ms |

An RGB `haar` crop costs 3.3× a gray one. JPEG2000 is 8–11× slower per crop. A 64 MiB cache brings an RGB `haar` crop on the random walk from 4.62 to 1.75 ms.

## What this means for an embedded target

- A Pi-class Cortex-A76 serves a random 256² crop of a 4096² gray pyramid in under 1.5 ms on one core, and the 512² crop in under 5 ms. Three cores stay free for other work, or for parallel block decoding later.
- Encoding a 4096² gray pyramid takes about 0.1 s on all four cores (0.22 s on one), so reference images can be rebuilt on the device. The automatic thread count needs no tuning on 4 cores; an application that encodes on a core it shares with time-critical work should pass `threads = 1` or a small N.
- At this class of hardware, the arithmetic-light modes matter most. `haar` and `naive` are the fastest; `cdf53` is now within 1.2× of them on decode.
- The gap to JPEG2000 stays above an order of magnitude for crops.

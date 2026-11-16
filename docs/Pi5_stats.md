# Prmd on a Raspberry Pi 5

The same `prmdbench` runs as in [WRITEUP.md §4](WRITEUP.md), repeated on a Raspberry Pi 5, with the MacBook Pro numbers from WRITEUP.md next to them. Commands, input and methodology are identical: the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd at level 1, 300 random crops per row (100 for RGB), seed 1, one core, file in the page cache. Both runs were on 2026-11-16.

## Setup

| | Raspberry Pi 5 | MacBook Pro (WRITEUP.md) |
|---|---|---|
| CPU | 4× Cortex-A76 @ 2.4 GHz (`ondemand` governor) | Apple Silicon, 14 cores |
| RAM | 4 GB | 17 GB |
| OS | Debian 13 (trixie), kernel 6.18 `rpi-2712` | Ubuntu, kernel 7.0 |
| compiler | GCC 14.2, Release `-O2 -mcpu=native` | GCC 15.2, Release `-O2 -mcpu=native` |
| zstd · OpenJPEG · libjpeg · libpng | 1.5.7 · 2.5.3 · 2.1.5 · 1.6.48 | 1.5.7 · 2.5.4 · 2.1.5 · 1.6.57 |
| tests (`ctest`) | 198 / 198 pass | 198 / 198 pass |

The Pi had stock cooling and no overclock. It ran at 43–50 °C, and `vcgencmd get_throttled` reported `0x0` (no throttling) before and after. The only package the build needed beyond what was installed was `libopenjp2-7-dev`. The full benchmark took 7 min 3 s for gray and 4 min 3 s for RGB, against 3 min 42 s and 2 min 1 s on the MacBook Pro.

To reproduce on the Pi:

```sh
sudo apt install build-essential cmake pkg-config \
     libjpeg62-turbo-dev libpng-dev libzstd-dev libopenjp2-7-dev libgtest-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
ctest --test-dir build -j4
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 300 --lossy --out bench/results.md
build/prmdbench test_image.jpg --size 4096 --iters 100 --color rgb --no-entropy-table \
                --codecs haar,cdf53,naive,j2k-t256,haar+range --out bench/results_rgb.md
```

## Summary

- **The files are portable.** File sizes, bytes per level, blocks, tiles and bytes read per crop, and every PSNR value match the MacBook Pro to the last digit. The format and the encoder are deterministic across machines and compilers.
- **A random 256² gray crop takes 0.6–1.4 ms** at any level with `haar` (MacBook Pro: 0.3–0.6 ms), and a 512² crop takes 3.1–4.6 ms. The whole 5-level pyramid encodes in 283 ms.
- **The Pi is 2.1–3.0× slower than the MacBook Pro on `.prmd` crops,** 3.0–3.7× on encode and 2.5–5.0× on full-level decode. On JPEG2000 it is only 1.6–1.8× slower. OpenJPEG loses less ground, but `.prmd` crops still decode **11–21× faster** than JPEG2000 crops (13–24× on the MacBook Pro).
- **`naive` is slightly faster than `haar` on the Pi:** 5% at 256², 7% at 512², and 12% on a full L0 decode. On the MacBook Pro `haar` was within 10% at 256² and faster at 512². `haar` still saves 23% of the file.
- **`cdf53` falls behind most:** 2.4–3.0× the MacBook Pro's crop time and 5.0× its full-L0 time. It costs 1.4–1.7× `haar` per crop on the Pi.

## File size

Identical on both machines:

| codec | file size | vs raw L0 (16.00 MiB) | vs naive |
|---|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | **−25.7%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | −27.9% |

## Crop latency (gray, lossless)

Mean / p95 per crop on the Pi. The *MacBook Pro* column is the `haar` mean from WRITEUP.md. Tiles and bytes per crop are the same as in WRITEUP.md.

| crop | level | haar | naive | J2K, 256² tiles | J2K ÷ haar | MacBook Pro haar |
|---|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 1.41 / 1.61 ms | 1.34 / 1.37 ms | 23.4 / 26.1 ms | 16.6× | 0.64 ms |
| 256² | L1 | 1.35 / 1.52 ms | 1.32 / 1.35 ms | 21.4 / 22.8 ms | 15.8× | 0.60 ms |
| 256² | L2 | 1.29 / 1.58 ms | 1.30 / 1.32 ms | 15.7 / 16.4 ms | 12.1× | 0.58 ms |
| 256² | L3 | 1.32 / 1.33 ms | 1.29 / 1.30 ms | 14.0 / 14.4 ms | 10.6× | 0.63 ms |
| 256² | L4 (whole level) | 0.63 / 0.64 ms | 0.64 / 0.65 ms | 13.2 / 13.5 ms | 21.0× | 0.30 ms |
| 512² | L0 | 4.58 / 4.77 ms | 4.28 / 4.38 ms | 60.5 / 66.4 ms | 13.2× | 1.71 ms |
| 512² | L1 | 4.47 / 4.80 ms | 4.25 / 4.32 ms | 59.0 / 61.7 ms | 13.2× | 1.65 ms |
| 512² | L2 | 4.47 / 4.51 ms | 4.18 / 4.23 ms | 49.9 / 50.8 ms | 11.2× | 1.70 ms |
| 512² | L3 (whole level) | 3.14 / 3.15 ms | 2.81 / 2.85 ms | 43.0 / 43.3 ms | 13.7× | 1.19 ms |

The Pi's p95 sits close to its mean, at most about 20% above it. Decoding all of L0 and then cropping would cost 274 ms, about **190×** a 256² crop. On the 1000-step random walk, a 64 MiB block cache brings a `haar` crop from 1.38 ms down to 0.55 ms, and a `naive` one from 1.32 ms to 0.13 ms.

## Encode and decode time per mode

| mode | encode | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 | Pi ÷ MacBook Pro (256² · full L0) |
|---|---:|---:|---:|---:|---:|---:|
| `haar` | 283 ms | 1.41 / 1.61 ms | 4.58 / 4.77 ms | 274 ms | 64.9 ms | 2.2× · 3.2× |
| `cdf53` | 1237 ms | 1.99 / 2.22 ms | 7.75 / 8.20 ms | 1318 ms | 236 ms | 2.4× · 5.0× |
| `laplacian-box` | 932 ms | 1.82 / 2.05 ms | 6.32 / 6.56 ms | 367 ms | 87.3 ms | 2.2× · 3.2× |
| `laplacian-gauss` | 1058 ms | 1.73 / 1.93 ms | 6.06 / 6.31 ms | 356 ms | 82.7 ms | 2.2× · 3.4× |
| `naive` | 307 ms | 1.34 / 1.37 ms | 4.28 / 4.38 ms | 240 ms | 55.7 ms | 2.2× · 2.5× |
| JPEG2000, 256² tiles | 2677 ms | 23.4 / 26.1 ms | 60.5 / 66.4 ms | 2214 ms | 602 ms | 1.7× · 1.6× |

`haar` and `naive` still encode 3–4.4× faster than `cdf53` and `laplacian`, and about 9× faster than JPEG2000. `cdf53`'s full-level decode is 4.8× `haar`'s here, against 3.1× on the MacBook Pro. Its transform has the most memory traffic per pixel, and the Pi's smaller caches and lower memory bandwidth hurt it most.

## Entropy coders

| coder | file size | 256² L0 mean | 512² L1 mean | encode |
|---|---:|---:|---:|---:|
| haar + zstd 1 (default) | 11.58 MiB | 1.42 ms | 4.48 ms | 285 ms |
| haar + zstd 19 | 11.59 MiB | 1.43 ms | 4.47 ms | 3045 ms |
| haar + range coder | 11.13 MiB | 13.7 ms | 32.2 ms | |
| haar + raw | 32.01 MiB | 1.02 ms | 3.64 ms | |

The range coder decodes about 10× slower than zstd (12× on the MacBook Pro). zstd level 19 encodes 3–13× slower on gray (`haar` 3.0 s, `cdf53` 3.9 s, `naive` 3.9 s) and 5–24× slower on RGB (`haar` 18.8 s instead of 1.0 s). It decodes at the same speed. As on the MacBook Pro, it makes gray files no smaller and RGB files 2.7–4.1% smaller. These encode times come from a separate 20-crop run with `--codecs haar,haar+zstd19,cdf53,cdf53+zstd19,naive,naive+zstd19`.

## Lossy

Sizes and PSNR match the MacBook Pro exactly. Only the decode time differs.

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 | MacBook Pro |
|---|---|---:|---:|---:|---:|---:|
| haar | Q = 4 | 3.66 | 38.6 dB | 12 | 1.55 ms | 0.69 ms |
| cdf53 | Q = 4 | 3.53 | 41.9 dB | 11 | 2.13 ms | 0.87 ms |
| laplacian-gauss | Q = 4 | 4.72 | 46.4 dB | 2 | 2.01 ms | 0.89 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 15.6 ms | 9.1 ms |

`.prmd` lossy crops decode 7–10× faster than JPEG2000 on the Pi (10–13× on the MacBook Pro).

## RGB

| codec | file size | encode | 256² L0 mean | 512² L0 mean | full L0 |
|---|---:|---:|---:|---:|---:|
| haar | 23.91 MiB | 982 ms | 4.70 ms | 14.8 ms | 785 ms |
| cdf53 | 24.91 MiB | 3785 ms | 6.43 ms | 22.9 ms | 3666 ms |
| naive | 30.84 MiB | 1047 ms | 4.57 ms | 14.8 ms | 731 ms |
| JPEG2000, 256² tiles | 20.98 MiB | 5542 ms | 48.7 ms | 126 ms | 4599 ms |
| haar + range coder | 20.52 MiB | 2534 ms | 29.8 ms | 72.1 ms | 2556 ms |

An RGB `haar` crop costs 3.3× a gray one, and is about 2.4× the MacBook Pro's 1.96 ms. JPEG2000 is 10× slower per crop.

## What this means for an embedded target

- A Pi-class Cortex-A76 serves a random 256² crop of a 4096² gray pyramid in under 1.5 ms on one core, and the 512² crop in under 5 ms. Three cores stay free for other work, or for parallel block decoding later.
- At this class of hardware, the arithmetic-light modes matter most. `haar` and `naive` are the fastest, while `cdf53` loses most of its ground in full-level decode.
- The gap to JPEG2000 narrows on the Pi but stays above an order of magnitude for crops, with no change in file sizes.

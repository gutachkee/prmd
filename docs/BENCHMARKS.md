# Prmd benchmarks

The detailed tables behind [WRITEUP.md](WRITEUP.md). Modes are explained in [modes.md](modes.md), the benchmark method in [DESIGN.md §10](DESIGN.md), and the same runs on a Raspberry Pi 5 are in [Pi5_stats.md](Pi5_stats.md).

## Setup

All numbers come from `prmdbench` (commands in the README): the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd at the default level 1, 300 random crops per row, one core of a MacBook Pro's Apple Silicon processor (14 cores, Linux, AArch64), GCC 15.2 `-O2 -mcpu=native`, file in the page cache, run on 2026-11-16. Sizes are deterministic. Latencies vary by up to about 10% between runs, so compare them within one table rather than across runs. The crop and full-level decode times in the two latency tables are from a later run the same day, after the decoder started reusing one zstd context per thread; the other figures moved by less than 10% in that run and were kept. All times in this document are on that MacBook Pro.

## File size

| codec | file size | vs raw L0 (16.00 MiB) | vs raw pyramid (21.31 MiB) | vs naive |
|---|---:|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | 1.84× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | 1.91× smaller | **−25.7%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | 1.49× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 1.42× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | 1.97× smaller | −27.9% |

## Per-crop latency and I/O

A crop also decodes the blocks of the coarser levels it's built from. *Tiles* counts the distinct 256² tiles touched on every level a crop depends on, so for `.prmd` a 256² crop at L0 touches 4 tiles of its own level and 9.5 in total. *Bytes* is the block data read, not counting the index.

| crop | level | haar mean / p95 | tiles · bytes | naive mean / p95 | tiles · bytes | J2K mean / p95 | tiles · bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 0.59 / 0.68 ms | 9.5 · 154 KiB | 0.55 / 0.58 ms | 4 · 101 KiB | 13.5 / 15.2 ms | 4 · 191 KiB |
| 256² | L1 | 0.55 / 0.63 ms | 8.2 · 143 KiB | 0.53 / 0.56 ms | 4 · 103 KiB | 12.4 / 13.3 ms | 9 · 433 KiB |
| 256² | L2 | 0.53 / 0.66 ms | 6.8 · 129 KiB | 0.53 / 0.58 ms | 4 · 99 KiB | 8.8 / 9.3 ms | 25 · 1.2 MiB |
| 256² | L3 | 0.53 / 0.57 ms | 5.0 · 119 KiB | 0.52 / 0.54 ms | 4 · 99 KiB | 7.7 / 8.6 ms | 81 · 3.8 MiB |
| 256² | L4 (whole level) | 0.25 / 0.28 ms | 1.0 · 44 KiB | 0.25 / 0.27 ms | 1 · 44 KiB | 6.4 / 6.8 ms | 256 · 10.8 MiB |
| 512² | L0 | 1.74 / 2.04 ms | 17.4 · 362 KiB | 1.67 / 1.89 ms | 9 · 282 KiB | 37.5 / 44.4 ms | 9 · 429 KiB |
| 512² | L1 | 1.72 / 2.05 ms | 15.7 · 346 KiB | 1.67 / 1.96 ms | 9 · 286 KiB | 35.9 / 38.5 ms | 25 · 1.2 MiB |
| 512² | L2 | 1.64 / 1.91 ms | 14.0 · 331 KiB | 1.61 / 1.78 ms | 9 · 276 KiB | 30.1 / 32.4 ms | 81 · 3.8 MiB |
| 512² | L3 (whole level) | 1.17 / 1.36 ms | 5.0 · 179 KiB | 1.06 / 1.13 ms | 4 · 176 KiB | 26.1 / 29.4 ms | 256 · 10.8 MiB |

A `haar` crop decodes about 2× `naive`'s blocks and reads up to 1.5× its bytes (1.0–1.5× across the rows above), in exchange for a 23% smaller file. At 256² it is within 10% of `naive`'s speed, and at 512² `naive` is 2–10% faster (5–7% on the Pi 5), so the two are roughly equal. `haar`'s detail blocks need only a few adds and shifts per pixel, while `naive`'s pixel blocks run the MED predictor, which goes pixel by pixel. Decoding the whole of L0 and then cropping would cost 93 ms, about **160×** a crop. On a 1000-step random walk, a 64 MiB block cache brings a `haar` crop down to 0.19 ms.

## Encode and decode time per mode

Encode covers building and writing the whole 5-level pyramid of the 4096² image. Decode is a random crop at L0, or the whole of L0 or L1 at once.

| mode | encode | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 85 ms | 0.59 / 0.68 ms | 1.74 / 2.04 ms | 93 ms | 22.0 ms |
| `cdf53` | 332 ms | 0.79 / 0.90 ms | 2.53 / 2.81 ms | 270 ms | 47.1 ms |
| `laplacian-box` | 218 ms¹ | 0.80 / 0.90 ms | 2.28 / 2.57 ms | 116 ms | 26.8 ms |
| `laplacian-gauss` | 250 ms¹ | 0.75 / 0.85 ms | 2.09 / 2.25 ms | 109 ms | 23.5 ms |
| `naive` | 104 ms | 0.55 / 0.58 ms | 1.67 / 1.89 ms | 79 ms | 18.9 ms |
| JPEG2000, 256² tiles | 1453 ms | 13.5 / 15.2 ms | 37.5 / 44.4 ms | 1341 ms | 361 ms |

¹ Measured after the encoder stopped computing the `laplacian` residuals twice; before, 282 ms and 299 ms. Files are byte-identical.

At zstd level 1, most of the encode time is the transform, so `haar` and `naive` encode 3–4× faster than `cdf53` and 2–3× faster than `laplacian`. `cdf53`'s full-level decode is 3× `haar`'s, while its crops are only about 1.3–1.5× slower.

## Entropy coder and zstd level

**Coder.** zstd: 11.58 MiB at 0.62 ms per 256² crop. Range coder: 11.13 MiB at 7.6 ms. zstd is the default ([DESIGN.md §7.1](DESIGN.md)).

**zstd level.** Level 1 is the default. Level 19 encodes 4–34× slower (gray `haar`: 1.24 s instead of 85 ms; RGB `haar`: 9.2 s instead of 0.30 s) and decodes at the same speed. It makes gray lossless files no smaller, but RGB files 2.7–4.1% smaller and lossy files 3–12% smaller (the lossy figure is from an earlier run; the benchmark has no lossy zstd-19 row). Pass `--entropy-level 19` when size matters more than encode time ([DESIGN.md §7.2](DESIGN.md)).

## Level definition

`haar` levels are the exact 2×2 mean, but they alias. `cdf53` and `laplacian-gauss` levels are sharper or smoother and differ from the 2×2 mean by 24–30 dB PSNR, mostly a half-pixel shift. The image matcher's needs should decide the mode, not the codec. [modes.md](modes.md) has side-by-side samples and the PSNR table.

## Lossy

`--quant Q`. Max error is in 8-bit pixel values (0–255). Bits per pixel are at the default zstd level 1.

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|
| haar | Q = 4 | 3.66 | 38.6 dB | 12 | 0.69 ms |
| cdf53 | Q = 4 | 3.53 | 41.9 dB | 11 | 0.87 ms |
| laplacian-gauss | Q = 4 | 4.72 | 46.4 dB | **2** (±Q/2, guaranteed) | 0.89 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 9.1 ms |

`haar` and `cdf53` quantize open loop, so errors pile up toward level 0. `laplacian` is closed loop and keeps every level within ±Q/2, at the cost of a larger file. JPEG2000 wins rate–distortion: 44.7 dB at 3.14 bits/px, where `cdf53` needs 3.53 bits/px for 41.9 dB. It gets there through rate-distortion-optimised truncation and the smoother 9/7 filter. `.prmd` decodes 10–13× faster.

## RGB

100 crops per row. `haar` is 23.9 MiB (−22.5% vs `naive`) at 2.0 ms per 256² crop, or 22.9 MiB with `--entropy-level 19`. JPEG2000 is 21.0 MiB at 28 ms. On RGB, zstd files are 12–17% larger than with the range coder (12% at level 19, 17% at level 1), so JPEG2000 is smaller than the default `.prmd` RGB file; `haar+range` is still the smallest.

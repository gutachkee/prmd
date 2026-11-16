# Prmd benchmarks

The detailed tables behind [writeup.md](writeup.md). Modes are explained in [modes.md](modes.md), the benchmark method in [design.md §10](design.md), and the same runs on a Raspberry Pi 5 are in [benchmarks-pi5.md](benchmarks-pi5.md).

## Setup

All numbers come from `prmdbench` (commands in the README): the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd at the default level 1, 300 random crops per row, one core of a MacBook Pro's Apple Silicon processor (14 cores, Linux, AArch64), GCC 15.2 `-O2 -mcpu=native`, file in the page cache, run on 2026-11-16. Sizes are deterministic. Latencies vary by up to about 10% between runs, so compare them within one table rather than across runs. The crop and full-level decode times in the two latency tables are from a later run the same day, after the decoder started reusing one zstd context per thread; the other figures moved by less than 10% in that run and were kept. All times in this document are on that MacBook Pro.

## File size

| codec | file size | vs raw L0 (16.00 MiB) | vs raw pyramid (21.31 MiB) | vs naive |
|---|---:|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | 1.84× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | 1.91× smaller | **−25.7%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | 1.49× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 1.42× smaller | 0 |
| naive, `--no-med --no-ycocg`² | 18.52 MiB | 1.16× larger | 1.15× smaller | +23.1% |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | 1.97× smaller | −27.9% |

² From a separate `prmdbench` run on the same machine (2026-11-16, `--no-entropy-table --codecs naive,naive-no-med`), whose plain `naive` matched the figures here within 5%. On gray, `--no-ycocg` has no effect (there is no color transform), so this is `naive` without MED: base blocks store the pixel values themselves.

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
| `naive --no-med --no-ycocg`² | 96 ms | 0.37 / 0.46 ms | 1.07 / 1.26 ms | 55.5 ms | 13.3 ms |
| JPEG2000, 256² tiles | 1453 ms | 13.5 / 15.2 ms | 37.5 / 44.4 ms | 1341 ms | 361 ms |

¹ Measured after the encoder stopped computing the `laplacian` residuals twice; before, 282 ms and 299 ms. Files are byte-identical.

Without MED, `naive` crops decode about 1.5× faster (0.37 ms against 0.57 ms for plain `naive` in the same run) for a 23% larger file, and they are then faster than `haar`'s, whose file is 37% smaller.

At zstd level 1, most of the encode time is the transform, so `haar` and `naive` encode 3–4× faster than `cdf53` and 2–3× faster than `laplacian`. `cdf53`'s full-level decode is 3× `haar`'s, while its crops are only about 1.3–1.5× slower.

## Encode scaling (multithreaded encoder)

`EncodeOptions::threads` / `--threads` (design.md §1.4). The `.prmd` file is byte-identical for every thread count; every row below was checked. The other tables in this document encode with one thread (`prmdbench --threads 1`; the default is all hardware threads). Run on 2026-11-16 on the same MacBook Pro (14 cores).

**Full image** (`test_image.jpg`, 8824 × 5336, 5 levels), `prmd encode` wall time in ms, median of 3 (1 for zstd 19). *base* is the encoder before threading (fc0a629); its N = 1 column differs from it only through single-thread fixes in the same change (row-major Cdf53 vertical lifting, one upsample per Laplacian level, no serial zero-fill, fused colour conversion).

| config | base | N=1 | N=2 | N=4 | N=8 | N=14 | speedup vs base |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar gray | 273.0 | 232.6 | 129.0 | 80.2 | 55.3 | 55.2 | 4.9× |
| haar rgb | 821.5 | 763.3 | 423.0 | 248.1 | 169.8 | 159.4 | 5.2× |
| cdf53 gray | 1219.8 | 289.5 | 167.7 | 101.5 | 75.1 | 78.7 | 15.5× |
| cdf53 rgb q4 | 3740.2 | 1141.3 | 632.2 | 356.8 | 247.2 | 253.6 | 14.7× |
| laplacian rgb q4 | 2498.3 | 1854.2 | 988.4 | 538.8 | 325.2 | 289.8 | 8.6× |
| laplacian-gauss gray | 771.8 | 545.6 | 298.8 | 172.8 | 110.9 | 108.0 | 7.1× |
| naive gray | 331.3 | 310.2 | 168.3 | 99.5 | 65.5 | 59.5 | 5.6× |
| haar gray + range | 1461.7 | 1434.7 | 749.7 | 395.8 | 216.7 | 163.5 | 8.9× |
| haar rgb + zstd 19 | 27523.3 | 27955.9 | 14758.8 | 7317.3 | 3946.5 | 2950.4 | 9.3× |
| j2k gray (256² tiles) | 4107.6 | 4109.0 | 2515.4 | 1800.2 | 1820.5 | 1786.7 | 2.3× |
| j2k rgb (1024² tiles) | 8612.2 | 8560.7 | 4813.8 | 2717.9 | 1812.2 | 1760.1 | 4.9× |

**4096² centre crop** (`prmdbench --scaling 1,2,4,8,0`, encode_file wall time, median of 3; *auto* = 14 threads). *identical*: the file is the same at every count.

Gray:

| codec | N=1 | N=2 | N=4 | N=8 | auto | speedup | identical |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar | 73 ms | 40 ms | 25 ms | 19 ms | 21 ms (14 used) | 3.52× | yes |
| cdf53 | 93 ms | 55 ms | 35 ms | 29 ms | 34 ms (14 used) | 2.72× | yes |
| laplacian-box | 135 ms | 76 ms | 47 ms | 34 ms | 36 ms (14 used) | 3.73× | yes |
| laplacian-gauss | 186 ms | 101 ms | 59 ms | 42 ms | 44 ms (14 used) | 4.25× | yes |
| naive | 99 ms | 53 ms | 33 ms | 23 ms | 22 ms (14 used) | 4.39× | yes |
| j2k-t256 | 1483 ms | 907 ms | 631 ms | 655 ms (4 used) | 632 ms (4 used) | 2.35× | yes |
| j2k-t1024 | 1501 ms | 799 ms | 436 ms | 272 ms | 245 ms (14 used) | 6.12× | yes |
| haar+zstd19 | 1217 ms | 625 ms | 330 ms | 179 ms | 137 ms (14 used) | 8.88× | yes |
| haar+range | 517 ms | 265 ms | 140 ms | 78 ms | 60 ms (14 used) | 8.69× | yes |

RGB:

| codec | N=1 | N=2 | N=4 | N=8 | auto | speedup | identical |
|---|---:|---:|---:|---:|---:|---:|---:|
| haar | 261 ms | 142 ms | 83 ms | 57 ms | 60 ms (14 used) | 4.34× | yes |
| cdf53 | 328 ms | 185 ms | 111 ms | 87 ms | 102 ms (14 used) | 3.22× | yes |
| laplacian-box | 508 ms | 275 ms | 156 ms | 104 ms | 106 ms (14 used) | 4.80× | yes |
| laplacian-gauss | 656 ms | 353 ms | 198 ms | 126 ms | 130 ms (14 used) | 5.06× | yes |
| naive | 350 ms | 189 ms | 107 ms | 70 ms | 66 ms (14 used) | 5.30× | yes |
| j2k-t256 | 3045 ms | 1931 ms | 1480 ms | 1475 ms (4 used) | 1440 ms (4 used) | 2.11× | yes |
| j2k-t1024 | 3023 ms | 1665 ms | 929 ms | 601 ms | 572 ms (14 used) | 5.28× | yes |
| haar+zstd19 | 9163 ms | 4687 ms | 2471 ms | 1321 ms | 977 ms (14 used) | 9.37× | yes |
| haar+range | 1137 ms | 582 ms | 309 ms | 174 ms | 138 ms (14 used) | 8.21× | yes |

- Block coding scales almost linearly with the slow coders (range, zstd 19: 8–9× at 14 threads); with zstd 1 the whole encode reaches 3.5–5×, limited by memory bandwidth in the transforms and by what stays serial (allocating level planes, writing the file: 12 ms of the 54 ms gray `haar` encode at 14 threads, [pipeline_durations.md](pipeline_durations.md)).
- Beyond 8 threads the zstd-1 encodes gain nothing on this machine (its cores are not all equal), and `cdf53` gets slightly slower.
- JPEG2000 with 256² tiles uses at most 4 threads: OpenJPEG codes one tile at a time, and more threads made it slower. 1024² tiles scale to 5–6×. Its output is identical at every thread count too.
- Peak memory (`/usr/bin/time`, full image) fell with the same change and does not grow with threads: `haar` RGB 2.21 → 1.43 GB, `laplacian` RGB q4 3.91 → 1.68 GB, `haar` gray 0.87 → 0.57 GB.

## Entropy coder and zstd level

**Coder.** zstd: 11.58 MiB at 0.62 ms per 256² crop. Range coder: 11.13 MiB at 7.6 ms. zstd is the default ([design.md §7.1](design.md)).

**zstd level.** Level 1 is the default. Level 19 encodes 4–34× slower (gray `haar`: 1.24 s instead of 85 ms; RGB `haar`: 9.2 s instead of 0.30 s) and decodes at the same speed. It makes gray lossless files no smaller, but RGB files 2.7–4.1% smaller and lossy files 3–12% smaller (the lossy figure is from an earlier run; the benchmark has no lossy zstd-19 row). Pass `--entropy-level 19` when size matters more than encode time ([design.md §7.2](design.md)).

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

Same image and setup, converted to RGB8 (raw L0 48.00 MiB, raw pyramid 63.94 MiB), 100 crops per row, from one `prmdbench` run (command in the README; full report in `build/bench/results_rgb.md`). RGB stays bit-exact through the reversible YCoCg-R transform; every block holds all three channels, so a crop needs no extra reads.

### File size

| codec | file size | bits/px | vs raw L0 | vs raw pyramid | vs naive | encode |
|---|---:|---:|---:|---:|---:|---:|
| **haar** | 23.91 MiB | 11.96 | 2.01× smaller | 2.67× smaller | **−22.5%** | 328 ms |
| cdf53 | 24.91 MiB | 12.46 | 1.93× smaller | 2.57× smaller | −19.2% | 988 ms |
| laplacian-box | 31.96 MiB | 15.98 | 1.50× smaller | 2.00× smaller | +3.6% | 751 ms |
| laplacian-gauss | 32.38 MiB | 16.19 | 1.48× smaller | 1.98× smaller | +5.0% | 824 ms |
| naive (same coder) | 30.84 MiB | 15.42 | 1.56× smaller | 2.07× smaller | 0 | 351 ms |
| naive, `--no-med --no-ycocg`³ | 57.83 MiB | 28.92 | 1.20× larger | 1.11× smaller | +87.5% | 274 ms |
| haar, zstd 19 | 22.92 MiB | 11.46 | 2.09× smaller | 2.79× smaller | −25.7% | 8855 ms |
| haar, range coder | **20.52 MiB** | 10.26 | 2.34× smaller | 3.12× smaller | −33.5% | 1092 ms |
| JPEG2000, 256² tiles | 20.98 MiB | 10.49 | 2.29× smaller | 3.05× smaller | −32.0% | 2883 ms |

³ From a separate `prmdbench` run on the same machine (2026-11-16, `--codecs naive,naive-no-med,naive-no-ycocg,naive-no-med-no-ycocg`), whose plain `naive` matched the figures here within 6%. Base blocks store raw R, G and B values, with neither MED nor YCoCg-R.

Unlike gray, `cdf53` is larger than `haar` on RGB, and both `laplacian` variants are larger than `naive`: presumably their ~1.33× residual samples cost more than the cross-level prediction saves on the chroma planes (not investigated). On RGB, zstd files are 12–17% larger than with the range coder (12% at level 19, 17% at level 1), so JPEG2000 is smaller than the default `.prmd` RGB file; `haar+range` is still the smallest.

### Per-crop latency and I/O

*Tiles* and *bytes* as in the gray table.

| crop | level | haar mean / p95 | tiles · bytes | naive mean / p95 | tiles · bytes | J2K mean / p95 | tiles · bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 1.89 / 2.14 ms | 9.6 · 339 KiB | 1.79 / 1.88 ms | 4 · 196 KiB | 28.2 / 33.3 ms | 4 · 353 KiB |
| 256² | L2 | 1.55 / 1.87 ms | 6.7 · 330 KiB | 1.72 / 1.89 ms | 4 · 249 KiB | 22.2 / 23.7 ms | 25 · 2.1 MiB |
| 256² | L4 (whole level) | 0.79 / 0.92 ms | 1.0 · 113 KiB | 0.79 / 0.84 ms | 1 · 113 KiB | 18.1 / 19.4 ms | 256 · 21.0 MiB |
| 512² | L0 | 5.49 / 5.86 ms | 17.4 · 784 KiB | 5.23 / 5.47 ms | 9 · 557 KiB | 71.2 / 81.7 ms | 9 · 801 KiB |
| 512² | L3 (whole level) | 3.48 / 3.81 ms | 5.0 · 460 KiB | 3.24 / 3.38 ms | 4 · 451 KiB | 61.9 / 63.4 ms | 256 · 21.0 MiB |

An RGB crop costs about 3× a gray one (three planes plus the YCoCg-R inverse). `.prmd` stays 13–23× faster than JPEG2000, and at coarse levels JPEG2000 again reads the whole file.

### Encode and decode time per mode

| mode | encode | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 328 ms | 1.89 / 2.14 ms | 5.49 / 5.86 ms | 263 ms | 60.3 ms |
| `cdf53` | 988 ms | 2.47 / 2.77 ms | 8.01 / 8.75 ms | 768 ms | 134 ms |
| `laplacian-box` | 751 ms | 2.66 / 3.02 ms | 7.66 / 8.05 ms | 355 ms | 85.2 ms |
| `laplacian-gauss` | 824 ms | 2.61 / 3.12 ms | 7.18 / 7.61 ms | 316 ms | 75.8 ms |
| `naive` | 351 ms | 1.79 / 1.88 ms | 5.23 / 5.47 ms | 242 ms | 58.4 ms |
| `naive --no-med --no-ycocg`³ | 274 ms | 1.09 / 1.29 ms | 3.14 / 3.65 ms | 169 ms | 44.2 ms |
| `haar`, zstd 19 | 8855 ms | 2.21 / 2.61 ms | 5.73 / 6.07 ms | 262 ms | 63.5 ms |
| `haar`, range coder | 1092 ms | 16.2 / 21.2 ms | 37.5 / 42.8 ms | 1248 ms | 351 ms |
| JPEG2000, 256² tiles | 2883 ms | 28.2 / 33.3 ms | 71.2 / 81.7 ms | 2659 ms | 796 ms |

Without MED and YCoCg-R, RGB `naive` crops decode about 1.7× faster (1.09 against 1.86 ms in the same run), and the file nearly doubles (+87.5%), ending up larger than raw L0.

zstd 19 saves 4.1% on `haar` for a 27× slower encode. Full-level decode is the same speed; its 256² crops were 17% slower in this run (2.21 vs 1.89 ms), which the gray runs did not show. The range coder is 8.6× slower per 256² crop.

**Block cache.** On a 1000-step random walk of 256² crops at L0, a 64 MiB cache brings `haar` from 1.95 to 0.55 ms, `naive` from 1.79 to 0.16 ms and `haar+range` from 15.6 to 0.67 ms.

### Lossy

`--quant Q`, quantization on Y/Co/Cg. PSNR and max error are over all three RGB channels, in 8-bit pixel values (0–255). Bits per pixel are per pixel, not per channel.

| codec | setting | file size | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|---:|
| haar | Q = 4 | 13.29 MiB | 6.64 | 37.7 dB | 16 | 2.25 ms |
| haar | Q = 8 | 8.95 MiB | 4.47 | 34.4 dB | 28 | 2.15 ms |
| cdf53 | Q = 4 | 13.42 MiB | 6.71 | 40.6 dB | 15 | 2.77 ms |
| cdf53 | Q = 8 | 8.91 MiB | 4.45 | 36.3 dB | 23 | 2.68 ms |
| cdf53, range coder | Q = 4 | 10.46 MiB | 5.23 | 40.6 dB | 15 | 12.2 ms |
| laplacian-gauss | Q = 4 | 20.74 MiB | 10.37 | 44.4 dB | 4 | 3.09 ms |
| laplacian-gauss | Q = 8 | 14.47 MiB | 7.23 | 39.5 dB | 8 | 3.02 ms |
| JPEG2000 9/7 | 45 dB target | 9.02 MiB | 4.51 | 44.3 dB | 15 | 14.1 ms |
| JPEG2000 9/7 | 40 dB target | 6.32 MiB | 3.16 | 39.7 dB | 30 | 10.2 ms |

JPEG2000's rate–distortion lead is larger on RGB than on gray: at about 40 dB it needs 3.16 bits/px against 6.71 for `cdf53`. `laplacian`'s ±Q/2 guarantee holds on Y/Co/Cg, not per RGB channel, so its RGB max error reaches Q (4 at Q = 4). At similar PSNR, `.prmd` lossy crops decode about 4× faster than JPEG2000's (`cdf53` Q = 4 vs the 40 dB target: 2.77 vs 10.2 ms; `laplacian-gauss` Q = 4 vs 45 dB: 3.09 vs 14.1 ms).

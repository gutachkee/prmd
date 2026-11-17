# Prmd benchmarks

The detailed tables behind [writeup.md](writeup.md). Modes are explained in [modes.md](modes.md), the benchmark method in [design.md §10](design.md), and the same runs on a Raspberry Pi 5 are in [benchmarks-pi5.md](benchmarks-pi5.md).

## Setup

All numbers come from `prmdbench`: the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd at the default level 1, 300 random crops per row (100 for RGB), seed 1, file in the page cache (work directory on tmpfs, as the default `/tmp` is here). All times in this document are on one machine:

| | |
|---|---|
| machine | MacBook Pro, Apple Silicon (14 cores), Linux VM, AArch64 |
| compiler | GCC 15.2, Release `-O2 -mcpu=native` |
| code | branch `threading`, commit 7b0db91 |
| date | 2026-11-16 |

Each configuration was run twice, once with a single encoder thread and once with the default automatic thread count (14 threads on this machine for every 4096² encode, 4 for JPEG2000 with 256² tiles). Decoding is single-threaded in both, so only the encode columns differ:

```sh
# gray (every mode, entropy table, lossy table)
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 300 --lossy --threads 1
build/prmdbench test_image.jpg --size 4096 --levels 5 --iters 300 --lossy           # auto
# RGB
build/prmdbench test_image.jpg --size 4096 --iters 100 --color rgb --no-entropy-table \
                --codecs haar,cdf53,laplacian-box,laplacian-gauss,naive,j2k-t256,haar+range,haar+zstd19 \
                --lossy --out build/bench/results_rgb.md [--threads 1]
# encode scaling (crop latencies of these runs are not used)
build/prmdbench test_image.jpg --size 4096 --iters 20 --color gray|rgb --no-entropy-table \
                --codecs haar,cdf53,laplacian-box,laplacian-gauss,naive,j2k-t256,j2k-t1024,haar+zstd19,haar+range \
                --scaling 1,2,4,8,14,0
```

Encode columns read *1 thread / auto*. Decode tables are from the `--threads 1` runs; the automatic-thread runs gave the same decode times within run-to-run noise (full-level decodes within 6%, crop means mostly within 5%), as expected, since the decoder is unchanged by threading. Sizes are deterministic and identical at every thread count. Latencies vary by up to about 10% between runs, so compare them within one table rather than across runs; on this VM an occasional process also runs `naive` decode 10–25% slower throughout ([pipeline_durations.md](pipeline_durations.md#how-it-was-measured)).

Compared with the previous tables (main branch), single-thread encodes are faster because of fixes made with the multithreaded encoder (row-major `cdf53` vertical lifting, one upsample per Laplacian level, no serial zero-fill, fused colour conversion), and `cdf53` also decodes faster, since its inverse uses the same row-major lifting (full L0 270 → 97 ms). Every other decode figure is unchanged within noise.

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
| 256² | L0 | 0.56 / 0.65 ms | 9.5 · 154 KiB | 0.57 / 0.70 ms | 4 · 101 KiB | 13.5 / 15.3 ms | 4 · 191 KiB |
| 256² | L1 | 0.52 / 0.59 ms | 8.2 · 143 KiB | 0.55 / 0.68 ms | 4 · 103 KiB | 12.4 / 13.6 ms | 9 · 433 KiB |
| 256² | L2 | 0.50 / 0.62 ms | 6.8 · 129 KiB | 0.49 / 0.52 ms | 4 · 99 KiB | 8.9 / 9.9 ms | 25 · 1.2 MiB |
| 256² | L3 | 0.50 / 0.53 ms | 5.0 · 119 KiB | 0.50 / 0.56 ms | 4 · 99 KiB | 7.5 / 7.9 ms | 81 · 3.8 MiB |
| 256² | L4 (whole level) | 0.24 / 0.27 ms | 1.0 · 44 KiB | 0.23 / 0.24 ms | 1 · 44 KiB | 6.5 / 7.1 ms | 256 · 10.8 MiB |
| 512² | L0 | 1.64 / 1.96 ms | 17.4 · 362 KiB | 1.53 / 1.85 ms | 9 · 282 KiB | 35.3 / 40.1 ms | 9 · 429 KiB |
| 512² | L1 | 1.53 / 1.66 ms | 15.7 · 346 KiB | 1.45 / 1.53 ms | 9 · 286 KiB | 34.3 / 36.5 ms | 25 · 1.2 MiB |
| 512² | L2 | 1.52 / 1.55 ms | 14.0 · 331 KiB | 1.44 / 1.64 ms | 9 · 276 KiB | 28.5 / 30.3 ms | 81 · 3.8 MiB |
| 512² | L3 (whole level) | 1.08 / 1.10 ms | 5.0 · 179 KiB | 0.96 / 1.08 ms | 4 · 176 KiB | 24.1 / 25.7 ms | 256 · 10.8 MiB |

A `haar` crop decodes about 2× `naive`'s blocks and reads up to 1.5× its bytes (1.0–1.5× across the rows above), in exchange for a 23% smaller file. At 256² it is within 6% of `naive`'s speed, and at 512² `naive` is 5–11% faster, so the two are roughly equal. `haar`'s detail blocks need only a few adds and shifts per pixel, while `naive`'s pixel blocks run the MED predictor, which goes pixel by pixel. Decoding the whole of L0 and then cropping would cost 82 ms, about **145×** a crop. On a 1000-step random walk, a 64 MiB block cache brings a `haar` crop down to 0.19 ms.

## Encode and decode time per mode

Encode covers building and writing the whole 5-level pyramid of the 4096² image, with one thread and with the automatic count (14 threads here). Decode is a random crop at L0, or the whole of L0 or L1 at once, always on one thread.

| mode | encode: 1 thread / auto | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 72 / 20 ms | 0.56 / 0.65 ms | 1.64 / 1.96 ms | 82 ms | 21.0 ms |
| `cdf53` | 88 / 26 ms | 0.66 / 0.77 ms | 1.84 / 1.97 ms | 97 ms | 21.9 ms |
| `laplacian-box` | 129 / 31 ms | 0.77 / 0.89 ms | 2.31 / 2.52 ms | 118 ms | 27.7 ms |
| `laplacian-gauss` | 178 / 36 ms | 0.75 / 0.84 ms | 2.21 / 2.45 ms | 112 ms | 26.1 ms |
| `naive` | 92 / 21 ms | 0.57 / 0.70 ms | 1.53 / 1.85 ms | 73 ms | 17.1 ms |
| `naive --no-med --no-ycocg`² | 88 / 22 ms | 0.35 / 0.43 ms | 1.01 / 1.20 ms | 52.9 ms | 12.8 ms |
| JPEG2000, 256² tiles | 1380 / 638 ms | 13.5 / 15.3 ms | 35.3 / 40.1 ms | 1359 ms | 364 ms |
| JPEG2000, 1024² tiles | 1379 / 241 ms | 16.2 / 20.5 ms | 39.2 / 45.6 ms | 1318 ms | 355 ms |

Without MED, `naive` crops decode about 1.6× faster (0.35 ms against 0.56 ms for plain `naive` in the same run) for a 23% larger file, and they are then faster than `haar`'s, whose file is 37% smaller.

With one thread, `haar`, `cdf53` and `naive` encode within 1.3× of each other, and `laplacian` takes 1.8–2.5× `haar`'s time (before the single-thread fixes, `cdf53` took 4× `haar`'s). With the automatic thread count every `.prmd` mode encodes the 4096² image in 20–36 ms, 3.4–4.9× faster than with one thread. JPEG2000 encodes 19× slower than `haar` on one thread and 12–32× slower with threads (OpenJPEG uses at most 4 threads with 256² tiles). `cdf53` crops are 1.1–1.2× slower than `haar`'s and its full-level decode 1.2×.

## Encode scaling (multithreaded encoder)

`EncodeOptions::threads` / `--threads` (design.md §1.4). The `.prmd` file is byte-identical for every thread count; every row below was checked. *auto* is the default `threads = 0`, with the count it chose.

**Full image** (`test_image.jpg`, 8824 × 5336, 5 levels), `prmd encode` wall time in ms, median of 3 (1 for zstd 19), from an earlier run on this branch the same day (the 4096² tables below were rerun at 7b0db91 and agree with it). *base* is the encoder before threading (fc0a629); its N = 1 column differs from it only through single-thread fixes in the same change (row-major Cdf53 vertical lifting, one upsample per Laplacian level, no serial zero-fill, fused colour conversion).

| config | base | N=1 | N=2 | N=4 | N=8 | N=14 | auto (used) | auto vs base |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar gray | 276.4 | 234.2 | 135.4 | 78.7 | 62.9 | 56.9 | 55.1 (14) | 5.0× |
| haar rgb | 824.5 | 759.8 | 424.8 | 246.8 | 165.2 | 166.7 | 162.4 (14) | 5.1× |
| cdf53 gray | 1257.1 | 306.0 | 184.4 | 126.0 | 76.0 | 77.9 | 78.0 (14) | 16.1× |
| cdf53 rgb q4 | 3803.3 | 1163.6 | 645.3 | 357.1 | 235.3 | 275.8 | 272.5 (14) | 14.0× |
| laplacian rgb q4 | 2695.9 | 1885.4 | 983.2 | 535.1 | 319.7 | 280.0 | 281.1 (14) | 9.6× |
| laplacian-gauss gray | 766.7 | 543.2 | 295.4 | 166.8 | 106.2 | 101.2 | 99.6 (14) | 7.7× |
| naive gray | 329.8 | 308.8 | 164.5 | 95.3 | 63.2 | 57.5 | 59.5 (14) | 5.5× |
| haar gray + range | 1486.8 | 1415.8 | 734.0 | 391.3 | 219.1 | 163.1 | 163.8 (14) | 9.1× |
| haar rgb + zstd 19 | 26443.3 | 26355.3 | 13571.3 | 7036.5 | 3804.3 | 2778.9 | 2800.7 (14) | 9.4× |
| j2k gray (256² tiles) | 4022.7 | 4029.0 | 2458.0 | 1742.5 | 1767.8 | 1729.6 | 1725.2 (4) | 2.3× |
| j2k rgb (1024² tiles) | 8475.9 | 8480.6 | 4662.5 | 2615.4 | 1775.8 | 1731.9 | 1744.4 (14) | 4.9× |

**4096² centre crop** (`prmdbench --scaling 1,2,4,8,14,0`, command under [Setup](#setup); `encode_file` wall time, median of 3, 1 above 3 s). *best vs N=1*: the N=1 time over the fastest column's. *identical*: the file is the same at every count.

Gray:

| codec | N=1 | N=2 | N=4 | N=8 | N=14 | auto | best vs N=1 | identical |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | 70 ms | 40 ms | 24 ms | 18 ms | 17 ms | 17 ms (14 used) | 4.13× (N=14) | yes |
| cdf53 | 92 ms | 53 ms | 32 ms | 23 ms | 25 ms | 24 ms (14 used) | 3.98× (N=8) | yes |
| laplacian-box | 139 ms | 74 ms | 43 ms | 29 ms | 28 ms | 28 ms (14 used) | 5.06× (N=14) | yes |
| laplacian-gauss | 181 ms | 98 ms | 56 ms | 35 ms | 36 ms | 36 ms (14 used) | 5.18× (N=8) | yes |
| naive | 96 ms | 52 ms | 30 ms | 21 ms | 20 ms | 19 ms (14 used) | 5.18× (auto) | yes |
| j2k-t256 | 1476 ms | 878 ms | 616 ms | 614 ms (4 used) | 630 ms (4 used) | 630 ms (4 used) | 2.40× (N=8) | yes |
| j2k-t1024 | 1483 ms | 792 ms | 427 ms | 272 ms | 245 ms | 247 ms (14 used) | 6.06× (N=14) | yes |
| haar+zstd19 | 1218 ms | 626 ms | 326 ms | 177 ms | 138 ms | 134 ms (14 used) | 9.10× (auto) | yes |
| haar+range | 515 ms | 262 ms | 140 ms | 77 ms | 57 ms | 58 ms (14 used) | 9.00× (N=14) | yes |

RGB:

| codec | N=1 | N=2 | N=4 | N=8 | N=14 | auto | best vs N=1 | identical |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | 256 ms | 139 ms | 78 ms | 48 ms | 44 ms | 44 ms (14 used) | 5.86× (N=14) | yes |
| cdf53 | 319 ms | 180 ms | 104 ms | 68 ms | 66 ms | 64 ms (14 used) | 4.95× (auto) | yes |
| laplacian-box | 513 ms | 277 ms | 152 ms | 91 ms | 80 ms | 81 ms (14 used) | 6.40× (N=14) | yes |
| laplacian-gauss | 654 ms | 350 ms | 194 ms | 111 ms | 98 ms | 98 ms (14 used) | 6.68× (auto) | yes |
| naive | 357 ms | 190 ms | 104 ms | 61 ms | 50 ms | 49 ms (14 used) | 7.22× (auto) | yes |
| j2k-t256 | 3025 ms | 1904 ms | 1432 ms | 1451 ms (4 used) | 1452 ms (4 used) | 1415 ms (4 used) | 2.14× (auto) | yes |
| j2k-t1024 | 3003 ms | 1647 ms | 910 ms | 604 ms | 574 ms | 573 ms (14 used) | 5.24× (auto) | yes |
| haar+zstd19 | 9068 ms | 4650 ms | 2411 ms | 1289 ms | 924 ms | 925 ms (14 used) | 9.81× (N=14) | yes |
| haar+range | 1116 ms | 582 ms | 304 ms | 164 ms | 124 ms | 121 ms (14 used) | 9.20× (auto) | yes |

The N=1 column matches the `--threads 1` encode columns elsewhere in this document within 8% (separate runs), and *auto* matches N=14: the automatic count picks every hardware thread for a 4096² image. Differences of a few percent between N=8, N=14 and auto are run-to-run noise.

- Block coding scales almost linearly with the slow coders (range, zstd 19: 9–10× at 14 threads); with zstd 1 the whole encode reaches 4–7×, limited by memory bandwidth in the transforms and by what stays serial (allocating level planes, writing the file).
- Beyond 8 threads the zstd-1 encodes gain little on this machine (its cores are not all equal), and gray `cdf53` and `laplacian-gauss` get slightly slower (within noise).
- JPEG2000 with 256² tiles uses at most 4 threads: OpenJPEG codes one tile at a time, and more threads made it slower. 1024² tiles scale to 5–6×. Its output is identical at every thread count too.
- Peak memory (`/usr/bin/time`, full image) fell with the same change and does not grow with threads: `haar` RGB 2.21 → 1.43 GB, `laplacian` RGB q4 3.91 → 1.68 GB, `haar` gray 0.87 → 0.57 GB (the same at `--threads 1`, 14 and automatic).

### Automatic thread count

`threads = 0` uses ⌊√(W / 32 Ki)⌋ threads for W work units (level-0 samples × channels × the mode/coder weight, design.md §1.4), at most one per hardware thread and per level-0 code-block. The rule was fitted to the table below: centre crops of `test_image.jpg` (5 levels) encoded at fixed counts 1, 2, 3, 4, 6, 8, 11, 14 through the library (`encode_file`, median of 3–41 runs, file on tmpfs). Each cell: *one-thread ms* · *fastest count* (*its speedup over one thread*) · *count the automatic rule picks*. (J2K: the counts asked for; it uses at most 4 with 256² tiles.)

| size | gray haar | RGB haar | gray cdf53 | RGB laplacian | gray haar + range | RGB haar + zstd 19 | gray J2K (256² tiles) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 64² | 0.04 · 1 (1.0×) · 1 | 0.09 · 1 (1.0×) · 1 | 0.04 · 1 (1.0×) · 1 | 0.14 · 1 (1.0×) · 1 | 0.09 · 1 (1.0×) · 1 | 1.86 · 4 (1.2×) · 1 | 0.35 · 1 (1.0×) · 1 |
| 128² | 0.09 · 1 (1.0×) · 1 | 0.26 · 1 (1.0×) · 1 | 0.11 · 1 (1.0×) · 1 | 0.52 · 1 (1.0×) · 1 | 0.50 · 1 (1.0×) · 1 | 8.79 · 2 (1.1×) · 1 | 1.75 · 1 (1.0×) · 1 |
| 256² | 0.29 · 1 (1.0×) · 1 | 1.05 · 3 (1.1×) · 2 | 0.37 · 1 (1.0×) · 1 | 1.97 · 4 (1.2×) · 3 | 2.02 · 6 (1.7×) · 3 | 33.48 · 8 (3.2×) · 4 | 6.70 · 4 (1.4×) · 2 |
| 384² | 0.62 · 3 (1.2×) · 2 | 2.37 · 6 (1.6×) · 4 | 0.80 · 4 (1.3×) · 2 | 4.59 · 6 (1.9×) · 5 | 4.58 · 6 (2.6×) · 5 | 69.58 · 14 (5.8×) · 9 | - |
| 512² | 0.96 · 4 (1.3×) · 2 | 4.16 · 8 (2.0×) · 5 | 1.25 · 4 (1.6×) · 3 | 7.50 · 8 (2.5×) · 7 | 8.18 · 8 (3.6×) · 7 | 126.01 · 14 (6.8×) · 14 | 26.99 · 11 (1.8×) · 4 |
| 768² | 2.17 · 6 (2.0×) · 4 | 8.93 · 8 (3.0×) · 8 | 2.74 · 6 (2.0×) · 4 | 17.41 · 8 (3.6×) · 10 | 18.54 · 11 (5.1×) · 10 | 281.99 · 14 (7.9×) · 14 | - |
| 1024² | 4.08 · 6 (2.3×) · 5 | 15.89 · 8 (3.7×) · 10 | 5.15 · 6 (2.7×) · 6 | 29.95 · 8 (4.2×) · 14 | 33.52 · 14 (6.2×) · 14 | 521.88 · 14 (8.0×) · 14 | 102.97 · 14 (2.2×) · 4 |
| 1536² | 10.18 · 8 (3.0×) · 8 | 36.59 · 11 (4.6×) · 14 | 12.48 · 8 (3.6×) · 9 | 70.22 · 11 (5.4×) · 14 | 72.95 · 14 (7.7×) · 14 | 1267.94 · 14 (8.8×) · 14 | - |
| 2048² | 17.90 · 8 (3.3×) · 11 | 64.63 · 11 (5.2×) · 14 | 22.63 · 8 (3.9×) · 12 | 125.69 · 11 (5.4×) · 14 | 130.41 · 14 (7.6×) · 14 | 2261.12 · 14 (9.3×) · 14 | 378.90 · 11 (2.3×) · 4 |
| 3072² | 41.65 · 8 (3.6×) · 14 | 147.94 · 11 (4.9×) · 14 | 54.60 · 11 (3.8×) · 14 | 286.00 · 14 (5.6×) · 14 | 291.97 · 14 (9.1×) · 14 | - | - |
| 4096² | 73.25 · 11 (3.9×) · 14 | 259.34 · 11 (4.9×) · 14 | 96.84 · 11 (3.9×) · 14 | 504.01 · 11 (5.6×) · 14 | 516.57 · 14 (8.9×) · 14 | - | - |
| full | 217.71 · 11 (4.3×) · 14 | 739.55 · 14 (5.2×) · 14 | 288.27 · 11 (4.3×) · 14 | 1423.36 · 14 (5.9×) · 14 | 1413.94 · 14 (8.7×) · 14 | - | - |

Over these 15 configurations × 12 sizes (also gray/RGB cdf53, laplacian, naive, RGB range, q4) the automatic count is never more than 5% slower than one thread and on average 3.6% slower than the fastest fixed count (geometric mean; worst cases 1.2–1.3× slower than the fastest count at mid sizes, apart from one 14-thread outlier, RGB cdf53 at 3072², 1.6×, where 14 threads were slower than 11 in that run). A linear rule (one thread per fixed amount of work) either starts threads on images too small to gain or uses all 14 at 512², where 4–8 are fastest.

**Before and after** (threads = 1 vs the default, the same crops; *before* is the previous default, all hardware threads, up to the block count; ms, median):

| config | size | before: 1 | before: default (used) | after: 1 | after: auto (used) | best fixed (N) |
|---|---|---:|---:|---:|---:|---:|
| gray haar | 64² | 0.03 | 0.90 (5) | 0.04 | 0.05 (1) | 0.04 (1) |
| gray haar | 128² | 0.08 | 1.44 (5) | 0.09 | 0.09 (1) | 0.09 (1) |
| gray haar | 256² | 0.32 | 2.46 (8) | 0.27 | 0.27 (1) | 0.29 (1) |
| gray haar | 512² | 1.16 | 3.99 (14) | 1.03 | 0.92 (2) | 0.77 (4) |
| gray haar | 1024² | 4.29 | 4.86 (14) | 4.29 | 1.85 (5) | 1.79 (6) |
| gray haar | 2048² | 17.97 | 7.30 (14) | 18.50 | 4.33 (11) | 5.41 (8) |
| gray haar | 4096² | 75.68 | 20.63 (14) | 76.05 | 18.75 (14) | 18.56 (11) |
| gray haar | full | 228.84 | 50.04 (14) | 226.11 | 50.00 (14) | 51.11 (11) |
| RGB haar | 64² | 0.09 | 0.96 (5) | 0.10 | 0.09 (1) | 0.09 (1) |
| RGB haar | 128² | 0.25 | 2.07 (5) | 0.26 | 0.25 (1) | 0.26 (1) |
| RGB haar | 256² | 1.04 | 3.90 (8) | 0.98 | 1.02 (2) | 0.98 (3) |
| RGB haar | 512² | 3.99 | 9.17 (14) | 3.93 | 1.92 (5) | 2.04 (8) |
| RGB haar | 1024² | 15.82 | 13.92 (14) | 15.48 | 4.25 (10) | 4.27 (8) |
| RGB haar | 2048² | 65.10 | 23.07 (14) | 65.71 | 12.80 (14) | 12.52 (11) |
| RGB haar | 4096² | 261.68 | 63.99 (14) | 262.35 | 54.20 (14) | 53.16 (11) |
| RGB haar | full | 747.37 | 151.64 (14) | 740.66 | 143.33 (14) | 140.87 (14) |
| gray haar + range | 64² | 0.07 | 0.84 (5) | 0.09 | 0.08 (1) | 0.09 (1) |
| gray haar + range | 128² | 0.54 | 1.54 (5) | 0.44 | 0.42 (1) | 0.50 (1) |
| gray haar + range | 256² | 2.04 | 2.64 (8) | 1.96 | 1.34 (3) | 1.21 (6) |
| gray haar + range | 512² | 8.48 | 4.62 (14) | 8.17 | 2.32 (7) | 2.25 (8) |
| gray haar + range | 1024² | 32.96 | 7.96 (14) | 32.92 | 5.28 (14) | 5.43 (14) |
| gray haar + range | 2048² | 129.93 | 18.01 (14) | 130.13 | 15.04 (14) | 17.14 (14) |
| gray haar + range | 4096² | 515.55 | 61.85 (14) | 523.90 | 60.98 (14) | 58.27 (14) |
| gray haar + range | full | 1425.69 | 158.11 (14) | 1426.71 | 161.74 (14) | 161.98 (14) |
| RGB haar + zstd 19 | 64² | 1.76 | 2.17 (5) | 1.80 | 1.78 (1) | 1.57 (4) |
| RGB haar + zstd 19 | 128² | 8.82 | 9.26 (5) | 8.78 | 8.85 (1) | 7.77 (2) |
| RGB haar + zstd 19 | 256² | 33.16 | 13.56 (8) | 33.43 | 13.43 (4) | 10.56 (8) |
| RGB haar + zstd 19 | 512² | 125.68 | 26.48 (14) | 125.71 | 18.95 (14) | 18.65 (14) |
| RGB haar + zstd 19 | 1024² | 522.52 | 74.48 (14) | 521.86 | 64.34 (14) | 65.32 (14) |
| gray J2K | 64² | 0.23 | 2.63 (4) | 0.33 | 0.33 (1) | 0.35 (1) |
| gray J2K | 128² | 1.68 | 3.30 (4) | 1.57 | 1.84 (1) | 1.75 (1) |
| gray J2K | 256² | 6.83 | 5.09 (4) | 6.71 | 5.65 (2) | 4.87 (4) |
| gray J2K | 512² | 25.98 | 15.50 (4) | 26.59 | 14.78 (4) | 15.06 (11) |
| gray J2K | 1024² | 98.96 | 44.29 (4) | 98.81 | 46.69 (4) | 47.37 (14) |
| gray J2K | 2048² | 378.47 | 165.94 (4) | 378.60 | 160.28 (4) | 162.43 (11) |

The previous default was up to 13× slower than one thread on small images (a 128² RGB encode: 2.07 ms instead of 0.25); the automatic count matches one thread there (no thread is started) and the fastest fixed count on large images. Part of the gain at mid sizes is the pool itself: a parallel run now wakes only the workers that join it, workers spin for 100 µs between stages, and loops of ≤ 32 Ki samples run inline (1024² RGB at 14 threads: 14.4 → 4.8 ms; an empty 14-thread run: 320 → 29 µs). Every output file is now written to a temporary file and renamed, which adds about 7 µs per encode (64² gray: 0.033 → 0.040 ms).

## Entropy coder and zstd level

**Coder.** zstd: 11.58 MiB at 0.59 ms per 256² crop. Range coder: 11.13 MiB at 7.4 ms. zstd is the default ([design.md §7.1](design.md)).

**zstd level.** Level 1 is the default. Level 19 decodes at the same speed but encodes 13–17× slower on gray and 28–36× slower on RGB with one thread (gray `haar`: 1.22 s instead of 70 ms; RGB `haar`: 9.1 s instead of 0.26 s), and 6–10× / 15–21× slower with the automatic thread count (gray `haar` 134 vs 17 ms, RGB `haar` 0.93 s vs 44 ms), since its block coding scales better than the zstd-1 encode. These are from the scaling run above and a run with `--codecs haar,haar+zstd19,cdf53,cdf53+zstd19,naive,naive+zstd19 --scaling 1,0`. It makes gray lossless files no smaller, but RGB files 2.7–4.1% smaller and lossy files 3–12% smaller (the lossy figure is from an earlier run; the benchmark has no lossy zstd-19 row). Pass `--entropy-level 19` when size matters more than encode time ([design.md §7.2](design.md)).

## Level definition

`haar` levels are the exact 2×2 mean, but they alias. `cdf53` and `laplacian-gauss` levels are sharper or smoother and differ from the 2×2 mean by 24–30 dB PSNR, mostly a half-pixel shift. The image matcher's needs should decide the mode, not the codec. [modes.md](modes.md) has side-by-side samples and the PSNR table.

## Lossy

`--quant Q`. Max error is in 8-bit pixel values (0–255). Bits per pixel are at the default zstd level 1.

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|
| haar | Q = 4 | 3.66 | 38.6 dB | 12 | 0.68 ms |
| cdf53 | Q = 4 | 3.53 | 41.9 dB | 11 | 0.71 ms |
| laplacian-gauss | Q = 4 | 4.72 | 46.4 dB | **2** (±Q/2, guaranteed) | 0.91 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 8.7 ms |

`haar` and `cdf53` quantize open loop, so errors pile up toward level 0. `laplacian` is closed loop and keeps every level within ±Q/2, at the cost of a larger file. JPEG2000 wins rate–distortion: 44.7 dB at 3.14 bits/px, where `cdf53` needs 3.53 bits/px for 41.9 dB. It gets there through rate-distortion-optimised truncation and the smoother 9/7 filter. `.prmd` decodes 10–13× faster.

## RGB

Same image and setup, converted to RGB8 (raw L0 48.00 MiB, raw pyramid 63.94 MiB), 100 crops per row, from the RGB runs under [Setup](#setup) (full report in `build/bench/results_rgb.md`). RGB stays bit-exact through the reversible YCoCg-R transform; every block holds all three channels, so a crop needs no extra reads.

### File size

| codec | file size | bits/px | vs raw L0 | vs raw pyramid | vs naive | encode: 1 thread / auto |
|---|---:|---:|---:|---:|---:|---:|
| **haar** | 23.91 MiB | 11.96 | 2.01× smaller | 2.67× smaller | **−22.5%** | 288 / 64 ms |
| cdf53 | 24.91 MiB | 12.46 | 1.93× smaller | 2.57× smaller | −19.2% | 331 / 73 ms |
| laplacian-box | 31.96 MiB | 15.98 | 1.50× smaller | 2.00× smaller | +3.6% | 507 / 90 ms |
| laplacian-gauss | 32.38 MiB | 16.19 | 1.48× smaller | 1.98× smaller | +5.0% | 647 / 97 ms |
| naive (same coder) | 30.84 MiB | 15.42 | 1.56× smaller | 2.07× smaller | 0 | 350 / 50 ms |
| naive, `--no-med --no-ycocg`³ | 57.83 MiB | 28.92 | 1.20× larger | 1.11× smaller | +87.5% | 261 / 62 ms |
| haar, zstd 19 | 22.92 MiB | 11.46 | 2.09× smaller | 2.79× smaller | −25.7% | 9063 / 929 ms |
| haar, range coder | **20.52 MiB** | 10.26 | 2.34× smaller | 3.12× smaller | −33.5% | 1129 / 117 ms |
| JPEG2000, 256² tiles | 20.98 MiB | 10.49 | 2.29× smaller | 3.05× smaller | −32.0% | 2910 / 1472 ms |

³ From a separate `prmdbench` run on the same machine (2026-11-16, `--codecs naive,naive-no-med,naive-no-ycocg,naive-no-med-no-ycocg`), whose plain `naive` matched the figures here within 2%. Base blocks store raw R, G and B values, with neither MED nor YCoCg-R.

Unlike gray, `cdf53` is larger than `haar` on RGB, and both `laplacian` variants are larger than `naive`: presumably their ~1.33× residual samples cost more than the cross-level prediction saves on the chroma planes (not investigated). On RGB, zstd files are 12–17% larger than with the range coder (12% at level 19, 17% at level 1), so JPEG2000 is smaller than the default `.prmd` RGB file; `haar+range` is still the smallest.

### Per-crop latency and I/O

*Tiles* and *bytes* as in the gray table.

| crop | level | haar mean / p95 | tiles · bytes | naive mean / p95 | tiles · bytes | J2K mean / p95 | tiles · bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 1.89 / 2.14 ms | 9.6 · 339 KiB | 1.85 / 1.95 ms | 4 · 196 KiB | 28.0 / 33.9 ms | 4 · 353 KiB |
| 256² | L2 | 1.57 / 1.93 ms | 6.7 · 330 KiB | 1.76 / 1.84 ms | 4 · 249 KiB | 21.9 / 23.5 ms | 25 · 2.1 MiB |
| 256² | L4 (whole level) | 0.83 / 0.87 ms | 1.0 · 113 KiB | 0.83 / 0.89 ms | 1 · 113 KiB | 17.3 / 20.1 ms | 256 · 21.0 MiB |
| 512² | L0 | 5.52 / 5.82 ms | 17.4 · 784 KiB | 5.15 / 5.37 ms | 9 · 557 KiB | 72.4 / 83.6 ms | 9 · 801 KiB |
| 512² | L3 (whole level) | 3.38 / 3.44 ms | 5.0 · 460 KiB | 3.25 / 3.38 ms | 4 · 451 KiB | 61.5 / 63.5 ms | 256 · 21.0 MiB |

An RGB crop costs about 3–3.5× a gray one (three planes plus the YCoCg-R inverse). `.prmd` stays 13–21× faster than JPEG2000, and at coarse levels JPEG2000 again reads the whole file.

### Encode and decode time per mode

| mode | encode: 1 thread / auto | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 288 / 64 ms | 1.89 / 2.14 ms | 5.52 / 5.82 ms | 255 ms | 59.9 ms |
| `cdf53` | 331 / 73 ms | 2.13 / 2.41 ms | 6.27 / 6.72 ms | 311 ms | 68.8 ms |
| `laplacian-box` | 507 / 90 ms | 2.77 / 3.07 ms | 7.80 / 8.36 ms | 374 ms | 87.5 ms |
| `laplacian-gauss` | 647 / 97 ms | 2.71 / 3.04 ms | 7.43 / 7.78 ms | 359 ms | 83.4 ms |
| `naive` | 350 / 50 ms | 1.85 / 1.95 ms | 5.15 / 5.37 ms | 238 ms | 59.5 ms |
| `naive --no-med --no-ycocg`³ | 261 / 62 ms | 1.06 / 1.22 ms | 3.12 / 3.64 ms | 161 ms | 39.2 ms |
| `haar`, zstd 19 | 9063 / 929 ms | 2.29 / 2.67 ms | 5.94 / 6.89 ms | 262 ms | 62.0 ms |
| `haar`, range coder | 1129 / 117 ms | 16.6 / 20.8 ms | 39.0 / 44.5 ms | 1275 ms | 354 ms |
| JPEG2000, 256² tiles | 2910 / 1472 ms | 28.0 / 33.9 ms | 72.4 / 83.6 ms | 2641 ms | 808 ms |

Without MED and YCoCg-R, RGB `naive` crops decode about 1.7× faster (1.06 against 1.82 ms in the same run), and the file nearly doubles (+87.5%), ending up larger than raw L0.

With the automatic thread count the RGB `.prmd` encodes take 50–97 ms, 4.2–7× faster than with one thread; the slow coders gain most (range 9.6×, zstd 19 9.8×), JPEG2000 with 256² tiles only 2× (at most 4 threads). zstd 19 saves 4.1% on `haar` for a 31× slower encode with one thread, 15× with threads. Full-level decode is the same speed; its 256² crops were 21% slower in the single-thread run (2.29 vs 1.89 ms) and 5% slower in the automatic-thread run, so this is mostly noise. The range coder is 8.8× slower per 256² crop.

**Block cache.** On a 1000-step random walk of 256² crops at L0, a 64 MiB cache brings `haar` from 1.98 to 0.55 ms, `naive` from 1.80 to 0.16 ms and `haar+range` from 16.0 to 0.67 ms.

### Lossy

`--quant Q`, quantization on Y/Co/Cg. PSNR and max error are over all three RGB channels, in 8-bit pixel values (0–255). Bits per pixel are per pixel, not per channel.

| codec | setting | file size | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|---:|
| haar | Q = 4 | 13.29 MiB | 6.64 | 37.7 dB | 16 | 2.27 ms |
| haar | Q = 8 | 8.95 MiB | 4.47 | 34.4 dB | 28 | 2.18 ms |
| cdf53 | Q = 4 | 13.42 MiB | 6.71 | 40.6 dB | 15 | 2.53 ms |
| cdf53 | Q = 8 | 8.91 MiB | 4.45 | 36.3 dB | 23 | 2.42 ms |
| cdf53, range coder | Q = 4 | 10.46 MiB | 5.23 | 40.6 dB | 15 | 12.3 ms |
| laplacian-gauss | Q = 4 | 20.74 MiB | 10.37 | 44.4 dB | 4 | 3.09 ms |
| laplacian-gauss | Q = 8 | 14.47 MiB | 7.23 | 39.5 dB | 8 | 3.00 ms |
| JPEG2000 9/7 | 45 dB target | 9.02 MiB | 4.51 | 44.3 dB | 15 | 14.4 ms |
| JPEG2000 9/7 | 40 dB target | 6.32 MiB | 3.16 | 39.7 dB | 30 | 10.5 ms |

JPEG2000's rate–distortion lead is larger on RGB than on gray: at about 40 dB it needs 3.16 bits/px against 6.71 for `cdf53`. `laplacian`'s ±Q/2 guarantee holds on Y/Co/Cg, not per RGB channel, so its RGB max error reaches Q (4 at Q = 4). At similar PSNR, `.prmd` lossy crops decode about 4× faster than JPEG2000's (`cdf53` Q = 4 vs the 40 dB target: 2.53 vs 10.5 ms; `laplacian-gauss` Q = 4 vs 45 dB: 3.09 vs 14.4 ms).

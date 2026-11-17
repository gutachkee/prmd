# Pipeline durations

Where does the time go when you encode an image once and then read about 10 crops from it? This document times each step of the encode and decode pipelines, for several modes and options. Every number can be reproduced with `prmdpipeline` and the timers built into the library (see [How it was measured](#how-it-was-measured)).

**Short answer:**
- **Encoding is about 98% of encode + decode on one thread** (56% of the total once the JPEG load is included), and 91% with the default automatic thread count (14 threads here). With the defaults, loading the JPEG takes 180 ms and encoding it 237 ms on one thread or 58 ms with threads. Opening the file and decoding all 10 crops take 5.4 ms. The encode alone costs as much as about 430 crops on one thread, about 105 with threads; with threads, loading the JPEG is the largest step (74% of the scenario).
- **Encode** (one thread): with the default `haar` + zstd 1, more than half the encode is entropy coding (zstd compression 45%, byte packing 9%). The forward transform takes 21%, and converting the image to gray int32 planes 13%. With 14 threads the block stage runs 9× faster, and the serial `write_file` becomes a third of the encode.
- **Decode:** about 55% of every crop is entropy decoding (zstd decompression 38%, zigzag unpacking 16%). The inverse transform takes 21%. Reading from the file is 3%. Decoding is single-threaded, so decode times do not depend on `--threads`.
- **What changes it most:**
  - Encode (one thread): `--entropy-level 19` makes it 17.5× slower, `--entropy range` 6×, `laplacian` 2.3× and `--quant 4` 1.7×; `cdf53` is now only 1.24× (it was 4.2× before its vertical pass became row-major). With threads the slow coders gain most: zstd 19 is 9.2× faster than on one thread, range 8.6×, the zstd-1 defaults 4.1×.
  - Decode: `--entropy range` makes it 13.6× slower, and `rgb` 3.2×. The LRU cache makes it 37% faster.

## Setup

| | |
|---|---|
| image | `test_image.jpg`: 8824 × 5336 RGB JPEG (47 MP, 13 MiB) |
| pyramid | `--levels 6`, so L5 (276 × 167) is the coarsest level |
| defaults | `haar`, `gray`, zstd level 1, `--cblk 128`, `--tile 256`, lossless, no cache |
| machine | MacBook Pro, Apple Silicon (14 cores, aarch64) under Linux, GCC 15.2 `-O2 -mcpu=native`, zstd 1.5.7 |
| encoder threads | `--threads 1` (serial) for the breakdowns, and the default `--threads 0` (automatic: 14 threads for this image) where a column or table says *auto* or *14 threads* |
| code | branch `threading`, commit 7b0db91 |
| file | written to `/tmp` (tmpfs) with `--file` |
| repeats | every scenario run 5 times in one process; each number is the median |
| date | 2026-11-16 |

**Scenario.** Each run does these steps once, in this order:
1. `load_image`
2. `encode_file`
3. `open_decoder`
4. 10 crops of 256 × 256, all with `decode_clamped`, around the point at 60% / 40% of the image:

| # | level | step |
|---|---|---|
| 1 | L5 | overview: L5 is only 167 px tall, so the crop is clamped to 256 × 167 |
| 2–6 | L4 → L0 | zoom in on the same point, one level at a time |
| 7–9 | L0 | pan by +128 px, +256 px, then +256 / +128 px |
| 10 | L0 | jump to a distant area (−2000 / +1200 px) |

## How it was measured

The timers are part of the code, and are compiled out unless you ask for them.

- **`PRMD_TIMED("step")`** (`include/prmd/profile.h`, `src/profile.cpp`) times the rest of the enclosing block. Unless the macro `PRMD_PROFILE` is defined, it expands to nothing, so a normal build has no timer code; `nm` on its `libprmd_core.a` shows only the stub `prmd::profile::reset()` / `snapshot()` entry points. `cmake -DPRMD_PROFILE=ON` defines it for `prmd_core` and everything linked with it.
- **With timers on,** each scope reads `std::chrono::steady_clock` on entry and exit and adds the difference to a node of a call tree. A scope opened inside another becomes its child, and repeated calls add up in one node with a call count. So a per-block step such as `zstd_compress` appears once, with its 3906 calls summed.
- **Where the timers are:**
  - `src/image_io.cpp`: `load_image`.
  - `src/prmd_encoder.cpp`: every encode step: convert, decompose, building the stored planes, the laplacian closed loop, block extraction and MED, entropy encoding, serialize and write.
  - `src/blockcoder_bytes.cpp`, `src/blockcoder_range.cpp`: pack and unpack, `ZSTD_compress` and `ZSTD_decompress`, the range coder.
  - `src/prmd_decoder.cpp`, `src/crop_decoder.cpp`: open and header read, block lookup, `pread`, per-block decode, MED reconstruction, dequantize, YCoCg, copying blocks into the band buffers, the inverse transform and `from_planes`.
- **`prmdpipeline`** (`tools/prmd_pipeline.cpp`) runs the scenario. It times the four top-level steps and each crop with its own `steady_clock` reads, which are always on. When the library has timers it also prints the median call tree, with an `(other)` row for any parent time its children don't cover.
- **Two builds:**
  ```sh
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
  cmake -B build-profile -DCMAKE_BUILD_TYPE=Release -DPRMD_PROFILE=ON && cmake --build build-profile -j
  build/prmdpipeline test_image.jpg --runs 5 --levels 6 --threads 1                 # totals, one thread
  build/prmdpipeline test_image.jpg --runs 5 --levels 6                             # totals, automatic threads
  build-profile/prmdpipeline test_image.jpg --runs 5 --levels 6 --threads 1         # breakdown, one thread
  build-profile/prmdpipeline test_image.jpg --runs 5 --levels 6                     # breakdown, automatic threads
  build-profile/prmdpipeline test_image.jpg --runs 5 --levels 6 --threads 1 --mode cdf53 --quant 4   # any other row
  ```
  Totals come from `build`, and shares and step times from `build-profile`. Every row of [All modes and options](#all-modes-and-options) was run at `--threads 1` and at the default in both builds.
- **Overhead of the timers.** Single-thread encodes agree within ±4% between the two builds (the timer build was faster in 4 of 12 rows). The 10 crops took 0–10% longer in the timer build (14% for `naive`, see below); a crop runs about 110 timer scopes, so part of that is overhead and part noise. Shares within the timer build are not affected much.
- **Threads in the breakdown.** With more than one thread, `prmdpipeline` adds an "on workers" column. Steps opened on the calling thread around parallel work (`convert`, `forward`, `encode_blocks`, …) are wall time; steps that also ran on pool workers (`make_block`, `entropy_encode` and their children) add up the time of every thread, so they can exceed their parent ([profile.h](../include/prmd/profile.h), "Threads").
- **Host effects to know about.** On this machine an occasional process runs `naive` decode about 25% slower throughout: 7.1–7.3 ms for the 10 crops instead of 5.5–5.9 ms. That happened in about 1 of 6 processes in both builds, pinned to a core or not, and once switched to fast partway through a run. It looks like the virtual CPU being scheduled on a slower core. MED reconstruction is a serial chain of dependent operations and shows it most; `med_reconstruct` in the defaults moves between 0.40 and 0.70 ms for the same reason. Also, in this scenario the 10 crops after a 14-thread encode were 5–13% slower than after a one-thread encode (defaults: 5.9–6.2 ms against 5.3–5.5 ms over four processes each; `--threads 2` was like one thread). The decoder does not use threads and runs after the encoder's threads have been joined, so this is the state the encode leaves behind (caches, the heap, the VM's core placement), not decode work; `prmdbench`, which decodes long after its encodes, shows no such difference. The *10 crops* column below is from the `--threads 1` runs.
- **I/O.** The file has just been written, so every read is served from the OS page cache. A cold disk would make `pread` cost more. That was not measured.

## Threads: defaults on 1 and 14 threads

Defaults (`haar`, gray, zstd 1), timer build, median of 5, `prmdpipeline --levels 6 --threads 1` and the default (automatic, which uses all 14 threads for this image). At 14 threads, steps opened on the calling thread are wall time; the per-block steps under `encode_blocks` add up the time of all 14 threads (92–93% of it on workers), so they exceed their parent.

| step | 1 thread: ms | 14 threads: ms | 14 threads: what it is |
|---|---:|---:|---|
| **`encode_prmd`** | **233** | **57.4** | wall |
| └ `convert` (gray + int32 planes) | 30.2 | 8.5 | wall, row stripes |
| └ `decompose` / `forward` | 49.3 | 12.6 | wall, row stripes |
| └ `stored_planes` | 0.55 | 0.75 | wall (moves only, when lossless) |
| └ `encode_blocks` | 138 | 14.9 | wall, 3906 blocks over 14 threads |
|  └ `make_block` | 10.2 | 22.4 | summed over threads |
|  └ `entropy_encode` | 127 | 174 | summed over threads (zstd 143, pack 28.4) |
| └ `write_file`, 31.6 MiB | 13.2 | 18.7 | wall, serial |
| └ other (allocation, block loop) | 1.8 | 2.1 | wall |

The block stage scales 9×; summed over threads it costs 1.4× the single-thread time (shared caches and memory bandwidth, and unequal cores). What is left serial at 14 threads is mainly `write_file` (33% of the encode) and allocating the level planes. Loading the JPEG (179 ms) is 3.1× the encode. Every other row: [All modes and options](#all-modes-and-options); scaling for every mode: [benchmarks.md](benchmarks.md#encode-scaling-multithreaded-encoder).

## Timeline: defaults (`haar`, gray, zstd 1)

### Encode (once)

Timer build, median of 5 runs, `--threads 1`. Nested steps are indented and included in their parent. *Share* is of `encode_prmd` (233 ms). The same steps on 14 threads: [Threads](#threads-defaults-on-1-and-14-threads).

| step | calls | ms | share | what it does |
|---|---:|---:|---:|---|
| `load_image` (libjpeg) | 1 | 179 | — | decode the 47 MP RGB JPEG; before the encoder |
| **`encode_prmd`** | 1 | **233** | 100% | |
| └ `convert` | 1 | 30.2 | 13% | RGB → gray int32 plane in one pass |
| └ `decompose` / `forward` | 5 | 49.3 | 21% | Haar: L0 → L1 … L4 → L5 (LL plus 3 bands per level) |
| └ `stored_planes` | 1 | 0.55 | 0% | move the bands into their stored layout (no quantize pass when lossless) |
| └ `encode_blocks` | 1 | 138 | 59% | every block, coarsest level first |
|  └ `make_block` | 3906 | 10.2 | 4% | gather one block's samples |
|   └ `extract` | 3906 | 9.8 | 4% | copy the block's samples out of the plane |
|   └ `med_residual` | 6 | 0.04 | 0% | only the 6 base (L5) blocks |
|  └ `entropy_encode` | 3906 | 127 | 54% | code one block |
|   └ `pack` | 3906 | 21.0 | 9% | int32 → zigzag uint16, split into low and high bytes |
|   └ **`zstd_compress`** | 3906 | **104** | **45%** | **the largest single step** |
| └ `serialize` header + index | 1 | 0.00 | 0% | |
| └ `write_file`, 31.6 MiB | 1 | 13.2 | 6% | temporary file, renamed over the target when complete |
| └ other (geometry, allocation, block loop) | | 1.8 | 1% | |

The build without timers measured 237 ms for the same encode, and 58 ms with the default automatic thread count.

### Decode (open plus 10 crops)

Build without timers, median of 5 runs (after a `--threads 1` encode). "Blocks" counts the code-blocks decoded on all levels. "KiB read" is the compressed bytes read from the file.

| # | level | step | rect (x, y, w × h) | ms | blocks | KiB read | reads |
|---|---|---|---|---:|---:|---:|---:|
| — | | `open_decoder` (header + index) | | 0.03 | | | |
| 1 | L5 | overview | 20, 0, 256 × 167 | 0.22 | 6 | 31 | 2 |
| 2 | L4 | zoom | 203, 5, 256 × 256 | 0.47 | 13 | 94 | 5 |
| 3 | L3 | zoom | 533, 138, 256 × 256 | 0.46 | 14 | 115 | 6 |
| 4 | L2 | zoom | 1195, 405, 256 × 256 | 0.54 | 16 | 140 | 9 |
| 5 | L1 | zoom | 2519, 939, 256 × 256 | 0.63 | 20 | 184 | 13 |
| 6 | L0 | zoom | 5166, 2006, 256 × 256 | 0.64 | 20 | 187 | 13 |
| 7 | L0 | pan +128 | 5294, 2006, 256 × 256 | 0.63 | 20 | 187 | 13 |
| 8 | L0 | pan +256 | 5422, 2006, 256 × 256 | 0.68 | 22 | 198 | 15 |
| 9 | L0 | pan +256, +128 | 5422, 2134, 256 × 256 | 0.58 | 18 | 159 | 11 |
| 10 | L0 | jump | 3166, 3206, 256 × 256 | 0.59 | 17 | 155 | 9 |
| | | **total** | | **5.42** | 166 | 1451 | 96 |

A crop at L0 costs about 2.9× the overview crop at L5. Each crop decodes a few blocks on every level from L5 down to the level it asks for. Each finer level adds roughly 0.1 ms or less, because a crop needs just a few blocks per level.

**Where the 5.4 ms goes** (summed over the 10 `decode` calls, timer build, 5.88 ms):

| step | calls | ms | share |
|---|---:|---:|---:|
| **entropy decode** | 166 | **3.24** | **55%** |
| └ `zstd_decompress` | 166 | 2.21 | 38% |
| └ `unpack` (zigzag + byte planes) | 166 | 0.95 | 16% |
| `inverse` Haar | 35 | 1.25 | 21% |
| `med_reconstruct` (L5 base blocks) | 18 | 0.70 | 12% |
| `from_planes` (clamp + pack to uint8) | 10 | 0.29 | 5% |
| `pread` (page cache) | 96 | 0.16 | 3% |
| `copy_blocks` into the band/level buffers | 45 | 0.11 | 2% |
| other (`block_lookup`, wrapping blocks, `std::set`, `shared_ptr`) | | 0.13 | 2% |

## All modes and options

Every row is the same scenario, with the defaults changed only in the first column. *Encode*, *10 crops* and *×def* are from the build without timers: *encode 1* at `--threads 1`, *encode auto* at the default (14 threads for every row), *10 crops* after the `--threads 1` encode. The largest-step columns are shares of `encode_prmd` and of `decode`, from the timer build at `--threads 1`. Load (about 180 ms) is the same in every row and is not included.

| change from the defaults | file (MiB) | encode 1 (ms) | ×def | encode auto (ms) | 1 ÷ auto | 10 crops (ms) | ×def | largest encode steps (1 thread) | largest decode steps |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| none (`haar`, gray, zstd 1) | 31.60 | 237 | 1.0 | 58 | 4.1× | 5.42 | 1.0 | zstd compress 44%, forward 21% | zstd decompress 38%, inverse 21% |
| `--cblk 64` | 32.45 | 276 | 1.16 | 61 | 4.5× | **4.44** | **0.82** | zstd compress 50%, forward 18% | zstd decompress 35%, inverse 25% |
| `--cblk 256` | 31.59 | **215** | **0.91** | **54** | 4.0× | 8.77 | 1.6 | zstd compress 38%, forward 23% | zstd decompress 44%, unpack 22% |
| decoder cache 64 MiB | 31.60 | 239 | 1.0 | 59 | 4.1× | **3.41** | **0.63** | (same file as defaults) | inverse 37%, zstd decompress 28% |
| `--color rgb` | 65.31 | 763 | 3.2 | 155 | 4.9× | 17.21 | 3.2 | zstd compress 51%, forward 20% | zstd decompress 33%, inverse 19% |
| `--mode cdf53` | **30.49** | 294 | 1.24 | 75 | 3.9× | 6.02 | 1.11 | zstd compress 39%, forward 5/3 33% | zstd decompress 36%, inverse 28% |
| `--mode laplacian --down gauss` | 39.30 | 555 | 2.3 | 107 | 5.2× | 7.17 | 1.32 | Gauss downsample 33%, zstd compress 23% | zstd decompress 33%, inverse 28% |
| `--mode naive` | 40.80 | 310 | 1.31 | 64 | 4.8× | 5.15 | 0.95 | zstd compress 41%, MED residual 19% | **MED reconstruct 53%**, zstd decompress 24% |
| `--quant 4` (`haar`) | **19.83** | 412 | 1.74 | 68 | 6.0× | 5.96 | 1.10 | zstd compress 37%, quantize 32% | zstd decompress 34%, inverse 21% |
| `--mode laplacian --down gauss --quant 4` | 25.92 | 813 | 3.4 | 122 | 6.7× | 7.99 | 1.47 | residual + reconstruct 27%, zstd compress 26% | zstd decompress 37%, inverse 24% |
| `--entropy range` | 30.12 | 1419 | 6.0 | 165 | 8.6× | **73.46** | **13.6** | range coder 92% | range decoder 97% |
| `--entropy-level 19` | 31.52 | **4157** | **17.5** | 450 | 9.2× | 5.53 | 1.02 | zstd compress 97% | zstd decompress 36%, inverse 22% |

## Findings, by stage

### Encode

1. **zstd compression is the largest step with the defaults** (104 ms, 45% on one thread). It grows with how much the coder has to search:
   - Level 19 takes 4.1 s, for a gray file 0.3% smaller (on this full 47 MP image; on the 4096² benchmark crop it is no smaller).
   - `--quant 4` makes compression slower too (154 ms vs. 104 ms), though the output is smaller.
   - Smaller blocks mean more calls: 141 ms at `--cblk 64` (15,492 blocks), 83 ms at `--cblk 256` (1010 blocks).
2. **Loading the JPEG (180 ms)** costs more than any single encode step and is the same for every mode; with threads it is 3× the whole default encode. Converting to gray int32 planes (one fused pass) takes 30 ms, 13% of the default encode.
3. **The `cdf53` forward transform takes 99 ms, 2× the Haar forward (49 ms).** Its vertical pass used to copy one column at a time from a row-major buffer about 17 KiB wide at L0 (952 ms, 18× Haar); it now lifts whole rows, like Haar, in both the forward and the inverse transform, so `cdf53` encodes in 1.24× the default time instead of 4.2×.
4. **`laplacian` still pays twice before entropy coding.**
   - The Gauss downsample takes 182 ms (33%).
   - The closed loop (`laplacian_residuals`) takes 146 ms lossless and 323 ms at `--quant 4` (it was 340 and 517 ms): it now upsamples once per level (`predict_upsample`, 104–106 ms) and computes residual and reconstruction in one fused pass (`residual_reconstruct`, 40 ms lossless, 218 ms with quantization).
5. **Quantizing is slow.** Lossy `haar` spends 133 ms (32%) in `quantize`. `quantize()` divides once per sample by a step only known at run time. Precomputing a reciprocal or a shift would remove most of that. When lossless, the pass is now skipped (it used to walk every sample for 6.5 ms).
6. **`naive` spends 59 ms on MED prediction** (`med_residual`, 19%), because every level is stored as pixels. The wavelet modes run MED only on the 6 base blocks (0.04 ms).
7. **On one thread, writing the file** (10–34 ms, to tmpfs), serializing the index (< 0.01 ms) and MED on the base level don't matter. On 14 threads, the serial `write_file` is a third of the default encode (19 of 57 ms).
8. **Threads** cut every encode 3.9–9.2× (*1 ÷ auto* above). The slow coders gain most, since block coding is almost all of their time and scales nearly linearly; the zstd-1 encodes gain about 4–5×, limited by memory bandwidth in the transforms and the serial file write.

### Decode

1. **Entropy decoding is more than half of every crop with zstd.** The byte-plane unpack is 0.95 ms for every 2.2 ms of zstd decompression. It is a plain scalar zigzag loop that also allocates a fresh `std::vector` per plane. It looks like the easiest decode speedup.
2. **The inverse transform is about 20%** for `haar` and the lossy modes, and 24–28% for `laplacian` and `cdf53`. The `cdf53` inverse (1.8 ms vs. 1.25 ms for Haar) now lifts rows like the forward, so `cdf53` crops are only about 11% slower than `haar` crops (25% before).
3. **I/O is only about 3%,** with the file in the page cache. Each L0 crop needs 9–15 `pread` calls for about 190 KiB. On a cold disk or network storage, the number of reads would probably cost more than the decoding itself; that was not measured.
4. **Block size moves decode time more than mode does.**
   - `--cblk 64` decodes about 44 small blocks per L0 crop instead of 20, and reads about 105 KiB instead of 187 KiB: 18% faster.
   - `--cblk 256` reads 2.3× the bytes for the same crop: 62% slower.
5. **The LRU cache** cuts the scenario from 5.4 ms to 3.4 ms.
   - The zoom crops reuse the coarse blocks already decoded: crop 6 drops from 0.64 ms to 0.38 ms (11 of its 20 blocks are hits).
   - The pans reuse 14–17 of their blocks and cost 0.25–0.30 ms.
   - The jump to a new area gains little (0.54 ms, 2 hits).
   - With the cache on, the inverse transform becomes the largest decode step (37%), since it runs on every crop whether or not its blocks were cached.
6. **`naive`** decodes only one level per crop: 9 blocks, no inverse transform. Every pixel then goes through serial MED reconstruction, which takes about half of the decode. In the end it is only 5% faster than `haar` (5.2 ms vs. 5.4 ms), and its file is 29% larger.
7. **`range`** is 13.6× slower to decode: 97% of its time is the adaptive binary coder. For 4.7% smaller files, it is only worth it when storage matters much more than latency.
8. **RGB** costs about 3× gray everywhere. Its YCoCg-R steps are small: 32 ms of the encode, and 2.3 ms of the 19 ms decode.
9. **Decoding is single-threaded**, so none of this depends on `--threads` (see [How it was measured](#how-it-was-measured) for a host effect after 14-thread encodes).

### Encode vs. decode in this scenario

| defaults | 1 thread: ms | share | auto (14 threads): ms | share |
|---|---:|---:|---:|---:|
| load JPEG | 180 | 43% | 180 | 74% |
| encode | 237 | 56% | 58 | 24% |
| open + 10 crops | 5.4 | 1.3% | 5.4 | 2.2% |

Even the slowest decode (`range`, 73 ms for 10 crops) is less than its own encode (1.4 s on one thread, 165 ms on 14). To speed up "encode once, read a few crops":
- keep zstd at level 1 and the default automatic thread count;
- with threads, the remaining costs are the JPEG load (outside the encoder), the serial file write, and the `quantize` division for lossy files.

Decode speed only starts to matter after hundreds of crops per file (about a hundred with threads). Then the wins are a faster unpack, a smaller `--cblk`, and the LRU cache.

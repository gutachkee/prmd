# Pipeline durations

Where does the time go when you encode an image once and then read about 10 crops from it? This document times each step of the encode and decode pipelines, for several modes and options. Every number can be reproduced with `prmdpipeline` and the timers built into the library (see [How it was measured](#how-it-was-measured)).

**Short answer:**
- **Encoding is about 98% of encode + decode** (60% of the total once the JPEG load is included). With the defaults, loading the JPEG takes 180 ms and encoding it 281 ms. Opening the file and decoding all 10 crops take 6.1 ms. The encode alone costs as much as about 470 crops.
- **Encode:** with the default `haar` + zstd 1, half the encode is entropy coding (zstd compression 39%, byte packing 8%). The forward transform takes 19%, and converting the image to gray and to integer planes 19%.
- **Decode:** about 54% of every crop is entropy decoding (zstd decompression 35%, zigzag unpacking 19%). The inverse transform takes 21%. Reading from the file is 3%.
- **What changes it most:**
  - Encode: `--entropy-level 19` makes it 15× slower, `--entropy range` 5×, `cdf53` 4×, and `laplacian` about 2.7×.
  - Decode: `--entropy range` makes it 12× slower, and `rgb` 3×. The LRU cache makes it 40% faster.

## Setup

| | |
|---|---|
| image | `test_image.jpg`: 8824 × 5336 RGB JPEG (47 MP, 13 MiB) |
| pyramid | `--levels 6`, so L5 (276 × 167) is the coarsest level |
| defaults | `haar`, `gray`, zstd level 1, `--cblk 128`, `--tile 256`, lossless, no cache |
| machine | MacBook Pro, Apple Silicon (14 cores, aarch64) under Linux, GCC 15.2 `-O2 -mcpu=native`, zstd 1.5.7, single thread |
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
  build/prmdpipeline test_image.jpg --runs 5                    # totals
  build-profile/prmdpipeline test_image.jpg --runs 5            # breakdown
  build-profile/prmdpipeline test_image.jpg --runs 5 --mode cdf53 --quant 4   # any other row
  ```
  Totals come from `build`, and shares and step times from `build-profile`.
- **Overhead of the timers.** The two builds agree within run-to-run noise: encode within ±8% and the 10 crops within ±5%. The timer build was faster in about half the rows, so the differences are noise, not overhead. A crop runs about 110 timer scopes, an estimated 1% of its time.
- **One host effect to know about.** On this machine an occasional process runs `naive` decode about 25% slower throughout: 7.1–7.3 ms for the 10 crops instead of 5.5–5.9 ms. That happened in about 1 of 6 processes in both builds, pinned to a core or not, and once switched to fast partway through a run. It looks like the virtual CPU being scheduled on a slower core. MED reconstruction is a serial chain of dependent operations and shows it most. The `naive` row uses a rerun in the fast state, and `med_reconstruct` in the defaults moves between 0.40 and 0.70 ms for the same reason.
- **I/O.** The file has just been written, so every read is served from the OS page cache. A cold disk would make `pread` cost more. That was not measured.

## Timeline: defaults (`haar`, gray, zstd 1)

### Encode (once)

Timer build, median of 5 runs. Nested steps are indented and included in their parent. *Share* is of `encode_prmd` (276 ms).

| step | calls | ms | share | what it does |
|---|---:|---:|---:|---|
| `load_image` (libjpeg) | 1 | 180.4 | — | decode the 47 MP RGB JPEG; before the encoder |
| **`encode_prmd`** | 1 | **275.7** | 100% | |
| └ `convert` to gray | 1 | 28.8 | 10% | RGB → Gray8 |
| └ `decompose` | 1 | 77.9 | 28% | |
|  └ `to_planes` | 1 | 24.4 | 9% | uint8 image → int32 plane |
|  └ `forward` | 5 | 53.6 | 19% | Haar: L0 → L1 … L4 → L5 (LL plus 3 bands per level) |
| └ `stored_planes` | 1 | 13.6 | 5% | |
|  └ `copy_bands` | 15 | 7.1 | 3% | copy the bands into their stored layout |
|  └ `quantize` | 15 | 6.5 | 2% | a pass over every sample that changes nothing when lossless |
| └ `encode_blocks` | 1 | 145.2 | 53% | every block, coarsest level first |
|  └ `extract` | 3906 | 12.6 | 5% | copy the block's samples out of the plane |
|  └ `med_residual` | 6 | 0.04 | 0% | only the 6 base (L5) blocks |
|  └ `pack` | 3906 | 22.4 | 8% | int32 → zigzag uint16, split into low and high bytes |
|  └ **`zstd_compress`** | 3906 | **108.9** | **39%** | **the largest single step** |
| └ `serialize` header + index | 1 | 0.00 | 0% | |
| └ `write_file`, 31.6 MiB | 1 | 8.2 | 3% | |
| └ other (geometry, allocation, block loop) | | 2.0 | 1% | |

The build without timers measured 280.9 ms for the same encode.

### Decode (open plus 10 crops)

Build without timers, median of 5 runs. "Blocks" counts the code-blocks decoded on all levels. "KiB read" is the compressed bytes read from the file.

| # | level | step | rect (x, y, w × h) | ms | blocks | KiB read | reads |
|---|---|---|---|---:|---:|---:|---:|
| — | | `open_decoder` (header + index) | | 0.03 | | | |
| 1 | L5 | overview | 20, 0, 256 × 167 | 0.27 | 6 | 31 | 2 |
| 2 | L4 | zoom | 203, 5, 256 × 256 | 0.56 | 13 | 94 | 5 |
| 3 | L3 | zoom | 533, 138, 256 × 256 | 0.51 | 14 | 115 | 6 |
| 4 | L2 | zoom | 1195, 405, 256 × 256 | 0.61 | 16 | 140 | 9 |
| 5 | L1 | zoom | 2519, 939, 256 × 256 | 0.74 | 20 | 184 | 13 |
| 6 | L0 | zoom | 5166, 2006, 256 × 256 | 0.70 | 20 | 187 | 13 |
| 7 | L0 | pan +128 | 5294, 2006, 256 × 256 | 0.67 | 20 | 187 | 13 |
| 8 | L0 | pan +256 | 5422, 2006, 256 × 256 | 0.72 | 22 | 198 | 15 |
| 9 | L0 | pan +256, +128 | 5422, 2134, 256 × 256 | 0.62 | 18 | 159 | 11 |
| 10 | L0 | jump | 3166, 3206, 256 × 256 | 0.63 | 17 | 155 | 9 |
| | | **total** | | **6.03** | 166 | 1451 | 96 |

A crop at L0 costs about 2.5× the overview crop at L5. Each crop decodes a few blocks on every level from L5 down to the level it asks for. Each finer level adds roughly 0.1 ms or less, because a crop needs just a few blocks per level.

**Where the 6 ms goes** (summed over the 10 `decode` calls, timer build, 5.97 ms):

| step | calls | ms | share |
|---|---:|---:|---:|
| **entropy decode** | 166 | **3.25** | **54%** |
| └ `zstd_decompress` | 166 | 2.06 | 35% |
| └ `unpack` (zigzag + byte planes) | 166 | 1.13 | 19% |
| `inverse` Haar | 35 | 1.28 | 21% |
| `med_reconstruct` (L5 base blocks) | 18 | 0.70 | 12% |
| `from_planes` (clamp + pack to uint8) | 10 | 0.29 | 5% |
| `pread` (page cache) | 96 | 0.15 | 3% |
| `copy_blocks` into the band/level buffers | 45 | 0.11 | 2% |
| other (`block_lookup`, wrapping blocks, `std::set`, `shared_ptr`) | | 0.19 | 3% |

## All modes and options

Every row is the same scenario, with the defaults changed only in the first column. *Encode*, *10 crops* and *×def* are from the build without timers. The largest-step columns are shares of `encode_prmd` and of `decode`, from the timer build. Load (about 180 ms) is the same in every row and is not included.

| change from the defaults | file (MiB) | encode (ms) | ×def | 10 crops (ms) | ×def | largest encode steps | largest decode steps |
|---|---:|---:|---:|---:|---:|---|---|
| none (`haar`, gray, zstd 1) | 31.60 | 281 | 1.0 | 6.03 | 1.0 | zstd compress 39%, forward 19% | zstd decompress 35%, inverse 21% |
| `--cblk 64` | 32.45 | 331 | 1.2 | **4.80** | **0.80** | zstd compress 49%, forward 16% | zstd decompress 33%, inverse 26% |
| `--cblk 256` | 31.59 | **255** | **0.91** | 9.80 | 1.6 | zstd compress 34%, forward 22% | zstd decompress 44%, unpack 23% |
| decoder cache 64 MiB | 31.60 | 278 | 1.0 | **3.63** | **0.60** | (same file as defaults) | inverse 34%, zstd decompress 29% |
| `--color rgb` | 65.31 | 840 | 3.0 | 18.53 | 3.1 | zstd compress 48%, forward 19% | zstd decompress 34%, inverse 19% |
| `--mode cdf53` | **30.49** | 1178 | 4.2 | 7.68 | 1.3 | **forward 5/3: 81%**, zstd compress 10% | inverse 36%, zstd decompress 29% |
| `--mode laplacian --down gauss` | 39.30 | 763¹ | 2.7 | 7.65 | 1.3 | Gauss downsample 23%, reconstruct upsample 19%, predict upsample 19%¹ | zstd decompress 37%, unpack 21% |
| `--mode naive` | 40.80 | 334 | 1.2 | 5.64 | 0.94 | zstd compress 40%, MED residual 17% | **MED reconstruct 47%**, zstd decompress 26% |
| `--quant 4` (`haar`) | **19.83** | 437 | 1.6 | 6.36 | 1.1 | zstd compress 36%, quantize 29% | zstd decompress 33%, inverse 19% |
| `--mode laplacian --down gauss --quant 4` | 25.92 | 986¹ | 3.5 | 8.72 | 1.4 | zstd compress 22%, Gauss downsample 18%¹ | zstd decompress 38%, unpack 18% |
| `--entropy range` | 30.12 | 1458 | 5.2 | **70.25** | **11.7** | range coder 90% | range decoder 96% |
| `--entropy-level 19` | 31.52 | **4184** | **14.9** | 6.14 | 1.0 | zstd compress 96% | zstd decompress 35%, inverse 21% |

The `naive` row is a rerun in the fast state described under [How it was measured](#how-it-was-measured); the first run gave 6.85 ms.

¹ Remeasured after the encoder stopped computing `laplacian` residuals twice (finding 4); before, encode took 865 ms lossless and 1090 ms at `--quant 4`. The crop columns are from the original run.

## Findings, by stage

### Encode

1. **zstd compression is the largest step with the defaults** (109 ms, 39%). It grows with how much the coder has to search:
   - Level 19 takes 4.1 s, for a gray file 0.3% smaller (on this full 47 MP image; on the 4096² benchmark crop it is no smaller).
   - `--quant 4` makes compression slower too (163 ms vs. 109 ms), though the output is smaller.
   - Smaller blocks mean more calls: 165 ms at `--cblk 64` (15,492 blocks), 85 ms at `--cblk 256` (1010 blocks).
2. **Loading the JPEG (180 ms)** costs more than any single encode step and is the same for every mode. Converting to gray and to int32 planes adds another 53 ms, 19% of the default encode.
3. **The `cdf53` forward transform takes 952 ms, 18× the Haar forward (54 ms)**, although both do similar arithmetic. Each vertical pass (`Cdf53Transform::forward` in `src/transform.cpp`) copies one column at a time from a row-major buffer about 17 KiB wide at L0. Every sample read then touches a new cache line, which is the likely cause. A row-wise vertical lifting pass, like the one Haar uses, should bring `cdf53` encode close to `haar`.
4. **`laplacian` pays twice before entropy coding.**
   - The Gauss downsample takes 174 ms. It took 279 ms when `decompose` called `LaplacianTransform::forward`, which also computed open-loop residuals that the closed loop then discarded and recomputed; the encoder now only downsamples there (files are byte-identical).
   - The closed loop (`laplacian_residuals`) takes 340 ms lossless and 517 ms at `--quant 4`. It upsamples a full plane twice per level, once to predict (142 ms) and once to reconstruct (146 ms).
5. **Quantizing is slow, and runs even when lossless.**
   - Lossy `haar` spends 131 ms in `quantize`, and lossy `laplacian` 156 ms. `quantize()` divides once per sample by a step only known at run time. Precomputing a reciprocal or a shift would remove most of that.
   - When lossless, the same pass still walks every sample and changes nothing: 6.5 ms, half of `stored_planes`. Skipping it when `quant == 0` is a free 2%.
6. **`naive` spends 61 ms on MED prediction** (`med_residual`, 17%), because every level is stored as pixels. The wavelet modes run MED only on the 6 base blocks (0.04 ms).
7. Writing the file (6–17 ms), serializing the index (< 0.01 ms) and MED on the base level don't matter.

### Decode

1. **Entropy decoding is more than half of every crop with zstd.** The byte-plane unpack is a third of that: 1.1 ms for every 2.1 ms of zstd decompression. It is a plain scalar zigzag loop that also allocates a fresh `std::vector` per plane. It looks like the easiest decode speedup.
2. **The inverse transform is about 20%** for `haar`, `laplacian` and the lossy modes. For `cdf53` it is 36% (2.7 ms vs. 1.3 ms), so `cdf53` crops are about 25% slower than `haar` crops.
3. **I/O is only about 3%,** with the file in the page cache. Each L0 crop needs 9–15 `pread` calls for about 190 KiB. On a cold disk or network storage, the number of reads would probably cost more than the decoding itself; that was not measured.
4. **Block size moves decode time more than mode does.**
   - `--cblk 64` decodes about 44 small blocks per L0 crop instead of 20, and reads about 105 KiB instead of 187 KiB: 20% faster.
   - `--cblk 256` reads 2.3× the bytes for the same crop: 60% slower.
5. **The LRU cache** cuts the scenario from 6.0 ms to 3.6 ms.
   - The zoom crops reuse the coarse blocks already decoded: crop 6 drops from 0.70 ms to 0.40 ms (11 of its 20 blocks are hits).
   - The pans reuse 14–17 of their blocks and cost 0.26–0.31 ms.
   - The jump to a new area gains little (0.58 ms, 2 hits).
   - With the cache on, the inverse transform becomes the largest decode step (34%), since it runs on every crop whether or not its blocks were cached.
6. **`naive`** decodes only one level per crop: 9 blocks, no inverse transform. Every pixel then goes through serial MED reconstruction, which takes 47% of the decode. In the end it is no faster than `haar` (5.6 ms vs. 6.0 ms), and its file is 29% larger.
7. **`range`** is 12× slower to decode: 96% of its time is the adaptive binary coder. For 4.7% smaller files, it is only worth it when storage matters much more than latency.
8. **RGB** costs 3× gray everywhere. Its YCoCg-R steps are small: 31 ms of the encode, and 1.9 ms of the 19 ms decode.

### Encode vs. decode in this scenario

| defaults | ms | share of the total |
|---|---:|---:|
| load JPEG | 180 | 39% |
| encode | 281 | 60% |
| open + 10 crops | 6.1 | 1.3% |

Even the slowest decode (`range`, 70 ms for 10 crops) is less than its own 1.5 s encode. To speed up "encode once, read a few crops", make the encode faster:
- keep zstd at level 1;
- avoid `cdf53` until its vertical pass is fixed;
- remove the separate RGB → gray and uint8 → int32 copies, and skip `quantize` when lossless.

Decode speed only starts to matter after hundreds of crops per file. Then the wins are a faster unpack, a smaller `--cblk`, and the LRU cache.

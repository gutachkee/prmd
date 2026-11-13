# Prmd: random-access crops from a single-file image pyramid

**Question:** can a 2× image pyramid be stored as one file that (a) decodes any `(level, x, y, w, h)` crop without decoding the whole level, and (b) is smaller than the naive "every level stored as independent 256² tiles" layout?

**Answer: yes, on both counts.**
- **Size:** the lossless `.prmd` file holds all 5 levels of a 4096² image in **11.2–11.6 MiB**. The naive layout, using the same entropy coder, needs 15.0 MiB, so `.prmd` is **23–26% smaller**. The whole pyramid costs less than raw level 0 alone (16 MiB).
- **Speed:** a random 256² crop decodes in **0.5–0.7 ms** at any level, reading about 150 KiB.

All numbers come from `prmdbench` (commands in the README): the user's aerial JPEG, converted to gray and center-cropped to 4096², 5 levels, 300 random crops per row, on one Apple-class AArch64 core with the file in the page cache.

## 1. Levels

Level 0 is the full image (4096²). Level L is the image shrunk by 2^L per side: 2048², 1024², 512², 256². One level-L pixel covers a 2^L × 2^L patch of the original. Crop coordinates are in that level's own pixels. For example, `decode(2, 100, 50, 256, 256)` covers the area x = 400…1424 of the original. Odd sizes round up (`ceil(W/2^L)`).

What a level pixel *is* depends on the mode, and the mode is chosen at encode time:

| mode | level L+1 = | stored per level | notes |
|---|---|---|---|
| `haar` (default) | exact 2×2 mean of level L | 3 integer Haar detail bands | non-redundant, no context needed across blocks |
| `cdf53` | 5/3 wavelet lowpass (same as JPEG2000) | 3 detail bands | smoothest levels, best ratio; 1-coefficient margin per level |
| `laplacian` | any downsampler (`box`, `gauss`) | residual vs. upsampled coarse level | 1.33× samples; only mode with a hard per-pixel error bound when lossy |
| `naive` | 2×2 mean, stored independently | full pixels at every level | the baseline from the task |

## 2. Design and format

**Key idea.** Only the coarsest level is stored as pixels. Every finer level is stored as the *detail* that turns level L+1 into level L, so the full pyramid has the same number of samples as level 0, not 1.33×. The decoder reaches a level-L crop like this:
1. It takes the matching patch of the coarsest level.
2. It walks down, adding detail only for the region it needs. Each step halves the region, so ancestor work shrinks geometrically.
3. It stops at level L, so finer levels are never touched.

```
.prmd  ┌ header 64 B: magic, version, mode, W, H, N, color, tile, cblk, quant, entropy id, index CRC
       ├ level table: N × 16 B (size, block grid, first block id)
       ├ block index: u32 offset per block (+1)   ← read once at open, ~6 KB for 4096²
       ├ level N−1 blocks (base pixels, MED-predicted)
       ├ level N−2 … 0 blocks (detail bands / residuals)
       └ blocks are 128² code-blocks of their own level, stored tile-major (a 256² tile = 2×2 contiguous blocks)
```

**Random access.**
- `open()` reads the header and index in one read and checks the CRC.
- A crop maps to a block set with O(1) arithmetic. `block_id` is a closed-form function of (level, tile, block), and missing edge blocks are zero-length index entries. Nothing scans the file.
- Contiguous blocks are fetched with one `pread`.
- Each block is entropy-coded independently, so a crop is seamless by construction: the decoder rebuilds exactly the pixels a full decode would produce.
- Out-of-bounds requests throw `std::out_of_range`. `decode_clamped` / `--clamp` decodes the intersection and returns the actual rect.

**Entropy coding.**
- Coefficients are integers, coded per block with **zstd** (the default) or with a custom context-adaptive binary **range coder**.
- zstd files are 4–5% larger than range-coder files (gray) but decode **~12× faster**: 0.56 ms vs 7.04 ms per 256² L0 crop.
- The crop latency budget matters more than 4% of storage, so zstd is the default. The full decision record is in DESIGN.md §7.1.
- RGB uses the reversible YCoCg-R transform on the coefficients and stays bit-exact.

**Why not just JPEG2000?** It was implemented as a backend (OpenJPEG 2.5.4) and benchmarked rather than assumed. It compresses 3–6% better than `.prmd`+zstd, but it falls short on the two properties the task cares about most:
- **Speed:** crops are 15–25× slower.
- **Access granularity at coarse levels:** J2K tiles are defined at full resolution, so a tile's codestream holds all its resolutions. A 256² crop at L4 reads the **entire file** (10.8 MiB, 513 reads), against 44 KiB for `.prmd`. Larger J2K tiles cut the tile count but read even more per crop.

On an embedded ARM target, `.prmd` needs about 2 kLOC of plain C++17 plus zstd. It uses no FPU or SIMD, the decode memory is bounded by the crop plus about one block per level, and an optional LRU cache of decoded blocks is available.

## 3. Results (gray 4096², 5 levels, lossless)

| codec | file size | vs naive | 256² crop at L0: mean / p95 | 256² at L2 | 256² at L4 | 512² at L0: mean / p95 | bytes read, 256² L0 |
|---|---:|---:|---:|---:|---:|---:|---:|
| **haar** | 11.59 MiB | **−22.9%** | **0.54 / 0.64 ms** | 0.51 ms | 0.28 ms | 1.52 / 1.65 ms | 154 KiB |
| **cdf53** | 11.19 MiB | **−25.6%** | 0.72 / 0.84 ms | 0.64 ms | 0.27 ms | 2.23 / 2.40 ms | 162 KiB |
| laplacian-gauss | 14.29 MiB | −5.0% | 0.72 / 0.86 ms | 0.60 ms | 0.24 ms | 2.00 / 2.14 ms | 181 KiB |
| naive (same coder) | 15.04 MiB | 0 | 0.87 / 0.94 ms | 0.78 ms | 0.34 ms | 2.42 / 2.51 ms | 101 KiB |
| JPEG2000, 256 tiles | 10.84 MiB | −27.9% | 12.65 / 14.31 ms | 8.29 ms | 6.01 ms (reads all 10.8 MiB) | 32.0 / 35.0 ms | 191 KiB |
| raw level 0 / raw pyramid | 16.00 / 21.31 MiB | | | | | | |

- **Per crop:** a 256² crop at L0 touches 4 tiles of its own level. Counting the ancestor work, a haar crop decodes 17.5 code-blocks in 10 coalesced reads, against 8.9 blocks in 4 reads for naive.
- **Haar is still faster than naive.** Its detail blocks skip the serial MED predictor that every naive pixel block runs. The `raw` coder isolates this: 0.37 vs 0.65 ms.
- **Random access pays off.** Decoding the full L0 and then cropping costs 77 ms (haar), which is **140×** more than the crop.
- **Repeated crops:** along a 1000-step navigation-like random walk with a 64 MiB block cache, a haar crop drops from 0.51 ms to 0.17 ms.
- **RGB** (same image, 100 crops per row): haar is 22.9 MiB (−22.8% vs naive) and a 256² L0 crop takes 2.0 ms; J2K is 21.0 MiB at 25 ms.

**Lossy mode** (`--quant Q`):
- The decode path and random access are unchanged.
- The coarsest level stays exact.
- `laplacian` is closed-loop, so every level is within ±Q/2 of the exact pyramid.

| codec | setting | bits/px | PSNR L0 | max err | 256² L0 |
|---|---|---:|---:|---:|---:|
| cdf53 | Q = 4 | 3.41 | 41.9 dB | 11 | 0.74 ms |
| laplacian-gauss | Q = 4 | 4.60 | 46.4 dB | **2** (guaranteed) | 0.79 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 8.0 ms |

J2K wins rate–distortion by about 3 dB, thanks to rate-distortion-optimised truncation and the smoother 9/7 filter. `.prmd` decodes about 10× faster.

## 4. Correctness and proof of random access

There are 198 gtest cases (`ctest`).
- **Exactness:** every crop is compared exactly against a reference pyramid built by independent code in the test, not by the codec's transforms. The crop matrix covers every level, mode and color format, and these shapes:
  - inside a block, inside a tile, across 2 tiles (horizontal and vertical), across 4 tiles
  - every edge and corner, the full level, 1×1
  - odd sizes and tiny images
- **Fuzzing:** seeded random crops.
- **Random access is proven three ways:**
  - The decoded block set equals an independently derived expected set, and `bytes_read` equals the exact sum of those payloads.
  - Every payload *outside* that set is overwritten with random bytes, and the crop stays exact.
  - Negative control: corrupting one block *inside* the set breaks the crop, so the previous check cannot pass vacuously.
- **Also tested:** corrupted headers and indexes, option validation, lossy mode against an independent whole-plane simulation, and J2K crops against J2K's own full decode.

## 5. Trade-offs, limitations, next steps

**Trade-offs.**
- **Fewer bytes stored, more blocks per crop.** A crop decodes its ancestors' blocks too: about 2× naive's block count and 1.5× its bytes read. In return the file is 23–26% smaller and decoding is still faster. The block cache removes the ancestor cost on trajectories.
- **Level definition.** `haar` levels are the exact 2×2 mean but alias. `cdf53` and `laplacian-gauss` levels are smoother, at about 24–30 dB PSNR vs the 2×2 mean (mostly a half-pixel phase offset). The matcher's feature extractor should pick the mode, not the codec.

**Limitations.**
- Only 8-bit Gray and RGB.
- The decoder is single-threaded.
- Lossy haar/cdf53 errors accumulate toward level 0, so there is no per-pixel bound.
- On RGB, zstd costs 12% vs the range coder, which makes J2K the smallest RGB file.

**Next steps.**
- A context-adaptive **rANS** coder or a per-file zstd dictionary, to recover the 4–12% while keeping zstd-class speed.
- Decoding the blocks of one level in parallel.
- NEON inverse transforms.
- `mmap` I/O.
- A dead-zone quantizer with rate-distortion-weighted steps per level, to close the lossy gap to J2K.
- Optional storage of an extra "decimated" phase, so `cdf53` levels align to the pixel grid.

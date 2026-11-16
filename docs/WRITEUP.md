# Prmd: random-access crops from a single-file image pyramid

`prmd` stores an image and its 2× pyramid in **one file** and decodes any `(level, x, y, w, h)` crop by reading and decoding only the blocks that crop depends on. A lossless 4096² gray image with 5 levels takes **11.2–11.6 MiB**. That's **23–26% smaller** than storing every level as independent 256² tiles (15.0 MiB, same entropy coder) and smaller than raw level 0 alone (16 MiB). A random 256² crop decodes in **0.3–0.6 ms** at any level.

Modes and their options are explained with worked examples in [modes.md](modes.md). Interfaces and full design details are in [DESIGN.md](DESIGN.md).

## 1. The `prmd` tool

One C++17 library with three executables: `prmd` (encode, decode a crop, inspect a file), `prmdbench` (the benchmark) and `prmd_tests`. It depends on libjpeg, libpng, zstd and OpenJPEG; OpenJPEG is used only for the JPEG2000 comparison backend.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build                                    # 198 tests

build/prmd encode photo.jpg img.prmd --levels 5           # JPEG/PNG in; gray by default
build/prmd decode img.prmd --level 2 --x 100 --y 50 --w 256 --h 256 -o crop.png --stats
build/prmd info img.prmd                                  # levels, blocks, bytes per level
```

- **Encode options:** `--mode haar|cdf53|laplacian|naive` (what levels are and what's stored; see §2), `--color gray|rgb`, `--quant Q` (lossy), `--entropy zstd|range|raw`, `--tile`, `--cblk`, `--down box|gauss`. `--codec j2k` writes a JPEG2000 file instead, for comparison.
- **Decode:** coordinates are in the chosen level's own pixels. A request that goes outside the level is rejected with exit code 4; `--clamp` decodes the part that's inside instead. `--stats` prints tiles touched, blocks decoded, bytes read and read calls.
- **Library:** `open_decoder(path)` returns a decoder for `.prmd` or `.j2k` files (picked by the file's magic bytes), and `decode(level, rect)` returns the pixels plus the same stats. An optional LRU cache of decoded blocks speeds up repeated, overlapping crops.

## 2. Design

**Key idea.** Only the coarsest level is stored as pixels. Each finer level L is stored as the *detail* that turns level L+1 back into level L. With a wavelet, each 2×2 block of pixels becomes 1 coarse pixel plus 3 detail numbers, so the whole pyramid costs as many samples as level 0 alone, not 1.33×. To decode a level-L crop, the decoder takes the matching patch of the coarsest level and walks down, adding detail only for the region it needs. The region halves at each step up, so the work on coarser levels shrinks geometrically, and levels finer than L are never read.

**Modes.** The mode is chosen at encode time and sets what a level is:

| mode | level L+1 is | stored for each finer level |
|---|---|---|
| `haar` (default) | exact integer 2×2 mean | 3 integer Haar detail bands (1.0× samples) |
| `cdf53` | 5/3 wavelet lowpass, as in JPEG2000; sharper | 3 detail bands (1.0×) |
| `laplacian` | `box` (2×2 mean) or `gauss` downsample | residual vs. the upsampled coarser level (~1.33×) |
| `naive` | `box` or `gauss` downsample | every level as plain pixels (~1.33×), the task's baseline |

**Random access.** Opening a file reads the header and the block index in one read. A crop maps to its blocks by closed-form arithmetic on (level, tile, block), so nothing scans the file, and runs of blocks that sit next to each other on disk are fetched with one `pread`. Each block is entropy-coded on its own, so a crop reproduces exactly what a full decode would, and crops are seamless by construction.

The tests prove this. Every crop (inside a tile, across 2 and 4 tiles, edges, corners, the full level) is compared bit for bit with a reference pyramid built by separate code. Every block a crop shouldn't need is overwritten with random bytes, and the crop still decodes exactly.

**Entropy coding.** Each block's numbers are coded with zstd by default. A custom context-adaptive range coder makes files 4–5% smaller but decodes about 12× slower, and crop latency matters more than 4% of storage here.

**Color and lossy.** RGB uses the reversible YCoCg-R transform and stays bit-exact. `--quant Q` quantizes the detail or residual numbers. The decode path, and so random access, stays the same, and the coarsest level stays exact.

**Why not JPEG2000?** It's built in as a backend (OpenJPEG 2.5.4) and benchmarked. It compresses about 6% better, but crops decode **15–25× slower**. Its access granularity is also poor at coarse levels: JPEG2000 tiles are defined at full resolution and each holds all its resolutions, so a 256² crop at L4 reads the **whole file** (10.8 MiB, against 44 KiB for `.prmd`).

**Embedded ARM.** About 2 kLOC of plain C++17 plus zstd, integer arithmetic only, no SIMD required. Decode memory is bounded by the crop plus about one block per level.

## 3. Format layout

All integers are little-endian.

```
offset 0   ┌───────────────────────────────────────────────┐
           │ header, 64 B                                  │
           ├───────────────────────────────────────────────┤
           │ level table: N × 16 B                         │  level 0 … N−1
           ├───────────────────────────────────────────────┤
           │ block index: u32 offset × (blocks + 1)        │  ~5.5 KB for 4096², 5 levels
           ├───────────────────────────────────────────────┤ ← data_offset
           │ level N−1 blocks: pixels, MED-predicted       │  coarsest level first
           │ level N−2 blocks: detail bands or residuals   │
           │ …                                             │
           │ level 0 blocks                                │
           └───────────────────────────────────────────────┘
```

| bytes | header field |
|---|---|
| 0–3 | magic `PRMD` |
| 4–5 | version (1) |
| 6–7 | mode, downsampler |
| 8–15 | width, height of level 0 |
| 16–17 | number of levels N, color format |
| 18–23 | tile size, code-block size, quantizer step Q |
| 24–27 | total number of blocks |
| 28–29 | entropy coder, its level (informational) |
| 32–39 | `data_offset`, where block data starts |
| 30–31, 40–63 | reserved, zero |

- **Level entry:** width, height, block grid (columns × rows) and the id of the level's first block.
- **Blocks:** each level is cut into 128² code-blocks of its own pixels, the unit that's decoded, read and cached. Blocks are stored tile by tile, so a 256² tile's 2×2 blocks are next to each other on disk. A block's id is a formula of (level, tile, position in tile), and block *b* is the bytes from `offset[b]` to `offset[b+1]`. Edge tiles keep ids for their missing blocks, with zero length, so the formula stays simple.
- **Block payload:** the coder's output, with no per-block header. A block's size and contents follow from the level table and the header. All channels of an area are in the same block, so RGB needs no extra reads.
- **Validation on open:** header fields in range, level table equal to the geometry derived from the header, offsets starting at 0 and increasing, and file size equal to `data_offset` plus the last offset. There's no checksum (see §5).

## 4. Trade-offs

All numbers come from `prmdbench` (commands in the README): the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd, 300 random crops per row, one Apple AArch64 core, file in the page cache, run on 2026-11-16. Sizes are deterministic. Latencies vary by up to about 10% between runs, so compare them within one table rather than across runs.

**Fewer bytes stored, more blocks per crop.** A crop also decodes the blocks of the coarser levels it's built from.

| codec | file size | vs raw L0 (16.00 MiB) | vs raw pyramid (21.31 MiB) | vs naive |
|---|---:|---:|---:|---:|
| **haar** | 11.59 MiB | 1.38× smaller | 1.84× smaller | **−22.9%** |
| **cdf53** | 11.19 MiB | 1.43× smaller | 1.91× smaller | **−25.6%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | 1.49× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 1.42× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | 1.97× smaller | −27.9% |

Per-crop latency and I/O. *Tiles* counts the distinct 256² tiles touched on every level a crop depends on, so for `.prmd` a 256² crop at L0 touches 4 tiles of its own level and 9.5 in total. *Bytes* is the block data read, not counting the index.

| crop | level | haar mean / p95 | tiles · bytes | naive mean / p95 | tiles · bytes | J2K mean / p95 | tiles · bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 0.58 / 0.70 ms | 9.5 · 154 KiB | 0.57 / 0.59 ms | 4 · 101 KiB | 13.5 / 15.3 ms | 4 · 191 KiB |
| 256² | L1 | 0.52 / 0.59 ms | 8.2 · 143 KiB | 0.55 / 0.58 ms | 4 · 103 KiB | 12.3 / 13.1 ms | 9 · 433 KiB |
| 256² | L2 | 0.49 / 0.61 ms | 6.8 · 129 KiB | 0.55 / 0.56 ms | 4 · 99 KiB | 8.7 / 9.2 ms | 25 · 1.2 MiB |
| 256² | L3 | 0.51 / 0.53 ms | 5.0 · 119 KiB | 0.53 / 0.57 ms | 4 · 99 KiB | 7.6 / 8.3 ms | 81 · 3.8 MiB |
| 256² | L4 (whole level) | 0.26 / 0.26 ms | 1.0 · 44 KiB | 0.25 / 0.27 ms | 1 · 44 KiB | 6.3 / 6.9 ms | 256 · 10.8 MiB |
| 512² | L0 | 1.64 / 1.82 ms | 17.4 · 362 KiB | 1.67 / 1.97 ms | 9 · 282 KiB | 35.4 / 39.3 ms | 9 · 429 KiB |
| 512² | L1 | 1.55 / 1.71 ms | 15.7 · 346 KiB | 1.59 / 1.81 ms | 9 · 286 KiB | 35.1 / 38.0 ms | 25 · 1.2 MiB |
| 512² | L2 | 1.53 / 1.64 ms | 14.0 · 331 KiB | 1.59 / 1.95 ms | 9 · 276 KiB | 29.9 / 33.3 ms | 81 · 3.8 MiB |
| 512² | L3 (whole level) | 1.10 / 1.18 ms | 5.0 · 179 KiB | 1.03 / 1.12 ms | 4 · 176 KiB | 24.4 / 26.7 ms | 256 · 10.8 MiB |

A `haar` crop decodes about 2× `naive`'s blocks and reads 1.5× its bytes, in exchange for a 23% smaller file, and is about as fast. `haar`'s detail blocks need only a few adds and shifts per pixel. `naive`'s pixel blocks run the MED predictor, which goes pixel by pixel but was made branch-free (2.4× faster), so the two cost about the same overall. Decoding the whole of L0 and then cropping would cost 84 ms, about **145×** a crop. On a 1000-step random walk, a 64 MiB block cache brings a `haar` crop down to 0.19 ms.

**Encode and decode time per mode.** Encode covers building and writing the whole 5-level pyramid of the 4096² image. Decode is a random crop at L0, or the whole of L0 or L1 at once.

| mode | encode | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 1.18 s | 0.58 / 0.70 ms | 1.64 / 1.82 ms | 84 ms | 19.5 ms |
| `cdf53` | 1.42 s | 0.75 / 0.86 ms | 2.42 / 2.60 ms | 267 ms | 45.9 ms |
| `laplacian-box` | 1.61 s | 0.77 / 0.87 ms | 2.27 / 2.45 ms | 119 ms | 27.1 ms |
| `laplacian-gauss` | 1.66 s | 0.75 / 0.85 ms | 2.18 / 2.44 ms | 110 ms | 24.8 ms |
| `naive` | 1.58 s | 0.57 / 0.59 ms | 1.67 / 1.97 ms | 78 ms | 18.5 ms |
| JPEG2000, 256² tiles | 1.38 s | 13.5 / 15.3 ms | 35.4 / 39.3 ms | 1294 ms | 348 ms |

Encode times are close across modes, and zstd at its default level 19 is most of the cost: on RGB, `haar` takes 9.5 s to encode with zstd but 1.2 s with the range coder. With `--entropy-level 3`, gray `haar` encodes in 95 ms instead of 1.23 s, and the file is only 0.1% larger. `cdf53`'s full-level decode is 3× `haar`'s, while its crops are only about 1.3× slower.

**Speed vs. size of the entropy coder.** zstd: 11.59 MiB at 0.61 ms per 256² crop. Range coder: 11.13 MiB at 7.2 ms. zstd is the default.

**Level definition.** `haar` levels are the exact 2×2 mean, but they alias. `cdf53` and `laplacian-gauss` levels are sharper or smoother and differ from the 2×2 mean by 24–30 dB PSNR, mostly a half-pixel shift. The image matcher's needs should decide the mode, not the codec. [modes.md](modes.md) has side-by-side samples.

**Lossy** (`--quant Q`): max error is in 8-bit pixel values (0–255).

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|
| haar | Q = 4 | 3.56 | 38.6 dB | 12 | 0.62 ms |
| cdf53 | Q = 4 | 3.41 | 41.9 dB | 11 | 0.81 ms |
| laplacian-gauss | Q = 4 | 4.60 | 46.4 dB | **2** (±Q/2, guaranteed) | 0.83 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 8.9 ms |

`haar` and `cdf53` quantize open loop, so errors pile up toward level 0. `laplacian` is closed loop and keeps every level within ±Q/2, at the cost of a larger file. JPEG2000 gets about 3 dB more for the same size, thanks to rate-distortion-optimised truncation and the smoother 9/7 filter, but `.prmd` decodes 10–14× faster.

**RGB.** `haar` is 22.9 MiB (−22.8% vs `naive`) at 2.1 ms per 256² crop. JPEG2000 is 21.0 MiB at 30 ms.

## 5. Limitations

- Only 8-bit gray and RGB.
- The decoder is single-threaded.
- 32-bit block offsets cap a file at 4 GiB.
- No checksums: a damaged block can decode to wrong pixels without an error, so integrity is left to the storage layer.
- Lossy `haar` and `cdf53` have no per-pixel error bound.
- On RGB, zstd costs 12% vs. the range coder, which makes JPEG2000 the smallest RGB file.

## 6. Next steps

- An adaptive rANS coder or a per-file zstd dictionary, to recover the 4–12% at zstd speed.
- Decode the blocks of one level in parallel.
- NEON versions of the inverse transforms.
- `mmap` I/O.
- Rate-distortion-weighted quantizer steps per level, to close the lossy gap to JPEG2000.

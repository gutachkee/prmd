# Prmd: random-access crops from a single-file image pyramid

`prmd` stores an image and its 2× pyramid in **one file** and decodes any `(level, x, y, w, h)` crop by reading and decoding only the blocks that crop depends on. A lossless 4096² gray image with 5 levels takes **11.2–11.6 MiB**. That's **23–26% smaller** than storing every level as independent 256² tiles (15.0 MiB, same entropy coder) and smaller than raw level 0 alone (16 MiB). On one core of a MacBook Pro (Apple Silicon), a random 256² crop decodes in **0.3–0.6 ms** at any level, and the whole pyramid encodes in **85 ms**.

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

**Entropy coding.** Each block's numbers are coded with zstd, by default at level 1, the fastest to encode (§4 has the trade-off against level 19). A custom context-adaptive range coder makes files 4–5% smaller but decodes about 12× slower, and crop latency matters more than 4% of storage here.

**Color and lossy.** RGB uses the reversible YCoCg-R transform and stays bit-exact. `--quant Q` quantizes the detail or residual numbers. The decode path, and so random access, stays the same, and the coarsest level stays exact.

**Why not JPEG2000?** It's built in as a backend (OpenJPEG 2.5.4) and benchmarked. It compresses about 6% better, but crops decode **13–24× slower** and encoding is 17× slower. Its access granularity is also poor at coarse levels: JPEG2000 tiles are defined at full resolution and each holds all its resolutions, so a 256² crop at L4 reads the **whole file** (10.8 MiB, against 44 KiB for `.prmd`).

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

All numbers come from `prmdbench` (commands in the README): the aerial test JPEG converted to gray and center-cropped to 4096², 5 levels, lossless, zstd at the default level 1, 300 random crops per row, one core of a MacBook Pro's Apple Silicon processor (14 cores, Linux, AArch64), GCC 15.2 `-O2 -mcpu=native`, file in the page cache, run on 2026-11-16. Sizes are deterministic. Latencies vary by up to about 10% between runs, so compare them within one table rather than across runs. All times in this document are on that MacBook Pro. The same benchmark on a Raspberry Pi 5 is in [Pi5_stats.md](Pi5_stats.md).

**Fewer bytes stored, more blocks per crop.** A crop also decodes the blocks of the coarser levels it's built from.

| codec | file size | vs raw L0 (16.00 MiB) | vs raw pyramid (21.31 MiB) | vs naive |
|---|---:|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | 1.84× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | 1.91× smaller | **−25.7%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | 1.49× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 1.42× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | 1.97× smaller | −27.9% |

Per-crop latency and I/O. *Tiles* counts the distinct 256² tiles touched on every level a crop depends on, so for `.prmd` a 256² crop at L0 touches 4 tiles of its own level and 9.5 in total. *Bytes* is the block data read, not counting the index.

| crop | level | haar mean / p95 | tiles · bytes | naive mean / p95 | tiles · bytes | J2K mean / p95 | tiles · bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 0.64 / 0.76 ms | 9.5 · 154 KiB | 0.60 / 0.73 ms | 4 · 101 KiB | 14.1 / 16.1 ms | 4 · 191 KiB |
| 256² | L1 | 0.60 / 0.70 ms | 8.2 · 143 KiB | 0.57 / 0.63 ms | 4 · 103 KiB | 13.1 / 14.4 ms | 9 · 433 KiB |
| 256² | L2 | 0.58 / 0.75 ms | 6.8 · 129 KiB | 0.56 / 0.61 ms | 4 · 99 KiB | 9.4 / 10.5 ms | 25 · 1.2 MiB |
| 256² | L3 | 0.63 / 0.70 ms | 5.0 · 119 KiB | 0.57 / 0.71 ms | 4 · 99 KiB | 8.1 / 9.0 ms | 81 · 3.8 MiB |
| 256² | L4 (whole level) | 0.30 / 0.36 ms | 1.0 · 44 KiB | 0.30 / 0.36 ms | 1 · 44 KiB | 7.1 / 8.3 ms | 256 · 10.8 MiB |
| 512² | L0 | 1.71 / 1.90 ms | 17.4 · 362 KiB | 2.03 / 2.29 ms | 9 · 282 KiB | 38.0 / 42.3 ms | 9 · 429 KiB |
| 512² | L1 | 1.65 / 1.81 ms | 15.7 · 346 KiB | 1.97 / 2.24 ms | 9 · 286 KiB | 36.8 / 39.1 ms | 25 · 1.2 MiB |
| 512² | L2 | 1.70 / 1.81 ms | 14.0 · 331 KiB | 2.00 / 2.20 ms | 9 · 276 KiB | 30.4 / 31.7 ms | 81 · 3.8 MiB |
| 512² | L3 (whole level) | 1.19 / 1.37 ms | 5.0 · 179 KiB | 1.32 / 1.50 ms | 4 · 176 KiB | 25.2 / 25.6 ms | 256 · 10.8 MiB |

A `haar` crop decodes about 2× `naive`'s blocks and reads 1.3–1.5× its bytes, in exchange for a 23% smaller file. At 256² it is within 10% of `naive`'s speed, and at 512² it was 13–17% faster in this run. `haar`'s detail blocks need only a few adds and shifts per pixel, while `naive`'s pixel blocks run the MED predictor, which goes pixel by pixel. Decoding the whole of L0 and then cropping would cost 84 ms, about **130×** a crop. On a 1000-step random walk, a 64 MiB block cache brings a `haar` crop down to 0.19 ms.

**Encode and decode time per mode.** Encode covers building and writing the whole 5-level pyramid of the 4096² image. Decode is a random crop at L0, or the whole of L0 or L1 at once.

| mode | encode | 256² crop at L0: mean / p95 | 512² crop at L0: mean / p95 | full L0 | full L1 |
|---|---:|---:|---:|---:|---:|
| `haar` | 85 ms | 0.64 / 0.76 ms | 1.71 / 1.90 ms | 84 ms | 19.7 ms |
| `cdf53` | 332 ms | 0.83 / 0.97 ms | 2.59 / 2.94 ms | 261 ms | 46.8 ms |
| `laplacian-box` | 282 ms | 0.83 / 0.95 ms | 2.33 / 2.55 ms | 116 ms | 26.9 ms |
| `laplacian-gauss` | 299 ms | 0.77 / 0.92 ms | 2.17 / 2.41 ms | 106 ms | 24.2 ms |
| `naive` | 104 ms | 0.60 / 0.73 ms | 2.03 / 2.29 ms | 97 ms | 24.0 ms |
| JPEG2000, 256² tiles | 1453 ms | 14.1 / 16.1 ms | 38.0 / 42.3 ms | 1376 ms | 373 ms |

At zstd level 1, most of the encode time is the transform, so `haar` and `naive` encode about 3× faster than `cdf53` and `laplacian`. `cdf53`'s full-level decode is 3× `haar`'s, while its crops are only about 1.3–1.5× slower.

**Encode speed vs. size: the zstd level.** Level 1 is the default. Level 19 encodes 4–34× slower (gray `haar`: 1.24 s instead of 85 ms; RGB `haar`: 9.2 s instead of 0.30 s) and decodes at the same speed. It makes gray lossless files no smaller, but RGB files 2.7–4.1% smaller and lossy files 3–12% smaller (the lossy figure is from an earlier run; the benchmark has no lossy zstd-19 row). Pass `--entropy-level 19` when size matters more than encode time.

**Speed vs. size of the entropy coder.** zstd: 11.58 MiB at 0.62 ms per 256² crop. Range coder: 11.13 MiB at 7.6 ms. zstd is the default.

**Level definition.** `haar` levels are the exact 2×2 mean, but they alias. `cdf53` and `laplacian-gauss` levels are sharper or smoother and differ from the 2×2 mean by 24–30 dB PSNR, mostly a half-pixel shift. The image matcher's needs should decide the mode, not the codec. [modes.md](modes.md) has side-by-side samples.

**Lossy** (`--quant Q`): max error is in 8-bit pixel values (0–255). Bits per pixel are at the default zstd level 1.

| codec | setting | bits/px | PSNR L0 | max err | 256² at L0 |
|---|---|---:|---:|---:|---:|
| haar | Q = 4 | 3.66 | 38.6 dB | 12 | 0.69 ms |
| cdf53 | Q = 4 | 3.53 | 41.9 dB | 11 | 0.87 ms |
| laplacian-gauss | Q = 4 | 4.72 | 46.4 dB | **2** (±Q/2, guaranteed) | 0.89 ms |
| JPEG2000 9/7 | 45 dB target | 3.14 | 44.7 dB | 10 | 9.1 ms |

`haar` and `cdf53` quantize open loop, so errors pile up toward level 0. `laplacian` is closed loop and keeps every level within ±Q/2, at the cost of a larger file. JPEG2000 wins rate–distortion: 44.7 dB at 3.14 bits/px, where `cdf53` needs 3.53 bits/px for 41.9 dB. It gets there through rate-distortion-optimised truncation and the smoother 9/7 filter. `.prmd` decodes 10–13× faster.

**RGB** (100 crops per row). `haar` is 23.9 MiB (−22.5% vs `naive`) at 2.0 ms per 256² crop, or 22.9 MiB with `--entropy-level 19`. JPEG2000 is 21.0 MiB at 28 ms.

## 5. Limitations

- Only 8-bit gray and RGB.
- The decoder is single-threaded.
- 32-bit block offsets cap a file at 4 GiB.
- No checksums: a damaged block can decode to wrong pixels without an error, so integrity is left to the storage layer.
- Lossy `haar` and `cdf53` have no per-pixel error bound.
- On RGB, zstd files are 12–17% larger than with the range coder (12% at level 19, 17% at level 1), which makes JPEG2000 the smallest RGB file.

## 6. Next steps

- An adaptive rANS coder or a per-file zstd dictionary, to recover the 4–17% at zstd speed.
- Encode and decode the blocks of one level in parallel; blocks are independent.
- NEON versions of the transforms.
- `mmap` I/O.
- Rate-distortion-weighted quantizer steps per level, to close the lossy gap to JPEG2000.

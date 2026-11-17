# Prmd: random-access crops from a single-file image pyramid

`prmd` stores an 8-bit gray (or RGB) image and its N-level 2× pyramid in **one file** and decodes any `(level, x, y, w, h)` crop by reading and decoding only the blocks that crop depends on. A lossless 4096² gray image with 5 levels takes **11.2–11.6 MiB**: **23–26% smaller** than storing every level as independent 256² tiles (15.0 MiB, same entropy coder) and smaller than raw level 0 alone (16 MiB). On one core of a MacBook Pro (Apple Silicon), a random 256² crop decodes in **0.24–0.56 ms** at any level.

The [README](../README.md) has the build, the CLI (`prmd encode`, `prmd decode … -o crop.png`), the C++ API and the 885 tests; [design.md](design.md) and [benchmarks.md](benchmarks.md) the details.

## 1. Design

**Key idea.** Only the coarsest level is stored as pixels. Each finer level L is stored as the *detail* that turns level L+1 back into level L. With a wavelet, each 2×2 block of pixels becomes 1 coarse pixel plus 3 detail numbers, so the pyramid costs as many samples as level 0 alone, not 1.33×. A level-L crop starts from the matching patch of the coarsest level and walks down, adding detail only where needed; the patch halves at each step up, and levels finer than L are never read.

**Modes** set what a level is: `haar` (default) makes level L+1 the exact integer 2×2 mean and stores 3 Haar detail bands; `cdf53` uses the sharper JPEG2000 5/3 lowpass; `laplacian` stores residuals against any downsampler (~1.33× samples); `naive` stores every level as pixels, the task's baseline.

**Random access.** Opening a file reads the 64-byte header, then the level table and block index. A crop maps to its blocks by arithmetic on (level, tile, block), so nothing scans the file; adjacent blocks are fetched with one `pread`. Each block is entropy-coded on its own, so a crop equals the same region of a full decode and is seamless by construction. Tests compare crops inside one tile, across 2 and 4 tiles, on edges and corners, and full levels bit for bit with a reference pyramid built from the source by separate code; overwriting every block a crop shouldn't need with random bytes leaves it unchanged. **Out-of-bounds requests are rejected**: `decode()` throws `std::out_of_range` and the CLI exits with code 4; `decode_clamped()` (`--clamp`) decodes the intersection instead.

**Coding.** Each block is coded with zstd at level 1 (a custom range coder is optional). RGB uses the reversible YCoCg-R transform and stays bit-exact. `--quant Q` quantizes the detail or residual numbers without changing the decode path.

## 2. Format layout

All integers are little-endian.

```
offset 0   ┌───────────────────────────────────────────────┐
           │ header, 64 B                                  │  magic, version, mode, size, N, color,
           ├───────────────────────────────────────────────┤  tile/cblk, Q, block count, coder, data_offset
           │ level table: N × 16 B                         │  size, block grid, first block id per level
           ├───────────────────────────────────────────────┤
           │ block index: u32 offset × (blocks + 1)        │  ~5.5 KB for 4096², 5 levels
           ├───────────────────────────────────────────────┤ ← data_offset
           │ level N−1 blocks: pixels, MED-predicted       │  coarsest level first
           │ level N−2 … level 0 blocks: detail/residuals  │
           └───────────────────────────────────────────────┘
```

Each level is cut into 128² code-blocks (the unit decoded, read and cached), stored tile by tile so a 256² tile's 2×2 blocks are adjacent on disk. Block *b* is the bytes from `offset[b]` to `offset[b+1]`; all channels share one block. Everything is validated before allocation ([design.md §3](design.md)).

## 3. Results and trade-offs

Gray 4096², 5 levels, lossless, zstd 1, 300 random crops per row, one core, file in the page cache.

| codec | file size | vs raw L0 (16.00 MiB) | vs naive |
|---|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | **−25.7%** |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | −27.9% |

Mean / p95 latency, then tiles touched and block bytes read per crop (*tiles*: distinct 256² tiles on every level the crop depends on; bytes exclude the index):

| crop | level | haar | naive | JPEG2000 | tiles: haar · naive · J2K | bytes: haar · naive · J2K |
|---|---:|---:|---:|---:|---:|---:|
| 256² | L0 | 0.56 / 0.65 ms | 0.57 / 0.70 ms | 13.5 / 15.3 ms | 9.5 · 4 · 4 | 154 KiB · 101 KiB · 191 KiB |
| 256² | L2 | 0.50 / 0.62 ms | 0.49 / 0.52 ms | 8.9 / 9.9 ms | 6.8 · 4 · 25 | 129 KiB · 99 KiB · 1.2 MiB |
| 256² | L4 (whole level) | 0.24 / 0.27 ms | 0.23 / 0.24 ms | 6.5 / 7.1 ms | 1 · 1 · 256 | 44 KiB · 44 KiB · 10.8 MiB |
| 512² | L0 | 1.64 / 1.96 ms | 1.53 / 1.85 ms | 35.3 / 40.1 ms | 17.4 · 9 · 9 | 362 KiB · 282 KiB · 429 KiB |
| 512² | L3 (whole level) | 1.08 / 1.10 ms | 0.96 / 1.08 ms | 24.1 / 25.7 ms | 5 · 4 · 256 | 179 KiB · 176 KiB · 10.8 MiB |

**Compression vs. access granularity.** A `haar` crop also decodes the coarser blocks it's built from (about 2× `naive`'s blocks, 1.0–1.5× its bytes) for a 23% smaller file, yet is about as fast (within 6% at 256², 5–11% slower at 512²): detail blocks need only adds and shifts, while `naive`'s pixel blocks run the serial MED predictor. Decoding all of L0 and then cropping costs 82 ms, about **145×** a crop; a 64 MiB block cache brings a crop on a random walk down to 0.19 ms.

**Decode speed vs. size.** The range coder would save about 4% at 12× the decode time, and zstd level 19 saves nothing on gray lossless files; crop latency matters more here. Encoding is multithreaded with a byte-identical file: 4096² gray takes 72 → 20 ms on 14 cores, 219 → 96 ms on a 4-core Pi 5.

**Level definition and PSNR.** `haar` levels are the exact 2×2 mean but alias; `cdf53` is sharper (24–30 dB from the 2×2 mean: a different filter, not lower quality), so the matcher should pick the mode. Lossless files decode bit for bit. With `--quant 4`, `cdf53` gives 41.9 dB at 3.53 bits/px; only `laplacian` bounds the per-pixel error (±Q/2 on gray). Tables: [benchmarks.md](benchmarks.md#lossy).

**Why not JPEG2000?** It's built in (OpenJPEG 2.5.4) and benchmarked: it compresses about 6% better and wins lossy rate–distortion (44.7 dB at 3.14 bits/px), but crops decode **15–27× slower** and encoding is 19× slower on one thread. Its tiles are defined at full resolution and hold all their resolutions, so a 256² crop at L4 reads the **whole file** (10.8 MiB against 44 KiB), with every layout we tried ([design.md §8](design.md)).

**Embedded ARM.** About 4.8k lines of plain C++17 (non-blank, non-comment), plus zstd, which every embedded Linux ships. Integer arithmetic only, no SIMD required. Decode memory is O(crop area): `prmd decode` of a 512² L0 crop of the 4096² file peaks at about 8 MB RSS for the whole process (4.4 MB for `prmd info` alone). On a Raspberry Pi 5 a 256² crop takes 0.7–1.4 ms, 11–20× faster than JPEG2000.

## 4. Limitations

- Only 8-bit gray and RGB. The decoder is single-threaded.
- 32-bit block offsets cap a file at 4 GiB.
- No checksums: a damaged block can decode to wrong pixels silently.
- Lossy `haar` and `cdf53` have no per-pixel error bound, and JPEG2000 beats every mode on lossy rate–distortion.
- On RGB, zstd files are 12–17% larger than range-coded ones, so JPEG2000 beats the default `.prmd` RGB file (`haar+range` is smallest).

## 5. Next steps

- An adaptive rANS coder or a per-file zstd dictionary, to recover the 4–17% at zstd speed.
- Decode the blocks of one level in parallel, as the encoder does; blocks are independent.
- NEON versions of the transforms; `mmap` I/O.
- Rate-distortion-weighted quantizer steps per level, to close the lossy gap to JPEG2000.

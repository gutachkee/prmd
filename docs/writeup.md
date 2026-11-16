# Prmd: random-access crops from a single-file image pyramid

`prmd` stores an image and its 2× pyramid in **one file** and decodes any `(level, x, y, w, h)` crop by reading and decoding only the blocks that crop depends on. A lossless 4096² gray image with 5 levels takes **11.2–11.6 MiB**. That's **23–26% smaller** than storing every level as independent 256² tiles (15.0 MiB, same entropy coder) and smaller than raw level 0 alone (16 MiB). On one core of a MacBook Pro (Apple Silicon), a random 256² crop decodes in **0.25–0.6 ms** at any level, and the whole pyramid encodes in **85 ms**.

Build, CLI and API: [README](../README.md) (222 tests). Modes with worked examples: [modes.md](modes.md). Interfaces and design details: [design.md](design.md). Full benchmark tables (every mode, lossy, RGB): [benchmarks.md](benchmarks.md); Raspberry Pi 5: [benchmarks-pi5.md](benchmarks-pi5.md).

## 1. Design

**Key idea.** Only the coarsest level is stored as pixels. Each finer level L is stored as the *detail* that turns level L+1 back into level L. With a wavelet, each 2×2 block of pixels becomes 1 coarse pixel plus 3 detail numbers, so the whole pyramid costs as many samples as level 0 alone, not 1.33×. To decode a level-L crop, the decoder takes the matching patch of the coarsest level and walks down, adding detail only for the region it needs. The region halves at each step up, so the work on coarser levels shrinks geometrically, and levels finer than L are never read.

**Modes** (chosen at encode time; they set what a level is): `haar` (default) makes level L+1 the exact integer 2×2 mean and stores 3 Haar detail bands (1.0× samples); `cdf53` uses the sharper JPEG2000 5/3 lowpass (1.0×); `laplacian` takes any `box` or `gauss` downsample and stores the residual against the upsampled coarser level (~1.33×); `naive` stores every level as plain pixels (~1.33×), the task's baseline.

**Random access.** Opening a file takes two reads: the 64-byte header, then the level table and block index together. A crop maps to its blocks by closed-form arithmetic on (level, tile, block), so nothing scans the file, and runs of adjacent blocks are fetched with one `pread`. Each block is entropy-coded on its own, so a crop reproduces exactly what a full decode would and is seamless by construction. The tests prove this: every crop (inside a tile, across 2 and 4 tiles, edges, corners, the full level) is compared bit for bit with a reference pyramid built by separate code, and every block a crop shouldn't need is overwritten with random bytes without changing the result.

**Entropy coding, color, lossy.** Each block is coded with zstd at level 1. A custom context-adaptive range coder makes files 4–5% smaller but decodes about 12× slower. RGB uses the reversible YCoCg-R transform and stays bit-exact. `--quant Q` quantizes the detail or residual numbers. The decode path, and so random access, stays the same; `laplacian` is closed loop and keeps every level within ±Q/2.

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
           │ level N−2 blocks: detail bands or residuals   │
           │ …                                             │
           │ level 0 blocks                                │
           └───────────────────────────────────────────────┘
```

Each level is cut into 128² code-blocks of its own pixels, the unit that's decoded, read and cached. Blocks are stored tile by tile, so a 256² tile's 2×2 blocks are adjacent on disk. Block *b* is the bytes from `offset[b]` to `offset[b+1]`, and its id is a formula of (level, tile, position in tile); edge tiles keep zero-length ids for missing blocks. A payload has no header: its size and contents follow from the level table. All channels of an area are in one block, so RGB needs no extra reads. On open, every header field, the block count, the level table and the offsets are validated against the geometry before anything is allocated. Byte-level layout: [design.md §3](design.md).

## 3. Trade-offs

Gray 4096², 5 levels, lossless, zstd 1, 300 random crops per row, one core, file in the page cache ([benchmarks.md](benchmarks.md) has the setup and all tables).

| codec | file size | vs raw L0 (16.00 MiB) | vs naive |
|---|---:|---:|---:|
| **haar** | 11.58 MiB | 1.38× smaller | **−23.0%** |
| **cdf53** | 11.18 MiB | 1.43× smaller | **−25.7%** |
| laplacian-gauss | 14.29 MiB | 1.12× smaller | −5.0% |
| naive (same coder) | 15.04 MiB | 1.06× smaller | 0 |
| JPEG2000, 256² tiles | 10.84 MiB | 1.48× smaller | −27.9% |

Mean / p95 latency and bytes read per crop (block data, not counting the index):

| crop | level | haar | naive | JPEG2000 | bytes read: haar · naive · J2K |
|---|---:|---:|---:|---:|---:|
| 256² | L0 | 0.59 / 0.68 ms | 0.55 / 0.58 ms | 13.5 / 15.2 ms | 154 KiB · 101 KiB · 191 KiB |
| 256² | L2 | 0.53 / 0.66 ms | 0.53 / 0.58 ms | 8.8 / 9.3 ms | 129 KiB · 99 KiB · 1.2 MiB |
| 256² | L4 (whole level) | 0.25 / 0.28 ms | 0.25 / 0.27 ms | 6.4 / 6.8 ms | 44 KiB · 44 KiB · 10.8 MiB |
| 512² | L0 | 1.74 / 2.04 ms | 1.67 / 1.89 ms | 37.5 / 44.4 ms | 362 KiB · 282 KiB · 429 KiB |
| 512² | L3 (whole level) | 1.17 / 1.36 ms | 1.06 / 1.13 ms | 26.1 / 29.4 ms | 179 KiB · 176 KiB · 10.8 MiB |

**Compression vs. access granularity.** A `haar` crop also decodes the coarser blocks it's built from: about 2× `naive`'s blocks and 1.0–1.5× its bytes, in exchange for a 23% smaller file. Crops are about as fast as `naive`'s (within 10%), because detail blocks need only adds and shifts while `naive`'s pixel blocks run the pixel-by-pixel MED predictor. Decoding all of L0 and then cropping costs 93 ms, about **160×** a crop. A 64 MiB block cache brings a crop on a random walk down to 0.19 ms.

**Decode speed vs. size.** zstd level 1 encodes 4–34× faster than level 19 and gives the same gray lossless size (RGB and lossy files are 3–12% larger). The range coder would save 4–5% at 12× the decode time; crop latency matters more than 4% of storage here.

**Level definition.** `haar` levels are the exact 2×2 mean but alias; `cdf53` and `laplacian-gauss` are sharper or smoother and differ from the 2×2 mean by 24–30 dB PSNR. The matcher's needs should decide the mode. `cdf53` crops cost 1.3–1.5× `haar`'s.

**PSNR.** Lossless files decode every level bit for bit (∞ dB). With `--quant`, `cdf53` is the best `.prmd` mode (41.9 dB at 3.53 bits/px for Q = 4, 2–4 dB above `haar`), `laplacian` is the only one with a guaranteed per-pixel bound (±Q/2), and JPEG2000 still wins rate–distortion by about 3 dB. The 24–30 dB between `cdf53`/`gauss` levels and the 2×2 mean measures a different filter, not lower quality. [modes.md](modes.md#modes-and-psnr) explains every mode in terms of PSNR.

**Why not JPEG2000?** It's built in (OpenJPEG 2.5.4) and benchmarked. It compresses about 6% better and wins lossy rate–distortion (44.7 dB at 3.14 bits/px against 41.9 dB at 3.53 for `cdf53`), but crops decode **15–26× slower** and encoding is 17× slower. Its access granularity is poor at coarse levels: tiles are defined at full resolution and each holds all its resolutions, so in the benchmarked layout (256² tiles, one tile-part per tile, TLM) a 256² crop at L4 reads the **whole file** (10.8 MiB, against 44 KiB for `.prmd`). The format itself allows better: with tile-parts split by resolution, or one tile with precincts and packet-length markers (PLT), a decoder could fetch only the coarse resolutions. OpenJPEG 2.5.4 doesn't: in our measurements every such layout read as much or more, and untiled ones read the whole file for every crop ([design.md §8](design.md)). Getting coarse-level access out of JPEG2000 needs a different decoder (e.g. Kakadu or a JPIP-style packet fetcher).

**Embedded ARM.** About 3 kLOC of plain C++17 plus zstd (2.8k without the JPEG2000 backend and the profiler), integer arithmetic only, no SIMD required, and a mature, fuzzed decoder library that ships in every embedded Linux distribution. Decode memory is O(crop area): a few int32 planes for the crop and its ancestors (a 512² crop peaks at about 8 MB RSS), not the whole level. On a Raspberry Pi 5 a 256² crop takes 0.6–1.4 ms, still 11–21× faster than JPEG2000.

## 4. Limitations

- Only 8-bit gray and RGB. The decoder is single-threaded.
- 32-bit block offsets cap a file at 4 GiB.
- No checksums: a damaged block can decode to wrong pixels without an error, so integrity is left to the storage layer.
- Lossy `haar` and `cdf53` have no per-pixel error bound, and JPEG2000 beats all modes on lossy rate–distortion.
- On RGB, zstd files are 12–17% larger than with the range coder, which makes JPEG2000 smaller than the default `.prmd` RGB file (`haar+range` is still the smallest).

## 5. Next steps

- An adaptive rANS coder or a per-file zstd dictionary, to recover the 4–17% at zstd speed.
- Encode and decode the blocks of one level in parallel; blocks are independent.
- NEON versions of the transforms; `mmap` I/O.
- Rate-distortion-weighted quantizer steps per level, to close the lossy gap to JPEG2000.

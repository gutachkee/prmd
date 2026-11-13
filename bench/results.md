# Prmd benchmark results

- input: `test_image.jpg` (8824×5336 rgb), center 4096×4096
- CPU: ARM implementer 0x61 part 0x000, 14 cores (single-threaded decode)
- build: 15.2.0, Release -O2 -mcpu=native
- seed 1, 300 crops per latency row, file in page cache
- tiles 256×256 for every codec (prmd code-blocks 128×128), J2K code-blocks 64×64

## gray — 4096×4096, 5 levels

### File size (lossless; bits/px over level-0 pixels)

| codec | file size | bits/px | vs raw L0 | vs raw pyramid | vs naive | encode |
|---|---:|---:|---:|---:|---:|---:|
| haar | 11.59 MiB | 5.793 | 1.381× | 1.840× | -22.9% | 1081 ms |
| cdf53 | 11.19 MiB | 5.594 | 1.430× | 1.905× | -25.6% | 1306 ms |
| laplacian-box | 14.42 MiB | 7.209 | 1.110× | 1.478× | -4.1% | 1483 ms |
| laplacian-gauss | 14.29 MiB | 7.145 | 1.120× | 1.491× | -5.0% | 1504 ms |
| naive | 15.04 MiB | 7.518 | 1.064× | 1.418× | +0.0% | 1522 ms |
| j2k-t256 | 10.84 MiB | 5.422 | 1.476× | 1.966× | -27.9% | 1282 ms |
| j2k-t1024 | 10.82 MiB | 5.409 | 1.479× | 1.970× | -28.0% | 1283 ms |
| png-tiles (naive pyramid) | 14.91 MiB | 7.453 | 1.073× | 1.430× | -0.9% | - |
| raw level 0 | 16.00 MiB | 8.000 | 1.000× | - | - | - |
| raw pyramid | 21.31 MiB | 10.656 | - | 1.000× | - | - |

### Payload bytes per level (.prmd)

| codec | L0 | L1 | L2 | L3 | L4 |
|---|---:|---:|---:|---:|---:|
| haar | 8.70 MiB | 2.17 MiB | 542.3 KiB | 135.0 KiB | 43.9 KiB |
| cdf53 | 8.11 MiB | 2.29 MiB | 594.6 KiB | 150.1 KiB | 52.5 KiB |
| laplacian-box | 10.89 MiB | 2.67 MiB | 665.0 KiB | 164.9 KiB | 44.0 KiB |
| laplacian-gauss | 11.12 MiB | 2.40 MiB | 596.3 KiB | 147.7 KiB | 35.6 KiB |
| naive | 11.27 MiB | 2.85 MiB | 707.1 KiB | 176.0 KiB | 44.0 KiB |

### Random 256×256 crops (300 per row, cold cache, non-aligned positions)

| codec | level | mean | p50 | p95 | tiles touched | blocks decoded | bytes read | read calls |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | L0 | 0.54 ms | 0.53 ms | 0.64 ms | 9.5 | 17.5 | 153.6 KiB | 10.1 |
| haar | L1 | 0.51 ms | 0.49 ms | 0.59 ms | 8.2 | 16.2 | 143.2 KiB | 8.8 |
| haar | L2 | 0.51 ms | 0.49 ms | 0.68 ms | 6.8 | 14.7 | 128.8 KiB | 7.2 |
| haar | L3 | 0.53 ms | 0.52 ms | 0.60 ms | 5.0 | 12.9 | 119.3 KiB | 5.0 |
| haar | L4 | 0.28 ms | 0.27 ms | 0.33 ms | 1.0 | 4.0 | 43.9 KiB | 1.0 |
| cdf53 | L0 | 0.72 ms | 0.70 ms | 0.84 ms | 9.7 | 18.1 | 161.6 KiB | 10.4 |
| cdf53 | L1 | 0.67 ms | 0.65 ms | 0.77 ms | 8.4 | 16.7 | 160.4 KiB | 9.0 |
| cdf53 | L2 | 0.64 ms | 0.63 ms | 0.79 ms | 6.9 | 15.1 | 147.3 KiB | 7.3 |
| cdf53 | L3 | 0.68 ms | 0.66 ms | 0.83 ms | 5.0 | 13.1 | 137.9 KiB | 5.0 |
| cdf53 | L4 | 0.27 ms | 0.27 ms | 0.31 ms | 1.0 | 4.0 | 52.5 KiB | 1.0 |
| laplacian-box | L0 | 0.76 ms | 0.74 ms | 0.90 ms | 9.6 | 17.7 | 189.8 KiB | 10.2 |
| laplacian-box | L1 | 0.67 ms | 0.66 ms | 0.78 ms | 8.3 | 16.4 | 174.5 KiB | 8.9 |
| laplacian-box | L2 | 0.62 ms | 0.61 ms | 0.75 ms | 6.8 | 14.8 | 154.5 KiB | 7.2 |
| laplacian-box | L3 | 0.66 ms | 0.59 ms | 0.86 ms | 5.0 | 12.9 | 136.2 KiB | 5.0 |
| laplacian-box | L4 | 0.32 ms | 0.30 ms | 0.42 ms | 1.0 | 4.0 | 44.0 KiB | 1.0 |
| laplacian-gauss | L0 | 0.72 ms | 0.71 ms | 0.86 ms | 9.6 | 17.7 | 181.4 KiB | 10.2 |
| laplacian-gauss | L1 | 0.64 ms | 0.63 ms | 0.76 ms | 8.3 | 16.4 | 155.5 KiB | 8.9 |
| laplacian-gauss | L2 | 0.60 ms | 0.58 ms | 0.72 ms | 6.8 | 14.8 | 136.7 KiB | 7.2 |
| laplacian-gauss | L3 | 0.58 ms | 0.58 ms | 0.63 ms | 5.0 | 12.9 | 118.2 KiB | 5.0 |
| laplacian-gauss | L4 | 0.24 ms | 0.24 ms | 0.27 ms | 1.0 | 4.0 | 35.6 KiB | 1.0 |
| naive | L0 | 0.87 ms | 0.88 ms | 0.94 ms | 4.0 | 8.9 | 100.8 KiB | 4.0 |
| naive | L1 | 0.84 ms | 0.84 ms | 0.91 ms | 4.0 | 8.9 | 103.2 KiB | 4.0 |
| naive | L2 | 0.78 ms | 0.78 ms | 0.83 ms | 4.0 | 8.9 | 98.9 KiB | 4.0 |
| naive | L3 | 0.77 ms | 0.74 ms | 0.96 ms | 4.0 | 8.9 | 98.5 KiB | 4.0 |
| naive | L4 | 0.34 ms | 0.34 ms | 0.38 ms | 1.0 | 4.0 | 44.0 KiB | 1.0 |
| j2k-t256 | L0 | 12.65 ms | 12.66 ms | 14.31 ms | 4.0 | 4.0 | 190.7 KiB | 12.9 |
| j2k-t256 | L1 | 11.55 ms | 11.59 ms | 12.55 ms | 8.9 | 8.9 | 432.6 KiB | 27.8 |
| j2k-t256 | L2 | 8.29 ms | 8.20 ms | 9.40 ms | 24.9 | 24.9 | 1.16 MiB | 75.6 |
| j2k-t256 | L3 | 7.09 ms | 7.03 ms | 7.70 ms | 80.5 | 80.5 | 3.77 MiB | 242.4 |
| j2k-t256 | L4 | 6.01 ms | 5.92 ms | 6.43 ms | 256.0 | 256.0 | 10.84 MiB | 513.0 |
| j2k-t1024 | L0 | 14.92 ms | 14.53 ms | 18.92 ms | 1.4 | 1.4 | 969.3 KiB | 5.2 |
| j2k-t1024 | L1 | 14.73 ms | 14.41 ms | 18.23 ms | 2.0 | 2.0 | 1.37 MiB | 7.0 |
| j2k-t1024 | L2 | 13.40 ms | 13.34 ms | 14.15 ms | 4.0 | 4.0 | 2.72 MiB | 12.9 |
| j2k-t1024 | L3 | 11.37 ms | 11.41 ms | 11.69 ms | 8.9 | 8.9 | 6.11 MiB | 27.9 |
| j2k-t1024 | L4 | 5.43 ms | 5.41 ms | 5.63 ms | 16.0 | 16.0 | 10.82 MiB | 33.0 |

### Random 512×512 crops (300 per row, cold cache, non-aligned positions)

| codec | level | mean | p50 | p95 | tiles touched | blocks decoded | bytes read | read calls |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | L0 | 1.52 ms | 1.51 ms | 1.65 ms | 17.4 | 41.2 | 362.0 KiB | 16.0 |
| haar | L1 | 1.44 ms | 1.43 ms | 1.60 ms | 15.7 | 39.6 | 346.0 KiB | 14.1 |
| haar | L2 | 1.47 ms | 1.46 ms | 1.54 ms | 14.0 | 37.9 | 331.0 KiB | 12.0 |
| haar | L3 | 1.07 ms | 1.07 ms | 1.12 ms | 5.0 | 20.0 | 178.9 KiB | 2.0 |
| cdf53 | L0 | 2.23 ms | 2.21 ms | 2.40 ms | 17.8 | 42.5 | 371.3 KiB | 16.4 |
| cdf53 | L1 | 2.12 ms | 2.11 ms | 2.27 ms | 15.9 | 40.4 | 381.2 KiB | 14.3 |
| cdf53 | L2 | 2.14 ms | 2.13 ms | 2.23 ms | 14.1 | 38.4 | 373.4 KiB | 12.0 |
| cdf53 | L3 | 1.53 ms | 1.53 ms | 1.57 ms | 5.0 | 20.0 | 202.5 KiB | 2.0 |
| laplacian-box | L0 | 2.11 ms | 2.10 ms | 2.27 ms | 17.5 | 41.6 | 450.5 KiB | 16.2 |
| laplacian-box | L1 | 1.98 ms | 1.97 ms | 2.14 ms | 15.8 | 39.9 | 422.5 KiB | 14.2 |
| laplacian-box | L2 | 1.91 ms | 1.91 ms | 1.97 ms | 14.0 | 37.9 | 396.3 KiB | 12.0 |
| laplacian-box | L3 | 1.34 ms | 1.33 ms | 1.39 ms | 5.0 | 20.0 | 208.9 KiB | 2.0 |
| laplacian-gauss | L0 | 2.00 ms | 1.99 ms | 2.14 ms | 17.5 | 41.6 | 436.6 KiB | 16.2 |
| laplacian-gauss | L1 | 1.84 ms | 1.83 ms | 1.97 ms | 15.8 | 39.9 | 377.9 KiB | 14.2 |
| laplacian-gauss | L2 | 1.80 ms | 1.80 ms | 1.92 ms | 14.0 | 37.9 | 351.3 KiB | 12.0 |
| laplacian-gauss | L3 | 1.22 ms | 1.21 ms | 1.26 ms | 5.0 | 20.0 | 183.3 KiB | 2.0 |
| naive | L0 | 2.42 ms | 2.44 ms | 2.51 ms | 8.9 | 24.8 | 281.8 KiB | 7.0 |
| naive | L1 | 2.37 ms | 2.39 ms | 2.46 ms | 9.0 | 24.9 | 286.2 KiB | 7.0 |
| naive | L2 | 2.21 ms | 2.17 ms | 2.37 ms | 9.0 | 24.9 | 276.0 KiB | 7.0 |
| naive | L3 | 1.41 ms | 1.41 ms | 1.44 ms | 4.0 | 16.0 | 176.0 KiB | 1.0 |
| j2k-t256 | L0 | 32.01 ms | 31.88 ms | 35.04 ms | 8.9 | 8.9 | 428.7 KiB | 27.8 |
| j2k-t256 | L1 | 31.32 ms | 31.36 ms | 32.69 ms | 24.9 | 24.9 | 1.16 MiB | 75.8 |
| j2k-t256 | L2 | 26.00 ms | 26.00 ms | 26.65 ms | 80.7 | 80.7 | 3.78 MiB | 243.1 |
| j2k-t256 | L3 | 21.75 ms | 21.67 ms | 22.25 ms | 256.0 | 256.0 | 10.84 MiB | 513.0 |
| j2k-t1024 | L0 | 36.02 ms | 35.76 ms | 40.98 ms | 2.2 | 2.2 | 1.48 MiB | 7.5 |
| j2k-t1024 | L1 | 37.36 ms | 36.87 ms | 42.49 ms | 4.0 | 4.0 | 2.72 MiB | 12.9 |
| j2k-t1024 | L2 | 35.84 ms | 35.23 ms | 40.27 ms | 9.0 | 9.0 | 6.14 MiB | 27.9 |
| j2k-t1024 | L3 | 20.88 ms | 20.64 ms | 21.67 ms | 16.0 | 16.0 | 10.82 MiB | 33.0 |

### Entropy coders (300 crops per cell)

| coder | file size | vs zstd19 (default) | vs naive, same coder | mean 256² L0 | p95 256² L0 | mean 512² L1 |
|---|---:|---:|---:|---:|---:|---:|
| haar | 11.59 MiB | +0.0% | -22.9% | 0.56 ms | 0.66 ms | 1.47 ms |
| haar+zstd3 | 11.60 MiB | +0.1% | - | 0.54 ms | 0.63 ms | 1.48 ms |
| haar+range | 11.13 MiB | -3.9% | -22.3% | 7.04 ms | 8.61 ms | 16.20 ms |
| haar+raw | 32.01 MiB | +176.3% | -24.9% | 0.37 ms | 0.43 ms | 1.05 ms |
| cdf53 | 11.19 MiB | +0.0% | -25.6% | 0.73 ms | 0.85 ms | 2.19 ms |
| cdf53+zstd3 | 11.22 MiB | +0.3% | - | 0.72 ms | 0.82 ms | 2.17 ms |
| cdf53+range | 10.69 MiB | -4.4% | -25.3% | 7.41 ms | 9.07 ms | 17.89 ms |
| cdf53+raw | 32.01 MiB | +186.1% | -24.9% | 0.51 ms | 0.58 ms | 1.74 ms |
| naive | 15.04 MiB | +0.0% | - | 0.86 ms | 0.92 ms | 2.40 ms |
| naive+range | 14.33 MiB | -4.7% | - | 4.93 ms | 5.46 ms | 14.16 ms |
| naive+raw | 42.63 MiB | +183.5% | - | 0.65 ms | 0.68 ms | 1.88 ms |

### Trajectory: 1000-step random walk of 256² crops at L0 (±32 px/step)

| codec | cache | mean | p95 | blocks decoded | cache hits | bytes read |
|---|---:|---:|---:|---:|---:|---:|
| haar | off | 0.51 ms | 0.59 ms | 17.4 | 0.0 | 146.3 KiB |
| haar | 64 MiB | 0.17 ms | 0.19 ms | 0.1 | 17.3 | 1.3 KiB |
| cdf53 | off | 0.69 ms | 0.78 ms | 18.1 | 0.0 | 154.2 KiB |
| cdf53 | 64 MiB | 0.33 ms | 0.35 ms | 0.2 | 18.0 | 1.4 KiB |
| laplacian-box | off | 0.71 ms | 0.81 ms | 17.6 | 0.0 | 182.2 KiB |
| laplacian-box | 64 MiB | 0.25 ms | 0.28 ms | 0.2 | 17.5 | 1.7 KiB |
| laplacian-gauss | off | 0.67 ms | 0.77 ms | 17.6 | 0.0 | 174.0 KiB |
| laplacian-gauss | 64 MiB | 0.22 ms | 0.24 ms | 0.2 | 17.5 | 1.6 KiB |
| naive | off | 0.77 ms | 0.84 ms | 8.9 | 0.0 | 94.5 KiB |
| naive | 64 MiB | 0.05 ms | 0.05 ms | 0.1 | 8.8 | 1.1 KiB |

### Full-level decode (what a crop costs without random access)

| codec | full L0 decode | full L1 decode | full L2 decode |
|---|---:|---:|---:|
| haar | 76.6 ms | 17.9 ms | 4.4 ms |
| cdf53 | 246.8 ms | 41.5 ms | 7.3 ms |
| laplacian-box | 109.1 ms | 25.9 ms | 6.2 ms |
| laplacian-gauss | 98.5 ms | 22.7 ms | 5.6 ms |
| naive | 108.9 ms | 27.2 ms | 6.6 ms |
| j2k-t256 | 1177.5 ms | 324.1 ms | 83.3 ms |
| j2k-t1024 | 1215.3 ms | 325.2 ms | 81.4 ms |

### Level definitions compared: PSNR of each codec's level vs the 2×2-mean pyramid

Level 0 is identical (lossless) for every codec. Center 256² of each level 3 is saved in `bench/samples/`.

| codec | L1 | L2 | L3 | L4 |
|---|---:|---:|---:|---:|
| haar | ∞ (exact) | ∞ (exact) | ∞ (exact) | ∞ (exact) |
| cdf53 | 27.55 dB | 25.52 dB | 24.76 dB | 24.47 dB |
| laplacian-box | 50.19 dB | 45.60 dB | 42.43 dB | 40.00 dB |
| laplacian-gauss | 30.08 dB | 29.78 dB | 29.57 dB | 29.51 dB |
| naive | 50.19 dB | 45.60 dB | 42.43 dB | 40.00 dB |
| j2k-t256 | 27.57 dB | 25.55 dB | 24.81 dB | 24.56 dB |
| j2k-t1024 | 27.55 dB | 25.53 dB | 24.76 dB | 24.48 dB |

### Lossy: .prmd quantization vs JPEG2000 9/7

PSNR L0 is against the source (the same for every codec). PSNR L2 is against that codec's own lossless level 2, i.e. the error added at a coarse level (n/a for J2K: lossy J2K uses the 9/7 filter, so its levels are different images from its lossless 5/3 levels). The coarsest level of .prmd stays lossless.

| codec | setting | file size | bits/px | PSNR L0 | PSNR L2 | max |err| L0 | mean 256² L0 | p95 256² L0 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | quant 2 | 9.10 MiB | 4.551 | 40.33 dB | 45.07 dB | 8 | 0.61 ms | 0.70 ms |
| haar | quant 4 | 7.11 MiB | 3.557 | 38.61 dB | 42.67 dB | 12 | 0.59 ms | 0.69 ms |
| haar | quant 8 | 5.19 MiB | 2.595 | 35.12 dB | 38.49 dB | 21 | 0.57 ms | 0.67 ms |
| cdf53 | quant 2 | 8.80 MiB | 4.401 | 44.54 dB | 47.00 dB | 8 | 0.78 ms | 0.89 ms |
| cdf53 | quant 4 | 6.81 MiB | 3.406 | 41.86 dB | 43.63 dB | 11 | 0.74 ms | 0.86 ms |
| cdf53 | quant 8 | 4.93 MiB | 2.465 | 37.07 dB | 38.56 dB | 22 | 0.74 ms | 0.85 ms |
| laplacian-gauss | quant 2 | 11.78 MiB | 5.891 | 51.15 dB | 51.14 dB | 1 | 0.79 ms | 0.91 ms |
| laplacian-gauss | quant 4 | 9.20 MiB | 4.600 | 46.41 dB | 46.39 dB | 2 | 0.79 ms | 0.96 ms |
| laplacian-gauss | quant 8 | 6.75 MiB | 3.375 | 40.83 dB | 40.77 dB | 4 | 0.74 ms | 0.86 ms |
| cdf53+range | quant 4 | 6.29 MiB | 3.145 | 41.86 dB | 43.63 dB | 11 | 5.35 ms | 6.73 ms |
| j2k-t256 9/7 | target 50 dB | 7.88 MiB | 3.938 | 49.13 dB | n/a | 7 | 9.75 ms | 11.60 ms |
| j2k-t256 9/7 | target 45 dB | 6.28 MiB | 3.138 | 44.68 dB | n/a | 10 | 8.01 ms | 10.01 ms |
| j2k-t256 9/7 | target 40 dB | 4.70 MiB | 2.351 | 39.80 dB | n/a | 22 | 6.17 ms | 7.95 ms |
| j2k-t256 9/7 | target 35 dB | 3.07 MiB | 1.537 | 34.81 dB | n/a | 56 | 4.16 ms | 5.78 ms |


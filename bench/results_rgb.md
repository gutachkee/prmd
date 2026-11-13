# Prmd benchmark results

- input: `test_image.jpg` (8824×5336 rgb), center 4096×4096
- CPU: ARM implementer 0x61 part 0x000, 14 cores (single-threaded decode)
- build: 15.2.0, Release -O2 -mcpu=native
- seed 1, 100 crops per latency row, file in page cache
- tiles 256×256 for every codec (prmd code-blocks 128×128), J2K code-blocks 64×64

## rgb — 4096×4096, 5 levels

### File size (lossless; bits/px over level-0 pixels)

| codec | file size | bits/px | vs raw L0 | vs raw pyramid | vs naive | encode |
|---|---:|---:|---:|---:|---:|---:|
| haar | 22.92 MiB | 11.459 | 2.095× | 2.790× | -22.8% | 8270 ms |
| cdf53 | 24.24 MiB | 12.121 | 1.980× | 2.638× | -18.4% | 8415 ms |
| naive | 29.69 MiB | 14.847 | 1.617× | 2.153× | +0.0% | 11401 ms |
| j2k-t256 | 20.98 MiB | 10.488 | 2.288× | 3.048× | -29.4% | 2595 ms |
| haar+range | 20.52 MiB | 10.259 | 2.339× | 3.116× | -30.9% | 1098 ms |
| png-tiles (naive pyramid) | 42.22 MiB | 21.108 | 1.137× | 1.515× | +42.2% | - |
| raw level 0 | 48.00 MiB | 24.000 | 1.000× | - | - | - |
| raw pyramid | 63.94 MiB | 31.969 | - | 1.000× | - | - |

### Payload bytes per level (.prmd)

| codec | L0 | L1 | L2 | L3 | L4 |
|---|---:|---:|---:|---:|---:|
| haar | 16.37 MiB | 4.73 MiB | 1.36 MiB | 347.7 KiB | 113.2 KiB |
| cdf53 | 16.96 MiB | 5.30 MiB | 1.46 MiB | 388.2 KiB | 135.8 KiB |
| naive | 21.08 MiB | 6.35 MiB | 1.71 MiB | 447.4 KiB | 113.3 KiB |
| haar+range | 14.55 MiB | 4.26 MiB | 1.28 MiB | 325.8 KiB | 106.2 KiB |

### Random 256×256 crops (100 per row, cold cache, non-aligned positions)

| codec | level | mean | p50 | p95 | tiles touched | blocks decoded | bytes read | read calls |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | L0 | 2.03 ms | 1.98 ms | 2.41 ms | 9.6 | 17.6 | 329.1 KiB | 10.2 |
| haar | L1 | 1.92 ms | 1.85 ms | 2.22 ms | 8.3 | 16.2 | 337.7 KiB | 8.9 |
| haar | L2 | 1.82 ms | 1.80 ms | 2.45 ms | 6.7 | 14.6 | 330.3 KiB | 7.2 |
| haar | L3 | 2.21 ms | 2.22 ms | 2.27 ms | 5.0 | 13.0 | 307.9 KiB | 5.0 |
| haar | L4 | 1.22 ms | 1.23 ms | 1.32 ms | 1.0 | 4.0 | 113.2 KiB | 1.0 |
| cdf53 | L0 | 2.60 ms | 2.54 ms | 3.05 ms | 9.8 | 18.2 | 370.4 KiB | 10.4 |
| cdf53 | L1 | 2.50 ms | 2.43 ms | 2.89 ms | 8.6 | 16.8 | 389.9 KiB | 9.2 |
| cdf53 | L2 | 2.31 ms | 2.30 ms | 2.94 ms | 6.8 | 14.9 | 371.4 KiB | 7.2 |
| cdf53 | L3 | 2.53 ms | 2.53 ms | 2.65 ms | 5.0 | 13.1 | 356.2 KiB | 5.0 |
| cdf53 | L4 | 1.33 ms | 1.33 ms | 1.37 ms | 1.0 | 4.0 | 135.8 KiB | 1.0 |
| naive | L0 | 2.59 ms | 2.62 ms | 2.97 ms | 4.0 | 9.0 | 187.6 KiB | 4.0 |
| naive | L1 | 2.62 ms | 2.63 ms | 2.92 ms | 3.9 | 8.9 | 226.9 KiB | 3.9 |
| naive | L2 | 2.90 ms | 2.92 ms | 3.01 ms | 4.0 | 8.9 | 245.2 KiB | 4.0 |
| naive | L3 | 2.88 ms | 2.90 ms | 3.07 ms | 4.0 | 9.0 | 251.1 KiB | 4.0 |
| naive | L4 | 1.35 ms | 1.34 ms | 1.42 ms | 1.0 | 4.0 | 113.3 KiB | 1.0 |
| j2k-t256 | L0 | 25.19 ms | 24.74 ms | 29.90 ms | 4.0 | 4.0 | 353.2 KiB | 13.0 |
| j2k-t256 | L1 | 25.10 ms | 25.13 ms | 28.62 ms | 8.9 | 8.9 | 790.0 KiB | 27.7 |
| j2k-t256 | L2 | 19.95 ms | 20.00 ms | 21.24 ms | 24.9 | 24.9 | 2.13 MiB | 75.5 |
| j2k-t256 | L3 | 17.68 ms | 17.75 ms | 18.14 ms | 80.5 | 80.5 | 6.92 MiB | 242.4 |
| j2k-t256 | L4 | 15.07 ms | 15.04 ms | 15.34 ms | 256.0 | 256.0 | 20.97 MiB | 513.0 |
| haar+range | L0 | 15.59 ms | 15.35 ms | 19.53 ms | 9.6 | 17.6 | 298.8 KiB | 10.2 |
| haar+range | L1 | 16.01 ms | 15.70 ms | 18.51 ms | 8.3 | 16.2 | 310.5 KiB | 8.9 |
| haar+range | L2 | 15.84 ms | 15.99 ms | 19.29 ms | 6.7 | 14.6 | 310.5 KiB | 7.2 |
| haar+range | L3 | 15.35 ms | 15.21 ms | 16.69 ms | 5.0 | 13.0 | 289.1 KiB | 5.0 |
| haar+range | L4 | 5.97 ms | 5.87 ms | 6.42 ms | 1.0 | 4.0 | 106.2 KiB | 1.0 |

### Random 512×512 crops (100 per row, cold cache, non-aligned positions)

| codec | level | mean | p50 | p95 | tiles touched | blocks decoded | bytes read | read calls |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| haar | L0 | 5.29 ms | 5.26 ms | 5.73 ms | 17.4 | 41.3 | 757.8 KiB | 16.1 |
| haar | L1 | 5.09 ms | 5.01 ms | 5.88 ms | 15.7 | 39.6 | 804.5 KiB | 14.2 |
| haar | L2 | 5.14 ms | 5.15 ms | 5.36 ms | 13.9 | 37.7 | 847.1 KiB | 12.0 |
| haar | L3 | 3.81 ms | 3.82 ms | 3.96 ms | 5.0 | 20.0 | 460.9 KiB | 2.0 |
| cdf53 | L0 | 7.70 ms | 7.60 ms | 8.32 ms | 17.9 | 42.5 | 837.0 KiB | 16.5 |
| cdf53 | L1 | 7.57 ms | 7.48 ms | 8.39 ms | 16.1 | 40.8 | 921.8 KiB | 14.4 |
| cdf53 | L2 | 7.54 ms | 7.42 ms | 8.36 ms | 14.0 | 38.4 | 945.0 KiB | 11.9 |
| cdf53 | L3 | 5.15 ms | 5.14 ms | 5.33 ms | 5.0 | 20.0 | 524.0 KiB | 2.0 |
| naive | L0 | 7.44 ms | 7.52 ms | 8.30 ms | 8.9 | 24.9 | 533.3 KiB | 7.0 |
| naive | L1 | 8.23 ms | 8.32 ms | 8.63 ms | 8.9 | 24.9 | 633.1 KiB | 7.0 |
| naive | L2 | 8.25 ms | 8.32 ms | 8.50 ms | 9.0 | 24.8 | 678.0 KiB | 7.0 |
| naive | L3 | 5.52 ms | 5.49 ms | 5.75 ms | 4.0 | 16.0 | 447.4 KiB | 1.0 |
| j2k-t256 | L0 | 65.48 ms | 64.81 ms | 74.76 ms | 8.9 | 8.9 | 801.4 KiB | 27.8 |
| j2k-t256 | L1 | 70.13 ms | 69.94 ms | 76.10 ms | 24.9 | 24.9 | 2.14 MiB | 75.6 |
| j2k-t256 | L2 | 64.14 ms | 64.56 ms | 65.91 ms | 80.4 | 80.4 | 6.89 MiB | 242.1 |
| j2k-t256 | L3 | 56.08 ms | 55.80 ms | 57.05 ms | 256.0 | 256.0 | 20.97 MiB | 513.0 |
| haar+range | L0 | 36.87 ms | 36.20 ms | 43.20 ms | 17.4 | 41.3 | 684.9 KiB | 16.1 |
| haar+range | L1 | 39.49 ms | 39.13 ms | 43.30 ms | 15.7 | 39.6 | 737.4 KiB | 14.2 |
| haar+range | L2 | 42.86 ms | 42.41 ms | 47.23 ms | 13.9 | 37.7 | 796.4 KiB | 12.0 |
| haar+range | L3 | 24.02 ms | 23.69 ms | 26.70 ms | 5.0 | 20.0 | 432.0 KiB | 2.0 |

### Trajectory: 1000-step random walk of 256² crops at L0 (±32 px/step)

| codec | cache | mean | p95 | blocks decoded | cache hits | bytes read |
|---|---:|---:|---:|---:|---:|---:|
| haar | off | 1.79 ms | 2.13 ms | 17.4 | 0.0 | 316.3 KiB |
| haar | 64 MiB | 0.51 ms | 0.54 ms | 0.1 | 17.3 | 2.6 KiB |
| cdf53 | off | 2.37 ms | 2.86 ms | 18.1 | 0.0 | 362.3 KiB |
| cdf53 | 64 MiB | 0.97 ms | 1.03 ms | 0.2 | 18.0 | 3.1 KiB |
| naive | off | 2.34 ms | 2.70 ms | 8.9 | 0.0 | 179.1 KiB |
| naive | 64 MiB | 0.16 ms | 0.15 ms | 0.1 | 8.8 | 2.1 KiB |
| haar+range | off | 15.75 ms | 20.83 ms | 17.4 | 0.0 | 288.1 KiB |
| haar+range | 64 MiB | 0.63 ms | 0.58 ms | 0.1 | 17.3 | 2.4 KiB |

### Full-level decode (what a crop costs without random access)

| codec | full L0 decode | full L1 decode | full L2 decode |
|---|---:|---:|---:|
| haar | 246.7 ms | 58.7 ms | 14.1 ms |
| cdf53 | 744.3 ms | 128.3 ms | 22.3 ms |
| naive | 335.5 ms | 85.5 ms | 22.2 ms |
| j2k-t256 | 2395.4 ms | 727.5 ms | 204.0 ms |
| haar+range | 1222.4 ms | 343.4 ms | 100.2 ms |

### Level definitions compared: PSNR of each codec's level vs the 2×2-mean pyramid

Level 0 is identical (lossless) for every codec. Center 256² of each level 3 is saved in `bench/samples/`.

| codec | L1 | L2 | L3 | L4 |
|---|---:|---:|---:|---:|
| haar | ∞ (exact) | ∞ (exact) | ∞ (exact) | ∞ (exact) |
| cdf53 | 27.42 dB | 25.38 dB | 24.73 dB | 24.47 dB |
| naive | 50.19 dB | 45.60 dB | 42.43 dB | 40.01 dB |
| j2k-t256 | 27.44 dB | 25.39 dB | 24.73 dB | 24.49 dB |
| haar+range | ∞ (exact) | ∞ (exact) | ∞ (exact) | ∞ (exact) |


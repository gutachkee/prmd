# Random-access crop decoding from a tiled image pyramid

## Background
In visual navigation, we keep large reference images and need small regions at different resolutions with low latency, often on embedded hardware. Decoding the full image for every
crop wastes CPU, memory, and I/O.

## Problem
You have a compressed reference image of shape W × H (e.g., 4096 × 4096), with
downsampled versions (2056 × 2056, 1024 × 1024, 512 × 512, 256 × 256). You have a certain
bounding box (x, y, w, h, lvl). You want to crop the image from the corresponding
level in the image pyramid and return a new image of shape w × h; you are not allowed to load
the whole W × H image into memory to decode and crop it. Loading the whole image wastes
CPU cycles and creates unnecessary pressure on the memory bus.
A naive solution would be to store an image as a multi-resolution pyramid of 256 × 256 tiles, so
that any crop can be decoded by touching only the tiles it overlaps.

The source image is W × H, for example, 4096 × 4096. 8-bit grayscale is required; RGB
is optional.

The pyramid has N levels. Level 0 is full resolution, and each subsequent level is
downsampled by 2 (4096 → 2048 → 1024 → …).

Each level is split into 256 × 256 tiles. Edge tiles may be smaller.
The downside of this approach is that we need a lot of memory to store all the compressed tiles.
We can use the fact that each level is created by downsampling the previous one. Your task is
to investigate whether it is possible to encode the image as a single file, supporting an efficient
decoding that would not decode the whole image at a given level and would result in a better
compression ratio than a native option.

## Implement
1. Encoder: turns an image and N into a single file holding all levels.
2. Decoder: decode(level, x, y, w, h) -> pixels, with coordinates in that level's
pixels.

It decodes, and ideally reads, only the tiles that the rectangle overlaps.

It finds those tiles through an index or the format's own structure, not by scanning
the file.

Crops that span several tiles come out seamless. Out-of-bounds requests are
clamped or rejected, as you document.

You may use any approach: JPEG2000 (which natively supports tiles and resolution levels), a
custom container of compressed tiles, or an existing tiled format. Third-party libraries are
allowed. Justify your choice in terms of compression, decode speed, access granularity, and
suitability for an embedded ARM target.

## Deliverables

Code in [C++ / Python] with build and run instructions, plus a command-line tool to
encode an image and save a decoded crop as PNG.

Tests for crops inside one tile, across 2 and 4 tiles, on edges and corners, and the full
level. Compare each result with a reference crop made from the source image.

A benchmark on a 4096 × 4096 image:

file size compared with the raw size;

average and p95 latency for random 256 × 256 and 512 × 512 crops at several
levels;

tiles decoded and bytes read per crop;

PSNR if the compression is lossy.

A 1–2 page write-up covering your design, the format layout, trade-offs, limitations, and
next steps.


## Evaluation
We look at correctness, proven random access, performance backed by numbers, design
reasoning, code quality, and clarity of the write-up.
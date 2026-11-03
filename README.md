# OCR-AI — a from-scratch OCR stack in C++17

A complete optical-character-recognition pipeline with **no Python anywhere**:
synthetic data generator → CRNN/CTC trainer (NeoML) → proprietary model export →
cross-platform inference engine for Linux, Windows and Android.

The project is built in five deliverables. Their status is tracked below; nothing
in this file describes work that has not been built and run.

| # | Deliverable | Status |
|---|-------------|--------|
| 1 | Procedural synthetic data generator (`.ocrb` shards, manifest, verifier) | **done** |
| 2 | CRNN + CTC trainer on NeoML | not started |
| 3 | `.ocrbin` model exporter + `FORMAT.md` | not started |
| 4 | Inference engine (Linux / Windows / Android NDK) | not started |
| 5 | Documentation set (`README.md`, `FORMAT.md`, `MATH.md`, `BENCHMARKS.md`) | in progress — this file and `BENCHMARKS.md` |

`FORMAT.md` and `MATH.md` are written together with the model format and the trainer,
because until then there is nothing factual to describe.

---

## 1. Repository layout

```
CMakeLists.txt              top-level build (imports NeoML + FreeType, builds tools)
scripts/build_neoml.sh      pins and builds NeoML, CPU-only
scripts/build_freetype.sh   builds a static FreeType (system has runtime .so only)
third_party/neoml           vendored NeoML @ bad7a93f7b51637c1cd04e1c5fdad25e11045353
third_party/freetype        vendored FreeType @ 36db6edb (static build)
third_party/stb             stb_image.h v2.30 (background decoding)
src/common/                 SHA-256, image primitives, RNG (splitmix64)
src/generator/              data generator + verifier (Deliverable 1)
tools/hello_crnn/           NeoML smoke test: build, run, save, reload
data/                       generated datasets (not tracked)
```

## 2. Building

### 2.1 Prerequisites

* CMake ≥ 3.20, Ninja, a C++17 compiler (tested with g++ 14.2.0)
* NeoML and FreeType are **vendored**; no package-manager step is required.
  The machine this was developed on had no FreeType headers and no usable
  `sudo`, so FreeType is built from vendored source into `third_party/freetype/lib`.

### 2.2 Build order

```bash
scripts/build_neoml.sh        # NeoML + NeoMathEngine, CPU only   (~5 min)
scripts/build_freetype.sh     # static libfreetype.a
cmake -S . -B build -G Ninja
cmake --build build -j3
```

`scripts/build_neoml.sh` passes two options that matter:

* `-DNeoOnnx_BUILD=OFF` — the ONNX importer's `FetchContent` protobuf download
  hangs on restricted networks. Nothing in this project needs ONNX.
* `-DCMAKE_CXX_FLAGS="-include stdexcept"` — GCC 14 no longer transitively
  includes `<stdexcept>`, which `MathEngine.cpp` and
  `CpuMathEngineDnnDistributed.cpp` rely on. This is applied as a compile flag
  instead of patching vendored sources.

NeoML's math engine `dlopen`s `libNeoMathEngineAvx.so` relative to its own module
directory, so the script creates the expected symlink:

```
build/neoml/NeoMathEngine/src/libNeoMathEngineAvx.so -> CPU/x86/avx/libNeoMathEngineAvx.so
```

### 2.3 Smoke test

```bash
./build/tools/hello_crnn/hello_crnn
```

Builds a conv → relu → maxpool → fc → softmax network, runs a forward pass,
saves the network to `.neoml`, reloads it and compares outputs. Expected output
ends with `max_abs_diff_after_reload=0.000000000` and `RESULT=PASS`. This is the
gate that proves the framework builds, computes, and round-trips on this CPU.

## 3. Synthetic data generator

```bash
./build/src/generator/ocr_gen \
    --fonts /usr/share/fonts \
    --out data/train10k \
    --count 10000 --shard-size 10000 --seed 1 --quiet
```

Produces `shard_00000.ocrb` plus `manifest.json` (per-shard record count, byte
size, SHA-256). `--help` lists every option; the shard layout is described in
section 3.3.

### 3.1 What a sample is

Each image is 128×32, single-channel by default, with grayscale in `[0,255]`
(the trainer normalises to `[-1,1]`).

1. **Text sampling** — random ASCII strings, dictionary words
   (`--mode words --wordlist /usr/share/dict/words`) or digits. Runs of the same
   character are capped at 3, and lengths are biased toward the short end of
   `--len-min..--len-max`, so the label distribution is not dominated by
   `?????`-style rows.
2. **Font selection** — every font in `--fonts` is scanned once and its glyph
   coverage is recorded. Only fonts covering ≥ 60 % of the configured charset
   are used; script-specific faces that contain a handful of ASCII glyphs would
   otherwise generate degenerate labels. On this machine 430 of 2 476 fonts
   qualify. Labels are then drawn only from characters the chosen font actually
   has, so a render never fails on a missing glyph.
3. **Rendering** — FreeType rasterisation with random face, weight (synthetic
   bold), kerning, letter spacing, baseline jitter, shear-italic, then
   fit-to-128×32 preserving aspect. Foreground and background luminance are
   chosen from a WCAG contrast-ratio constraint, so text stays legible by
   construction rather than by luck.
4. **Backgrounds** — flat/tinted/gradient/patterned noise, or real photographs
   if `--backgrounds DIR` is given (PNG/JPEG via `stb_image`).
5. **Augmentation** (`--augment full`) — rotation, perspective, shear, Gaussian
   and motion blur, brightness/contrast/gamma, Gaussian noise, salt-and-pepper,
   real 8×8 DCT JPEG quantisation, scanlines, chromatic aberration.

### 3.2 Legibility control

The point of the dataset is that the strings stay *hard but readable*. Every
candidate sample is measured: a normalised cross-correlation between the clean
render and the degraded image, over ink pixels only, converted to a CER proxy.
If the sample is more legible than `--min-cer` the severity is raised (×1.45);
if it is less legible than the upper bound it is lowered (×0.70), for up to 8
attempts. Severity is clamped to `[0.2, 1.8]`, and blur is additionally capped
at 1.25× its base sigma, because past ~2.5 px of sigma a 32 px line loses its
stroke structure and teaches the network nothing.

A render that is still far outside the band after all attempts is discarded and a
fresh one is drawn, which catches the occasional sample whose background and text
colour leave nothing to degrade (5 of 10 000 on the shipped configuration).

Measured on the shipped configuration: **92.3 % of samples land in the
0.05–0.20 CER band, mean CER 0.117** (see `BENCHMARKS.md` for the method and the
raw runs).

### 3.3 Shard format

All integers little-endian.

```
file   := "OCRB" | version:u32 | num_records:u32 | record{num_records}
record := label_len:u32 | label:u8[label_len]
        | h:u32 | w:u32 | ch:u32 | pixels:u8[h*w*ch]
```

`version` is `1`. The manifest records `count`, `bytes` and `sha256` per shard.

### 3.4 Reproducibility

`--seed` fixes the dataset exactly: each record's RNG is seeded from
`deriveSeed(master_seed, record_index)`, and the writer reorders records by index
before writing, so the same `(seed, options)` pair produces byte-identical
shards regardless of thread count or scheduling.

### 3.5 Verification

```bash
./build/src/generator/ocr_gen --verify data/train10k \
    --dump data/samples --verify-count 50
```

Re-reads every shard, checks magic/version/record bounds for truncation or
overflow, counts records, recomputes SHA-256 against the manifest, and dumps N
samples as PNM next to `labels.txt` together with a visual checklist.

## 4. Measured performance

See `BENCHMARKS.md` for hardware, method and the full table. Summary on
2 cores / 4 threads, 5 000 images, seed 11:

| `--augment` | throughput | in-band |
|-------------|-----------|---------|
| `full`  | 380–436 img/s | 91.5 % |
| `light` | 473–514 img/s | 87.1 % |
| `none`  | 1010–1169 img/s | — |

A 100 000-image run completed at 221.6 img/s in 10 shards with resident memory
plateauing at ~155 MB (no growth over the run).

## 5. Known limitations

* Training, export and inference are not implemented yet; this repository
  currently builds the generator, the verifier and the NeoML smoke test only.
* Font scanning is single-threaded and takes a few seconds on a 2 476-font tree.
* Legibility control is a proxy, not a real recogniser; ~9 % of samples fall
  outside the target band and are still written to disk (they are not resampled
  indefinitely).
* Windows and Android builds of the generator are untested; it currently uses
  POSIX threads and font enumeration via `dirent`.

# Benchmarks

Every number here was produced by a command in this repository on the hardware
described below, and the command is quoted so it can be re-run. Numbers that
were not measured are marked *not yet measured* rather than extrapolated.

## Hardware and toolchain

| Item | Value |
|------|-------|
| CPU | Intel Core i5-6300U, 2 physical cores / 4 threads, 2.4 GHz base |
| RAM | 16 GB |
| GPU | none used (CPU-only build of NeoML) |
| OS | Linux (kernel 6.x) |
| Compiler | g++ 14.2.0, `-O3` via CMake Release |
| CMake / Ninja | 3.31.6 / 1.12.1 |
| NeoML | `bad7a93f7b51637c1cd04e1c5fdad25e11045353`, CPU-only |
| Fonts | 2 476 `.ttf`/`.otf` under `/usr/share/fonts`, 430 usable |

## 1. Framework build (gate for everything else)

```bash
scripts/build_neoml.sh
```

| Metric | Result |
|--------|--------|
| Targets built | 84 / 84 |
| Errors | 0 |
| Artifacts | `libNeoML.so`, `libNeoMathEngine.so` (+ AVX kernel `.so`) |
| Configure time | < 1 s with `-DNeoOnnx_BUILD=OFF` (vs. hang indefinitely with the ONNX importer enabled — its protobuf `FetchContent` download stalls on this network) |

Round-trip check (`./build/tools/hello_crnn/hello_crnn`): a conv → relu →
maxpool → fc → softmax net runs a forward pass, serialises, reloads and
re-evaluates with `max_abs_diff_after_reload = 0.000000000`, `RESULT=PASS`.

## 2. Data generator throughput

Command (identical apart from `--augment`):

```bash
./build/src/generator/ocr_gen --fonts /usr/share/fonts --out /tmp/bm_<profile> \
    --count 5000 --shard-size 5000 --seed 11 --augment <profile> --quiet
```

Machine otherwise idle, default thread count (4). Two runs each, because the
spread between runs on a 15 W mobile CPU is real:

| Profile | Images | Run 1 | Run 2 | Throughput | Mean CER | In band (0.05–0.20) |
|---------|--------|-------|-------|-----------|----------|---------------------|
| `full`  | 5 000 | 13.17 s | 11.48 s | **380–436 img/s** | 0.115 | 91.5 % |
| `light` | 5 000 | 9.73 s | 10.56 s | **473–514 img/s** | 0.109 | 87.1 % |
| `none`  | 5 000 | 4.28 s | 4.95 s | **1010–1169 img/s** | 0.000 | 100 % (nothing degraded) |

Single-threaded, full profile, to show how much the thread pool actually buys:

```bash
./build/src/generator/ocr_gen --fonts /usr/share/fonts --out /tmp/bm_t1 \
    --count 2000 --shard-size 2000 --seed 11 --threads 1 --quiet
```

| Threads | Images | Wall time | Throughput |
|---------|--------|-----------|-----------|
| 1 | 2 000 | 9.44 s | 211.8 img/s |
| 4 | 5 000 | 11.48–13.17 s | 380–436 img/s |

Scaling is ~1.9×, not 4×, for two reasons: the legibility loop makes each sample
CPU-bound rather than I/O-bound (so there is no latency to hide), and the CPU has
only 2 physical cores, so 4 threads are 4 hyperthreads. The throughput target for
this deliverable was 30–80 img/s; the full profile exceeds it by ~5–5.5× on this
machine.

An earlier measurement of the same command gave 222 img/s while other builds and
runs shared the CPU, which is why the idle numbers above are the ones quoted.
Hit `--threads 1` if you want a number that is insensitive to background load.

### Interpretation of the profiles

* `full` is the training profile: every augmentation family can fire, and the
  legibility loop re-renders until the sample lands in the CER band.
* `light` is for large datasets where throughput matters more.
* `none` exists to isolate augmentation cost; it is not a training profile.

The gap between `none` (~1100 img/s) and `full` (~400 img/s) is the cost of
augmentation plus up to 8 legibility attempts per accepted sample.

## 3. Long run: memory and stability

```bash
./build/src/generator/ocr_gen --fonts /usr/share/fonts --out /tmp/leak100k \
    --count 100000 --shard-size 10000 --seed 7 --quiet
```

| Metric | Result |
|--------|--------|
| Images | 100 000 |
| Shards | 10 × 10 000 records |
| Wall time | 451.32 s |
| Throughput | 221.6 img/s (run concurrently with builds; see §2 for idle numbers) |
| RSS first sample | 3.6 MB |
| RSS plateau | 152–160 MB |
| RSS at end | ~153 MB |
| Verification | all 10 shards `structure=ok checksums=ok` |

RSS rises to its plateau as the font cache fills and then stays flat for the
remaining ~90 000 images, which is the point of the run: there is no per-image
growth. Each shard's SHA-256 in `manifest.json` matched on re-read.

## 4. Dataset produced for training

```bash
./build/src/generator/ocr_gen --fonts /usr/share/fonts --out data/train10k \
    --count 10000 --shard-size 10000 --seed 1 --quiet
./build/src/generator/ocr_gen --verify data/train10k --dump data/samples --verify-count 50
```

| Metric | Result |
|--------|--------|
| Records | 10 000 |
| Shard size | 41 205 043 bytes (4 096 px/record + label) |
| SHA-256 | `929ab46ebb188f65496cb0dea9b483dd64f52a1bf59b5113f90fe1f2b949b4a5` |
| Structure / checksums | `ok` / `ok` |
| Mean label length | 8.50 characters |
| Generation time | 23.22 s (430.6 img/s) |
| In band | 92.3 %, mean CER 0.117 |
| Contrast-rejected renders | 5 (0.05 %) |

## 5. Training (Deliverable 2)

*Not yet measured.* The trainer is not implemented; this section will record
per-epoch wall time, samples/s, CTC loss, validation CER and peak RSS for the
10 k and 100 k datasets, on the same machine.

## 6. Inference (Deliverable 4)

*Not yet measured.* Will report per-image latency and throughput for the
single-threaded CPU path on Linux, plus model size and cold-start time, on the
same hardware.

## Reproducing

All generator numbers come from a build produced by the commands in
`README.md` §2.2. Seeds are fixed, so re-running the same command on the same
font tree and dataset options reproduces the same shards byte-for-byte; font
tree differences will change the rendered glyphs and therefore the shards.

# OCRB — Binary OCR Dataset Format v1

**File layout** (all integers little-endian):

```
file := "OCRB" | version:u32 | num_records:u32 | record{num_records}

record := label_len:u32 | label:u8[label_len]
        | h:u32 | w:u32 | ch:u32 | pixels:u8[h*w*ch]
```

- `magic` = the 4 bytes `O C R B` (ASCII), checked first on every re-read.
- `version` = `1`. Bumped on any structural change (not on cosmetic edits).
- Each `record` stores one image+label pair.

**Record fields** (in order, all u32 little-endian unless noted):

| Field | Size | Description |
|-------|------|-------------|
| `label_len` | 4 bytes | Number of bytes in the label string. |
| `label` | `label_len` bytes | Raw UTF-8 label text. No null terminator; length is explicit. |
| `h` | 4 bytes | Image height in pixels. Fixed at `32` for the shipped config. |
| `w` | 4 bytes | Image width in pixels. Fixed at `128` for the shipped config. |
| `ch` | 4 bytes | Number of channels. `1` = grayscale, `3` = RGB. |
| `pixels` | `h * w * ch` bytes | Raw raster data, top-to-bottom, left-to-right. Grayscale values in `[0,255]`. The trainer normalises to `[-1,1]`. |

**Manifest** (`manifest.json` written alongside the shard directory):

```json
{
  "format": "OCRB-shard",
  "version": 1,
  "seed": <u32>,
  "image_width": 128,
  "image_height": 32,
  "image_channels": 1,
  "charset": "<string of used characters>",
  "total_records": <u32>,
  "shards": [
    {
      "file": "shard_00000.ocrb",
      "records": <u32>,
      "bytes": <u64>,
      "sha256": "<hex64>"
    }
  ]
}
```

- `sha256` is the hex-encoded SHA-256 of the shard file's **entire contents** (magic + version + records), computed after the file is fully written.
- `bytes` is the file size in bytes of the corresponding `.ocrb` file.

**Determinism**:

- `--seed` fixes the entire dataset. Per-record seeds are derived via
  `deriveSeed(master_seed, record_index)`, so the same `(seed, options)` pair
  always produces byte-identical shards regardless of thread count or scheduling.
- Records are reordered by index before writing, so the on-disk order matches
  the index sequence exactly.

**Verification** (`--verify`):

1. Re-reads the magic bytes, ensures they are `OCRB`.
2. Reads `version`, ensures it is `1`.
3. Counts records from the file size and validates each record's `label_len`
   does not cause an overflow past the file end.
4. Recomputes SHA-256 over the whole file and compares against the manifest.
5. Optionally dumps N samples as PNM next to `labels.txt` with a visual checklist.

**Format stability**:

- `version` = `1` for the current implementation. Incrementing it is required
  on any change to the on-disk layout (adding/removing fields, changing endianness,
  altering record order, etc.). Cosmetic changes (e.g. severity clamping, font
  coverage thresholds) do **not** bump the version.
- Backwards-incompatible changes require a new version and a migration path.

**Sample file** (PNM, written by `--dump`):

- Written as P5 (grayscale) or P6 (RGB) PNM, raw (no compression).
- Header format: `P5\nw h\n255\n` or `P6\nw h\n255\n` followed by binary pixel data.
- Used only for visual inspection; the authoritative format is the binary `.ocrb`.
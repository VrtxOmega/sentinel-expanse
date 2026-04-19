# Project Sentinel-Expanse
## Technical Manual & Walkthrough
**Version:** v6/v7 (Multi-Dictionary Streaming Engine)
**Author:** RJ Lopez / VRTXOmega
**Origin:** Inspired by Silicon Valley's Pied Piper compression arc
**Status:** Research-grade. Battle-tested on structured log data.

---

## 1. What It Is

Sentinel-Expanse is a custom dictionary-based compression engine built from scratch in Python and C. It is **not** a wrapper around an existing algorithm — it is a complete compression pipeline with its own:

- Binary container format (`.god`, `.sntl`)
- Dictionary training system
- Multi-dictionary chunk routing
- Benchmark harness with a custom performance metric (the Weisman Score)
- Native C implementation for production throughput

The core insight: standard zlib uses a single 32KB sliding window as its compression dictionary. Sentinel trains **multiple dictionaries** from different regions of the target data, then selects the best dictionary per chunk at compression time. For structured, repetitive data like logs, this produces measurably better results.

---

## 2. Architecture Overview

```
Input File
    │
    ├─► DictionaryTrainer
    │       ├── Phase Training (split file into N segments)
    │       ├── Token Frequency Analysis per segment
    │       └── Output: N × 32KB trained dictionaries
    │
    └─► SentinelV6 Compressor
            ├── Read chunk (4MB)
            ├── Probe chunk against all dictionaries (level 1, fast)
            ├── Select best dictionary (lowest compressed probe size)
            ├── Compress chunk with winner (level 6, full)
            ├── Write chunk header: [dict_id | uncompressed_len | compressed_len | adler32]
            └── Repeat until EOF

Output: SNTL binary container
```

---

## 3. The File Format

### 3.1 File Header (v5)
```
Offset  Size  Field
0       4     Magic: "SNTL"
4       4     Version: 5
8       4     Dictionary length (bytes)
12      4     Dictionary Adler-32 checksum
16      N     Dictionary data (raw 32KB)
20+N    ...   Compressed payload (zlib stream with zdict)
```

### 3.2 File Header (v6)
```
Offset  Size  Field
0       4     Magic: "SNTL"
4       4     Version: 6
8       4     Flags (reserved, 0)
12      2     Dictionary count (N)
14      2     Reserved
16      ...   Dictionary table: N × (len[4] + adler32[4] + data[32KB])
...     ...   Chunk stream
```

### 3.3 Chunk Format (v6)
```
Offset  Size  Field
0       1     Dictionary ID (which dict was used)
1       4     Uncompressed length
5       4     Compressed length
9       4     Adler-32 of uncompressed data
13      N     Compressed data
```

Every chunk is independently decompressible. The dictionary ID in the chunk header tells the decompressor which context to restore. This makes the format **seekable** — you can decompress any chunk without reading from the beginning.

---

## 4. The Dictionary Trainer

This is the core innovation of Sentinel-Expanse.

### 4.1 Phase Training
```python
# File is split into N equal segments
seg_size = file_size // num_dicts

for i in range(num_dicts):
    start = i * seg_size
    sample = read(start, 1MB)      # 1MB training window per segment
    dictionary = extract_tokens(sample)
    dictionaries.append(dictionary)
```

Rather than using the first 32KB of the file as the dictionary (the naive approach, and what v5 does), v6 reads a 1MB sample from each region of the file and trains a separate dictionary per region.

### 4.2 Token Extraction
```python
def _extract_tokens(data: bytes) -> bytes:
    tokens = data.split(b' ')
    
    # Score by: length × frequency
    # Longer frequent tokens compress better than short ones
    for tok, count in Counter(tokens).most_common(2000):
        if len(tok) < 3: continue
        score = len(tok) * count
        candidates.append((score, tok))
    
    # Pack highest-scoring tokens into 32KB dictionary
    candidates.sort(by_score, descending)
    ...
```

The scoring function `length × frequency` prioritizes tokens that are both long and common. A 20-character token appearing 50 times scores higher than a 4-character token appearing 100 times, because the longer match provides more compression leverage.

---

## 5. Dictionary Selection (The Probe Heuristic)

At compression time, Sentinel doesn't just assign dictionary 0 to chunk 0, dictionary 1 to chunk 1, etc. It actively probes:

```python
if heuristic == 'probe' and len(dicts) > 1:
    sample = chunk[:16KB]         # Fast probe on first 16KB
    best_sz = infinity
    
    for i, dict in enumerate(dicts):
        # Level 1 = fast, not final quality
        co = zlib.compressobj(level=1, zdict=dict)
        probe_result = co.compress(sample) + co.flush()
        
        if len(probe_result) < best_sz:
            best_sz = len(probe_result)
            best_dict_id = i

# Then compress the full chunk at level 6 with the winner
```

The probe is fast (level 1 on 16KB) so the overhead is small relative to the benefit of picking the right dictionary for the full 4MB chunk compressed at level 6.

---

## 6. The Weisman Score

Named after the fictional Pied Piper engineer from Silicon Valley.

```
Weisman Score = Compression Ratio × Throughput (MB/s)
```

**Why this metric matters:**

Standard benchmarks optimize for ratio (how small?) or speed (how fast?) independently. In practice both matter. A compressor with 10× ratio at 1 MB/s and a compressor with 2× ratio at 100 MB/s both produce Weisman Score of ~10 and ~200 respectively — the faster one wins even with a lower ratio because real-world use values throughput.

The benchmark harness runs 5 repetitions per engine and reports mean ± standard deviation of the Weisman Score:

```
Engine          | Ratio  | Speed (MB/s)  | Weisman    |
----------------|--------|---------------|------------|
ZLIB (Level 6)  | 3.2x   | 180 MB/s      | ~576       |
LZMA (Preset 1) | 4.1x   | 12 MB/s       | ~49        |
Sentinel v5.1   | 3.4x   | 160 MB/s      | ~544       |
Sentinel v6-1D  | 3.5x   | 140 MB/s      | ~490       |
Sentinel v6-4D  | 3.8x   | 120 MB/s      | ~456       |
```

*Approximate values — actual numbers vary by data characteristics.*

LZMA achieves a better ratio but gets destroyed on the Weisman Score because it's so slow. Standard zlib is fast but doesn't adapt to data structure. Sentinel v6 with 4 dictionaries trades some speed for better ratio on structured data.

---

## 7. The C Implementation (sentinel_v6.c / sentinel_v7.c)

The Python implementation is the research vehicle. The C implementation is the production engine.

Key additions in the C version:

### 7.1 Rolling Hash Fingerprinting
```c
typedef struct {
    uint8_t *data;
    size_t len;
    uLong adler;
    uint8_t fingerprint[4096];  // Bloom-like fingerprint table
} Dictionary;

static inline uint32_t roll_hash(uint32_t h, uint8_t old_c, uint8_t new_c) {
    return ((h << 1) | (h >> 31)) ^ new_c;
}
```

Instead of running a full compression probe for dictionary selection (expensive), the C version computes a rolling hash fingerprint of each dictionary and uses Bloom-filter-style lookups to estimate which dictionary best matches a chunk. This eliminates the probe step entirely, recovering the speed overhead of multi-dictionary selection.

### 7.2 Up to 16 Dictionary Slots
The Python version supports up to ~4 dictionaries practically. The C version supports 16 slots (`MAX_DICTS = 16`), enabling much finer-grained region specialization for large files.

### 7.3 Compilation
```bash
# Linux/macOS
gcc -O3 sentinel_v6.c -o sentinel_v6 -lz

# Windows (MSVC)
cl /O2 sentinel_v6.c zlib.lib
```

---

## 8. CLI Reference

### Compress (v5 engine)
```bash
python sentinel_expanse.py compress input.log output.god --mode v5
```

### Compress (v6 multi-dictionary)
```bash
python sentinel_expanse.py compress input.log output.god --mode v6 --dicts 4
```

### Compress (native C engine)
```bash
python sentinel_expanse.py compress input.log output.god --mode native --dicts 4
```

### Decompress (auto-detects v5 or v6)
```bash
python sentinel_expanse.py decompress output.god restored.log
```

### Verify integrity
```bash
python sentinel_expanse.py verify output.god
# Output: PASS or FAIL with error details
```

### Benchmark
```bash
python sentinel_expanse.py benchmark input.log
# Runs 5-rep Weisman evaluation across all engines
```

### GOD TIER mode
```bash
python sentinel_expanse.py god input.log
# Runs benchmark + compresses to .god + verifies bit-perfect round-trip
```

---

## 9. Walkthrough: End-to-End Example

### Step 1: Generate test data
```bash
python benchmark_harness.py
# Generates 10MB of synthetic structured logs
# Format: [timestamp] INFO/ERROR/DEBUG messages with variable fields
```

### Step 2: Compress with v6
```bash
python sentinel_expanse.py compress test_data_god.log test_data.god --mode v6 --dicts 4
```

What happens internally:
1. `DictionaryTrainer.train()` reads the file, splits into 4 equal segments
2. For each segment, reads 1MB sample, runs token frequency analysis
3. Packs highest-scoring tokens into a 32KB dictionary
4. Writes file header: SNTL magic + version + 4 dictionary blocks
5. Reads file in 4MB chunks
6. For each chunk: probes 16KB against all 4 dicts at level 1
7. Selects winner, compresses full chunk at level 6
8. Writes chunk: dict_id + sizes + adler32 + compressed data

### Step 3: Verify
```bash
python sentinel_expanse.py verify test_data.god
# Reads header, loads dictionaries, decompresses all chunks
# Verifies adler32 on each chunk
# Output: PASS
```

### Step 4: Decompress and check bit-perfect
```bash
python sentinel_expanse.py decompress test_data.god restored.log
sha256sum test_data_god.log restored.log
# Both hashes should match
```

The benchmark harness does this automatically and exits with code 1 if hashes don't match.

---

## 10. Data Files Reference

| File | Description |
|------|-------------|
| `sentinel_expanse.py` | Main Python research engine (v5 + v6) |
| `benchmark_harness.py` | Benchmark suite with synthetic log generator |
| `sentinel_v6.c` | C implementation with rolling hash fingerprinting |
| `sentinel_v7.c` | Updated C implementation |
| `test_data.god` | Compressed test data (v5 format) |
| `test_data.god.lzma` | LZMA comparison output |
| `test_data.god.zlib` | ZLIB comparison output |
| `test_data.sntl` | Alternate extension test |
| `out.v6` | v6 compression output |
| `out.v6.restored` | Verified restoration |
| `test_benchmark.log` | Benchmark run results |

---

## 11. Connection to the VERITAS Provenance Layer

The compression research in Project SV directly informed the Omega Brain provenance store design.

The provenance layer originally used zlib compression on SQLite BLOB columns before insertion — the same approach as Sentinel v5's `GodTierEngine`. When the corpus scaled to 200K+ files, this hit a WASM OOM boundary (the `zlib.deflateSync` Array Buffer saturation issue documented in the technical manual).

The fix — dropping in-memory compression, using empty BLOBs, and relying on FTS5 for retrieval — was informed by understanding exactly what the compression was and wasn't buying at scale. That understanding came from building this.

The multi-dictionary approach from Sentinel v6 was not carried forward into the provenance layer because the retrieval pattern (semantic similarity, not byte compression) is fundamentally different. But the research established the baseline for what compression could and couldn't do on structured AI-generated data.

---

## 12. Repository Structure

```
sentinel-expanse/
├── README.md
├── TECHNICAL_MANUAL.md          (this document)
├── LICENSE
│
├── python/
│   ├── sentinel_expanse.py      (Python research engine — v5 + v6)
│   └── benchmark_harness.py     (benchmark suite with synthetic log generator)
│
├── c/
│   ├── sentinel_v6.c            (C production engine)
│   ├── sentinel_v7.c            (C updated engine)
│   └── Makefile                 (build + test targets)
│
├── tests/
│   └── test_roundtrip.py        (bit-perfect verification — v5, v6, native)
│
├── dashboard/
│   ├── app.py                   (Flask server — 13 KB)
│   ├── templates/
│   │   └── dashboard.html       (Dark UI with side-by-side cards — 25 KB)
│   ├── static/css/
│   │   └── style.css            (Engineering dark theme — 13 KB)
│   ├── compressed_sentinel/     (Sentinel v5.1 output files)
│   ├── compressed_zlib/         (ZLIB output files)
│   ├── uploads/                 (Incoming file storage)
│   └── results/
│       └── sessions.json        (Session history persistence)
│
├── docs/
│   ├── weisman_score.md         (metric definition)
│   └── format_spec.md           (binary format reference)
│
├── .github/
│   └── workflows/ci.yml         (GitHub Actions CI pipeline)
│
└── data/
    └── .gitkeep
```

---

## 13. Weisman Score Dashboard

The `dashboard/` directory contains a Flask-based web application for visual, side-by-side compression benchmarking. It is the interactive evaluation frontend for the Sentinel-Expanse engine.

### 13.1 Architecture

```
                          ┌──────────────────────────┐
                          │    dashboard.html (UI)    │
                          │  JetBrains Mono + Inter   │
                          │  Drag-and-drop upload     │
                          │  Side-by-side result cards│
                          │  Session history panel    │
                          └──────────┬───────────────┘
                                     │ POST /upload
                                     ▼
                          ┌──────────────────────────┐
                          │       app.py (Flask)       │
                          │  Port 5555 · 2GB max       │
                          │                            │
                          │  1. Read file_a, file_b    │
                          │  2. ZLIB Level 6 compress  │
                          │  3. Sentinel v5.1 compress │
                          │  4. Compute Weisman Scores │
                          │  5. SHA-256 verify         │
                          │  6. Store + respond JSON   │
                          └──────────────────────────┘
                           │             │            │
                     compressed_zlib/  compressed_sentinel/  results/sessions.json
```

### 13.2 Key Components

| File | Size | Purpose |
|------|------|---------|
| `app.py` | 13 KB | Flask server with embedded `GodTierEngine` (v5.1), ZLIB baseline, Weisman computation, SHA-256 integrity verification, JSON session persistence |
| `dashboard.html` | 25 KB | Dark-themed responsive UI with drag-and-drop upload zones, animated progress bar, side-by-side engine comparison cards, per-file winner badges, session history timeline |
| `style.css` | 13 KB | Engineering dark theme with JetBrains Mono for metrics, Inter for UI text, glassmorphism cards, gold accent highlights |

### 13.3 API Endpoints

| Route | Method | Description |
|-------|--------|-------------|
| `/` | GET | Dashboard UI |
| `/upload` | POST | Accept two files (`file_a`, `file_b`), compress with both engines, return JSON results |
| `/result/<sid>` | GET | Retrieve results for a session by ID |
| `/download/<filename>` | GET | Download a compressed output file |
| `/history` | GET | Return all stored sessions as JSON array |

### 13.4 Compression Pipeline

For each uploaded file, the server runs two independent compression pipelines:

1. **ZLIB (Level 6)** — `zlib.compress(data, level=6)` — raw zlib with no dictionary
2. **Sentinel v5.1 (GodTierEngine)** — streaming compressor with adaptive 32KB dictionary extracted from first bytes of input, Adler-32 chunk integrity, binary `SNTL` container

Each pipeline produces:
- **Compressed output** saved to `compressed_zlib/` or `compressed_sentinel/`
- **Weisman Score** = `Compression Ratio × Throughput (MB/s)`
- **SHA-256 hash** of compressed output
- **Integrity verification** via decompress-and-compare

### 13.5 Running the Dashboard

```bash
cd dashboard
pip install flask          # Only dependency
python app.py              # Starts on http://127.0.0.1:5555
```

The server loads any previously stored sessions from `results/sessions.json` on startup.

### 13.6 UI Workflow

1. **Upload** — Drag-and-drop or browse for two files (up to 2 GB each)
2. **Processing** — Animated progress bar while compression runs server-side
3. **Results** — Side-by-side comparison cards showing:
   - Original size, compressed size, ratio
   - Throughput (MB/s), elapsed time
   - Weisman Score (highlighted)
   - SHA-256 fingerprint
   - Integrity status (✓ PASS / ✗ FAIL)
   - Per-file winner badge (Sentinel vs ZLIB)
4. **Summary** — Overall Weisman totals and session winner
5. **History** — Scrollable timeline of past benchmark sessions

---

## 14. Live Benchmark Results

The following results were obtained from `benchmark_harness.py` on 10 MB of synthetic structured log data (3 template patterns: `Service.Auth`, `Database.Connection`, `Kernel`). Each engine was run 5 times. Values shown are mean ± standard deviation.

```
────────────────────────────────────────────────────────────
WEISMAN EVALUATION (Disk-to-Disk) — 5 Repetitions × 10 MB
────────────────────────────────────────────────────────────
Engine          | Ratio  | Speed (MB/s)  | WEISMAN        |
----------------|--------|---------------|----------------|
ZLIB (Level 6)  | 7.36×  | 115.81 MB/s   | 852.89 (±13.2) |
LZMA (Preset 1) | 7.98×  | 29.89 MB/s    | 238.64 (±13.9) |
SENTINEL (v5)   | 6.98×  | 187.98 MB/s   | 1311.42 (±23.4)|
────────────────────────────────────────────────────────────
VERIFICATION: PASS (Bit-Perfect)
```

### 14.1 Analysis

- **Sentinel v5 wins the Weisman Score by 54%** over ZLIB despite a slightly lower compression ratio (6.98× vs 7.36×). The adaptive dictionary delivers 62% higher throughput (188 vs 116 MB/s), which more than compensates for the ratio delta.
- **LZMA achieves the best ratio** (7.98×) but its 30 MB/s throughput produces the lowest Weisman Score — 3.6× worse than ZLIB and 5.5× worse than Sentinel. This confirms the Weisman metric's utility: it correctly penalizes slow-but-compressive algorithms for real-world use.
- **Standard deviation** is low across all engines (1.5–5.8% relative), indicating stable, repeatable benchmarks suitable for comparative evaluation.

### 14.2 Test Suite Verification

```
tests/test_roundtrip.py::test_god_tier_roundtrip             PASSED
tests/test_roundtrip.py::test_sentinel_v6_python_roundtrip   PASSED
```

Both Python engines pass bit-perfect SHA-256 roundtrip verification on 1 MB synthetic data.

---

*Technical manual generated from source code analysis and live benchmark data.*
*Project Sentinel-Expanse — Copyright 2026 RJ Lopez / VRTXOmega*

<div align="center">

# Ω SENTINEL-Compression

### Multi-Dictionary Streaming Compression Engine

*Part of the VERITAS Sovereign Infrastructure Stack*

---

![Python](https://img.shields.io/badge/Python-3.10+-gold?style=flat-square&logo=python&logoColor=black)
![C](https://img.shields.io/badge/C-Native_Engine-gold?style=flat-square&logo=c&logoColor=black)
![License](https://img.shields.io/badge/License-MIT-gold?style=flat-square)
![Status](https://img.shields.io/badge/Status-Research_Grade-gold?style=flat-square)
![Author](https://img.shields.io/badge/Author-VRTXOmega-gold?style=flat-square)

</div>

---

## ◈ What It Is

Sentinel-Compression is a custom dictionary-based streaming compression engine built from scratch in Python and C. It is **not** a wrapper around an existing algorithm — it is a complete compression pipeline with its own binary container format, dictionary training system, multi-dictionary chunk routing, and a custom performance metric.

Inspired by the Pied Piper compression arc from Silicon Valley, this project started as a research exercise asking: *what actually makes a compression algorithm good?*

The answer informed the design of the [Omega Brain MCP](https://github.com/VrtxOmega/omega-brain-mcp) provenance layer.

---

## ◈ The Core Insight

Standard zlib uses a single 32KB sliding window as its compression dictionary — trained on the first bytes of a file regardless of what's in the rest of it.

Sentinel-Compression trains **multiple dictionaries** from different regions of the target data, then selects the best dictionary per chunk at compression time. For structured, repetitive data like AI-generated logs and provenance records, this produces measurably better results without sacrificing throughput.

---

## ◈ Architecture

```
Input File
    │
    ├─► DictionaryTrainer
    │       ├── Split file into N equal segments
    │       ├── Read 1MB training sample per segment
    │       ├── Token frequency scoring: length × count
    │       └── Pack top tokens into N × 32KB dictionaries
    │
    └─► SentinelV6 Compressor
            ├── Read chunk (4MB)
            ├── Probe 16KB against all N dicts at level 1
            ├── Select best dictionary (lowest probe size)
            ├── Compress full chunk at level 6 with winner
            ├── Write: [dict_id | ulen | clen | adler32 | data]
            └── Repeat until EOF
                        │
                        ▼
              SNTL Binary Container
              (seekable, integrity-verified)
```

---

## ◈ Engines

| Engine | Language | Dictionaries | Selection |
|--------|----------|-------------|-----------|
| `GodTierEngine` v5.1 | Python | 1 (first 32KB) | Static |
| `SentinelV6` | Python | Up to 4 | Level-1 probe |
| `sentinel_v6.c` | C | Up to 16 | Rolling hash fingerprint |
| `sentinel_v7.c` | C | Up to 16 | Hardened build |

The C implementation replaces the Python probe with a rolling hash fingerprint — a Bloom-filter-style lookup that selects dictionaries without running a compression pass. This eliminates the probe overhead entirely.

---

## ◈ The Weisman Score

Named after the fictional Pied Piper engineer.

```
Weisman Score = Compression Ratio × Throughput (MB/s)
```

Standard benchmarks optimize for ratio or speed in isolation. The Weisman Score treats them as a tradeoff — because in practice, a 3.8× compressor at 120 MB/s outperforms a 5× compressor at 8 MB/s for real workloads.

The benchmark harness runs **5 repetitions per engine** and reports mean ± standard deviation.

---

## ◈ File Format

### v5 Header
```
[SNTL][version:4][dict_len:4][adler32:4][dictionary:32KB][payload...]
```

### v6 Header
```
[SNTL][version:4][flags:4][dict_count:2][reserved:2]
[N × (dict_len:4 + adler32:4 + dict_data:32KB)]
[chunk stream...]
```

### Chunk Format (v6)
```
[dict_id:1][uncompressed_len:4][compressed_len:4][adler32:4][data...]
```

Every chunk is **independently decompressible**. The format is seekable — any chunk can be restored without reading from the beginning.

---

## ◈ Quick Start

```bash
# Python — compress with 4 dictionaries
python sentinel_expanse.py compress input.log output.god --mode v6 --dicts 4

# Python — decompress (auto-detects v5 or v6)
python sentinel_expanse.py decompress output.god restored.log

# Python — verify integrity
python sentinel_expanse.py verify output.god

# Python — run Weisman benchmark
python sentinel_expanse.py benchmark input.log

# GOD TIER — benchmark + compress + verify round-trip
python sentinel_expanse.py god input.log
```

```bash
# C — compile
gcc -O3 c/sentinel_v6.c -o sentinel_v6 -lz

# C — compress with 4 dicts
./sentinel_v6 compress input.log output.god 4

# C — decompress
./sentinel_v6 decompress output.god restored.log
```

---

## ◈ Benchmark Harness

```bash
python benchmark_harness.py
```

Generates 10MB of synthetic structured logs (auth events, DB timeouts, kernel syscalls) and runs a full Weisman evaluation comparing ZLIB, LZMA, and Sentinel v5 with SHA-256 bit-perfect verification.

---

## ◈ Weisman Score Dashboard

Here's what was built at `C:\Veritas_Lab\sentinel-expanse\dashboard\`:

```text
dashboard/
├── app.py                        # Flask server (13 KB)
├── templates/dashboard.html       # Dark UI with side-by-side cards (25 KB)
├── static/css/style.css           # Engineering dark theme (13 KB)
├── compressed_sentinel/           # Sentinel v5.1 output files
├── compressed_zlib/               # ZLIB output files
├── uploads/                       # Incoming file storage
└── results/sessions.json         # Session history persistence
```

**What it does:**

1. **Upload** — drag-and-drop or browse for two files (up to 2 GB each)
2. **Compresses both** with Sentinel v5.1 (GodTierEngine) and ZLIB Level 6
3. **Scores each** using `Weisman Score = Ratio × Speed (MB/s)` — higher is better, rewards both compression ratio AND throughput
4. **Verifies** bit-perfect integrity after decompression (SHA-256)
5. **Displays** side-by-side per-file results: ratio, speed, Weisman, time, hash, integrity, download link
6. **Session history** persists across restarts via JSON
7. **Overall winner** computed as sum of Weisman Scores across both files

**To start the server:**
```bash
cd C:\Veritas_Lab\sentinel-expanse\dashboard
python app.py
```

---
## ◈ Repository Structure

```
sentinel-expanse/
├── README.md
├── TECHNICAL_MANUAL.md          — full architecture + format spec
├── python/
│   ├── sentinel_expanse.py      — research engine (v5 + v6)
│   └── benchmark_harness.py     — Weisman evaluation suite
├── c/
│   ├── sentinel_v6.c            — native engine, rolling hash fingerprint
│   ├── sentinel_v7.c            — hardened build
│   └── Makefile
└── tests/
    └── test_roundtrip.py        — bit-perfect verification
```

---

## ◈ Connection to the VERITAS Stack

The provenance layer in [Omega Brain MCP](https://github.com/VrtxOmega/omega-brain-mcp) originally used zlib compression on SQLite BLOB columns before insertion — the same pattern as Sentinel v5's `GodTierEngine`. At scale (200K+ file corpus) this saturated the WASM Array Buffer and caused fatal OOM failures.

Understanding exactly what the compression was and wasn't buying at that scale — built directly from this research — informed the decision to remove it in favor of FTS5 retrieval. The result: corpus scan time dropped from >60s (OOM fail) to ~7.5s stable.

The research wasn't wasted. It produced the understanding that changed the architecture.

---

## ◈ VERITAS Ecosystem

| Project | Description |
|---------|-------------|
| [omega-brain-mcp](https://github.com/VrtxOmega/omega-brain-mcp) | Provenance RAG + 10-gate VERITAS pipeline |
| [ollama-mcp](https://github.com/VrtxOmega/ollama-omega) | Sovereign Ollama bridge for MCP-compatible IDEs |
| [veritas-portfolio](https://vrtxomega.github.io/veritas-portfolio) | Live portfolio |

---

<div align="center">

*Built sovereign. Verified trustless.*

**[VRTXOmega](https://github.com/VrtxOmega)** · Illinois · 2026

</div>

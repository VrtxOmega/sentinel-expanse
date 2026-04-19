# Sentinel-Expanse

A custom dictionary-based streaming compression engine, built from scratch in Python and C.

Inspired by the Pied Piper compression arc from Silicon Valley — specifically the question: *what actually makes a compression algorithm good?*

---

## What It Does

Standard zlib compression uses a single 32KB dictionary trained on the first bytes of a file. Sentinel-Expanse takes a different approach: it splits the input into N segments, trains a separate dictionary per segment using token frequency analysis, then at compression time probes each chunk against all dictionaries and picks the best one.

For structured, repetitive data like logs — which is what the VERITAS provenance layer produces — this measurably improves compression ratio without significant speed loss.

---

## The Weisman Score

Named after the fictional Pied Piper engineer.

```
Weisman Score = Compression Ratio × Throughput (MB/s)
```

Most benchmarks optimize for ratio or speed independently. The Weisman Score treats them as a tradeoff — a 10× compressor at 1 MB/s scores the same as a 2× compressor at 5 MB/s. In practice, throughput usually matters more than an extra half-point of ratio.

The benchmark harness runs 5 repetitions per engine and reports mean ± standard deviation.

---

## Engines

**v5 — GodTierEngine (Python)**
Dictionary compression using the first 32KB of input as the training dictionary. Baseline implementation. Level 4 compression offers the optimal Weisman Score for structured log data.

**v6 — SentinelV6 (Python)**
Multi-dictionary engine. Trains N dictionaries from N equal segments of the input. Probes first 16KB of each chunk at level 1 to select the best dictionary, then compresses the full chunk at level 6.

**v6/v7 Native (C)**
Production engine. Replaces the Python probe heuristic with rolling hash fingerprinting — a Bloom-filter-style lookup that selects the best dictionary without running a full compression probe. Supports up to 16 dictionary slots. Compiles with gcc or MSVC.

---

## File Format

Both versions use the `SNTL` magic header with versioned binary format. Every dictionary and every chunk carries an Adler-32 checksum. The format is seekable — any chunk can be decompressed independently without reading from the beginning.

See `TECHNICAL_MANUAL.md` for the full format specification.

---

## Quick Start

### Python
```bash
# Compress
python sentinel_expanse.py compress input.log output.god --mode v6 --dicts 4

# Decompress (auto-detects v5 or v6)
python sentinel_expanse.py decompress output.god restored.log

# Verify integrity
python sentinel_expanse.py verify output.god

# Benchmark (Weisman evaluation, 5 reps)
python sentinel_expanse.py benchmark input.log

# GOD TIER mode — benchmark + compress + verify round-trip
python sentinel_expanse.py god input.log
```

### C
```bash
gcc -O3 c/sentinel_v6.c -o sentinel_v6 -lz
./sentinel_v6 compress input.log output.god 4
./sentinel_v6 decompress output.god restored.log
```

### Run the benchmark harness
```bash
python benchmark_harness.py
# Generates 10MB of synthetic structured logs
# Runs Weisman evaluation across ZLIB, LZMA, and Sentinel v5
# Verifies bit-perfect round-trip with SHA-256
```

---

## Project Structure

```
sentinel-expanse/
├── README.md
├── TECHNICAL_MANUAL.md       — full architecture and format spec
├── python/
│   ├── sentinel_expanse.py   — Python research engine (v5 + v6)
│   └── benchmark_harness.py  — benchmark suite with synthetic data generator
├── c/
│   ├── sentinel_v6.c         — C engine with rolling hash fingerprinting
│   ├── sentinel_v7.c         — updated C engine
│   └── Makefile
└── tests/
    └── test_roundtrip.py     — bit-perfect verification
```

---

## Background

This project started as a research exercise and ended up informing the design of the Omega Brain provenance layer in the VERITAS ecosystem. The provenance layer originally used zlib compression on SQLite BLOB columns — the same pattern as Sentinel v5. At scale (200K+ files) this hit a WASM OOM boundary. Understanding exactly what the compression was and wasn't buying at that scale — knowledge built directly from this project — informed the decision to remove it in favor of FTS5 retrieval.

The research wasn't wasted. It produced the understanding that changed the architecture.

---

## Author

RJ Lopez / VRTXOmega  
[github.com/VRTXOmega](https://github.com/VRTXOmega)

---

*Copyright 2026 RJ Lopez. MIT License.*

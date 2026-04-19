# Weisman Score

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

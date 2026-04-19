# Binary Container Format (`.god`, `.sntl`)

## 1. File Header (v5)
```
Offset  Size  Field
0       4     Magic: "SNTL"
4       4     Version: 5
8       4     Dictionary length (bytes)
12      4     Dictionary Adler-32 checksum
16      N     Dictionary data (raw 32KB)
20+N    ...   Compressed payload (zlib stream with zdict)
```

## 2. File Header (v6)
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

## 3. Chunk Format (v6)
```
Offset  Size  Field
0       1     Dictionary ID (which dict was used)
1       4     Uncompressed length
5       4     Compressed length
9       4     Adler-32 of uncompressed data
13      N     Compressed data
```

Every chunk is independently decompressible. The dictionary ID in the chunk header tells the decompressor which context to restore. This makes the format **seekable** — you can decompress any chunk without reading from the beginning.

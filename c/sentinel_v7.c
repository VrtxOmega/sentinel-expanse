/*
 * SENTINEL-EXPANSE v7.1 (NATIVE RESEARCH)
 * 
 * Architecture:
 * - Chunked Streaming (4MB Blocks)
 * - Parallel Compression (OpenMP)
 * - Multi-Dictionary (16 slots)
 * - Heuristic: Rolling Hash Sampling (Fast & Predictive)
 * - Format: Big-Endian, Portable, Fail-Fast
 *
 * Compilation:
 *   gcc -O3 -fopenmp sentinel_v7.c -o sentinel_v7 -lz
 *   cl /O2 /openmp sentinel_v7.c zlib.lib
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h> // for htonl
#include <windows.h>
#include <io.h>
#define setmode _setmode
#else
#include <arpa/inet.h>
#include <unistd.h>
#define setmode(fd, mode)
#define O_BINARY 0
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#include "zlib.h"

// CONSTANTS
#define MAGIC_V7 "SNTL"
#define VER_7 7
#define DICT_SIZE (32 * 1024)
#define CHUNK_SIZE (4 * 1024 * 1024)
#define PROBE_SIZE (16 * 1024) // 16KB Sample
#define MAX_DICTS 16
#define HASH_WINDOW 8
#define PREFETCH_CHUNKS 8 // Parallelism depth

// TYPES
typedef struct {
    uint8_t *data;
    size_t len;
    uLong adler;
    // Heuristic: Rolling Hash Fingerprint (4096-bit Bloom-like table)
    uint8_t fingerprint[8192]; 
} Dictionary;

typedef struct {
    uint8_t *raw;
    size_t raw_len;
    uint8_t *compressed;
    size_t compressed_len;
    uLong adler;
    int dict_id;
} Chunk;

// GLOBALS
Dictionary dicts[MAX_DICTS];
int dict_count = 0;
int comp_level = 6;
int mem_level = 8;
int strategy = Z_DEFAULT_STRATEGY;

// HELPER: Timer
double get_time() {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

// HELPER: Rolling Hash (Rabin-Karp Style, very simple for speed)
static inline uint32_t roll_hash(uint32_t h, uint8_t old_c, uint8_t new_c) {
    // Determine a simple rolling scheme. 
    // H = ((H - old * D^(w-1)) * D + new) % M
    // Simply summing or XORing is not enough for order.
    // We use a shift-XOR mix for speed.
    return ((h << 1) | (h >> 31)) ^ new_c;
}

// HELPER: Compute Fingerprint
void compute_fingerprint(Dictionary *d) {
    memset(d->fingerprint, 0, 8192);
    if (d->len < HASH_WINDOW) return;
    
    uint32_t h = 0;
    for (int i=0; i<HASH_WINDOW; i++) h = roll_hash(h, 0, d->data[i]);
    
    d->fingerprint[h & 0x1FFF] = 1;
    
    for (size_t i=HASH_WINDOW; i < d->len; i++) {
        h = roll_hash(h, d->data[i-HASH_WINDOW], d->data[i]);
        d->fingerprint[h & 0x1FFF] = 1;
    }
}

// HELPER: Select Dictionary (Rolling Hash Intersection)
int select_dict(const uint8_t *chunk, size_t len) {
    if (dict_count <= 1) return 0;
    size_t sample = (len < PROBE_SIZE) ? len : PROBE_SIZE;
    if (sample < HASH_WINDOW) return 0;
    
    int best_id = 0;
    int max_hits = -1;
    
    // Evaluate chunk fingerprint on the fly against dicts
    // To minimize work, we compute chunk hash once and check all dicts
    
    int scores[MAX_DICTS] = {0};
    
    uint32_t h = 0;
    for (int i=0; i<HASH_WINDOW; i++) h = roll_hash(h, 0, chunk[i]);
    
    // Check initial
    for(int d=0; d<dict_count; d++) if(dicts[d].fingerprint[h & 0x1FFF]) scores[d]++;
    
    for (size_t i=HASH_WINDOW; i < sample; i++) {
        h = roll_hash(h, chunk[i-HASH_WINDOW], chunk[i]);
        uint32_t idx = h & 0x1FFF;
        // Unroll?
        for(int d=0; d<dict_count; d++) {
            // Branchless add?
            scores[d] += dicts[d].fingerprint[idx];
        }
    }
    
    for (int d=0; d<dict_count; d++) {
        if (scores[d] > max_hits) {
            max_hits = scores[d];
            best_id = d;
        }
    }
    
    return best_id;
}

// IO HELPERS
void write_u32(FILE *f, uint32_t v) { v = htonl(v); fwrite(&v, 1, 4, f); }
void write_u16(FILE *f, uint16_t v) { v = htons(v); fwrite(&v, 1, 2, f); }
uint32_t read_u32(FILE *f) { uint32_t v; if(fread(&v, 1, 4, f)<4) return 0; return ntohl(v); }
uint16_t read_u16(FILE *f) { uint16_t v; if(fread(&v, 1, 2, f)<2) return 0; return ntohs(v); }

// COMMAND: Load Dicts (Training Mode)
void load_dicts(const char *path, int count, int sample_mode) {
    if (count > MAX_DICTS) count = MAX_DICTS;
    dict_count = count;
    
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Training file not found\n"); exit(1); }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long seg = fsize / count;
    
    for (int i=0; i<count; i++) {
        dicts[i].data = malloc(DICT_SIZE);
        
        // Sampling Strategy
        if (sample_mode == 1) { // Sampled (Multiple offsets)
            // Read 4 chunks of 8KB
            size_t off = 0;
            for(int j=0; j<4; j++) {
                fseek(f, (i*seg) + (j * (seg/4)), SEEK_SET);
                size_t r = fread(dicts[i].data + off, 1, DICT_SIZE/4, f);
                off += r;
            }
            dicts[i].len = off;
        } else { // Greedy (Start of Segment)
            fseek(f, i*seg, SEEK_SET);
            dicts[i].len = fread(dicts[i].data, 1, DICT_SIZE, f);
        }
        
        // Ensure checksum
        dicts[i].adler = adler32(0L, Z_NULL, 0);
        dicts[i].adler = adler32(dicts[i].adler, dicts[i].data, dicts[i].len);
        compute_fingerprint(&dicts[i]);
    }
    fclose(f);
}

// COMMAND: Process Chunk (Parallel Worker)
void process_chunk(Chunk *c) {
    // Select Dict
    c->dict_id = select_dict(c->raw, c->raw_len);
    
    // Checksum
    c->adler = adler32(0L, Z_NULL, 0);
    c->adler = adler32(c->adler, c->raw, c->raw_len);
    
    // Compress
    z_stream strm = {0};
    deflateInit2(&strm, comp_level, Z_DEFLATED, -15, mem_level, strategy);
    
    if (dicts[c->dict_id].len > 0) {
        deflateSetDictionary(&strm, dicts[c->dict_id].data, dicts[c->dict_id].len);
    }
    
    // Safety Bound
    uLong bound = deflateBound(&strm, c->raw_len);
    c->compressed = malloc(bound);
    
    strm.next_in = c->raw; 
    strm.avail_in = c->raw_len;
    strm.next_out = c->compressed;
    strm.avail_out = bound;
    
    deflate(&strm, Z_FINISH);
    c->compressed_len = strm.total_out;
    deflateEnd(&strm);
}

// COMMAND: Compress
void compress_file(const char *inpath, const char *outpath) {
    FILE *fin = fopen(inpath, "rb");
    FILE *fout = fopen(outpath, "wb");
    if (!fin || !fout) return;
    
    // Header
    // Magic(4) Ver(4) Flags(4) DictCount(2) Reserved(2)
    fwrite(MAGIC_V7, 1, 4, fout);
    write_u32(fout, VER_7);
    write_u32(fout, 0); 
    write_u16(fout, dict_count);
    write_u16(fout, 0); 
    
    for (int i=0; i<dict_count; i++) {
        write_u32(fout, dicts[i].len);
        write_u32(fout, dicts[i].adler);
        fwrite(dicts[i].data, 1, dicts[i].len, fout);
    }
    
    Chunk chunks[PREFETCH_CHUNKS]; // Circular buffer or batch? Batch is easier.
    
    while (1) {
        int loaded = 0;
        
        // Read Batch
        for (int i=0; i<PREFETCH_CHUNKS; i++) {
            chunks[i].raw = malloc(CHUNK_SIZE);
            chunks[i].raw_len = fread(chunks[i].raw, 1, CHUNK_SIZE, fin);
            if (chunks[i].raw_len == 0) {
                free(chunks[i].raw);
                break;
            }
            loaded++;
        }
        
        if (loaded == 0) break;
        
        // Parallel Compress
        #pragma omp parallel for
        for (int i=0; i<loaded; i++) {
            process_chunk(&chunks[i]);
        }
        
        // Write Batch in Order
        for (int i=0; i<loaded; i++) {
            fputc(chunks[i].dict_id, fout);
            write_u32(fout, chunks[i].raw_len);
            write_u32(fout, chunks[i].compressed_len);
            write_u32(fout, chunks[i].adler);
            fwrite(chunks[i].compressed, 1, chunks[i].compressed_len, fout);
            
            free(chunks[i].raw);
            free(chunks[i].compressed);
        }
    }
    
    fclose(fin);
    fclose(fout);
}

// COMMAND: Decompress / Verify
void decompress_file(const char *inpath, const char *outpath, int verify_only) {
    FILE *fin = fopen(inpath, "rb");
    FILE *fout = verify_only ? NULL : fopen(outpath, "wb");
    
    char magic[4]; fread(magic, 1, 4, fin);
    if (memcmp(magic, MAGIC_V7, 4)) { fprintf(stderr, "Invalid Magic\n"); exit(1); }
    
    read_u32(fin); // Ver
    read_u32(fin); // Flags
    int dc = read_u16(fin);
    read_u16(fin); // Res
    
    Dictionary *local_dicts = malloc(dc * sizeof(Dictionary));
    for (int i=0; i<dc; i++) {
        uint32_t len = read_u32(fin);
        uint32_t sum = read_u32(fin);
        local_dicts[i].data = malloc(len);
        local_dicts[i].len = len;
        fread(local_dicts[i].data, 1, len, fin);
        
        uLong calc = adler32(0L, Z_NULL, 0);
        calc = adler32(calc, local_dicts[i].data, len);
        if (calc != sum) { fprintf(stderr, "Dict %d Corrupt\n", i); exit(1); }
    }
    
    // Buffers for Decompression
    // To handle arbitrary sizes safely, we realloc.
    size_t c_cap = CHUNK_SIZE + 4096;
    uint8_t *cbuf = malloc(c_cap);
    uint8_t *ubuf = malloc(CHUNK_SIZE); 
    
    while (1) {
        int did = fgetc(fin);
        if (did == EOF) break;
        
        uint32_t ulen = read_u32(fin);
        uint32_t clen = read_u32(fin);
        uint32_t usum = read_u32(fin);
        
        // Safe Read
        if (clen > c_cap) {
            c_cap = clen + 4096;
            cbuf = realloc(cbuf, c_cap);
        }
        if (fread(cbuf, 1, clen, fin) != clen) { fprintf(stderr, "Truncated Chunk\n"); exit(1); }
        
        z_stream strm = {0};
        inflateInit2(&strm, -15);
        if (local_dicts[did].len > 0) inflateSetDictionary(&strm, local_dicts[did].data, local_dicts[did].len);
        
        strm.next_in = cbuf;
        strm.avail_in = clen;
        strm.next_out = ubuf;
        strm.avail_out = CHUNK_SIZE;
        
        int ret = inflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) { fprintf(stderr, "Inflate Error %d\n", ret); exit(1); }
        
        if (strm.total_out != ulen) { fprintf(stderr, "Size Mismatch\n"); exit(1); }
        
        uLong calc = adler32(0L, Z_NULL, 0);
        calc = adler32(calc, ubuf, ulen);
        if (calc != usum) { fprintf(stderr, "Checksum Mismatch\n"); exit(1); }
        
        if (fout) fwrite(ubuf, 1, ulen, fout);
        inflateEnd(&strm);
    }
    
    free(cbuf);
    free(ubuf);
    if (fout) fclose(fout);
    fclose(fin);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: sentinel_v7 <cmd> <in> <out> [dicts] [level]\n");
        printf("Cmds: compress, decompress, verify, benchmark\n");
        return 1;
    }
    setmode(fileno(stdin), O_BINARY);
    setmode(fileno(stdout), O_BINARY);
    
    const char *cmd = argv[1];
    const char *inp = argv[2];
    const char *out = argv[3];
    int dc = (argc > 4) ? atoi(argv[4]) : 4;
    if (argc > 5) comp_level = atoi(argv[5]);
    
    if (strcmp(cmd, "compress") == 0) {
        load_dicts(inp, dc, 1);
        double t0 = get_time();
        compress_file(inp, out);
        double dt = get_time() - t0;
        printf("Completed in %.2fs\n", dt);
        
    } else if (strcmp(cmd, "benchmark") == 0) {
        load_dicts(inp, dc, 1);
        printf("Benchmarking %s (N=5)...\n", inp);
        for(int i=0; i<5; i++) {
             double t0 = get_time();
             compress_file(inp, "benchmark.tmp");
             double dt = get_time() - t0;
             long sz = 0;
             FILE *f = fopen("benchmark.tmp", "rb"); fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f);
             long osz = 0;
             f = fopen(inp, "rb"); fseek(f, 0, SEEK_END); osz = ftell(f); fclose(f);
             
             double mb = (osz/1024.0/1024.0)/dt;
             double ratio = (double)osz/sz;
             printf("Run %d: %.2f MB/s | %.2fx Ratio | Weisman: %.0f\n", i, mb, ratio, mb*ratio);
        }
        
    } else if (strcmp(cmd, "decompress") == 0) {
        decompress_file(inp, out, 0);
    } else if (strcmp(cmd, "verify") == 0) {
        decompress_file(inp, NULL, 1);
        printf("PASS\n");
    }
    return 0;
}

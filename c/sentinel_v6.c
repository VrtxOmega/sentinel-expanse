/*
 * SENTINEL-EXPANSE v7 (NATIVE HARDENED)
 * 
 * Architecture:
 * - Chunked Streaming (4MB Blocks)
 * - Multi-Dictionary (16 slots)
 * - Heuristic: Rolling Hash Sampling (Fast & Predictive)
 * - Format: Big-Endian, Portable, Fail-Fast
 *
 * Compilation:
 *   gcc -O3 sentinel_v6.c -o sentinel_v6 -lz
 *   cl /O2 sentinel_v6.c zlib.lib
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>

#ifdef _WIN32
#include <winsock2.h> // for htonl
#include <io.h>
#define setmode _setmode
#else
#include <arpa/inet.h>
#include <unistd.h>
#define setmode(fd, mode)
#define O_BINARY 0
#endif

#include "zlib.h"

// CONSTANTS
#define MAGIC_V6 "SNTL"
#define VER_6 6
#define DICT_SIZE (32 * 1024)
#define CHUNK_SIZE (4 * 1024 * 1024)
#define PROBE_SIZE (16 * 1024) // 16KB Sample
#define MAX_DICTS 16
#define HASH_WINDOW 4

// TYPES
typedef struct {
    uint8_t *data;
    size_t len;
    uLong adler;
    // Heuristic: Rolling Hash Fingerprint (4096-bit Bloom-like table)
    uint8_t fingerprint[4096]; 
} Dictionary;

// GLOBALS
Dictionary dicts[MAX_DICTS];
int dict_count = 0;

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
    memset(d->fingerprint, 0, 4096);
    if (d->len < HASH_WINDOW) return;
    
    uint32_t h = 0;
    for (int i=0; i<HASH_WINDOW; i++) h = roll_hash(h, 0, d->data[i]);
    
    d->fingerprint[h & 0xFFF] = 1;
    
    for (size_t i=HASH_WINDOW; i < d->len; i++) {
        h = roll_hash(h, d->data[i-HASH_WINDOW], d->data[i]);
        d->fingerprint[h & 0xFFF] = 1;
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
    for(int d=0; d<dict_count; d++) if(dicts[d].fingerprint[h & 0xFFF]) scores[d]++;
    
    for (size_t i=HASH_WINDOW; i < sample; i++) {
        h = roll_hash(h, chunk[i-HASH_WINDOW], chunk[i]);
        uint32_t idx = h & 0xFFF;
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

// COMMAND: Load Dicts
void load_dicts(const char *path, int count) {
    if (count > MAX_DICTS) count = MAX_DICTS;
    dict_count = count;
    
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Training file not found\n"); exit(1); }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long seg = fsize / count;
    
    for (int i=0; i<count; i++) {
        dicts[i].data = malloc(DICT_SIZE);
        fseek(f, i*seg, SEEK_SET);
        dicts[i].len = fread(dicts[i].data, 1, DICT_SIZE, f);
        dicts[i].adler = adler32(0L, Z_NULL, 0);
        dicts[i].adler = adler32(dicts[i].adler, dicts[i].data, dicts[i].len);
        compute_fingerprint(&dicts[i]);
    }
    fclose(f);
}

// COMMAND: Compress
void compress_file(const char *inpath, const char *outpath) {
    FILE *fin = fopen(inpath, "rb");
    FILE *fout = fopen(outpath, "wb");
    if (!fin || !fout) return;
    
    fwrite(MAGIC_V6, 1, 4, fout);
    write_u32(fout, VER_6);
    write_u32(fout, 0); // Flags
    write_u16(fout, dict_count);
    write_u16(fout, 0); // Reserved
    
    for (int i=0; i<dict_count; i++) {
        write_u32(fout, dicts[i].len);
        write_u32(fout, dicts[i].adler);
        fwrite(dicts[i].data, 1, dicts[i].len, fout);
    }
    
    uint8_t *inbuf = malloc(CHUNK_SIZE);
    
    // Safe Output Buffer: 4MB + 0.1% + 12 (zlib bound)
    size_t out_cap = CHUNK_SIZE + (CHUNK_SIZE >> 10) + 1024; 
    uint8_t *outbuf = malloc(out_cap);
    
    while (1) {
        size_t rlen = fread(inbuf, 1, CHUNK_SIZE, fin);
        if (rlen == 0) break;
        
        int did = select_dict(inbuf, rlen);
        
        z_stream strm = {0};
        deflateInit2(&strm, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        if (dicts[did].len > 0) deflateSetDictionary(&strm, dicts[did].data, dicts[did].len);
        
        strm.next_in = inbuf;
        strm.avail_in = rlen;
        strm.next_out = outbuf;
        strm.avail_out = out_cap;
        
        deflate(&strm, Z_FINISH);
        size_t clen = strm.total_out;
        deflateEnd(&strm);
        
        uLong crc = adler32(0L, Z_NULL, 0);
        crc = adler32(crc, inbuf, rlen);
        
        fputc(did, fout);
        write_u32(fout, rlen);
        write_u32(fout, clen);
        write_u32(fout, crc);
        fwrite(outbuf, 1, clen, fout);
    }
    
    free(inbuf);
    free(outbuf);
    fclose(fin);
    fclose(fout);
}

// COMMAND: Decompress / Verify
void decompress_file(const char *inpath, const char *outpath, int verify_only) {
    FILE *fin = fopen(inpath, "rb");
    FILE *fout = verify_only ? NULL : fopen(outpath, "wb");
    
    char magic[4]; fread(magic, 1, 4, fin);
    if (memcmp(magic, MAGIC_V6, 4)) { fprintf(stderr, "Invalid Magic\n"); exit(1); }
    
    read_u32(fin); // Ver
    read_u32(fin); // Flags
    int dc = read_u16(fin);
    read_u16(fin); // Res
    
    // Load Dicts from Stream
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
    
    uint8_t *cbuf = malloc(CHUNK_SIZE + 4096); // Compressed buffer
    uint8_t *ubuf = malloc(CHUNK_SIZE);        // Uncompressed buffer
    
    while (1) {
        int did = fgetc(fin);
        if (did == EOF) break;
        
        uint32_t ulen = read_u32(fin);
        uint32_t clen = read_u32(fin);
        uint32_t usum = read_u32(fin);
        
        if (clen > CHUNK_SIZE + 4096) { fprintf(stderr, "Chunk too large\n"); exit(1); }
        if (fread(cbuf, 1, clen, fin) != clen) { fprintf(stderr, "Truncated Chunk\n"); exit(1); }
        
        z_stream strm = {0};
        inflateInit2(&strm, -15);
        
        if (local_dicts[did].len > 0) {
             inflateSetDictionary(&strm, local_dicts[did].data, local_dicts[did].len);
        }
        
        strm.next_in = cbuf;
        strm.avail_in = clen;
        strm.next_out = ubuf;
        strm.avail_out = CHUNK_SIZE;
        
        int ret = inflate(&strm, Z_FINISH);
        if (ret == Z_NEED_DICT) {
             fprintf(stderr, "Inflate Needs Dict (Logic Error)\n"); exit(1);
        }
        if (ret != Z_STREAM_END) {
             fprintf(stderr, "Inflate Error: %d\n", ret); exit(1);
        }
        
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
    // Cleanup dicts...
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: sentinel_v6 <compress|decompress|verify> <input> <output> [dict_count]\n");
        return 1;
    }
    setmode(fileno(stdin), O_BINARY);
    setmode(fileno(stdout), O_BINARY);
    
    const char *cmd = argv[1];
    const char *inp = argv[2];
    const char *out = argv[3];
    int dc = (argc > 4) ? atoi(argv[4]) : 4;
    
    if (strcmp(cmd, "compress") == 0) {
        load_dicts(inp, dc);
        compress_file(inp, out);
    } else if (strcmp(cmd, "decompress") == 0) {
        decompress_file(inp, out, 0);
    } else if (strcmp(cmd, "verify") == 0) {
        decompress_file(inp, NULL, 1);
        printf("PASS\n");
    }
    return 0;
}

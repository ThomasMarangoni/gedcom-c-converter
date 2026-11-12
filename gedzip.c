/**
 * A minimal store-only ZIP archive writer; see gedzip.h
 *
 * Format reference: PKWARE APPNOTE.TXT, sections 4.3.7 (local file
 * header), 4.3.12 (central directory header), and 4.3.16 (end of
 * central directory record).
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gedzip.h"

struct gedzip_entry {
    char *name;
    unsigned long crc, size, offset;
};

struct GedZip_t {
    FILE *out;
    unsigned long written; // bytes written so far
    unsigned long crctable[256];
    unsigned short dostime, dosdate;
    struct gedzip_entry *entries;
    size_t count, cap;
    int failed;
};

GedZip *gedzip_create(FILE *out) {
    GedZip *z = calloc(1, sizeof(GedZip));
    z->out = out;

    // CRC-32 as used by ZIP: reflected polynomial 0xEDB88320
    for(unsigned long i=0; i<256; i+=1) {
        unsigned long c = i;
        for(int k=0; k<8; k+=1)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        z->crctable[i] = c;
    }

    time_t now = time(0);
    struct tm *t = localtime(&now);
    if (t && t->tm_year >= 80) {
        z->dostime = (t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec >> 1);
        z->dosdate = ((t->tm_year - 80) << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday;
    } else {
        z->dosdate = (1 << 5) | 1; // 1980-01-01
    }
    return z;
}

static unsigned long gedzip_crc(GedZip *z, unsigned long crc, const unsigned char *data, size_t length) {
    crc ^= 0xFFFFFFFFUL;
    for(size_t i=0; i<length; i+=1)
        crc = z->crctable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

/// ZIP is little-endian throughout
static void gedzip_u16(GedZip *z, unsigned long v) {
    unsigned char b[2] = {v & 0xFF, (v >> 8) & 0xFF};
    if (fwrite(b, 1, 2, z->out) != 2) z->failed = 1;
    z->written += 2;
}
static void gedzip_u32(GedZip *z, unsigned long v) {
    unsigned char b[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF};
    if (fwrite(b, 1, 4, z->out) != 4) z->failed = 1;
    z->written += 4;
}
static void gedzip_raw(GedZip *z, const void *data, size_t length) {
    if (length && fwrite(data, 1, length, z->out) != length) z->failed = 1;
    z->written += length;
}

/// records a new entry and writes its local file header
static void gedzip_header(GedZip *z, const char *name, unsigned long crc, unsigned long size) {
    if (z->count >= z->cap) {
        z->cap = z->cap ? z->cap * 2 : 8;
        z->entries = realloc(z->entries, sizeof(struct gedzip_entry) * z->cap);
    }
    struct gedzip_entry *e = z->entries + z->count;
    z->count += 1;
    e->name = malloc(strlen(name) + 1);
    strcpy(e->name, name);
    e->crc = crc;
    e->size = size;
    e->offset = z->written;

    gedzip_u32(z, 0x04034b50UL); // local file header signature
    gedzip_u16(z, 10);           // version needed: 1.0, stored
    gedzip_u16(z, 0x0800);       // flags: name is UTF-8
    gedzip_u16(z, 0);            // method: stored
    gedzip_u16(z, z->dostime);
    gedzip_u16(z, z->dosdate);
    gedzip_u32(z, crc);
    gedzip_u32(z, size);         // compressed size
    gedzip_u32(z, size);         // uncompressed size
    gedzip_u16(z, strlen(name));
    gedzip_u16(z, 0);            // extra field length
    gedzip_raw(z, name, strlen(name));
}

int gedzip_add_bytes(GedZip *z, const char *name, const unsigned char *data, size_t length) {
    gedzip_header(z, name, gedzip_crc(z, 0, data, length), length);
    gedzip_raw(z, data, length);
    return z->failed;
}

int gedzip_add_stream(GedZip *z, const char *name, FILE *src) {
    unsigned char buf[4096];
    unsigned long crc = 0, size = 0;
    size_t got;

    // two passes: CRC and size must precede the data in the header
    if (fflush(src) || fseek(src, 0, SEEK_SET)) return z->failed = 1;
    while((got = fread(buf, 1, sizeof(buf), src)) > 0) {
        crc = gedzip_crc(z, crc, buf, got);
        size += got;
    }
    if (ferror(src) || fseek(src, 0, SEEK_SET)) return z->failed = 1;

    gedzip_header(z, name, crc, size);
    while((got = fread(buf, 1, sizeof(buf), src)) > 0)
        gedzip_raw(z, buf, got);
    if (ferror(src)) z->failed = 1;
    return z->failed;
}

int gedzip_finish(GedZip *z) {
    unsigned long start = z->written;
    for(size_t i=0; i<z->count; i+=1) {
        struct gedzip_entry *e = z->entries + i;
        gedzip_u32(z, 0x02014b50UL); // central directory header signature
        gedzip_u16(z, 20);           // version made by: 2.0, MS-DOS attributes
        gedzip_u16(z, 10);           // version needed
        gedzip_u16(z, 0x0800);       // flags: name is UTF-8
        gedzip_u16(z, 0);            // method: stored
        gedzip_u16(z, z->dostime);
        gedzip_u16(z, z->dosdate);
        gedzip_u32(z, e->crc);
        gedzip_u32(z, e->size);
        gedzip_u32(z, e->size);
        gedzip_u16(z, strlen(e->name));
        gedzip_u16(z, 0);            // extra field length
        gedzip_u16(z, 0);            // comment length
        gedzip_u16(z, 0);            // disk number start
        gedzip_u16(z, 0);            // internal attributes
        gedzip_u32(z, 0);            // external attributes
        gedzip_u32(z, e->offset);
        gedzip_raw(z, e->name, strlen(e->name));
    }
    unsigned long size = z->written - start;

    gedzip_u32(z, 0x06054b50UL);     // end of central directory signature
    gedzip_u16(z, 0);                // this disk
    gedzip_u16(z, 0);                // disk with central directory
    gedzip_u16(z, z->count);         // entries on this disk
    gedzip_u16(z, z->count);         // total entries
    gedzip_u32(z, size);
    gedzip_u32(z, start);
    gedzip_u16(z, 0);                // comment length

    if (fflush(z->out)) z->failed = 1;

    int failed = z->failed;
    for(size_t i=0; i<z->count; i+=1) free(z->entries[i].name);
    free(z->entries);
    free(z);
    return failed;
}

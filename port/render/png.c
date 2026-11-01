/* The PNG writer (png.h), from King's Field IV's port/platform/video.c. */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    size_t i;

    if (crc_table[1] == 0) {
        uint32_t k;

        for (k = 0; k < 256; k++) {
            uint32_t c = k;
            int b;

            for (b = 0; b < 8; b++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            crc_table[k] = c;
        }
    }
    for (i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, size_t size) {
    uint8_t head[8];
    uint8_t tail[4];
    uint32_t crc;

    put32(head, (uint32_t)size);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (size) {
        fwrite(data, 1, size, f);
    }
    crc = crc32_update(0xFFFFFFFFu, head + 4, 4);
    crc = crc32_update(crc, data, size) ^ 0xFFFFFFFFu;
    put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

/* RGB rows, each after its filter byte (0), in a zlib stream of stored blocks */
int render_png_write(const char *path, const uint32_t *rgba, uint32_t width, uint32_t height) {
    size_t row = (size_t)width * 3 + 1;
    size_t raw_size = row * height;
    size_t blocks = (raw_size + 65534) / 65535;
    size_t z_size = 2 + raw_size + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_size);
    uint8_t *z = malloc(z_size);
    uint8_t header[13];
    uint32_t a = 1, b = 0;
    size_t i, at, done;
    uint32_t x, y;
    FILE *f;
    int ok = 0;

    if (raw == NULL || z == NULL) {
        free(raw);
        free(z);
        return 0;
    }
    for (y = 0; y < height; y++) {
        uint8_t *out = raw + row * y;

        *out++ = 0;
        for (x = 0; x < width; x++) {
            uint32_t p = rgba[(size_t)y * width + x];

            *out++ = (uint8_t)p;
            *out++ = (uint8_t)(p >> 8);
            *out++ = (uint8_t)(p >> 16);
        }
    }
    z[0] = 0x78;
    z[1] = 0x01;
    at = 2;
    for (done = 0; done < raw_size;) {
        size_t n = raw_size - done > 65535 ? 65535 : raw_size - done;

        z[at++] = done + n == raw_size;
        z[at++] = (uint8_t)n;
        z[at++] = (uint8_t)(n >> 8);
        z[at++] = (uint8_t)~n;
        z[at++] = (uint8_t)(~n >> 8);
        memcpy(z + at, raw + done, n);
        at += n;
        done += n;
    }
    for (i = 0; i < raw_size; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put32(z + at, (b << 16) | a);
    at += 4;

    put32(header, width);
    put32(header + 4, height);
    header[8] = 8;  /* bits */
    header[9] = 2;  /* RGB */
    header[10] = 0; /* deflate */
    header[11] = 0; /* filters */
    header[12] = 0; /* not interlaced */

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "[render] cannot write %s\n", path);
    } else {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        write_chunk(f, "IHDR", header, sizeof(header));
        write_chunk(f, "IDAT", z, at);
        write_chunk(f, "IEND", NULL, 0);
        fclose(f);
        ok = 1;
    }
    free(raw);
    free(z);
    return ok;
}

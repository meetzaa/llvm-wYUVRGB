#include "fpga_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int read_yuv_file(const char *path, size_t need, uint8_t **buf) {
    *buf = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fopen('%s'): %s\n", path, strerror(errno));
        return -1;
    }

    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz >= 0 && (size_t)sz < need) {
            fprintf(stderr, "Input too small (%ld < %zu)\n", sz, need);
            fclose(f);
            return -1;
        }
        fseek(f, 0, SEEK_SET);
    }

    uint8_t *p = (uint8_t *)malloc(need);
    if (!p) {
        fprintf(stderr, "malloc %zu failed\n", need);
        fclose(f);
        return -1;
    }

    size_t rd = fread(p, 1, need, f);
    fclose(f);
    if (rd != need) {
        fprintf(stderr, "Short read: %zu/%zu\n", rd, need);
        free(p);
        return -1;
    }

    *buf = p;
    return 0;
}

int write_rgb_file(const char *inpath, const uint8_t *rgb, size_t sz) {
    char out[512];
    snprintf(out, sizeof(out), "%s_out.rgb", inpath);
    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "fopen('%s'): %s\n", out, strerror(errno));
        return -1;
    }

    size_t wr = fwrite(rgb, 1, sz, f);
    fclose(f);

    if (wr != sz) {
        fprintf(stderr, "Short write: %zu/%zu\n", wr, sz);
        return -1;
    }

    printf("OK -> %s\n", out);
    return 0;
}

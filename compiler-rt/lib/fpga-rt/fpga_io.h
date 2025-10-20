#ifndef FPGA_IO_H
#define FPGA_IO_H

#include <stdint.h>
#include <stddef.h>

int read_yuv_file(const char *path, size_t need, uint8_t **buf);
int write_rgb_file(const char *inpath, const uint8_t *rgb, size_t sz);

#endif

#ifndef CONVERSION_H
#define CONVERSION_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef enum 
{
  CONVERSION_CPU,
  CONVERSION_FPGA,
  BOTH
} ConversionMethod;

typedef struct 
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
} RGB;

long time_diff_us(struct timespec start, struct timespec end);
long sum_conversion_times(const long *durations, size_t count);
uint8_t clip(uint16_t value);
void yuv422_to_rgb(uint8_t y0, uint8_t u, uint8_t y1, uint8_t v, RGB* pixel0, RGB* pixel1);
void convert_cpu(uint8_t *yuv_buffer, uint8_t *rgb_buffer, uint32_t width, uint32_t height);
int convert_fpga(uint8_t *yuv_buffer, uint8_t *rgb_buffer, uint32_t width, uint32_t height);
void compare_rgb(uint8_t *rgb_buffer_fpga, uint8_t *rgb_buffer_cpu, uint32_t WIDTH, uint32_t HEIGHT);

#endif // CONVERSION_H

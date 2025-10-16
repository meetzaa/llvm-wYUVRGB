#include "conversion.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <omp.h>

#define APB_BASE_ADDR 0x41100000
#define MAP_SIZE      4096           // 4K mapati - o pagina de mem

long time_diff_us(struct timespec start, struct timespec end) 
{
  return (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_nsec - start.tv_nsec) / 1000L;
}

long sum_conversion_times(const long *durations, size_t count)
{
  long total = 0;
  for (size_t i = 0; i < count; i++)
  {
    total += durations[i];
  }
  return total;
}

uint8_t clip(uint16_t value) 
{
  if (value < 0) 
    return 0;
  if (value > 255) 
    return 255;
  return (uint8_t)value;
}

void yuv422_to_rgb(uint8_t y0, uint8_t u, uint8_t y1, uint8_t v, RGB* pixel0, RGB* pixel1) 
{
  int32_t r_temp, g_temp, b_temp;
  int16_t r0, g0, b0;
  int16_t r1, g1, b1;

  r_temp = (1436 * (v - 128)) >> 10;                                // R = Y + 1.402 * (V - 128)
  g_temp = ((352 * (u - 128)) >> 10) + ((731 * (v - 128)) >> 10);   // G = Y - 0.344 * (U - 128) - 0.714 * (V - 128)
  b_temp = (1814 * (u - 128)) >> 10;                                // B = Y + 1.772 * (U - 128)

  r0 = y0 + r_temp;
  g0 = y0 - g_temp;
  b0 = y0 + b_temp;

  r1 = y1 + r_temp;
  g1 = y1 - g_temp;
  b1 = y1 + b_temp;
  
  pixel0->r = clip(r0);
  pixel0->g = clip(g0);
  pixel0->b = clip(b0);

  pixel1->r = clip(r1);
  pixel1->g = clip(g1);
  pixel1->b = clip(b1);
}

void convert_cpu(uint8_t *yuv_buffer, uint8_t *rgb_buffer, uint32_t WIDTH, uint32_t HEIGHT)
{
  uint32_t i; 
  long *duration_conversion = malloc(sizeof(long) * (WIDTH * HEIGHT * 2 / 8));

//  #pragma omp parallel for 
 #pragma omp parallel for schedule(static, 8)  
 for( i = 0; i < WIDTH * HEIGHT * 2; i += 8)
  {
    uint32_t pixel_index = 0;
    uint32_t rgb_index = (i/2) * 3;
    RGB pixels[4];

//    struct timespec start, stop;
//    clock_gettime(CLOCK_MONOTONIC_RAW, &start);

    yuv422_to_rgb(yuv_buffer[i], yuv_buffer[i+1], yuv_buffer[i+2], yuv_buffer[i+3], &pixels[pixel_index++], &pixels[pixel_index++]); // Pixel 0 + 1: Y0, U0, Y1, V0
  
    yuv422_to_rgb(yuv_buffer[i+4], yuv_buffer[i+5], yuv_buffer[i+6], yuv_buffer[i+7], &pixels[pixel_index++], &pixels[pixel_index++]); // Pixel 2 + 3: Y2, U1, Y3, V1

//    clock_gettime(CLOCK_MONOTONIC_RAW, &stop);
//    duration_conversion[i / 8] = time_diff_us(start, stop); 
    
    pixel_index = 0;

    for(int j = 0; j < 4; j++)
    {
      rgb_buffer[rgb_index++] = pixels[j].r;
      rgb_buffer[rgb_index++] = pixels[j].g;
      rgb_buffer[rgb_index++] = pixels[j].b;
      //printf("Valoare RGB: %x, %x, %x\n", pixels[j].r, pixels[j].g, pixels[j].b);
    }
  }

//  long total_us = sum_conversion_times(duration_conversion, WIDTH * HEIGHT * 2 / 8);
//  printf("Timp total de conversie CPU:%ldm%ld.%06lds\n", total_us / 60000000, (total_us / 1000000) % 60, total_us % 1000000);
}

int convert_fpga(uint8_t *yuv_buffer, uint8_t *rgb_buffer, uint32_t WIDTH, uint32_t HEIGHT)
{
  int mem_fd = open("/dev/mem", O_RDWR | O_SYNC); // O_SYNC - scrieri directe, fara buffer/ cache
  printf("Mme_fd = %u\n", mem_fd);
  if (mem_fd < 0) {
    perror("open");
    return 1;
  }

  void *map_base = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, APB_BASE_ADDR); // MAP_SHARED - modificarile afecteaza si memoria fizica
  if (map_base == MAP_FAILED) 
  {
    perror("mmap");
    close(mem_fd);
    return 1;
  }

  volatile uint32_t *y0u0y1v0 = (volatile uint32_t *)(map_base + 0x00000010);
  volatile uint32_t *y2u1y3v1 = (volatile uint32_t *)(map_base + 0x00000014);
  volatile uint32_t *rgb0 = (volatile uint32_t *)(map_base + 0x00000030);
  volatile uint32_t *rgb1 = (volatile uint32_t *)(map_base + 0x00000034);
  volatile uint32_t *rgb2 = (volatile uint32_t *)(map_base + 0x00000038);
  volatile uint32_t *rgb3 = (volatile uint32_t *)(map_base + 0x0000003C);
  volatile uint32_t *status = (volatile uint32_t *) (map_base + 0x00000020);

  uint32_t i; 
  uint32_t pixel_index = 0;

//  long *duration_conversion = malloc(sizeof(long) * (WIDTH * HEIGHT * 2 / 8));

  for( i = 0; i < WIDTH * HEIGHT * 2; i += 8)
  {
//    struct timespec start, stop;
//    clock_gettime(CLOCK_MONOTONIC_RAW, &start);

    *y0u0y1v0 = ((uint32_t)yuv_buffer[i+3] << 24) |
                ((uint32_t)yuv_buffer[i+2] << 16) |
                ((uint32_t)yuv_buffer[i+1] << 8)  |
                ((uint32_t)yuv_buffer[i+0]);

    *y2u1y3v1 = ((uint32_t)yuv_buffer[i+7] << 24) |
                ((uint32_t)yuv_buffer[i+6] << 16) |
                ((uint32_t)yuv_buffer[i+5] << 8)  |
                ((uint32_t)yuv_buffer[i+4]);

    volatile uint32_t rgb0_reg;
    volatile uint32_t rgb1_reg;
    volatile uint32_t rgb2_reg;
    volatile uint32_t rgb3_reg;
    volatile uint32_t status_reg;

    int tries = 100;
    while (tries--) 
    {
      status_reg = *status;
     // printf("Try : %d, Status : %x/n", tries, status_reg);
      if (status_reg == 0xFFFFFFFF)
      {
      	rgb0_reg = *rgb0;
      	rgb1_reg = *rgb1;
      	rgb2_reg = *rgb2;
      	rgb3_reg = *rgb3;
      //  printf("Try : %d, RGB0 : %x, RGB1 : %x, RGB2 : %x, RGB3 : %x\n", tries, rgb0_reg, rgb1_reg, rgb2_reg, rgb3_reg);
//        clock_gettime(CLOCK_MONOTONIC_RAW, &stop);
//        duration_conversion[i / 8] = time_diff_us(start, stop);
        break; 
       }
       usleep(100);
    }
    
    if (tries == -1)
    { 
      printf("Nu s-a primit intreruperea!\n");
      return 1;
    }
     
    rgb_buffer[pixel_index++] = (rgb0_reg >> 24) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb0_reg >> 16) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb0_reg >> 8) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb1_reg >> 24) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb1_reg >> 16) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb1_reg >> 8) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb2_reg >> 24) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb2_reg >> 16) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb2_reg >> 8) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb3_reg >> 24) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb3_reg >> 16) & 0xFF;
    rgb_buffer[pixel_index++] = (rgb3_reg >> 8) & 0xFF;
  }

//  long total_us = sum_conversion_times(duration_conversion, WIDTH * HEIGHT * 2 / 8);
//  printf("Timp total de conversie FPGA: %ldm%ld.%06lds\n", total_us / 60000000, (total_us / 1000000) % 60, total_us % 1000000);
  return 0;
}

void compare_rgb(uint8_t *rgb_buffer_fpga, uint8_t *rgb_buffer_cpu, uint32_t WIDTH, uint32_t HEIGHT) 
{

  size_t differences = 0;

  for (uint32_t i = 0; i < WIDTH * HEIGHT * 3; i++) 
  {
    if (rgb_buffer_cpu[i] != rgb_buffer_fpga[i]) 
    {
      differences++;
      printf("Diferenta %ld la pozitia %u: %u - %u\n", differences, i, rgb_buffer_cpu[i], rgb_buffer_fpga[i]);
    }
  }
  if (!differences)
    printf("Fisierele generate sunt identice!\n");
}

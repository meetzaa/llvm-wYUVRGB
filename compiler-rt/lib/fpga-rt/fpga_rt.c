#include "fpga_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define FPGA_PHYS_BASE  0x41100000ULL
#define FPGA_MAP_LEN    0x1000

static void *g_mmio = NULL;

int map_fpga_base_for_builtin(void) {
    if (g_mmio)
        return 0; // already mapped

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return -1;
    }

    void *want_addr = (void *)(uintptr_t)FPGA_PHYS_BASE;
    void *p = mmap(want_addr, FPGA_MAP_LEN,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED,
                   fd, FPGA_PHYS_BASE);
    close(fd);

    if (p == MAP_FAILED) {
        perror("mmap /dev/mem MAP_FIXED");
        return -1;
    }

    if (p != want_addr) {
        fprintf(stderr, "mmap did not return fixed addr\n");
        munmap(p, FPGA_MAP_LEN);
        return -1;
    }

    g_mmio = p;
    return 0;
}

void unmap_fpga_base_for_builtin(void) {
    if (g_mmio) {
        munmap(g_mmio, FPGA_MAP_LEN);
        g_mmio = NULL;
    }
}

void* get_fpga_mmio(void) {
    return g_mmio;
}

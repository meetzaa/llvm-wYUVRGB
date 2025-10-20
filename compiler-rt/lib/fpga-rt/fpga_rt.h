#ifndef FPGA_RT_H
#define FPGA_RT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  map_fpga_base_for_builtin(void);
void unmap_fpga_base_for_builtin(void);

void* get_fpga_mmio(void);

#ifdef __cplusplus
}
#endif
#endif

# ⚙️ FPGA-Augmented RISC-V LLVM Backend

This repository integrates two main components that together form an end-to-end **hardware/software co-design framework**:
- **LLVM Compiler Extension** (`llvm/`) — adds a new RISC-V pseudo-instruction for YUV→RGB conversion.
- **FPGA Gateware** (`fpga/`) — implements the corresponding accelerator in Verilog for the **BeagleV-Fire** platform.

Both components are part of a larger academic and research project developed at **Transilvania University of Brașov** in collaboration with **NXP Semiconductors** and **BeagleBoard.org**, presented at **SIITME 2025**.

---

## 🖥️ Platform

The system targets the **BeagleV-Fire** development board, powered by the **Microchip PolarFire SoC (MPFS025T)**.  
This device combines:
- 5× RISC-V cores (1× E51 monitor core, 4× U54 application cores)
- Integrated FPGA fabric (~23k logic elements, 68 DSP blocks)
- Shared AXI/AMBA interconnect between CPU and FPGA
- DDR, PCIe, and multiple I/O interfaces

The platform runs **Linux natively on RISC-V**, allowing direct interaction with the FPGA fabric through memory-mapped I/O and DMA.

---

## 🧩 Project Overview

This project demonstrates **FPGA-accelerated YUV422 → RGB image conversion** using a **custom LLVM compiler intrinsic** and a **hardware accelerator**.

It bridges two layers:

1. **Software (Compiler + Runtime)**
   - Introduces `__builtin_riscv_yuvrgb(yuv, rgb, H, W)` as a new **Clang builtin**, mapped to an **LLVM intrinsic**.
   - The intrinsic is lowered to a **RISC-V pseudo-instruction**, which expands into MMIO-based control and polling logic that drives the FPGA.
   - A small user-space runtime stages image data, invokes the builtin, and writes the converted output back to disk.

2. **Hardware (FPGA Accelerator)**
   - Verilog implementation of the YUV→RGB pipeline, connected to the SoC via the **APB/AXI bus**.
   - Two configurations were tested:
     - **Processor-driven** transfer (without DMA)
     - **DMA-enabled** transfer for high-throughput image streaming
   - The accelerator converts 4 pixels in parallel, with data synchronization handled by a finite-state machine.

---

## 🧠 Compiler Integration

The compiler flow includes:
- **Clang Front-End:** Defines the new builtin in `BuiltinsRISCV.td` and emits an intrinsic call.
- **LLVM IR:** Declares `@llvm.riscv.yuvrgb` with proper memory-side attributes (`ReadOnly`, `WriteOnly`, `IntrArgMemOnly`).
- **RISC-V Back-End:** Matches the intrinsic to a pseudo-instruction (`RISCVExpandPseudoInsts.cpp`) that expands into load/store sequences for the FPGA MMIO region.
- **Runtime:** Handles image allocation, file parsing, and invocation of the intrinsic via:
  ```c
  __builtin_riscv_yuvrgb(yuv, rgb, H, W);
`
  This approach keeps the frontend portable and isolates all hardware semantics in the backend, enabling flexible experimentation with FPGA-accelerated execution.
  

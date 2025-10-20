cmake_minimum_required(VERSION 3.16)
project(fpga_rt C)

add_library(fpga-rt STATIC
    fpga_rt.c
    fpga_io.c
)

target_include_directories(fpga-rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(fpga-rt PRIVATE -fvisibility=hidden)

set_target_properties(fpga-rt PROPERTIES OUTPUT_NAME "fpga-rt")

add_library(yuvrgb_runtime STATIC
  conversion.c
  runtime_yuv_rgb.c
)
target_include_directories(yuvrgb_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

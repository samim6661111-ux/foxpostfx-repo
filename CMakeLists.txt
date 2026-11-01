cmake_minimum_required(VERSION 3.22)
project(foxpostfx CXX)

option(FOXPOSTFX_DROPIN "Build as libgtavvk.so (drop-in replacement of the 6 KB loader bridge)" OFF)
set(FOXPOSTFX_GEN_DIR "${CMAKE_BINARY_DIR}/gen" CACHE PATH "dir with spv_shaders.h (tools/gen_spv.sh output)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(foxpostfx SHARED foxpostfx.cpp)
target_include_directories(foxpostfx PRIVATE "${FOXPOSTFX_GEN_DIR}")
target_compile_options(foxpostfx PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti)
target_link_libraries(foxpostfx PRIVATE log dl)
# 16 KB page size (Android 15+), keep our symbols private except the two vk entry points
target_link_options(foxpostfx PRIVATE -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL -Wl,--no-undefined)

if(FOXPOSTFX_DROPIN)
  set_target_properties(foxpostfx PROPERTIES OUTPUT_NAME gtavvk)
  target_compile_definitions(foxpostfx PRIVATE FOXPOSTFX_DROPIN=1)
else()
  set_target_properties(foxpostfx PROPERTIES OUTPUT_NAME foxpostfx)
endif()

# Test app: runs Torch's bk.o2r extraction on the 3DS and reports the time and peak memory.
# Build from the Lighthouse folder, in the lighthouse-3ds image:
#   cmake -S tools/3ds/extract-test -B build-extract-test -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-extract-test
cmake_minimum_required(VERSION 3.20)
project(ExtractTest C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The same Torch setup as Lighthouse: a library, Banjo-Kazooie only
set(USE_STANDALONE OFF CACHE BOOL "" FORCE)
foreach(_game SM64 MK64 SF64 PM64 FZERO MARIO_ARTIST NAUDIO OOT)
    set(BUILD_${_game} OFF CACHE BOOL "" FORCE)
endforeach()
set(BUILD_BK64 ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../Torch ${CMAKE_BINARY_DIR}/Torch)

add_executable(ExtractTest main.cpp)
target_link_libraries(ExtractTest PRIVATE torch)
# Count and time every SD card call (see FS_TIMED in main.cpp)
foreach(_fs FSFILE_Write FSFILE_Read FSUSER_OpenFile FSUSER_OpenDirectory FSUSER_CreateDirectory FSFILE_SetSize
            FSFILE_GetSize FSFILE_Close)
    target_link_options(ExtractTest PRIVATE -Wl,--wrap=${_fs})
endforeach()

ctr_generate_smdh(ExtractTest.smdh NAME "bk.o2r extract test" DESCRIPTION "Torch on the 3DS" AUTHOR "Lighthouse")
ctr_create_3dsx(ExtractTest SMDH ExtractTest.smdh)

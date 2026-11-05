cmake_minimum_required(VERSION 3.25)
project(red_two LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(R2_DEFS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

# --- Aurora (renderer, window, input): extern/aurora, MIT ---------------------------------
# The game's own SDK code runs recompiled, so only Aurora's renderer and platform layer are
# used: no DVD (we read the disc), no THP, no memory card (EXI is ours).
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# The game's own main.dol (from the user's disc; never committed).
set(R2_DOL "${CMAKE_SOURCE_DIR}/orig/GSWE64/sys/main.dol" CACHE FILEPATH "main.dol of Star Wars: Rogue Leader (GSWE64)")
set(R2_GEN_FILES 96 CACHE STRING "number of generated code files")
# Generated files are large: cap how many compile at once (memory).
set(R2_GEN_JOBS 12 CACHE STRING "parallel compiles of generated code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS r2gen=${R2_GEN_JOBS})

# --- recompiler -------------------------------------------------------------------------
add_executable(r2recomp
    src/recompiler/main.cpp
    src/recompiler/analysis.cpp
    src/recompiler/emit.cpp
    src/recompiler/ppc.cpp)
target_compile_definitions(r2recomp PRIVATE ${R2_DEFS})

# --- generated code ---------------------------------------------------------------------
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_SOURCES "${GEN_DIR}/table.cpp")
math(EXPR _last "${R2_GEN_FILES} - 1")
foreach(i RANGE ${_last})
    string(LENGTH "${i}" _len)
    if(_len EQUAL 1)
        set(_n "00${i}")
    elseif(_len EQUAL 2)
        set(_n "0${i}")
    else()
        set(_n "${i}")
    endif()
    list(APPEND GEN_SOURCES "${GEN_DIR}/code_${_n}.cpp")
endforeach()
file(GLOB R2_CONFIG "${CMAKE_SOURCE_DIR}/config/GSWE64/*.txt")
add_custom_command(
    OUTPUT ${GEN_SOURCES} "${GEN_DIR}/funcs.h"
    COMMAND r2recomp "${R2_DOL}" "${CMAKE_SOURCE_DIR}/config/GSWE64" "${GEN_DIR}" --files ${R2_GEN_FILES}
    DEPENDS r2recomp "${R2_DOL}" ${R2_CONFIG}
    COMMENT "Recompiling main.dol"
    VERBATIM)

add_library(r2game STATIC ${GEN_SOURCES})
set_target_properties(r2game PROPERTIES JOB_POOL_COMPILE r2gen)
target_compile_definitions(r2game PRIVATE ${R2_DEFS})
target_include_directories(r2game PUBLIC src/runtime "${GEN_DIR}")
if(MSVC)
    # clang-cl: generated code is huge and full of labels
    target_compile_options(r2game PRIVATE /O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
        -fno-strict-aliasing /GS-)
else()
    target_compile_options(r2game PRIVATE -O2 -Wno-unused-label -fno-strict-aliasing)
endif()

# --- runtime ----------------------------------------------------------------------------
add_executable(red_two
    src/runtime/main.cpp
    src/runtime/memory.cpp
    src/runtime/cpu.cpp
    src/runtime/events.cpp
    src/runtime/threads.cpp
    src/runtime/sjlj.cpp
    src/runtime/vm.cpp
    src/runtime/interp.cpp
    src/runtime/profiler.cpp
    src/runtime/hle_os.cpp
    src/runtime/hw/hw.cpp
    src/runtime/hw/gx_fifo.cpp
    src/runtime/gfx/gx_bridge.cpp
    src/runtime/input.cpp)
target_include_directories(red_two PRIVATE src/runtime "${GEN_DIR}" extern/aurora/lib)
target_compile_definitions(red_two PRIVATE ${R2_DEFS})
target_link_libraries(red_two PRIVATE r2game winmm aurora::core aurora::gx aurora::vi aurora::pad xxHash::xxhash dbghelp)
if(MSVC)
    target_compile_options(red_two PRIVATE -fno-strict-aliasing)
    target_link_options(red_two PRIVATE /STACK:16777216)
endif()

# Dawn and SDL3 are DLLs on Windows: copy them next to the executable.
include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(red_two)

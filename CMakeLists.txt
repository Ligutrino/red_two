cmake_minimum_required(VERSION 3.25)
project(red_two LANGUAGES C CXX RC)

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

# The game's own main.dol, or the user's disc image (ISO/GCM/RVZ/WIA) it is read from; never
# committed. Defaults to the first that exists of the usual places.
set(_r2_dol_default "${CMAKE_SOURCE_DIR}/orig/GSWE64/sys/main.dol")
foreach(_c orig/GSWE64/sys/main.dol orig/GSWE64.iso orig/GSWE64.rvz)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_c}")
        set(_r2_dol_default "${CMAKE_SOURCE_DIR}/${_c}")
        break()
    endif()
endforeach()
set(R2_DOL "${_r2_dol_default}" CACHE FILEPATH "main.dol or disc image of Star Wars: Rogue Leader (GSWE64)")
set(R2_GEN_FILES 96 CACHE STRING "number of generated code files")
# Generated files are large: cap how many compile at once (memory).
set(R2_GEN_JOBS 12 CACHE STRING "parallel compiles of generated code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS r2gen=${R2_GEN_JOBS})

# --- recompiler -------------------------------------------------------------------------
add_executable(r2recomp
    src/recompiler/main.cpp
    src/recompiler/analysis.cpp
    src/recompiler/emit.cpp
    src/recompiler/ppc.cpp
    src/common/disc_image.cpp)
target_compile_definitions(r2recomp PRIVATE ${R2_DEFS})
target_link_libraries(r2recomp PRIVATE zstd::libzstd)

# disc_compare <a> <b>: check the disc image reader (not built by default)
add_executable(disc_compare EXCLUDE_FROM_ALL src/common/disc_compare.cpp src/common/disc_image.cpp)
target_link_libraries(disc_compare PRIVATE zstd::libzstd)

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
    src/runtime/audio.cpp
    src/runtime/hle_musyx.cpp
    src/common/disc_image.cpp
    src/runtime/hw/musyx_dsp.cpp
    src/runtime/hle_os.cpp
    src/runtime/hw/hw.cpp
    src/runtime/hw/gx_fifo.cpp
    src/runtime/gfx/gx_bridge.cpp
    src/runtime/gfx/tex_dump.cpp
    src/runtime/gfx/button_icons.cpp
    src/runtime/input.cpp
    src/runtime/settings.cpp
    src/runtime/hle_game.cpp
    src/runtime/mods.cpp
    src/runtime/codes.cpp
    src/runtime/card_image.cpp
    src/runtime/dolphin_import.cpp
    src/runtime/gfx/menu.cpp)
target_include_directories(red_two PRIVATE src/runtime "${GEN_DIR}" extern/aurora/lib)
# Program icon: your own res/icon.ico (not in the repository), built in when it exists.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/res/icon.ico")
    target_sources(red_two PRIVATE res/red_two.rc)
    set_source_files_properties(res/red_two.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/res/icon.ico")
    target_compile_definitions(red_two PRIVATE R2_HAS_ICON=1)
endif()
target_compile_definitions(red_two PRIVATE ${R2_DEFS})
target_link_libraries(red_two PRIVATE r2game winmm aurora::core aurora::gx aurora::vi aurora::pad xxHash::xxhash zstd::libzstd dbghelp)
if(MSVC)
    target_compile_options(red_two PRIVATE -fno-strict-aliasing)
    # GUI subsystem (no console window), still entering through main()
    target_link_options(red_two PRIVATE /STACK:16777216 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# Dawn and SDL3 are DLLs on Windows: copy them next to the executable.
include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(red_two)

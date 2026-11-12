cmake_minimum_required(VERSION 3.22)
project(bancada VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BANCADA_APP "Build the game window (needs SDL3)" ON)
option(BANCADA_SANITIZE "Build with ASan and UBSan" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(bancada_warnings INTERFACE)
target_compile_options(bancada_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(BANCADA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Everything links statically on Windows: the game is one .exe that runs from any folder. On Linux
# only the system's C library stays dynamic, which is why releases are built on an older distro.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
elseif(UNIX AND NOT BANCADA_SANITIZE)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

add_library(bancada_sim STATIC
    src/sim/netlist.cpp
    src/sim/compile.cpp
    src/sim/interpreter.cpp
    src/sim/event.cpp)
target_include_directories(bancada_sim PUBLIC src)
target_link_libraries(bancada_sim PRIVATE bancada_warnings)

add_library(bancada_text STATIC src/text/truetype.cpp)
target_include_directories(bancada_text PUBLIC src)
target_link_libraries(bancada_text PRIVATE bancada_warnings)

add_library(bancada_electric STATIC src/electric/solver.cpp src/electric/units.cpp)
target_include_directories(bancada_electric PUBLIC src)
target_link_libraries(bancada_electric PRIVATE bancada_warnings)

add_executable(bancada_electric_cli tools/electric_cli.cpp)
target_link_libraries(bancada_electric_cli PRIVATE bancada_electric bancada_warnings)

add_library(bancada_i18n STATIC src/i18n/i18n.cpp src/i18n/english.cpp)
target_include_directories(bancada_i18n PUBLIC src)
target_link_libraries(bancada_i18n PRIVATE bancada_warnings)

add_library(bancada_asm STATIC src/asm/assembler.cpp src/asm/mips.cpp)
target_include_directories(bancada_asm PUBLIC src)
target_link_libraries(bancada_asm PUBLIC bancada_i18n PRIVATE bancada_warnings)

add_library(bancada_editor STATIC
    src/editor/board.cpp
    src/editor/boxes.cpp
    src/editor/computer16.cpp
    src/editor/computer32.cpp
    src/editor/computer32_view.cpp
    src/editor/computer_view.cpp
    src/editor/analog.cpp
    src/editor/electric_examples.cpp
    src/editor/text_edit.cpp
    src/editor/circuit.cpp
    src/editor/router.cpp
    src/editor/session.cpp
    src/editor/truth_table.cpp
    src/editor/lessons.cpp
    src/editor/course.cpp
    src/editor/gallery.cpp
    src/editor/gallery_circuits.cpp)
target_include_directories(bancada_editor PUBLIC src)
target_link_libraries(bancada_editor PUBLIC bancada_sim bancada_asm bancada_electric PRIVATE bancada_warnings)

find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES libzstd.a zstd REQUIRED)
find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
find_path(JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_library(bancada_io STATIC
    src/io/container.cpp
    src/io/files.cpp
    src/io/profiles.cpp
    src/io/project.cpp)
target_include_directories(bancada_io PUBLIC src)
target_include_directories(bancada_io SYSTEM PUBLIC ${JSON_INCLUDE_DIR})
target_include_directories(bancada_io SYSTEM PRIVATE ${ZSTD_INCLUDE_DIR} ${XXHASH_INCLUDE_DIR})
target_link_libraries(bancada_io PUBLIC bancada_editor PRIVATE ${ZSTD_LIBRARY} bancada_warnings)

enable_testing()
add_executable(bancada_tests tests/sim_tests.cpp)
target_link_libraries(bancada_tests PRIVATE bancada_sim bancada_warnings)
add_test(NAME sim COMMAND bancada_tests)

add_executable(bancada_differential tests/differential.cpp)
target_link_libraries(bancada_differential PRIVATE bancada_sim bancada_warnings)
add_test(NAME differential COMMAND bancada_differential)

add_executable(bancada_editor_tests tests/editor_tests.cpp)
target_link_libraries(bancada_editor_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME editor COMMAND bancada_editor_tests)

add_executable(bancada_font_tests tests/font_tests.cpp)
target_link_libraries(bancada_font_tests PRIVATE bancada_text bancada_warnings)
target_compile_definitions(bancada_font_tests PRIVATE BANCADA_FONTS="${CMAKE_SOURCE_DIR}/assets/fonts")
add_test(NAME fonts COMMAND bancada_font_tests)

add_executable(bancada_electric_tests tests/electric_tests.cpp)
target_link_libraries(bancada_electric_tests PRIVATE bancada_electric bancada_warnings)
add_test(NAME electric COMMAND bancada_electric_tests)

add_executable(bancada_analog_tests tests/analog_tests.cpp)
target_link_libraries(bancada_analog_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME analog COMMAND bancada_analog_tests)

add_executable(bancada_cpu_tests tests/cpu_tests.cpp)
target_link_libraries(bancada_cpu_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME cpu COMMAND bancada_cpu_tests)

add_executable(bancada_mips_tests tests/mips_tests.cpp)
target_link_libraries(bancada_mips_tests PRIVATE bancada_asm bancada_warnings)
add_test(NAME mips COMMAND bancada_mips_tests)

add_executable(bancada_cpu32_tests tests/cpu32_tests.cpp)
target_link_libraries(bancada_cpu32_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME cpu32 COMMAND bancada_cpu32_tests)

add_executable(bancada_lesson_tests tests/lesson_tests.cpp)
target_link_libraries(bancada_lesson_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME lessons COMMAND bancada_lesson_tests)

add_executable(bancada_i18n_tests tests/i18n_tests.cpp)
target_link_libraries(bancada_i18n_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME i18n COMMAND bancada_i18n_tests)

add_executable(bancada_gallery_tests tests/gallery_tests.cpp)
target_link_libraries(bancada_gallery_tests PRIVATE bancada_editor bancada_warnings)
add_test(NAME gallery COMMAND bancada_gallery_tests)

add_executable(bancada_io_tests tests/io_tests.cpp)
target_include_directories(bancada_io_tests SYSTEM PRIVATE ${XXHASH_INCLUDE_DIR})
target_compile_definitions(bancada_io_tests PRIVATE BANCADA_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
target_link_libraries(bancada_io_tests PRIVATE bancada_io bancada_warnings)
add_test(NAME io COMMAND bancada_io_tests)

add_executable(bancada_bench tools/bench.cpp)
target_link_libraries(bancada_bench PRIVATE bancada_sim bancada_warnings)

if(BANCADA_APP)
    # MSYS2 has SDL3 as a package. Ubuntu 22.04 doesn't, so it's built from the pinned release.
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.4.20
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    add_executable(bancada WIN32 src/app/main.cpp src/app/draw.cpp src/app/text.cpp src/app/fonts_data.cpp src/app/paths.cpp)
    target_link_libraries(bancada PRIVATE bancada_text bancada_io bancada_warnings)
    if(TARGET SDL3::SDL3-static)
        target_link_libraries(bancada PRIVATE bancada_text SDL3::SDL3-static)
    else()
        target_link_libraries(bancada PRIVATE bancada_text SDL3::SDL3)
    endif()

    # On Windows the .exe carries its icon, name and version. Signing it is a separate step, with a
    # certificate only its author can hold: tools/assinar_windows.ps1.
    if(WIN32)
        get_filename_component(BANCADA_COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        find_program(BANCADA_WINDRES NAMES windres x86_64-w64-mingw32-windres HINTS ${BANCADA_COMPILER_DIR} REQUIRED)
        set(BANCADA_AUTHOR "Projeto Bancada" CACHE STRING "Author shown in the .exe properties (plain ASCII)")
        string(TIMESTAMP BANCADA_YEAR "%Y")
        configure_file(assets/icone/bancada.ico ${CMAKE_BINARY_DIR}/bancada.ico COPYONLY)
        configure_file(src/app/bancada.rc.in ${CMAKE_BINARY_DIR}/bancada.rc @ONLY)
        # windres hands its paths to the preprocessor in the Windows code page, which mangles an accent in
        # the user name ("Estácio"). So it runs inside the build folder and only ever sees short names.
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/bancada_rc.o
            COMMAND ${BANCADA_WINDRES} -O coff -i bancada.rc -o bancada_rc.o
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS ${CMAKE_BINARY_DIR}/bancada.rc ${CMAKE_BINARY_DIR}/bancada.ico
            VERBATIM)
        set_source_files_properties(${CMAKE_BINARY_DIR}/bancada_rc.o PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        target_sources(bancada PRIVATE ${CMAKE_BINARY_DIR}/bancada_rc.o)
    endif()
endif()

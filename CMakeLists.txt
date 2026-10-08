cmake_minimum_required(VERSION 3.16)
project(PipeClean C)

# cmake -S . -B build -DROM=path/to/game.gb && cmake --build build
set(ROM "" CACHE FILEPATH "ROM to load/recompile")
if(NOT ROM)
  message(FATAL_ERROR "pass -DROM=path/to/game.gb")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(SDL2 REQUIRED)

set(GEN ${CMAKE_BINARY_DIR}/generated)
set(ROOTS ${CMAKE_SOURCE_DIR}/roots.txt)
set(ROOT_ARGS "")
if(EXISTS ${ROOTS})
  set(ROOT_ARGS --roots ${ROOTS})
endif()

# Mapper/banked cartridges such as Super Mario Land 2 are run through the
# interpreter.  ROM-only 32 KiB cartridges still use the static recompiler.
file(SIZE "${ROM}" ROM_SIZE)
set(RECOMP_ARGS "")
if(ROM_SIZE GREATER 32768)
  set(RECOMP_ARGS --interp-only)
endif()

add_custom_command(
  OUTPUT ${GEN}/game.c ${GEN}/interp.c ${GEN}/game_info.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp.py
          ${ROM} -o ${GEN} ${ROOT_ARGS} ${RECOMP_ARGS}
  DEPENDS ${ROM} ${CMAKE_SOURCE_DIR}/tools/recomp.py ${CMAKE_SOURCE_DIR}/tools/sm83.py
)

file(GLOB RT CONFIGURE_DEPENDS runtime/*.c)
if(WIN32)
  set(PIPECLEAN_ICON_RC ${CMAKE_SOURCE_DIR}/assets/pipeclean.rc)
endif()

add_executable(PipeClean ${RT} ${GEN}/game.c ${GEN}/interp.c ${PIPECLEAN_ICON_RC})
target_include_directories(PipeClean PRIVATE runtime ${GEN})
target_compile_definitions(PipeClean PRIVATE USE_SDL)
# runtime/main.c intentionally defines SDL_MAIN_HANDLED, so PipeClean owns
# the normal C entry point and does not link SDL2main on Windows.
target_link_libraries(PipeClean PRIVATE SDL2::SDL2)
if(NOT WIN32)
  target_link_libraries(PipeClean PRIVATE m)
endif()
if(WIN32)
  target_link_libraries(PipeClean PRIVATE shell32 ole32 comdlg32 uuid)
endif()

add_custom_target(pipeclean_assets ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/assets
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/assets/pipeclean-icon.bmp
          ${CMAKE_BINARY_DIR}/assets/pipeclean-icon.bmp
)
add_dependencies(PipeClean pipeclean_assets)

# Small hardware-independent regression tests.
enable_testing()
add_executable(test_sml2_wide tests/test_sml2_wide.c runtime/widescreen.c)
target_include_directories(test_sml2_wide PRIVATE runtime)
add_test(NAME sml2_widescreen COMMAND test_sml2_wide)

add_executable(test_sml_wide tests/test_sml_wide.c runtime/widescreen.c)
target_include_directories(test_sml_wide PRIVATE runtime)
add_test(NAME sml_widescreen COMMAND test_sml_wide)

add_executable(test_cart_mbc1 tests/test_cart_mbc1.c runtime/cart.c runtime/patch.c)
target_include_directories(test_cart_mbc1 PRIVATE runtime)
add_test(NAME cart_mbc1 COMMAND test_cart_mbc1)

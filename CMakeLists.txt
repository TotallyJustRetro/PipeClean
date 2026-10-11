cmake_minimum_required(VERSION 3.16)
project(PipeClean C)

# Desktop: cmake -S . -B build -DROM=path/to/game.gb
# Android: open android/ in Android Studio after android/setup-android.ps1.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ROM "" CACHE FILEPATH "ROM used by the static-recompilation generator")

if(ANDROID)
  if(NOT ROM)
    set(ROM "${CMAKE_BINARY_DIR}/pipeclean_build_stub.gb")
    execute_process(
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/make_android_build_stub.py" "${ROM}"
      RESULT_VARIABLE STUB_RESULT
      ERROR_VARIABLE STUB_ERROR
    )
    if(NOT STUB_RESULT EQUAL 0)
      message(FATAL_ERROR "Could not create Android build stub ROM: ${STUB_ERROR}")
    endif()
  endif()

  set(PIPECLEAN_SDL2_SOURCE_DIR "" CACHE PATH "Path to SDL2 source checkout (release-2.32.10)")
  set(PIPECLEAN_SDL2_IMAGE_SOURCE_DIR "" CACHE PATH "Path to SDL2_image source checkout (release-2.8.12)")
  if(NOT EXISTS "${PIPECLEAN_SDL2_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set PIPECLEAN_SDL2_SOURCE_DIR to the SDL2 source checkout; see android/README.md")
  endif()
  if(NOT EXISTS "${PIPECLEAN_SDL2_IMAGE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set PIPECLEAN_SDL2_IMAGE_SOURCE_DIR to the SDL2_image source checkout; see android/README.md")
  endif()

  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL2IMAGE_VENDORED ON CACHE BOOL "" FORCE)
  set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL2IMAGE_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL2IMAGE_AVIF OFF CACHE BOOL "" FORCE)
  set(SDL2IMAGE_JXL OFF CACHE BOOL "" FORCE)
  set(SDL2IMAGE_TIF OFF CACHE BOOL "" FORCE)
  set(SDL2IMAGE_SVG OFF CACHE BOOL "" FORCE)
  add_subdirectory("${PIPECLEAN_SDL2_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL2")
  add_subdirectory("${PIPECLEAN_SDL2_IMAGE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL2_image")
else()
  if(NOT ROM)
    message(FATAL_ERROR "pass -DROM=path/to/your/game.gb for desktop builds")
  endif()
  find_package(SDL2 REQUIRED)
  find_package(SDL2_image REQUIRED)
endif()

set(GEN ${CMAKE_BINARY_DIR}/generated)
set(ROOTS ${CMAKE_SOURCE_DIR}/roots.txt)
set(ROOT_ARGS "")
if(EXISTS ${ROOTS})
  set(ROOT_ARGS --roots ${ROOTS})
endif()

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

if(ANDROID)
  # SDLActivity loads libmain.so from the APK. Keep the desktop target name unchanged.
  add_library(main SHARED ${RT} ${GEN}/game.c ${GEN}/interp.c)
  set(PIPECLEAN_RUNTIME_TARGET main)
else()
  add_executable(PipeClean ${RT} ${GEN}/game.c ${GEN}/interp.c ${PIPECLEAN_ICON_RC})
  set(PIPECLEAN_RUNTIME_TARGET PipeClean)
endif()
target_include_directories(${PIPECLEAN_RUNTIME_TARGET} PRIVATE runtime ${GEN})
target_compile_definitions(${PIPECLEAN_RUNTIME_TARGET} PRIVATE USE_SDL)
option(PIPECLEAN_DEV_BUILD "Enable live multiplayer diagnostics window and session logs" OFF)
if(PIPECLEAN_DEV_BUILD)
  target_compile_definitions(${PIPECLEAN_RUNTIME_TARGET} PRIVATE PIPECLEAN_DEV_BUILD=1)
endif()
if(ANDROID)
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE SDL2::SDL2 SDL2_image)
  if(TARGET SDL2main)
    target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE SDL2main)
  endif()
else()
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE SDL2::SDL2 SDL2_image::SDL2_image)
endif()
if(NOT WIN32)
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE m)
endif()
if(WIN32)
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE shell32 ole32 comdlg32 uuid)
endif()

if(NOT ANDROID)
enable_testing()
add_executable(test_sml2_wide tests/test_sml2_wide.c runtime/widescreen.c)
target_include_directories(test_sml2_wide PRIVATE runtime)
add_test(NAME sml2_widescreen COMMAND test_sml2_wide)

add_executable(test_sml_wide tests/test_sml_wide.c runtime/widescreen.c)
target_include_directories(test_sml_wide PRIVATE runtime)
add_test(NAME sml_wide COMMAND test_sml_wide)

add_executable(test_cart_mbc1 tests/test_cart_mbc1.c runtime/cart.c runtime/patch.c)
target_include_directories(test_cart_mbc1 PRIVATE runtime)
add_test(NAME cart_mbc1 COMMAND test_cart_mbc1)

add_executable(test_recomp_guard tests/test_recomp_guard.c runtime/cart.c runtime/patch.c)
target_include_directories(test_recomp_guard PRIVATE runtime ${GEN})
# game_info.h is generated alongside the native-code module.
add_dependencies(test_recomp_guard ${PIPECLEAN_RUNTIME_TARGET})
get_filename_component(ROM_TEST_PATH "${ROM}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
add_test(NAME recomp_guard COMMAND test_recomp_guard "${ROM_TEST_PATH}")

endif() # NOT ANDROID: host-only regression executables

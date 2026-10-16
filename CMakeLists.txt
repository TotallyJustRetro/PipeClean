cmake_minimum_required(VERSION 3.16)
project(PipeClean C)

# Desktop: cmake -S . -B build -DROM=path/to/game.gb
# Android: open android/ in Android Studio after android/setup-android.ps1.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Secure Internet relays use mbedTLS and a current Mozilla-derived CA bundle.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/cacert.pem")
  file(DOWNLOAD "https://curl.se/ca/cacert.pem"
       "${CMAKE_SOURCE_DIR}/assets/cacert.pem"
       STATUS PIPECLEAN_CA_DOWNLOAD_STATUS TLS_VERIFY ON)
  list(GET PIPECLEAN_CA_DOWNLOAD_STATUS 0 PIPECLEAN_CA_DOWNLOAD_RESULT)
  if(NOT PIPECLEAN_CA_DOWNLOAD_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not download assets/cacert.pem; download https://curl.se/ca/cacert.pem and retry")
  endif()
endif()

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
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
  set(PIPECLEAN_MBEDTLS_SOURCE_DIR "" CACHE PATH "Path to mbedTLS source checkout (mbedtls-3.6.2)")
  if(NOT EXISTS "${PIPECLEAN_SDL2_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set PIPECLEAN_SDL2_SOURCE_DIR to the SDL2 source checkout; see android/README.md")
  endif()
  if(NOT EXISTS "${PIPECLEAN_SDL2_IMAGE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set PIPECLEAN_SDL2_IMAGE_SOURCE_DIR to the SDL2_image source checkout; see android/README.md")
  endif()
  if(NOT EXISTS "${PIPECLEAN_MBEDTLS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set PIPECLEAN_MBEDTLS_SOURCE_DIR to the mbedTLS source checkout; run android/setup-android.ps1")
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
  set(SDL2IMAGE_WEBP OFF CACHE BOOL "" FORCE)
  add_subdirectory("${PIPECLEAN_SDL2_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL2")
  add_subdirectory("${PIPECLEAN_SDL2_IMAGE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL2_image")
  add_subdirectory("${PIPECLEAN_MBEDTLS_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mbedtls")
else()
  if(NOT ROM)
    message(FATAL_ERROR "pass -DROM=path/to/your/game.gb for desktop builds")
  endif()
  find_package(SDL2 REQUIRED)
  find_package(SDL2_image REQUIRED)
  include(FetchContent)
  FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG mbedtls-3.6.2
    GIT_SHALLOW TRUE
    GIT_SUBMODULES framework
  )
  FetchContent_MakeAvailable(mbedtls)
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
target_compile_definitions(${PIPECLEAN_RUNTIME_TARGET} PRIVATE "PIPECLEAN_SOURCE_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/assets\"")
option(PIPECLEAN_DEV_BUILD "Enable live multiplayer diagnostics window and session logs" OFF)
if(PIPECLEAN_DEV_BUILD)
  target_compile_definitions(${PIPECLEAN_RUNTIME_TARGET} PRIVATE PIPECLEAN_DEV_BUILD=1)
endif()
if(ANDROID)
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE SDL2::SDL2 SDL2_image mbedtls mbedx509 mbedcrypto)
  if(TARGET SDL2main)
    target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE SDL2main)
  endif()
else()
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE SDL2::SDL2 SDL2_image::SDL2_image mbedtls mbedx509 mbedcrypto)
endif()
if(NOT WIN32)
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE m)
endif()
if(WIN32)
  target_link_libraries(${PIPECLEAN_RUNTIME_TARGET} PRIVATE shell32 ole32 comdlg32 uuid ws2_32)
endif()

if(NOT ANDROID)
enable_testing()
add_executable(test_companion_deck_state tests/test_companion_deck_state.c)
target_include_directories(test_companion_deck_state PRIVATE runtime)
add_test(NAME companion_deck_state COMMAND test_companion_deck_state)

add_executable(test_collectible_museum tests/test_collectible_museum.c runtime/museum.c runtime/settings.c)
target_include_directories(test_collectible_museum PRIVATE runtime)
add_test(NAME collectible_museum COMMAND test_collectible_museum)

add_executable(test_sml2_wide tests/test_sml2_wide.c runtime/widescreen.c)
target_include_directories(test_sml2_wide PRIVATE runtime)
add_test(NAME sml2_widescreen COMMAND test_sml2_wide)

add_executable(test_sml_wide tests/test_sml_wide.c runtime/widescreen.c)
target_include_directories(test_sml_wide PRIVATE runtime)
add_test(NAME sml_wide COMMAND test_sml_wide)

add_executable(test_wario_sml3_wide tests/test_wario_sml3_wide.c runtime/widescreen.c)
target_include_directories(test_wario_sml3_wide PRIVATE runtime)
add_test(NAME wario_sml3_widescreen COMMAND test_wario_sml3_wide)

add_executable(test_wario_land2_wide tests/test_wario_land2_wide.c runtime/widescreen.c)
target_include_directories(test_wario_land2_wide PRIVATE runtime)
add_test(NAME wario_land2_widescreen COMMAND test_wario_land2_wide)

add_executable(test_wario_land3_wide tests/test_wario_land3_wide.c runtime/widescreen.c)
target_include_directories(test_wario_land3_wide PRIVATE runtime)
add_test(NAME wario_land3_widescreen COMMAND test_wario_land3_wide)

add_executable(test_wario_event_state tests/test_wario_event_state.c)
target_include_directories(test_wario_event_state PRIVATE runtime)
add_test(NAME wario_event_state COMMAND test_wario_event_state)

add_executable(test_wario_land2_state tests/test_wario_land2_state.c)
target_include_directories(test_wario_land2_state PRIVATE runtime)
add_test(NAME wario_land2_state COMMAND test_wario_land2_state)

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

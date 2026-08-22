cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

set(FFMPEG_INCLUDE_DIR "" CACHE PATH "Directory containing FFmpeg headers")
set(AVCODEC_LIB "" CACHE FILEPATH "Path to avcodec import library")
set(AVUTIL_LIB "" CACHE FILEPATH "Path to avutil import library")
set(SWRESAMPLE_LIB "" CACHE FILEPATH "Path to swresample import library")
set(SCRCPY_PACKAGE_DIR "C:/Program Files/scrcpy/4.1" CACHE PATH "scrcpy package directory")

if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
  message(FATAL_ERROR "FFMPEG_INCLUDE_DIR must point to FFmpeg include directory")
endif()

foreach(import_lib IN ITEMS "${AVCODEC_LIB}" "${AVUTIL_LIB}" "${SWRESAMPLE_LIB}")
  if(NOT EXISTS "${import_lib}")
    message(FATAL_ERROR "Missing FFmpeg import library: ${import_lib}")
  endif()
endforeach()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/airplay-log-filter.cpp
    src/airplay-runtime.cpp
    src/airplay-source.cpp
    src/airplay-video-format.cpp
    src/scrcpy-command.cpp
    src/scrcpy-disconnected-frame.cpp
    src/scrcpy-low-latency.cpp
    src/scrcpy-runtime.cpp
    src/scrcpy-settings.cpp
    src/scrcpy-source.cpp
    src/airplay-log-filter.h
    src/airplay-runtime.h
    src/airplay-source.h
    src/airplay-video-format.h
    src/scrcpy-command.h
    src/scrcpy-disconnected-frame.h
    src/scrcpy-low-latency.h
    src/scrcpy-runtime.h
    src/scrcpy-settings.h
    src/scrcpy-source.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${FFMPEG_INCLUDE_DIR}" src)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    "${AVCODEC_LIB}"
    "${AVUTIL_LIB}"
    "${SWRESAMPLE_LIB}"
    Ws2_32
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

include(CTest)
if(BUILD_TESTING)
  add_executable(
    scrcpy-command-test
    tests/scrcpy-command-test.cpp
    src/scrcpy-command.cpp
  )
  target_include_directories(scrcpy-command-test PRIVATE src)
  add_test(NAME scrcpy-command-test COMMAND scrcpy-command-test)

  add_executable(
    scrcpy-disconnected-frame-test
    tests/scrcpy-disconnected-frame-test.cpp
    src/scrcpy-disconnected-frame.cpp
  )
  target_include_directories(scrcpy-disconnected-frame-test PRIVATE src)
  add_test(
    NAME scrcpy-disconnected-frame-test
    COMMAND scrcpy-disconnected-frame-test
  )

  add_executable(
    scrcpy-low-latency-test
    tests/scrcpy-low-latency-test.cpp
    src/scrcpy-low-latency.cpp
  )
  target_include_directories(scrcpy-low-latency-test PRIVATE src)
  add_test(NAME scrcpy-low-latency-test COMMAND scrcpy-low-latency-test)

  add_executable(
    airplay-video-format-test
    tests/airplay-video-format-test.cpp
    src/airplay-video-format.cpp
  )
  target_include_directories(
    airplay-video-format-test
    PRIVATE
      "${FFMPEG_INCLUDE_DIR}"
      src
  )
  add_test(NAME airplay-video-format-test COMMAND airplay-video-format-test)

  add_executable(
    airplay-log-filter-test
    tests/airplay-log-filter-test.cpp
    src/airplay-log-filter.cpp
  )
  target_include_directories(airplay-log-filter-test PRIVATE src)
  add_test(NAME airplay-log-filter-test COMMAND airplay-log-filter-test)
endif()

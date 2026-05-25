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
set(SCRCPY_PACKAGE_DIR "C:/Users/rokas/Downloads/scrcpy-win64-v4.0" CACHE PATH "scrcpy package directory")

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
    src/scrcpy-command.cpp
    src/scrcpy-runtime.cpp
    src/scrcpy-settings.cpp
    src/scrcpy-source.cpp
    src/scrcpy-command.h
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


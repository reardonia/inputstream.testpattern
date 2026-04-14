cmake_minimum_required(VERSION 3.10)
project(inputstream.testpattern)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

if(NOT PLATFORM_TAG)
  set(PLATFORM_TAG "linux")
endif()

find_package(Kodi REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)
pkg_check_modules(LIBDRM REQUIRED libdrm)

set(TESTPATTERN_SOURCES
    src/addon.cpp
    src/TestPatternCodec.cpp)

set(TESTPATTERN_HEADERS
    src/TestPatternInputStream.h
    src/TestPatternCodec.h)

set(DEPLIBS ${EGL_LIBRARIES} ${GLESV2_LIBRARIES} ${LIBDRM_LIBRARIES})

include_directories(${KODI_INCLUDE_DIR}/..
                    ${EGL_INCLUDE_DIRS}
                    ${GLESV2_INCLUDE_DIRS}
                    ${LIBDRM_INCLUDE_DIRS})

addon_version(inputstream.testpattern TESTPATTERN)
build_addon(inputstream.testpattern TESTPATTERN DEPLIBS)

include(CPack)

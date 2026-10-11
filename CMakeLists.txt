cmake_minimum_required(VERSION 3.16)
project(hazf VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(HAZF_BUILD_TESTS "Build tests" ON)

# Dependencies: Zstandard
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(ZSTD libzstd)
endif()

if(NOT ZSTD_FOUND)
    # Fallback to find_library / find_path
    find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
    find_library(ZSTD_LIBRARY NAMES zstd)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
        set(ZSTD_FOUND TRUE)
        set(ZSTD_INCLUDE_DIRS ${ZSTD_INCLUDE_DIR})
        set(ZSTD_LIBRARIES ${ZSTD_LIBRARY})
        set(ZSTD_CFLAGS_OTHER "")
        set(ZSTD_LIBRARY_DIRS "")
    endif()
endif()

if(NOT ZSTD_FOUND)
    message(FATAL_ERROR "libzstd not found. Install libzstd-dev (apt) or zstd (brew).")
endif()

# Executable
add_executable(hazf
    src/main.cpp
)

target_include_directories(hazf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZSTD_INCLUDE_DIRS}
)

target_link_directories(hazf PRIVATE
    ${ZSTD_LIBRARY_DIRS}
)

target_link_libraries(hazf PRIVATE
    ${ZSTD_LIBRARIES}
)

# Include dirs for ZSTD may need extra flags
if(ZSTD_CFLAGS_OTHER)
    target_compile_options(hazf PRIVATE ${ZSTD_CFLAGS_OTHER})
endif()

target_compile_options(hazf PRIVATE -Wall -Wextra -Wpedantic)

# Filesystem linking for older GCC
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(hazf PRIVATE stdc++fs)
endif()

# Install
include(GNUInstallDirs)

install(TARGETS hazf
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# MIME descriptor for desktop Linux (shared-mime-info)
install(FILES packaging/debian/hazf.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/mime/packages
)

# Tests
if(HAZF_BUILD_TESTS)
    enable_testing()
    # smoke.sh will build check after install or directly run binary
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        add_test(NAME smoke
            COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke.sh
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        # Ensure test can find built binary via PATH or direct
        set_tests_properties(smoke PROPERTIES
            ENVIRONMENT "HAZF_BIN=$<TARGET_FILE:hazf>"
        )
    endif()
endif()

# Package info
set(CPACK_PACKAGE_NAME "hazf")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Hyggshi OS <tfhy5321@gmail.com>")
set(CPACK_GENERATOR "TGZ;DEB")
include(CPack)

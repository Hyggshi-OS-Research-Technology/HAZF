cmake_minimum_required(VERSION 3.16)
project(hazf VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(HAZF_BUILD_TESTS "Build tests" ON)
option(HAZF_BUILD_GUI "Build GTK GUI" ON)

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
    message(STATUS "libzstd not found via system, fetching from GitHub via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        SOURCE_SUBDIR build/cmake
    )
    # Disable programs/tests for faster fetch build
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static)
        set(ZSTD_LIBRARIES libzstd_static)
        set(ZSTD_INCLUDE_DIRS ${zstd_SOURCE_DIR}/lib)
        set(ZSTD_LIBRARY_DIRS "")
        set(ZSTD_CFLAGS_OTHER "")
        set(ZSTD_FOUND TRUE)
    elseif(TARGET zstd::libzstd_static)
        set(ZSTD_LIBRARIES zstd::libzstd_static)
        set(ZSTD_INCLUDE_DIRS ${zstd_SOURCE_DIR}/lib)
        set(ZSTD_LIBRARY_DIRS "")
        set(ZSTD_CFLAGS_OTHER "")
        set(ZSTD_FOUND TRUE)
    else()
        message(FATAL_ERROR "Failed to fetch libzstd via FetchContent")
    endif()
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

# GUI (GTK4 - latest)
if(HAZF_BUILD_GUI)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GTK4 QUIET gtk4)
        # Optional: libadwaita for even more polish, but not required
        pkg_check_modules(ADWAITA QUIET libadwaita-1)
    endif()
    if(GTK4_FOUND)
        message(STATUS "Found GTK4 ${GTK4_VERSION} — building hazf-gui")
        add_executable(hazf-gui
            gui/src/gui_main.cpp
        )
        target_include_directories(hazf-gui PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${ZSTD_INCLUDE_DIRS}
            ${GTK4_INCLUDE_DIRS}
        )
        target_link_directories(hazf-gui PRIVATE
            ${ZSTD_LIBRARY_DIRS}
            ${GTK4_LIBRARY_DIRS}
        )
        target_link_libraries(hazf-gui PRIVATE
            ${ZSTD_LIBRARIES}
            ${GTK4_LIBRARIES}
        )
        target_compile_options(hazf-gui PRIVATE
            ${ZSTD_CFLAGS_OTHER}
            ${GTK4_CFLAGS_OTHER}
            -Wall -Wextra -Wpedantic
        )
        # If libadwaita found, enable it
        if(ADWAITA_FOUND)
            target_include_directories(hazf-gui PRIVATE ${ADWAITA_INCLUDE_DIRS})
            target_link_directories(hazf-gui PRIVATE ${ADWAITA_LIBRARY_DIRS})
            target_link_libraries(hazf-gui PRIVATE ${ADWAITA_LIBRARIES})
            target_compile_definitions(hazf-gui PRIVATE HAVE_ADWAITA=1)
            target_compile_options(hazf-gui PRIVATE ${ADWAITA_CFLAGS_OTHER})
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
            target_link_libraries(hazf-gui PRIVATE stdc++fs)
        endif()
        install(TARGETS hazf-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES gui/data/com.hyggshi.hazf.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
        install(FILES gui/data/icons/hicolor/scalable/apps/com.hyggshi.hazf.svg DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
        # Also install GUI CSS as resource (optional)
        install(FILES gui/resources/style.css DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/hazf)
    else()
        message(STATUS "GTK4 not found (install libgtk-4-dev) — skipping hazf-gui. CLI still builds.")
    endif()
endif()

# Package info
set(CPACK_PACKAGE_NAME "hazf")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Hyggshi OS <tfhy5321@gmail.com>")
set(CPACK_GENERATOR "TGZ;DEB")
include(CPack)

cmake_minimum_required(VERSION 3.20)
project(Revenant VERSION 3.0.0 LANGUAGES CXX
        DESCRIPTION "x64 function hooking engine for Windows")

if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Revenant supports Windows x64 only")
endif()

set(REVENANT_TOP_LEVEL OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(REVENANT_TOP_LEVEL ON)
endif()

option(REVENANT_SHARED       "Build revenant as a DLL instead of a static library" OFF)
option(REVENANT_GRAPHICS     "Include revenant::graphics (D3D9/11/12/OpenGL entry finders)" ON)
option(REVENANT_BUILD_TESTS  "Build the test suite" ${REVENANT_TOP_LEVEL})
option(REVENANT_BUILD_DEMO   "Build the D3D11 + ImGui overlay demo" OFF)
option(REVENANT_INSTALL      "Generate install rules" ${REVENANT_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REVENANT_SOURCES
    src/alloc.cpp
    src/engine.cpp
    src/freeze.cpp
    src/hwbp.cpp
    src/reloc.cpp
    src/lde/rvlde.cpp
)
if (REVENANT_GRAPHICS)
    list(APPEND REVENANT_SOURCES src/graphics.cpp)
endif()

if (REVENANT_SHARED)
    add_library(revenant SHARED ${REVENANT_SOURCES} src/dllmain.cpp)
    target_compile_definitions(revenant PRIVATE REVENANT_EXPORTS)
else()
    add_library(revenant STATIC ${REVENANT_SOURCES})
    target_compile_definitions(revenant PUBLIC REVENANT_STATIC)
endif()
add_library(revenant::revenant ALIAS revenant)

target_include_directories(revenant
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE src)
target_compile_features(revenant PUBLIC cxx_std_20)

if (MSVC)
    target_compile_options(revenant PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(revenant PRIVATE -Wall -Wextra)
endif()

if (REVENANT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (REVENANT_BUILD_DEMO)
    add_subdirectory(demo)
endif()

if (REVENANT_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS revenant EXPORT revenantTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/revenant DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT revenantTargets
        NAMESPACE revenant::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/revenant)

    configure_package_config_file(cmake/revenantConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/revenantConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/revenant)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/revenantConfigVersion.cmake
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/revenantConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/revenantConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/revenant)
endif()

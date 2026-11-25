cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)    # no /W3 in the default flags of MSVC, so that /W4 is the only level
endif()

project(mncf VERSION 1.1.0 LANGUAGES C)

option(MNCF_TESTS "Build the unit tests and the conformance runner" ON)
option(MNCF_AMALGAMATION "Build the library from its single-file amalgamation (mncf.c and mncf.h)" OFF)
set(MNCF_SANITIZE "" CACHE STRING "Sanitizers for GCC and Clang, such as address,undefined")
option(MNCF_WERROR "Treat warnings as errors" OFF)

set(MNCF_HEADERS src/internal.h src/encode.h src/model.h src/evaluate.h src/zip.h)
set(MNCF_SOURCES
    src/util.c src/toml.c src/crypto.c src/signature.c src/platform.c src/encode.c
    src/schema.c src/load.c src/evaluate.c src/engine.c src/zip.c src/pack.c src/update.c)

# The amalgamation: include/mncf.h and every source in one file, made by tools/amalgamate.cmake.
set(MNCF_AMALGAMATION_DIR ${CMAKE_CURRENT_BINARY_DIR}/amalgamation)
add_custom_command(
    OUTPUT ${MNCF_AMALGAMATION_DIR}/mncf.c ${MNCF_AMALGAMATION_DIR}/mncf.h
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT_DIR=${MNCF_AMALGAMATION_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.cmake
    DEPENDS include/mncf.h ${MNCF_HEADERS} ${MNCF_SOURCES} tools/amalgamate.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Making the amalgamation mncf.c and mncf.h")
add_custom_target(mncf_amalgamation DEPENDS ${MNCF_AMALGAMATION_DIR}/mncf.c ${MNCF_AMALGAMATION_DIR}/mncf.h)

if(MNCF_AMALGAMATION)
    add_library(mncf ${MNCF_AMALGAMATION_DIR}/mncf.c)
    target_include_directories(mncf PUBLIC $<BUILD_INTERFACE:${MNCF_AMALGAMATION_DIR}>)
else()
    add_library(mncf ${MNCF_SOURCES})
    target_include_directories(mncf PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
endif()
target_include_directories(mncf PUBLIC $<INSTALL_INTERFACE:include>)
set_target_properties(mncf PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                      VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
if(BUILD_SHARED_LIBS)
    target_compile_definitions(mncf PUBLIC MNCF_SHARED PRIVATE MNCF_BUILDING)
    set_target_properties(mncf PROPERTIES C_VISIBILITY_PRESET hidden)
endif()

if(MSVC)
    set(MNCF_WARNINGS /W4 /wd4996)
    if(MNCF_WERROR)
        list(APPEND MNCF_WARNINGS /WX)
    endif()
else()
    set(MNCF_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual
                      -Wformat=2 -Wundef)
    if(MNCF_WERROR)
        list(APPEND MNCF_WARNINGS -Werror)
    endif()
    find_library(MNCF_LIBM m)
    if(MNCF_LIBM)
        target_link_libraries(mncf PRIVATE ${MNCF_LIBM})
    endif()
endif()
target_compile_options(mncf PRIVATE ${MNCF_WARNINGS})

if(MNCF_SANITIZE)
    set(MNCF_SANITIZE_FLAGS -fsanitize=${MNCF_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_compile_options(mncf PUBLIC ${MNCF_SANITIZE_FLAGS})
    target_link_libraries(mncf PUBLIC ${MNCF_SANITIZE_FLAGS})
endif()

if(MNCF_TESTS)
    enable_testing()
    set(MNCF_SUITE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)

    add_executable(mncf_conformance tests/conformance.c)
    target_link_libraries(mncf_conformance PRIVATE mncf)
    set_target_properties(mncf_conformance PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    target_compile_options(mncf_conformance PRIVATE ${MNCF_WARNINGS})
    add_test(NAME conformance COMMAND mncf_conformance ${MNCF_SUITE})

    add_executable(mncf_unit tests/unit.c)
    target_include_directories(mncf_unit PRIVATE src)
    target_link_libraries(mncf_unit PRIVATE mncf)
    set_target_properties(mncf_unit PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    target_compile_options(mncf_unit PRIVATE ${MNCF_WARNINGS})
    if(NOT MSVC AND MNCF_LIBM)
        target_link_libraries(mncf_unit PRIVATE ${MNCF_LIBM})
    endif()
    add_test(NAME unit COMMAND mncf_unit ${MNCF_SUITE})
endif()

include(GNUInstallDirs)
install(TARGETS mncf EXPORT mncf-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/mncf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT mncf-targets NAMESPACE mncf:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mncf FILE mncf-config.cmake)

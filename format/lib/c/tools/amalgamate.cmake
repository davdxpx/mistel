# Makes the single-file amalgamation of the C library: mncf.h and mncf.c, which hold the whole library.
#
#     cmake -DSOURCE_DIR=format/lib/c -DOUTPUT_DIR=<folder> -P format/lib/c/tools/amalgamate.cmake
#
# mncf.h is include/mncf.h as it is. mncf.c is the internal headers and the sources in the order below,
# each without its own #include lines for the library's headers, after one #include "mncf.h".

if(NOT SOURCE_DIR OR NOT OUTPUT_DIR)
    message(FATAL_ERROR "amalgamate.cmake needs -DSOURCE_DIR=<format/lib/c> and -DOUTPUT_DIR=<folder>")
endif()

set(parts
    src/internal.h src/encode.h src/model.h src/evaluate.h src/zip.h
    src/util.c src/toml.c src/crypto.c src/signature.c src/platform.c src/encode.c
    src/schema.c src/load.c src/evaluate.c src/engine.c src/zip.c src/pack.c src/update.c)

file(READ ${SOURCE_DIR}/include/mncf.h header)
string(REGEX MATCH "#define MNCF_VERSION \"[^\"]*\"" version_line "${header}")
string(REGEX REPLACE ".*\"([^\"]*)\".*" "\\1" version "${version_line}")

set(out "/* mncf.c: the C library of the Mistel Native Cheat Format, version ${version}, in one file. MIT License.\n")
string(APPEND out " *\n")
string(APPEND out " * Made by tools/amalgamate.cmake from format/lib/c of the Mistel repository; change those files, not\n")
string(APPEND out " * this one. Compile it with mncf.h next to it, as C99 or later.\n")
string(APPEND out " */\n")
string(APPEND out "#include \"mncf.h\"\n")

foreach(part ${parts})
    file(READ ${SOURCE_DIR}/${part} text)
    # The library's own headers are part of this file already.
    string(REGEX REPLACE "#include \"[a-z_]+\\.h\"\n" "" text "${text}")
    string(APPEND out "\n/* ${part} */\n\n${text}")
endforeach()

file(MAKE_DIRECTORY ${OUTPUT_DIR})
file(WRITE ${OUTPUT_DIR}/mncf.h "${header}")
file(WRITE ${OUTPUT_DIR}/mncf.c "${out}")

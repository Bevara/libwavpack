cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBWAVPACK_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_wavpack.c
)

SET(LIBWAVPACK_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libwavpack.a
)

SET(LIBWAVPACK_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libwavpack
        "${LIBWAVPACK_SRC}"
        "${LIBWAVPACK_LIBS}"
        []
        ""
        "${LIBWAVPACK_INC}"
        ""
        "1")

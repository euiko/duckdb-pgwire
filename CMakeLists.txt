cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME duckdb_pgwire)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME}
    LANGUAGES CXX
    VERSION 0.0.1
)

include_directories(src/include)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXTENSION_SOURCES src/duckdb_pgwire_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# dependencies
option(PGWIRE_BUILD_DEMO "Build demo" OFF)
option(PGWIRE_BUILD_TEST "Build test" OFF)
add_subdirectory(pgwire)

target_link_libraries(${EXTENSION_NAME} pgwire)
target_link_libraries(${LOADABLE_EXTENSION_NAME} pgwire)

install(
    TARGETS ${EXTENSION_NAME} pgwire asio endian promise
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)

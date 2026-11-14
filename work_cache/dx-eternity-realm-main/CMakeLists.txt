cmake_minimum_required(VERSION 3.20)
project(dx-eternity-realm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always output built server binaries directly into misc/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/misc")

set(SQLITE3_AMALGAMATION_DIR "${CMAKE_SOURCE_DIR}/.deps/src")

add_executable(dx-eternity-realm
    Server/main.cpp
    Server/admin/realm_admin_server.cpp
    Server/admin/realm_updater.cpp
    Server/anticheat/anomaly_detector.cpp
    Server/auth/realm_auth.cpp
    Server/db/realm_database.cpp
    Server/game/item_tables.cpp
    Server/game/item_verifier.cpp
    Server/net/packet_codec.cpp
    Server/net/realm_server.cpp
    Server/session/realm_session.cpp
    Server/vault/realm_vault.cpp
)

target_include_directories(dx-eternity-realm PRIVATE
    "${CMAKE_SOURCE_DIR}/Server"
    "${SQLITE3_AMALGAMATION_DIR}"
)

target_compile_definitions(dx-eternity-realm PRIVATE
    SODIUM_STATIC=1
)

if(WIN32)
    # Windows 64-bit static cross-compilation using cached MinGW libs in .deps/win64
    set(DEPS_WIN64_DIR "${CMAKE_SOURCE_DIR}/.deps/win64")
    target_include_directories(dx-eternity-realm PRIVATE "${DEPS_WIN64_DIR}/include")
    target_link_libraries(dx-eternity-realm PRIVATE
        "${DEPS_WIN64_DIR}/lib/libsqlite3.a"
        "${DEPS_WIN64_DIR}/lib/libsodium.a"
        ws2_32
        iphlpapi
    )
    target_link_options(dx-eternity-realm PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic
        -lwinpthread
    )
else()
    # Linux x86_64 static library linking for single-file VPS deployment
    if(EXISTS "${CMAKE_SOURCE_DIR}/.deps/linux/lib/libsqlite3.a")
        set(SQLITE3_STATIC_LIB "${CMAKE_SOURCE_DIR}/.deps/linux/lib/libsqlite3.a")
    else()
        find_library(SQLITE3_STATIC_LIB NAMES libsqlite3.a sqlite3 REQUIRED)
    endif()
    find_library(SODIUM_STATIC_LIB NAMES libsodium.a sodium REQUIRED)
    target_link_libraries(dx-eternity-realm PRIVATE
        "${SQLITE3_STATIC_LIB}"
        "${SODIUM_STATIC_LIB}"
        pthread
        dl
    )
    target_link_options(dx-eternity-realm PRIVATE
        -static-libgcc
        -static-libstdc++
    )
endif()

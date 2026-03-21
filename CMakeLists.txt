cmake_minimum_required(VERSION 3.13...3.25)

project(session-sqlite
    VERSION 1.0.0
    DESCRIPTION "SQLite C++ extensions and helpers"
    LANGUAGES CXX)

include(cmake/session-deps/Deps.cmake)

# We support multiple modes of operation:
# - building a bundled sqlite3-multiple-ciphers, supporting ChaCha20 and SQLCipher4 databases.  Note
#   that SQLCipher4 support will be slower than full SQLCipher4 and is mainly provided so that you
#   can still open SQLCipher4 databases.
#   sup

option(SESSION_SQLITE_BUILD_SQLITE3MC "Build a static sqlite3-multiple-ciphers library." ON)
option(SQLITE3MC_WITH_LIBICU "If performing a static sqlite3-multiple-ciphers build, also build and enable libicu" OFF)
option(LIBICU_SQLITE_ONLY "For a static sqlite3mc build, strip out various data from libicu not needed for sqlite+icu usage to significantly reduce binary size (removes icu features such as charset conversions, currency formatting, and timezone handling).  Disable if other things are using libicu depending on the extra data." OFF)

option(SESSION_EXTERNAL_SQLITE_MC "Set to true if using an external SQLite::SQLite3 target that is actually sqlite-multiple-ciphers, enabling ChaCha20 and SQLCipher support" OFF)
option(SESSION_EXTERNAL_SQLITE_SQLCIPHER "Set to true if using an external SQLite::SQLite3 target that is actually sqlcipher4, enabling SQLCipher support." OFF)
option(SESSION_EXTERNAL_SQLITE_LIBICU "Set to true if using an external SQLite::SQLite3 target that contains libicu support" OFF)

set(is_sqlite_mc FALSE)
set(is_sqlcipher FALSE)
if(SESSION_SQLITE_BUILD_SQLITE3MC)
    if(SQLite::SQLite3)
        message(FATAL_ERROR "SESSION_SQLITE_BUILD_SQLITE3MC is defined but there is also a pre-existing SQLite::SQLite3 target; use one or the other!")
    endif()

    set(BUILD_STATIC_sqlite3mc ON CACHE BOOL "" FORCE)
    if(SQLITE3MC_WITH_LIBICU)
        set(BUILD_STATIC_icu-io ON CACHE BOOL "" FORCE)
    endif()

    session_dep(sqlite3mc 2.2.5)

    # CMake won't let an alias link to an alias, so link to the session-deps internal target name:
    add_library(SQLite::SQLite3 ALIAS sessiondep_ext_sqlite3mc)

    set(is_sqlite_mc TRUE)

elseif(NOT TARGET SQLite::SQLite3)
    message(FATAL_ERROR "Either SESSION_SQLITE_BUILD_SQLITE3MC=ON or an SQLite::SQLite3 target must be defined")

    if(SESSION_EXTERNAL_SQLITE_SQLCIPHER)
        if(SESSION_EXTERNAL_SQLITE_MC)
            message(FATAL_ERROR "SESSION_EXTERNAL_SQLITE_SQLCIPHER and SESSION_EXTERNAL_SQLITE_MC are mutually exclusive")
        endif()
        set(is_sqlcipher TRUE)
    elseif(SESSION_EXTERNAL_SQLITE_MC)
        set(is_sqlite_mc TRUE)
    endif()
endif()

add_library(sqlite_sodium INTERFACE)
if(TARGET libsodium::sodium-internal)
    # This target is a tweaked version of libsodium used by libsession-util that exposes some internal
    # libsodium functions needed for libsession-util's crypto.
    target_link_libraries(sqlite_sodium INTERFACE libsodium::sodium-internal)
else()
    session_dep(libsodium 1.0)
    target_link_libraries(sqlite_sodium INTERFACE sessiondep::libsodium)
endif()

# Don't use SQLiteCpp's bundled sqlite:
set(SQLITECPP_INTERNAL_SQLITE OFF CACHE BOOL "" FORCE)

set(SQLITECPP_INCLUDE_SCRIPT OFF CACHE BOOL "" FORCE)

# This is not often used, and it's hard to know for sure whether our sqlite3 actually supports it,
# so just define it here defaulted to disabled so that SQLiteCpp's later declaration of it uses our
# default rather than its default enabled.  This allows an external caller to still override it if
# they want it (where a set(... FORCE) would not).
option(SQLITE_ENABLE_COLUMN_METADATA "Enable Column::getColumnOriginName(). Require support from sqlite3 library." OFF)

# Do the same for omitting extension loading, so that we default to ON, i.e. disabled, overriding SQLiteCpp's
# default on OFF, i.e. enabled extension loading:
option(SQLITE_OMIT_LOAD_EXTENSION "Omit SQLiteCpp extension loading support" ON)

set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)

# We force this off because otherwise SQLiteCpp unconditionally goes looking for a system sqlcipher,
# but even if we worked around that, all it does is enable implementations of
# `SQLite::Database::key()` and `::rekey()`, but those implementations take a std::string which is
# insecure by design (see SQLiteCpp issue 533).
set(SQLITECPP_HAS_CODEC OFF CACHE BOOL "" FORCE)


# Hack around SQLiteCpp's attempts to locate sqlite3 because we *don't* want to link against the
# system one, but don't download and build the embedded one until build time.  Thankfully it
# actually links against the SQLite::SQLite3 cmake target if it already exists, so all we have to do
# is set that up and circumvent some of the non-target bits of its FindSQLite3.cmake.
set(SQLite3_FOUND TRUE CACHE BOOL "" FORCE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ignored")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ignored/sqlite3.h" "#define SQLITE_VERSION \"${SQLite3_VERSION}\"")
set(SQLite3_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ignored" CACHE STRING "" FORCE)
set(SQLite3_LIBRARY "ignored" CACHE STRING "" FORCE)
set(SQLITE_ENABLE_COLUMN_METADATA OFF CACHE BOOL "" FORCE)
set(SQLITE_OMIT_LOAD_EXTENSION ON CACHE BOOL "" FORCE)


add_subdirectory(SQLiteCpp EXCLUDE_FROM_ALL)

# session::secure_buffer is a separate target so that code needing only secure memory management
# (e.g. the libsession-util crypto library) can depend on it without pulling in SQLite and the
# rest of the session-sqlite library.
add_library(session_secure_buffer src/secure_buffer.cpp)
target_compile_features(session_secure_buffer PUBLIC cxx_std_20)
target_include_directories(session_secure_buffer PUBLIC include)
target_link_libraries(session_secure_buffer PRIVATE sqlite_sodium)
add_library(session::secure_buffer ALIAS session_secure_buffer)

add_library(session_sqlite
    src/sqlite.cpp
    src/conn.cpp)
target_compile_features(session_sqlite PUBLIC cxx_std_20)
target_include_directories(session_sqlite PUBLIC include)
target_link_libraries(session_sqlite PUBLIC SQLiteCpp fmt session::secure_buffer)
target_link_libraries(session_sqlite PRIVATE sessiondep::sqlite3mc)

if((SESSION_SQLITE_BUILD_SQLITE3MC AND SQLITE3MC_WITH_LIBICU) OR
    (NOT SESSION_SQLITE_BUILD_SQLITE3MC AND SESSION_EXTERNAL_SQLITE_LIBICU))
    target_compile_definitions(session_sqlite PRIVATE SESSION_SQLITE_WITH_LIBICU)
endif()


if(is_sqlite_mc)
    target_compile_definitions(session_sqlite PRIVATE SESSION_SQLITE_MULTIPLE_CIPHERS)
    set(status "with ChaCha20, AEGIS256, and SQLCipher")
elseif(is_sqlcipher)
    target_compile_definitions(session_sqlite PRIVATE SESSION_SQLITE_SQLCIPHER)
    set(status "with SQLCipher only (no ChaCha20, AEGIS256)")
else()
    set(status "without any")
endif()
message(STATUS "Building session-sqlite ${status} encryption support")

if(SESSION_SQLITE_ENCRYPT)
    target_compile_definitions(session_sqlite PUBLIC SESSION_SQLITE_ENCRYPT)

    # Alter SQLiteCpp's so that it actually enables the key API.  This is a bit dirty, but currently
    # necessary because SQLiteCpp's cmake is a bit inflexible.
    target_compile_definitions(SQLiteCpp PRIVATE SQLITE_HAS_CODEC)
endif()

add_library(session::SQLite ALIAS session_sqlite)

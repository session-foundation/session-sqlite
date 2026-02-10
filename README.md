# SQLite interfaces and build scripts

This repository exists to serve as a common submodule for various Session projects that needs to
interact with SQLite.  It consists of three components:

- SQLiteCpp as a submodule, for a more C++ interface to interacting with an SQLite database than
  plain sqlite C API.

- A wrapper (session::sqlite::Database) around SQLiteCpp that adds some utility functions enabling
  simpler binding and thread-safe prepared query storage.

- Various utility functions to simplify query argument binding and value extraction from a result
  set.

- cmake build scipts for building and linking to SQLite3 Multiple Ciphers, with or without libicu
  support.

- Encrypted database support, using either sqlite3-mc or sqlcipher.  The former is recommended as it
  supports better performing (and at least as secure) encryption options such as AEGIS-256 and
  ChaCha20 as well as compatibility with SQLCipher's encryption format.

## Using this submodule from a cmake project

This project, when used via `add_subdirectory`, provides a session::SQLite target, and is intended
to be used as:

    add_directory(session-sqlite)
    target_link_libraries(mytarget PRIVATE session::SQLite)

By default, this will build a static sqlite3-multiple-ciphers for the sqlite implementation without
sqlite icu support.  You can, however, control this via the supported cmake options.  For example to
build sqlite3mc with icu support with a trimmed libicu data:

    set(SQLITE3MC_WITH_LIBICU ON CACHE BOOL "")
    set(LIBICU_SQLITE_ONLY ON CACHE BOOL "")
    add_subdirectory(session-sqlite)

Alternatively you can bring your own sqlite3 target; see the options in CMakeLists.txt for more
details on the available options and how to inform the library about the capability of the sqlite3
target you are bringing.

### Supported cmake options

You can control the build via these cmake options.

#### `SESSION_SQLITECPP_BUILD_SQLITE3MC`

To disable the sqlite3-mc auto-build behaviour, you can set this option cmake option to false using
one of:

    -DSESSION_SQLITECPP_BUILD_SQLITE3MC=OFF                         # from command-line
    set(SESSION_SQLITECPP_BUILD_SQLITE3MC OFF CACHE BOOL "" FORCE)  # from parent CMakeLists.txt

If this is OFF then the SQLite3::SQLite target must already exist (and a fatal error is raised if
not present).

When this is disabled (and the SQLite3::SQLite target is used, the existing SQLite target is not
assumed to support encryption APIs *unless* you also set one of the following options.

#### `SESSION_EXTERNAL_SQLITE_MC`, `SESSION_SQLITE_SQLCIPHER`

When using an external SQLite3::SQLite target (i.e. when using
SESSION_SQLITECPP_BUILD_SQLITE3MC=OFF) one of these two options can be set to communicate that
encryption support is available in the SQLite3::SQLite target (i.e. that it is actuall SQLite3-mc or
SQLCipher under the hood).  If SQLite3-multiple-ciphers then this enables all encryption types; if
SQLCipher then you only get SQLCipher3/4 support.

If neither is set then it is assumed that the external target is a regular sqlite3 (which does not
support the API) and encryption support will be disabled entirely.

This option has no effect when SESSION_SQLITECPP_BUILD_SQLITE3MC is enabled (i.e. all encryption
types will be supported).

#### `SQLITE3MC_WITH_LIBICU`

If this is option is enabled (the default is disabled) *and* we are building sqlite3-mc, then this
also builds and enables the libicu extension.  By default this will attempt to use a system libicu,
but that can be overridden via `BUILD_STATIC_icu-i18n=ON` (see the session-deps repo).

If using an *external* SQLite3::SQLite target then this option is ignored: libicu support is up to
whatever provides the external target.

## C++ API

### Overview

The starting point of this library is the `session::sqlite::Database` class, which manages database
connections and provides thread-safe access to multiple connections to the same database (allowing
threads to interact with the same database without blocking each other).

Once you have a Database object, you "check out" a Connection from it by calling `auto conn =
database.conn();`.  This gives you a per-thread connection object that you can use as long as
needed, but should only use from the same thread.  Upon destruction of the Connection wrapper, the
underlying connection is returned to the Database's pool of connection objects to be returned by the
next call to `database.conn()`.

This is designed to be thread-safe as long as Connection objects are thread-unique: if a thread that
already has a connection checked out tries to get another one (e.g. `f()` holds a Connection object
while it calls `g()`, which also wants to get a Connection) then it simply gets the same Connection
back (i.e. sharing with other instances in the same thread).  In most cases, this is safe: SQLite3
can execute multiple queries simultaneously on the same connection.

If a different thread tries to obtain a connection then Database operates as a connection pool: it
returns that thread a Connection object that does not share the underlying sqlite3 connection with
any other thread.

If there are no idle connections when a thread tries to check out a new one then the Database will
establish a new connection and return that.  Thus the maximum number of connections established will
effectively equal the maximum simultaneous thread parallelism of all database-using threads.  That
is, if you 10 threads but no more than 3 of them access a Connection at the same time, you will have
no more than 3 actual sqlite connections (even if those 3 threads differ).  This is normally fine,
but Database also provides methods to limit the number of idle connections if needed.

Each underlying connection (i.e. the real connection, not the public Connection wrapper) also
maintains a prepared statement cache of any previously prepared statements on that connection.  That
means that `auto st = conn.prepared_st("SELECT id FROM users WHERE name = ?");` (or any of the other
`conn.prepared_*` methods) will prepare that statement and reuse it (thus significantly improving
performance) if you prepare it again.  (This is also reentrant safe: i.e. if you prepare a statement
A and while processing its results, call code that prepares statement A again, you get a separate
prepared statement so as to not interfere with the first, in-use prepared statement).

### Database construction

You construct a Database as:

    using namespace session::sqlite;
    Database db{path_to_db, Encryption::AEGIS256, options...};

Supported Encryption values are as follows:

    None -- no encryption
    AEGIS256 -- Fast, modern, AES-based AEAD (faster with AES CPU instructions).  Recommended choice
                when primarily targetting modern desktop/laptop/mobile devices.
    ChaCha20 -- Fast, modern AEAD that is fast even on oldest hardware, like Raspberry Pis (<5) or
                older phones.  Recommended choice when targetting both newer and older devices.
    Ascon128 -- Extremely lightweight, modern AEAD designed for extremely limited capability
                special-purpose devices, such as IoT.
    SQLCipher4 -- Dated AES-CBC encryption with a HMAC-SHA512 per-page MAC.  The encryption is fine,
                  but the use of per-page HMAC-SHA512 is terrible for performance.  Recommended only
                  if interoperability with SQLCipher is required.
    SQLCipher3 -- Dated AES-CBC encryption with a HMAC-SHA1 per-page MAC, and fairly weak
                  password hashing.  Not recommended except when an existing SQLCipher3 database
                  must be opened.

When using the SQLCipher backend, only the SQLCipher options are available; all options are
supported by SQLite3-MC.  Encryption::None is the only allowed value if neither encryption
implementation is in use.

The remaining `options...` arguments passed to the constructor can be provided in any order.

For anything other than Encryption::None, exactly one of the following must be provided:

    raw_key{...} -- specifies a raw, 32-byte database key, which should ideally be cryptographically
                    secure random.
    argon2id_password{"pass"} -- uses argon2id for key generation (instead of the database's
                                 built-in default plaintext password KDF).
    plaintext_password{"pass"} -- uses the encryption type's default 

Other supported option values (none are required) are:

    plaintext_header{salt} -- this is used to open the database in "plaintext header" mode, which is
                              required for special snowflake iOS to be able to keep a database lock
                              held when a process is put to sleep.  The option requires the database
                              salt value to be stored externally and provided.
    busy_timeout{5s} -- a query timeout value to apply in case of a SQLite query timeout (typically
                        because of database modification contention): the query will be retried
                        automatically for up to this amount of time.
    no_wal -- the value should rarely be used: it allows disabling WAL mode, which significantly
              reduces database performance.  It is, however, sometimes needed for exotic setups such
              as when sharing database access via a network filesystem across multiple hosts.
    no_create -- when opening a database that does not exist, it is normally created automatically.
                 This option can be passed to make it fail instead.
    readonly -- can be passed to open the database in readonly mode.

### Prepared queries

The connection object provides convenience wrappers for binding query parameters, such as:

    auto st = conn.prepared_bind("SELECT id FROM users WHERE name = ? AND age > ?", n, 21);

This returns a StatementWrapper which wraps a SQLite::Transaction with automatic statement reset
(and return to the connections statement cache) upon destruction.

As well as conveinece methods to execute a no results query, a single result query, or an optional
result query:

    conn.prepared_exec("DELETE FROM users WHERE age > ?", 160);

    // id is an std::optional<int64_t>:
    auto id = conn.prepared_maybe_get<int64_t>(
        "SELECT id FROM users WHERE username = ? AND age >= ?", name, 21);

    if (id) {
        auto [name, last_login] = conn.prepared_get<std::string, int64_t>(
            "SELECT name, last_login FROM users WHERE id = ?", *id);
        std::cout << "Login by " << name << ", last login timestamp: " << last_login << "\n";
    }

Iterating through results (without pre-loading results) is available through the `prepared_results`
method, which similarly to the above provides a single value or tuple of values, but via an
iterator:

    int elderly = 70;
    for (const auto& [name, age] : conn.prepared_results<std::string_view, int>(
            "SELECT name, age FROM users WHERE age >= ?", elderly)
        std::cout << name << " is getting on at " << age << "years old\n";

### Additional utilities

The library also providers helper classes to easily bind and retrieve BLOB values: `blob` can be
used to efficiently extract blob values (i.e. without intermediate copies); `blob_guts` can be used
for one-shot value retrieval of blobs into structs (i.e. with a copy), and `blob_binder` and
`bind_guts` provide mechanisms to bind a buffer or contents of a simple struct, respectively, as
a query parameter.

The `placeholders()` function (via `#include <session/sqlite/placeholders.hpp>`) provider an fmt
helper to simplify `IN (...)` placeholder construction such as:

    std::vector<int64_t> ids = /*...*/;
    auto query = fmt::format("DELETE FROM users WHERE id IN ({})", placeholders(del.size()));

to build a query "DELETE FROM users WHERE id IN (?, ?, ?, ?)".  This function is not included by
default as it requires that fmt be already available (which isn't otherwise required by this
library).

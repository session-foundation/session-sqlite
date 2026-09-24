#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <session/sqlite.hpp>
#include <string>

using namespace session::sqlite;

// Reads `SELECT x FROM foo` from the in-memory database behind `c` through a separate raw
// connection using the plain memdb VFS, which bypasses any encryption shim: this sees the
// database pages exactly as they are stored.  Returns the sqlite result code of the query.
//
// Only usable on encrypted in-memory databases: sqlite3_db_filename returns "" for a database on
// the plain memdb VFS (but not for one on sqlite3mc's shim over it), and opening "" would give a
// fresh, unrelated database.
static int raw_memdb_read(Connection& c) {
    std::string name = sqlite3_db_filename(c.sql.getHandle(), "main");
    REQUIRE_FALSE(name.empty());
    auto uri = "file:" + name + "?vfs=memdb";
    sqlite3* raw;
    REQUIRE(sqlite3_open_v2(uri.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) ==
            SQLITE_OK);
    int rc = sqlite3_exec(raw, "SELECT x FROM foo", nullptr, nullptr, nullptr);
    UNSCOPED_INFO("raw read of " << uri << ": " << sqlite3_errmsg(raw));
    sqlite3_close(raw);
    return rc;
}

TEST_CASE("In-memory database is shared across connections", "[memory]") {
    Database db{":memory:"};

    auto c1 = db.conn();
    c1.sql.exec("CREATE TABLE foo (x INTEGER)");
    c1.sql.exec("INSERT INTO foo VALUES (42)");

    auto c2 = db.unique_conn();
    REQUIRE(&c1.sql != &c2.sql);

    REQUIRE(c2.table_exists("foo"));
    CHECK(c2.prepared_get<int>("SELECT x FROM foo") == 42);
}

TEST_CASE("Encrypted in-memory database is shared across connections", "[memory]") {
    if (!enabled(Encryption::AEGIS256))
        SKIP("AEGIS256 not supported by this build");

    std::array<std::byte, 32> key;
    key.fill(std::byte{0x42});
    Database db{":memory:", Encryption::AEGIS256, raw_key{key}};

    auto c1 = db.conn();
    c1.sql.exec("CREATE TABLE foo (x INTEGER)");
    c1.sql.exec("INSERT INTO foo VALUES (42)");

    auto c2 = db.unique_conn();
    REQUIRE(c2.table_exists("foo"));
    CHECK(c2.prepared_get<int>("SELECT x FROM foo") == 42);
}

TEST_CASE("Encrypted in-memory database is stored encrypted", "[memory]") {
    if (!enabled(Encryption::AEGIS256))
        SKIP("AEGIS256 not supported by this build");

    std::array<std::byte, 32> key;
    key.fill(std::byte{0x42});
    Database db{":memory:", Encryption::AEGIS256, raw_key{key}};
    auto c = db.conn();
    c.sql.exec("CREATE TABLE foo (x INTEGER)");

    // Not SQLITE_ERROR ("no such table"), which is what an empty or unrelated database would give:
    // NOTADB means it found our pages but couldn't parse them.
    CHECK(raw_memdb_read(c) == SQLITE_NOTADB);
}

TEST_CASE("Separate in-memory databases are independent", "[memory]") {
    Database db1{":memory:"};
    Database db2{":memory:"};

    auto c1 = db1.conn();
    c1.sql.exec("CREATE TABLE foo (x INTEGER)");

    auto c2 = db2.conn();
    CHECK_FALSE(c2.table_exists("foo"));
}

TEST_CASE("A new in-memory database does not inherit a destroyed one", "[memory]") {
    // Both iterations construct at the same stack address, so anything keyed on the Database
    // address would see the previous iteration's database.
    for (int i = 0; i < 2; i++) {
        Database db{":memory:"};
        auto c = db.conn();
        REQUIRE_FALSE(c.table_exists("foo"));
        c.sql.exec("CREATE TABLE foo (x INTEGER)");
    }
}

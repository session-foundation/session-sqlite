#include <catch2/catch_test_macros.hpp>
#include <session/sqlite.hpp>

using namespace session::sqlite;

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

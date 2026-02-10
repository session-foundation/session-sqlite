#include <thread>
#ifdef SESSION_SQLITE_SQLCIPHER
// sqlcipher for reasons that haven't been applicable for a decade doesn't expose its key API unless
// this is defined, so we need to ensure this is defined before anything that might include
// sqlite.hpp:
#define SQLITE_HAS_CODEC
#endif

#include <SQLiteCpp/Database.h>
#include <oxenc/hex.h>
#include <sodium/core.h>
#include <sodium/crypto_pwhash.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include <sqlite3.h>

#include <fstream>
#include <mutex>
#include <session/sqlite.hpp>
#include <stdexcept>

#include "conn.hpp"

namespace session::sqlite {

static void sodium_init() {
    if (0 != ::sodium_init())
        throw std::runtime_error{"Sodium init failed"};
}

using namespace std::literals;

bool enabled(Encryption type) {
    switch (type) {
        case Encryption::None: return true;
        case Encryption::ChaCha20:
        case Encryption::AEGIS256:
        case Encryption::Ascon128:
#ifdef SESSION_SQLITE_MULTIPLE_CIPHERS
            return true;
#else
            return false;
#endif
        case Encryption::SQLCipher3:
        case Encryption::SQLCipher4:
#if defined(SESSION_SQLITE_MULTIPLE_CIPHERS) || defined(SESSION_SQLITE_SQLCIPHER)
            return true;
#else
            return false;
#endif
    };
    return false;
}

Database::Database(
        std::filesystem::path db_path,
        Encryption enc,
        std::optional<plaintext_password> plaintext_pass,
        std::optional<raw_key> raw_key,
        std::optional<argon2id_password> argon2id_pass,
        std::optional<plaintext_header> plaintext_header_salt,
        std::optional<busy_timeout> busy_t_o,
        std::optional<wal_mode> wal_mode,
        std::optional<open_create> create,
        std::optional<open_readonly> readonly) :
        _db_path{std::move(db_path)}, _enc{enc} {

    if (!enabled(_enc))
        throw std::runtime_error{
                "Database: this build does not support the selected encryption type"};
    if (_enc == Encryption::None &&
        (raw_key || plaintext_pass || argon2id_pass || plaintext_header_salt))
        throw std::invalid_argument{
                "Database: cannot use Encryption::None with database encryption options"};

    if (_enc != Encryption::None && !(raw_key || plaintext_pass || argon2id_pass))
        throw std::invalid_argument{
                "Database: opening an encrypted database requires one of "
                "raw_key/plaintext_password/argon2id_password"};

    // If you have *explicitly* given create=true and readonly=true then that is a
    // error.  If you've only given readonly but omitted create then we imply create is
    // false.
    if (create && create->create && readonly && readonly->readonly)
        throw std::invalid_argument{
                "Database: conflict constructor arguments: readonly mode cannot be combined with "
                "an explicit open_create{true} argument"};

    _open_flags = (readonly && readonly->readonly ? SQLite::OPEN_READONLY : SQLite::OPEN_READWRITE)
                // "NOMUTEX" here actually means no *full* mutex, but makes sqlite safe to use from
                // multiple threads as long as a connection (and any connection-derived objects) are
                // not used from multiple threads.
                | SQLite::OPEN_NOMUTEX;
    _busy_timeout =
            (busy_t_o && busy_t_o->timeout >= 0s) ? busy_t_o->timeout : busy_timeout::DEFAULT;
    _wal = wal_mode ? wal_mode->wal : true;

    std::optional<std::array<std::byte, 16>> salt;

    if (plaintext_header_salt) {
        _plaintext_header = true;
        if (_enc == Encryption::SQLCipher3)
            throw std::invalid_argument{"SQLCipher3 and plaintext_header_salt are incompatible"};
        salt.emplace();
        std::memcpy(salt->data(), plaintext_header_salt->salt.data(), salt->size());
    }

    auto store_raw_key = [&](std::span<const std::byte, 32> key) {
        // We have to pass this to sqlcipher or sqlite-mc as "x'...'" where ... is hex; there is no
        // interface provided to pass a key as raw bytes.  This is dumb, of course, because it
        // requires pointless hex/unhex on each side *and* because if someone actually tried to use
        // x'hexstring' as a password, well, tough luck.
        auto rw = _key.resize(67 + (salt ? 32 : 0));
        rw.buf[0] = std::byte{'x'};
        rw.buf[1] = std::byte{'\''};
        oxenc::to_hex(key.begin(), key.end(), reinterpret_cast<char*>(rw.buf.data() + 2));
        if (salt)
            oxenc::to_hex(
                    salt->begin(), salt->end(), reinterpret_cast<char*>(rw.buf.data() + 2 + 64));
        rw.buf.back() = std::byte{'\''};
    };

    if (argon2id_pass) {
        bool salt_from_file = false;
        if (!salt) {
            salt.emplace();
            if (exists(_db_path)) {
                // If we aren't using a plaintext header then the first 16 bytes of the file are the
                // salt.  If the file doesn't exist, we select a random one and specify that when
                // opening the file.
                std::ifstream f;
                f.exceptions(std::ifstream::failbit | std::ifstream::badbit);
                f.open(_db_path, std::ios::binary);
                f.read(reinterpret_cast<char*>(salt->data()), salt->size());

                if (std::string_view{reinterpret_cast<const char*>(salt->data()), salt->size()} ==
                    "SQLite Format 3\0"sv)
                    throw std::runtime_error{
                            "Database exists but uses a plaintext header (or is not encrypted); "
                            "plaintext header mode is required (using plaintext_header_salt)"};

                salt_from_file = true;
            } else {
                randombytes_buf(salt->data(), salt->size());
            }
        }

        std::array<std::byte, 32> key;
        argon2id_pass->compute(key, *salt);

        if (salt_from_file)
            salt.reset();  // We don't need to pass it: sqlite will read it itself

        store_raw_key(key);

        sodium_memzero(key.data(), key.size());

    } else if (raw_key) {

        store_raw_key(raw_key->key);

    } else if (plaintext_pass) {
        // TODO FIXME: we could do a lot better here by precomputing the PBKDF2-SHA512-HMAC (or
        // whatever) and storing that as a key so that we don't make the database layer do the
        // PBKDF2 on every new connection.
        //
        // But really you should just switch to argon2 instead because PBKDF2 doesn't offer much
        // practical protection.

        _key.update(std::span{
                reinterpret_cast<const std::byte*>(plaintext_pass->pass.data()),
                plaintext_pass->pass.size()});

        if (salt)
            // If we have a salt (via plaintext header) then we have to pass it via pragma after
            // setting the key but before accessing anything (because in plaintext mode sqlite
            // cannot store the salt itself).
            //
            // (For raw key mode, we don't have to do this as the salt is simply appended to the key
            // value we pass in).
            _pragma_salt.emplace(fmt::format(
                    "PRAGMA cipher_salt = \"x'{}'\"", oxenc::to_hex(salt->begin(), salt->end())));
    }

    // Get an initial connection so that we are testing that we can connect here in the constructor.
    // We immediately drop it, which returns that single connection to the idle conns pool to be
    // returned as soon as someone calls `conn()` to get it (without needing to reconnect again).

    // std::lock_guard lock{_conn_mutex}; // Not necessary - we're still in the constructor
    get_or_make_conn(std::thread::id{}, !create || create->create ? SQLite::OPEN_CREATE : 0);
}

Connection::Connection(Database& db, std::shared_ptr<detail::conn> conn) :
        db{db}, _conn{std::move(conn)}, sql{_conn->sql} {}

Connection::~Connection() {
    if (_conn) {
        _conn.reset();
        db.conn_returned();
    }
}

void Database::conn_returned() {
    // Called during Connection destruction after releasing the detail::conn shared_ptr to return
    // any now-unused connections from _conn_in_use into _conn_unused.
    std::lock_guard lock{_conn_mutex};
    for (auto it = _conn_in_use.begin(); it != _conn_in_use.end();) {
        auto& [th, conn] = *it;
        assert(conn);  // If we end up with a dead pointer in here then some code is broken
        if (conn.unique()) {
            if (_conn_max_idle < 0 || _conn_unused.size() < static_cast<size_t>(_conn_max_idle))
                _conn_unused.push_back(std::move(conn));
            // else we have enough idle connections so let it drop

            it = _conn_in_use.erase(it);
        } else
            ++it;
    }
}

void Database::set_max_idle_conns(int max_idle) {
    std::lock_guard lock{_conn_mutex};

    _conn_max_idle = max_idle;
    if (_conn_max_idle >= 0 && _conn_unused.size() > static_cast<size_t>(_conn_max_idle))
        _conn_unused.resize(_conn_max_idle);
}

Connection Database::get_or_make_conn(std::thread::id tid, int extra_open_flags) {

    if (!_conn_unused.empty()) {
        auto c = std::move(_conn_unused.back());
        _conn_unused.pop_back();
        _conn_in_use.emplace_back(tid, c);
        c->reset_thread();
        return Connection{*this, std::move(c)};
    }

    // Run out of idle connections so we need to make a new one (this also gets called during
    // construction for the very first connection)

    auto conn = std::make_shared<detail::conn>(
            _db_path, _open_flags | extra_open_flags, static_cast<int>(_busy_timeout.count()));

    auto& sql = conn->sql;

    auto connect_pragma = [&](const std::string& pragma, const char* failing_thing) {
        if (char* errmsg; SQLITE_OK != sqlite3_exec(
                                               sql.getHandle(),
                                               "PRAGMA cipher_compatibility = 3",
                                               nullptr,
                                               nullptr,
                                               &errmsg)) {
            auto err = fmt::format("Failed to {}: {}", failing_thing, errmsg);
            sqlite3_free(errmsg);
            throw std::runtime_error{std::move(err)};
        }
    };

    connect_pragma(
            "PRAGMA trusted_schema = OFF", "disable horrible default trusted_schema setting");

    if (_enc != Encryption::None) {
        auto ro = _key.access();
        sqlite3_key(sql.getHandle(), ro.buf.data(), ro.buf.size());
    }

#ifdef SESSION_SQLITE_SQLCIPHER
    // SQLCipher
    if (_enc == Encryption::SQLCipher3)
        connect_pragma("PRAGMA cipher_compatibility = 3", "enable sqlcipher3 compat mode");
    else if (_enc != Encryption::SQLCipher4 & _enc != Encryption::None)
        throw std::runtime_error{
                "The given encryption type is not supported when using the SQLCipher backend"};

#else
    // SQLite3 multiple ciphers
    const char* name = nullptr;
    int legacy = -1;
    int algorithm = -1;
    switch (_enc) {
        case Encryption::AEGIS256:
            name = "aegis";
            algorithm = 4;  // Magic sqlite3-mc constant for AEGIS-256
            break;
        case Encryption::Ascon128: name = "ascon128"; break;
        case Encryption::ChaCha20: name = "chacha20"; break;
        case Encryption::SQLCipher3:
            name = "sqlcipher";
            legacy = 3;
            break;
        case Encryption::SQLCipher4:
            name = "sqlcipher";
            legacy = 4;
            break;
        case Encryption::None: break;
    };

    if (name) {
        int cipher_idx = sqlite3mc_cipher_index(name);
        if (cipher_idx == -1)
            throw std::runtime_error{
                    fmt::format("Current SQLite-MC build does not support cipher '{}'", name)};
        if (-1 == sqlite3mc_config(sql.getHandle(), "cipher", cipher_idx))
            throw std::runtime_error{"Cipher selection failed"};
        if (legacy >= 0)
            if (-1 == sqlite3mc_config_cipher(sql.getHandle(), name, "legacy", legacy))
                throw std::runtime_error{"SQLCipher version mode selection failed"};
        if (algorithm >= 0)
            if (-1 == sqlite3mc_config_cipher(sql.getHandle(), name, "algorithm", algorithm))
                throw std::runtime_error{"Failed to set cipher algorithm"};
    }
#endif

    if (_plaintext_header)
        connect_pragma(
                fmt::format(
                        "PRAGMA plaintext_header_size = {}",
                        (_enc == Encryption::SQLCipher3 || _enc == Encryption::SQLCipher4) ? 32
                                                                                           : 24),
                "enable plaintext header mode");

    if (_pragma_salt)
        connect_pragma(*_pragma_salt, "set cipher salt");

    // Now make sure we can query something: this is our failure point (via exception) if the
    // authentication key is incorrect as this will be the first place that an actual read happens.
    sql.exec("SELECT COUNT(*) FROM sqlite_master");

    if (_wal)
        // Don't fail on these because we can still work (albeit slower) even if they fail for some
        // reason
        if (sql.tryExec("PRAGMA journal_mode = WAL") == SQLite::OK)
            sql.tryExec("PRAGMA synchronous = NORMAL");

    // Foreign key enforcement being off is another absolutely miserable sqlite3 default, but
    // apparently we not only have to set it but also verify that it has been set because some
    // versions and or compilation options can just silently *not* set it because it's better to
    // have horrible defaults and silently ignore fixing them rather than risk breaking some
    // sqlite-using PHP script from the late 90s.
    try {
        sql.exec("PRAGMA foreign_keys = ON");
        int fk_enabled = sql.execAndGet("PRAGMA foreign_keys").getInt();
        if (fk_enabled != 1)
            throw std::runtime_error{
                    "Foreign key enabling query succeeded, but foreign keys were not actually "
                    "enabled!"};
    } catch (const std::exception& e) {
        throw std::runtime_error{
                fmt::format("Failed to enable foreign key integrity: {}", e.what())};
    }

    _conn_in_use.emplace_back(tid, conn);

    return Connection{*this, std::move(conn)};
}

Connection Database::unique_conn() {
    std::lock_guard lock{_conn_mutex};
    return get_or_make_conn(std::thread::id{});
}

Connection Database::conn() {
    std::lock_guard lock{_conn_mutex};

    auto t = std::this_thread::get_id();
    for (auto& [th, conn] : _conn_in_use)
        if (th == t)
            return Connection{*this, conn};

    return get_or_make_conn(std::this_thread::get_id());
}

Database::~Database() {
    assert(_conn_in_use.empty());
}

StatementWrapper Connection::prepared_st(const std::string& query) {
    return _conn->prepared_st(query);
}

StatementWrapper::~StatementWrapper() {
    if (st) {
        st->tryReset();
        conn.statement_finished(std::move(st));
    }
}

void argon2id_password::compute(std::span<std::byte, 32> out, std::span<const std::byte, 16> salt) {
    sodium_init();

    if (0 != crypto_pwhash(
                     reinterpret_cast<unsigned char*>(out.data()),
                     out.size(),

                     plaintext_pass.data(),
                     plaintext_pass.size(),
                     reinterpret_cast<const unsigned char*>(salt.data()),
                     opslimit,
                     memlimit,
                     crypto_pwhash_argon2id_ALG_ARGON2ID13))
        throw std::runtime_error{"Failed to compute argon2 hash (most likely insufficient memory)"};
}

std::array<std::byte, 16> argon2id_password::compute(std::span<std::byte, 32> out) {
    sodium_init();
    std::array<std::byte, 16> salt;
    randombytes_buf(salt.data(), salt.size());
    compute(out, salt);
    return salt;
}

}  // namespace session::sqlite

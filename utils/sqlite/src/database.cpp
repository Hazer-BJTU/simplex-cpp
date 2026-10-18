#include "sqlite/database.hpp"

#include <sqlite3.h>

#include <cctype>
#include <limits>
#include <utility>

namespace sqlite {

namespace detail {

struct Connection {
    sqlite3* handle = nullptr;
    bool usable = true;
    bool transaction_owned = false;

    ~Connection() {
        if (handle != nullptr) {
            // Every statement retains Connection and finalizes before releasing
            // it. close_v2 also safely defers native cleanup if SQLite needs it.
            sqlite3_close_v2(handle);
        }
    }

    void check() const {
        if (!usable) {
            throw std::logic_error("SQLite connection is unusable after failed rollback");
        }
    }

    void execute(const char* sql, std::string_view operation) {
        check();
        const int result = sqlite3_exec(handle, sql, nullptr, nullptr, nullptr);
        if (result != SQLITE_OK) {
            throw Error(operation, sqlite3_extended_errcode(handle));
        }
    }
};

struct PreparedStatement {
    std::shared_ptr<Connection> connection;
    sqlite3_stmt* handle = nullptr;
    bool started = false;
    bool row = false;
    bool done = false;
    bool failed = false;

    ~PreparedStatement() {
        if (handle != nullptr) {
            sqlite3_finalize(handle);
        }
    }

    void check_binding(int parameter) const {
        connection->check();
        if (started) {
            throw std::logic_error("Cannot bind a statement after stepping it");
        }
        if (parameter < 1 || parameter > sqlite3_bind_parameter_count(handle)) {
            throw std::out_of_range("SQLite parameter index out of range");
        }
    }

    void check_column(int column) const {
        connection->check();
        if (!row) {
            throw std::logic_error("SQLite statement has no current row");
        }
        if (column < 0 || column >= sqlite3_column_count(handle)) {
            throw std::out_of_range("SQLite column index out of range");
        }
    }
};

} // namespace detail

namespace {

void check_string(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQLite path/SQL must be nonempty and contain no NUL");
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("SQLite path/SQL exceeds the supported length");
    }
}

void check_result(int result, std::string_view operation) {
    if (result != SQLITE_OK) {
        throw Error(operation, result);
    }
}

} // namespace

Error::Error(std::string_view operation, int extended_code)
    : std::runtime_error(
          "SQLite " + std::string(operation) + " failed: "
          + sqlite3_errstr(extended_code) + " (" + std::to_string(extended_code) + ")"),
      _extended_code(extended_code) {}

Error::~Error() = default;

int Error::code() const noexcept {
    return _extended_code & 0xff;
}

int Error::extended_code() const noexcept {
    return _extended_code;
}

Database::Database(std::string_view path, OpenMode mode)
    : _connection(std::make_shared<detail::Connection>()) {
    check_string(path);
    const std::string filename(path);
    const int flags =
        (mode == OpenMode::ReadOnly
             ? SQLITE_OPEN_READONLY
             : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
        | SQLITE_OPEN_FULLMUTEX;
    const int result = sqlite3_open_v2(
        filename.c_str(), &_connection->handle, flags, nullptr);
    if (result != SQLITE_OK) {
        const int code = _connection->handle != nullptr
            ? sqlite3_extended_errcode(_connection->handle) : result;
        throw Error("open", code);
    }
    check_result(sqlite3_extended_result_codes(_connection->handle, 1), "configure errors");
}

Database::~Database() = default;

void Database::execute(std::string_view sql) {
    check_string(sql);
    const std::string text(sql);
    _connection->execute(text.c_str(), "execute");
}

Statement Database::prepare(std::string_view sql) {
    return Statement(_connection, sql);
}

std::int64_t Database::last_insert_rowid() const {
    _connection->check();
    return sqlite3_last_insert_rowid(_connection->handle);
}

Statement::Statement(std::shared_ptr<detail::Connection> connection, std::string_view sql)
    : _statement(std::make_unique<detail::PreparedStatement>()) {
    check_string(sql);
    _statement->connection = std::move(connection);
    _statement->connection->check();
    const char* tail = nullptr;
    const int result = sqlite3_prepare_v2(
        _statement->connection->handle, sql.data(), static_cast<int>(sql.size()),
        &_statement->handle, &tail);
    if (result != SQLITE_OK) {
        throw Error("prepare", sqlite3_extended_errcode(_statement->connection->handle));
    }
    if (_statement->handle == nullptr) {
        throw std::invalid_argument("SQLite SQL contains no statement");
    }
    for (const char* remaining = tail; remaining != sql.data() + sql.size(); ++remaining) {
        if (!std::isspace(static_cast<unsigned char>(*remaining))) {
            throw std::invalid_argument("SQLite prepare accepts one statement with trailing whitespace only");
        }
    }
}

Statement::~Statement() = default;

void Statement::bind_null(int parameter) {
    _statement->check_binding(parameter);
    check_result(sqlite3_bind_null(_statement->handle, parameter), "bind NULL");
}

void Statement::bind_int64(int parameter, std::int64_t value) {
    _statement->check_binding(parameter);
    check_result(sqlite3_bind_int64(_statement->handle, parameter, value), "bind integer");
}

void Statement::bind_text(int parameter, std::string_view value) {
    _statement->check_binding(parameter);
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("SQLite text exceeds the supported length");
    }
    check_result(sqlite3_bind_text(
        _statement->handle, parameter, value.empty() ? "" : value.data(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT), "bind text");
}

StepResult Statement::step() {
    auto& statement = *_statement;
    statement.connection->check();
    if (statement.failed) {
        throw std::logic_error("Cannot step a failed SQLite statement");
    }
    if (statement.done) {
        return StepResult::Done;
    }
    statement.started = true;
    statement.row = false;
    const int result = sqlite3_step(statement.handle);
    if (result == SQLITE_ROW) {
        statement.row = true;
        return StepResult::Row;
    }
    if (result == SQLITE_DONE) {
        statement.done = true;
        return StepResult::Done;
    }
    statement.failed = true;
    throw Error("step", sqlite3_extended_errcode(statement.connection->handle));
}

bool Statement::column_is_null(int column) const {
    _statement->check_column(column);
    return sqlite3_column_type(_statement->handle, column) == SQLITE_NULL;
}

std::int64_t Statement::column_int64(int column) const {
    _statement->check_column(column);
    return sqlite3_column_int64(_statement->handle, column);
}

std::string Statement::column_text(int column) const {
    _statement->check_column(column);
    const auto* text = sqlite3_column_text(_statement->handle, column);
    if (text == nullptr) {
        // NULL and an allocation failure both return a null pointer. Preserve
        // the error instead of accidentally turning OOM into empty text.
        const int code = sqlite3_errcode(_statement->connection->handle);
        if (code == SQLITE_NOMEM) {
            throw Error("read text", code);
        }
        return {};
    }
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(sqlite3_column_bytes(_statement->handle, column)));
}

Transaction::Transaction(Database& database)
    : _connection(database._connection) {
    _connection->check();
    if (_connection->transaction_owned || !sqlite3_get_autocommit(_connection->handle)) {
        throw std::logic_error("Nested SQLite transactions are not supported");
    }
    _connection->execute("BEGIN", "begin transaction");
    _connection->transaction_owned = true;
    _active = true;
}

Transaction::~Transaction() noexcept {
    if (!_active) {
        return;
    }
    if (!sqlite3_get_autocommit(_connection->handle)) {
        sqlite3_exec(_connection->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        if (!sqlite3_get_autocommit(_connection->handle)) {
            _connection->usable = false;
        }
    }
    _connection->transaction_owned = false;
}

void Transaction::commit() {
    if (!_active) {
        throw std::logic_error("SQLite transaction has already ended");
    }
    // SQLite can automatically roll back after a statement error. COMMIT then
    // fails explicitly, rather than reporting success for a lost transaction.
    _connection->execute("COMMIT", "commit transaction");
    _active = false;
    _connection->transaction_owned = false;
}

void Transaction::rollback() {
    if (!_active) {
        throw std::logic_error("SQLite transaction has already ended");
    }
    _connection->check();
    if (!sqlite3_get_autocommit(_connection->handle)) {
        _connection->execute("ROLLBACK", "rollback transaction");
    }
    _active = false;
    _connection->transaction_owned = false;
}

} // namespace sqlite

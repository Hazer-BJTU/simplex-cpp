#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sqlite {

namespace detail {
struct Connection;
struct PreparedStatement;
}

/** SQLite failure with a primary and extended result code.
 * Messages identify the operation and generic SQLite error, without including
 * SQL, bound values or database paths. Misuse of the wrapper throws standard
 * invalid_argument, out_of_range or logic_error exceptions instead.
 */
class Error : public std::runtime_error {
public:
    Error(std::string_view operation, int extended_code);
    ~Error() override;

    int code() const noexcept;
    int extended_code() const noexcept;

private:
    int _extended_code;
};

class Statement;
class Transaction;

/** Owns an SQLite connection opened read/write, creating the file if needed.
 * Use ":memory:" for a private in-memory database. Paths are UTF-8, may not
 * contain NUL, and are interpreted as filenames, not SQLite URI parameters.
 * Parent directories must already exist. Empty temporary-database paths are
 * deliberately excluded from this initial interface.
 *
 * This is synchronous I/O. Serialize all operations on a connection, including
 * its statements and transactions; SQLite's mutexes do not serialize entire
 * application transactions. Do not run blocking queries on an I/O event loop.
 * Objects are neither copyable nor movable. Statements and transaction guards
 * retain the native connection if the Database facade is destroyed first.
 */
class Database {
public:
    explicit Database(std::string_view path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    /** Execute SQL with no returned rows; multiple statements are allowed.
     * Use prepare() and binding for external values. Partial effects are
     * possible if a batch fails outside a transaction. Do not execute manual
     * transaction/savepoint control while a Transaction guard owns the scope.
     */
    void execute(std::string_view sql);

    /** Prepare exactly one statement, with optional trailing whitespace.
     * Empty SQL, embedded NUL and additional SQL (including trailing comments)
     * are rejected. Guaranteed copy elision permits returning this nonmovable
     * object: `auto query = database.prepare("SELECT ...");`.
     */
    Statement prepare(std::string_view sql);

private:
    friend class Transaction;
    std::shared_ptr<detail::Connection> _connection;
};

enum class StepResult { Row, Done };

/** Owns one prepared statement and retains its connection.
 * Bind parameters before the first step (one-based indexes). This small API
 * executes a statement once; prepare a new statement to run it again. Values
 * returned by column_text() are owning copies, valid after the next step.
 */
class Statement {
public:
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) = delete;
    Statement& operator=(Statement&&) = delete;

    /** Bind an SQL NULL, signed integer, or copied UTF-8 text value.
     * Text may contain embedded NUL and may be empty; neither means SQL NULL.
     */
    void bind_null(int parameter);
    void bind_int64(int parameter, std::int64_t value);
    void bind_text(int parameter, std::string_view value);

    /** Advance to a row or completion. After Done, further calls return Done
     * without executing SQL again. After an SQLite error, discard this object.
     */
    StepResult step();

    /** Read a zero-based column of the current row. NULL must be distinguished
     * using column_is_null(); SQLite converts other values to the requested
     * type (NULL converts to zero or an empty string). Access outside a row or
     * outside the result column range throws before calling SQLite.
     */
    bool column_is_null(int column) const;
    std::int64_t column_int64(int column) const;
    std::string column_text(int column) const;

private:
    friend class Database;
    Statement(std::shared_ptr<detail::Connection> connection, std::string_view sql);
    std::unique_ptr<detail::PreparedStatement> _statement;
};

/** A single deferred transaction; explicit commit or rollback ends its scope.
 * Nesting (including a pre-existing manually started transaction) is rejected.
 * Keep all work on the connection serialized for the entire scope. Do not
 * issue separate BEGIN/COMMIT/ROLLBACK/SAVEPOINT statements inside this guard.
 *
 * Destruction attempts rollback without throwing. If that rollback fails and
 * SQLite still has an active transaction, the connection is marked unusable:
 * subsequent wrapper calls fail instead of reusing uncertain transaction state.
 * Explicit commit/rollback report failures; failed commit leaves this guard
 * responsible for rollback. SQLite may itself roll back on certain errors.
 */
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction() noexcept;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    void commit();
    void rollback();

private:
    std::shared_ptr<detail::Connection> _connection;
    bool _active = false;
};

} // namespace sqlite

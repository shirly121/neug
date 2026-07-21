#include "sqlite_fts_sqlite.h"

#include <sqlite3.h>

#include <utility>

#include "neug/utils/exception/exception.h"

namespace neug::sqlite_fts_ext {
namespace {

std::string SQLiteError(sqlite3* database, const std::string& context,
                        int code) {
  const char* message =
      database ? sqlite3_errmsg(database) : sqlite3_errstr(code);
  return context + ": " + (message ? message : "unknown SQLite error") +
         " (code " + std::to_string(code) + ")";
}

}  // namespace

SQLiteStatement::~SQLiteStatement() {
  if (statement_) {
    sqlite3_finalize(statement_);
  }
}

SQLiteStatement::SQLiteStatement(SQLiteStatement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr)) {}

SQLiteStatement& SQLiteStatement::operator=(SQLiteStatement&& other) noexcept {
  if (this != &other) {
    if (statement_) {
      sqlite3_finalize(statement_);
    }
    statement_ = std::exchange(other.statement_, nullptr);
  }
  return *this;
}

void SQLiteStatement::BindText(int parameter, const std::string& value) {
  auto code =
      sqlite3_bind_text(statement_, parameter, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT);
  if (code != SQLITE_OK) {
    THROW_RUNTIME_ERROR(SQLiteError(sqlite3_db_handle(statement_),
                                    "SQLite bind text failed", code));
  }
}

void SQLiteStatement::BindInt64(int parameter, int64_t value) {
  auto code = sqlite3_bind_int64(statement_, parameter, value);
  if (code != SQLITE_OK) {
    THROW_RUNTIME_ERROR(SQLiteError(sqlite3_db_handle(statement_),
                                    "SQLite bind integer failed", code));
  }
}

int SQLiteStatement::Step() {
  auto code = sqlite3_step(statement_);
  if (code != SQLITE_ROW && code != SQLITE_DONE) {
    THROW_RUNTIME_ERROR(SQLiteError(sqlite3_db_handle(statement_),
                                    "SQLite statement execution failed", code));
  }
  return code;
}

int64_t SQLiteStatement::ColumnInt64(int column) const {
  return sqlite3_column_int64(statement_, column);
}

double SQLiteStatement::ColumnDouble(int column) const {
  return sqlite3_column_double(statement_, column);
}

SQLiteDatabase::~SQLiteDatabase() { Close(); }

void SQLiteDatabase::Open(const std::string& path) {
  Close();
  auto code =
      sqlite3_open_v2(path.c_str(), &database_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (code != SQLITE_OK) {
    auto error = SQLiteError(database_, "SQLite database open failed", code);
    Close();
    THROW_RUNTIME_ERROR(error);
  }
  sqlite3_extended_result_codes(database_, 1);
  Execute("PRAGMA journal_mode=DELETE;");
  Execute("PRAGMA synchronous=FULL;");
}

void SQLiteDatabase::Close() {
  if (!database_) {
    return;
  }
  auto* database = std::exchange(database_, nullptr);
  auto code = sqlite3_close(database);
  if (code != SQLITE_OK) {
    sqlite3_close_v2(database);
  }
}

void SQLiteDatabase::Execute(const std::string& sql) {
  char* raw_error = nullptr;
  auto code =
      sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &raw_error);
  if (code != SQLITE_OK) {
    std::string error = raw_error ? raw_error : sqlite3_errmsg(database_);
    sqlite3_free(raw_error);
    THROW_RUNTIME_ERROR("SQLite execute failed: " + error + " (code " +
                        std::to_string(code) + ")");
  }
}

SQLiteStatement SQLiteDatabase::Prepare(const std::string& sql) {
  sqlite3_stmt* statement = nullptr;
  auto code =
      sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr);
  if (code != SQLITE_OK) {
    THROW_RUNTIME_ERROR(
        SQLiteError(database_, "SQLite statement prepare failed", code));
  }
  return SQLiteStatement(statement);
}

void SQLiteDatabase::Flush() {
  auto code = sqlite3_db_cacheflush(database_);
  if (code != SQLITE_OK) {
    THROW_RUNTIME_ERROR(
        SQLiteError(database_, "SQLite database flush failed", code));
  }
}

bool SQLiteDatabase::HasFTS5() {
  if (sqlite3_compileoption_used("ENABLE_FTS5") != 0) {
    return true;
  }
  sqlite3* database = nullptr;
  if (sqlite3_open(":memory:", &database) != SQLITE_OK) {
    if (database) {
      sqlite3_close(database);
    }
    return false;
  }
  char* error = nullptr;
  auto code = sqlite3_exec(database,
                           "CREATE VIRTUAL TABLE fts5_probe USING fts5(text);",
                           nullptr, nullptr, &error);
  sqlite3_free(error);
  sqlite3_close(database);
  return code == SQLITE_OK;
}

std::string SQLiteDatabase::Version() { return sqlite3_libversion(); }

}  // namespace neug::sqlite_fts_ext

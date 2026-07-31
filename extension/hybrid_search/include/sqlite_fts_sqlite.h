#pragma once

#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace neug::sqlite_fts_ext {

class SQLiteStatement {
 public:
  SQLiteStatement() = default;
  explicit SQLiteStatement(sqlite3_stmt* statement) : statement_(statement) {}
  ~SQLiteStatement();

  SQLiteStatement(const SQLiteStatement&) = delete;
  SQLiteStatement& operator=(const SQLiteStatement&) = delete;
  SQLiteStatement(SQLiteStatement&& other) noexcept;
  SQLiteStatement& operator=(SQLiteStatement&& other) noexcept;

  void BindText(int parameter, const std::string& value);
  void BindInt64(int parameter, int64_t value);
  void Reset();
  int Step();
  int64_t ColumnInt64(int column) const;
  double ColumnDouble(int column) const;
  sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3_stmt* statement_{nullptr};
};

class SQLiteDatabase {
 public:
  SQLiteDatabase() = default;
  ~SQLiteDatabase();

  SQLiteDatabase(const SQLiteDatabase&) = delete;
  SQLiteDatabase& operator=(const SQLiteDatabase&) = delete;

  void Open(const std::string& path);
  void Close();
  void Execute(const std::string& sql);
  SQLiteStatement Prepare(const std::string& sql);
  void Flush();

  bool IsOpen() const { return database_ != nullptr; }
  static bool HasFTS5();
  static std::string Version();

 private:
  sqlite3* database_{nullptr};
};

}  // namespace neug::sqlite_fts_ext

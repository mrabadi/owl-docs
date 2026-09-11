#include "docxstudio/app/ChatStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace docxstudio::app {
namespace {

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            statement_ = nullptr;
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_{};
};

void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void bindBlob(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_blob(statement, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

RecoveryRecord readRecovery(sqlite3_stmt* statement) {
    const auto textColumn = [statement](int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
    };
    const auto* payload = static_cast<const char*>(sqlite3_column_blob(statement, 3));
    const int payloadSize = sqlite3_column_bytes(statement, 3);
    return {textColumn(0), textColumn(1), textColumn(2),
            payload && payloadSize > 0
                ? std::string(payload, static_cast<std::size_t>(payloadSize))
                : std::string{},
            sqlite3_column_int64(statement, 4)};
}

bool preparePrivateDatabaseFile(const std::string& path, std::string& error) {
    const int descriptor = ::open(
        path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        error = "could not create private local state: " +
                std::string(std::strerror(errno));
        return false;
    }
    const bool secured = ::fchmod(descriptor, 0600) == 0;
    const int savedError = errno;
    ::close(descriptor);
    if (!secured) {
        error = "could not secure private local state: " +
                std::string(std::strerror(savedError));
        return false;
    }
    return true;
}

bool secureSQLiteSidecar(const std::string& path, std::string& error) {
    if (::chmod(path.c_str(), 0600) == 0 || errno == ENOENT) return true;
    error = "could not secure SQLite state file '" + path + "': " +
            std::string(std::strerror(errno));
    return false;
}

}  // namespace

ChatStore::~ChatStore() { close(); }

bool ChatStore::open(const std::string& path, std::string& error) {
    close();
    if (!preparePrivateDatabaseFile(path, error)) return false;
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        error = database_ ? sqlite3_errmsg(database_) : "could not open SQLite database";
        close();
        return false;
    }
    sqlite3_busy_timeout(database_, 2000);
    if (!exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", error)) {
        close();
        return false;
    }
    if (!exec(
        "CREATE TABLE IF NOT EXISTS app_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO app_meta(key,value) VALUES('schema_version','2');"
        "UPDATE app_meta SET value='2' WHERE key='schema_version';"
        "CREATE TABLE IF NOT EXISTS document_threads ("
        " document_key TEXT PRIMARY KEY, thread_id TEXT NOT NULL, updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS chat_messages ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, document_key TEXT NOT NULL,"
        " role TEXT NOT NULL CHECK(role IN ('user','assistant','system')),"
        " body TEXT NOT NULL, created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS chat_messages_document_idx "
        "ON chat_messages(document_key,id);"
        "CREATE TABLE IF NOT EXISTS recovery_journals ("
        " journal_key TEXT PRIMARY KEY, source_path TEXT NOT NULL,"
        " display_name TEXT NOT NULL, payload BLOB NOT NULL,"
        " updated_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS recovery_journals_updated_idx "
        "ON recovery_journals(updated_at);",
        error)) {
        close();
        return false;
    }
    if (!secureSQLiteSidecar(path, error) ||
        !secureSQLiteSidecar(path + "-wal", error) ||
        !secureSQLiteSidecar(path + "-shm", error)) {
        close();
        return false;
    }
    return true;
}

void ChatStore::close() noexcept {
    if (database_) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
}

bool ChatStore::exec(const char* sql, std::string& error) {
    char* rawError = nullptr;
    if (sqlite3_exec(database_, sql, nullptr, nullptr, &rawError) != SQLITE_OK) {
        error = rawError ? rawError : "SQLite operation failed";
        sqlite3_free(rawError);
        return false;
    }
    return true;
}

std::optional<std::string> ChatStore::threadForDocument(const std::string& documentKey) const {
    if (!database_) {
        return std::nullopt;
    }
    Statement statement(database_,
                        "SELECT thread_id FROM document_threads WHERE document_key=?1;");
    if (!statement.get()) {
        return std::nullopt;
    }
    bindText(statement.get(), 1, documentKey);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    const auto* value = sqlite3_column_text(statement.get(), 0);
    return value ? std::optional<std::string>(reinterpret_cast<const char*>(value)) : std::nullopt;
}

bool ChatStore::bindThread(const std::string& documentKey,
                           const std::string& threadId,
                           std::string& error) {
    if (!database_) {
        error = "chat store is not open";
        return false;
    }
    Statement statement(database_,
                        "INSERT INTO document_threads(document_key,thread_id,updated_at) "
                        "VALUES(?1,?2,CAST(strftime('%s','now') AS INTEGER)) "
                        "ON CONFLICT(document_key) DO UPDATE SET "
                        "thread_id=excluded.thread_id,updated_at=excluded.updated_at;");
    if (!statement.get()) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    bindText(statement.get(), 1, documentKey);
    bindText(statement.get(), 2, threadId);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool ChatStore::appendMessage(const std::string& documentKey,
                              const std::string& role,
                              const std::string& text,
                              std::string& error) {
    if (!database_) {
        error = "chat store is not open";
        return false;
    }
    Statement statement(database_,
                        "INSERT INTO chat_messages(document_key,role,body,created_at) "
                        "VALUES(?1,?2,?3,CAST(strftime('%s','now') AS INTEGER));");
    if (!statement.get()) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    bindText(statement.get(), 1, documentKey);
    bindText(statement.get(), 2, role);
    bindText(statement.get(), 3, text);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

std::vector<StoredChatMessage> ChatStore::messages(const std::string& documentKey,
                                                   std::size_t limit) const {
    std::vector<StoredChatMessage> result;
    if (!database_ || limit == 0) {
        return result;
    }
    Statement statement(database_,
                        "SELECT id,role,body,created_at FROM chat_messages "
                        "WHERE document_key=?1 ORDER BY id DESC LIMIT ?2;");
    if (!statement.get()) {
        return result;
    }
    bindText(statement.get(), 1, documentKey);
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const auto* role = sqlite3_column_text(statement.get(), 1);
        const auto* body = sqlite3_column_text(statement.get(), 2);
        result.push_back({sqlite3_column_int64(statement.get(), 0),
                          role ? reinterpret_cast<const char*>(role) : "system",
                          body ? reinterpret_cast<const char*>(body) : "",
                          sqlite3_column_int64(statement.get(), 3)});
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::optional<RecoveryRecord> ChatStore::recoveryRecord(
    const std::string& journalKey) const {
    if (!database_) return std::nullopt;
    Statement statement(
        database_,
        "SELECT journal_key,source_path,display_name,payload,updated_at "
        "FROM recovery_journals WHERE journal_key=?1;");
    if (!statement.get()) return std::nullopt;
    bindText(statement.get(), 1, journalKey);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return readRecovery(statement.get());
}

std::vector<RecoveryRecord> ChatStore::recoveryRecords() const {
    std::vector<RecoveryRecord> result;
    if (!database_) return result;
    Statement statement(
        database_,
        "SELECT journal_key,source_path,display_name,payload,updated_at "
        "FROM recovery_journals ORDER BY updated_at,journal_key;");
    if (!statement.get()) return result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
        result.push_back(readRecovery(statement.get()));
    return result;
}

bool ChatStore::upsertRecovery(const RecoveryRecord& record, std::string& error) {
    if (!database_) {
        error = "local state store is not open";
        return false;
    }
    if (record.journalKey.empty() || record.payload.empty()) {
        error = "recovery journal key and payload must not be empty";
        return false;
    }
    Statement statement(
        database_,
        "INSERT INTO recovery_journals("
        "journal_key,source_path,display_name,payload,updated_at) "
        "VALUES(?1,?2,?3,?4,CAST(strftime('%s','now') AS INTEGER)) "
        "ON CONFLICT(journal_key) DO UPDATE SET "
        "source_path=excluded.source_path,display_name=excluded.display_name,"
        "payload=excluded.payload,updated_at=excluded.updated_at;");
    if (!statement.get()) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    bindText(statement.get(), 1, record.journalKey);
    bindText(statement.get(), 2, record.sourcePath);
    bindText(statement.get(), 3, record.displayName);
    bindBlob(statement.get(), 4, record.payload);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool ChatStore::deleteRecovery(const std::string& journalKey, std::string& error) {
    if (!database_) {
        error = "local state store is not open";
        return false;
    }
    Statement statement(database_,
                        "DELETE FROM recovery_journals WHERE journal_key=?1;");
    if (!statement.get()) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    bindText(statement.get(), 1, journalKey);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

}  // namespace docxstudio::app

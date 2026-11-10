#include "docxstudio/app/ChatStore.h"

#include "docxstudio/codex/editor_tools.hpp"

#include <sqlite3.h>
#include <QCryptographicHash>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string_view>
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

constexpr std::size_t kMaximumSourcePackageBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaximumSourceContextBytes = 64U * 1024U * 1024U;

std::string sourceHash(const std::string& bytes) {
    return QCryptographicHash::hash(
        QByteArray::fromRawData(bytes.data(), static_cast<int>(bytes.size())),
        QCryptographicHash::Sha256).toHex().toStdString();
}

RecoveryRecord readRecovery(sqlite3_stmt* statement) {
    const auto textColumn = [statement](int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
    };
    const auto* payload = static_cast<const char*>(sqlite3_column_blob(statement, 3));
    const int payloadSize = sqlite3_column_bytes(statement, 3);
    RecoveryRecord record{textColumn(0), textColumn(1), textColumn(2),
            payload && payloadSize > 0
                ? std::string(payload, static_cast<std::size_t>(payloadSize))
                : std::string{},
            sqlite3_column_int64(statement, 4)};
    if (sqlite3_column_type(statement, 5) != SQLITE_NULL) {
        const auto blobColumn = [statement](int column, std::size_t maximum) {
            const int size = sqlite3_column_bytes(statement, column);
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, column));
            return bytes && size > 0 && static_cast<std::size_t>(size) <= maximum
                ? std::string(bytes, static_cast<std::size_t>(size)) : std::string{};
        };
        record.source = RecoverySource{
            blobColumn(6, kMaximumSourcePackageBytes),
            blobColumn(7, kMaximumSourceContextBytes + 65U)};
        const auto contextEnvelope = std::move(record.source->context);
        if (contextEnvelope.size() > 65U && contextEnvelope[64] == '\n') {
            record.source->context = contextEnvelope.substr(65U);
        }
        if (record.source->packageBytes.empty() || record.source->context.empty() ||
            sourceHash(record.source->packageBytes) != textColumn(5) ||
            contextEnvelope.size() <= 65U || contextEnvelope[64] != '\n' ||
            sourceHash(record.source->context) != contextEnvelope.substr(0, 64U)) {
            record.source->error = "Retained source package or context is missing, corrupt, or exceeds its limit";
        }
    }
    return record;
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

bool executeSql(sqlite3* database, const char* sql, std::string& error) {
    char* rawError = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &rawError) == SQLITE_OK) {
        return true;
    }
    error = rawError ? rawError : "SQLite operation failed";
    sqlite3_free(rawError);
    return false;
}

bool ensureCurrentEditorToolCatalog(sqlite3* database, std::string& error) {
    {
        Statement current(
            database,
            "SELECT value FROM app_meta WHERE key='editor_tool_catalog_version';");
        if (!current.get()) {
            error = sqlite3_errmsg(database);
            return false;
        }
        const int currentStep = sqlite3_step(current.get());
        if (currentStep == SQLITE_ROW) {
            const auto* encoded = sqlite3_column_text(current.get(), 0);
            if (encoded &&
                std::string_view(reinterpret_cast<const char*>(encoded)) ==
                    codex::kEditorToolCatalogVersion) {
                return true;
            }
        } else if (currentStep != SQLITE_DONE) {
            error = sqlite3_errmsg(database);
            return false;
        }
    }

    // The app-server protocol accepts dynamicTools only on thread/start, not
    // thread/resume. Drop only the server-side thread bindings when the local
    // catalog changes; the user's local chat history remains intact.
    if (!executeSql(database, "BEGIN IMMEDIATE;", error)) return false;
    const auto rollback = [database] {
        char* ignored = nullptr;
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, &ignored);
        sqlite3_free(ignored);
    };
    if (!executeSql(database, "DELETE FROM document_threads;", error)) {
        rollback();
        return false;
    }
    {
        Statement update(
            database,
            "INSERT INTO app_meta(key,value) VALUES('editor_tool_catalog_version',?1) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
        if (!update.get()) {
            error = sqlite3_errmsg(database);
            rollback();
            return false;
        }
        bindText(update.get(), 1, std::string(codex::kEditorToolCatalogVersion));
        if (sqlite3_step(update.get()) != SQLITE_DONE) {
            error = sqlite3_errmsg(database);
            rollback();
            return false;
        }
    }
    if (!executeSql(database, "COMMIT;", error)) {
        rollback();
        return false;
    }
    return true;
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
        "INSERT OR IGNORE INTO app_meta(key,value) VALUES('schema_version','3');"
        "UPDATE app_meta SET value='3' WHERE key='schema_version';"
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
        "ON recovery_journals(updated_at);"
        "CREATE TABLE IF NOT EXISTS recovery_packages ("
        " hash TEXT PRIMARY KEY, bytes BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS recovery_sources ("
        " journal_key TEXT PRIMARY KEY, package_hash TEXT NOT NULL, context BLOB NOT NULL);",
        error)) {
        close();
        return false;
    }
    if (!ensureCurrentEditorToolCatalog(database_, error)) {
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
        "SELECT j.journal_key,j.source_path,j.display_name,j.payload,j.updated_at,"
        "s.package_hash,p.bytes,s.context FROM recovery_journals j "
        "LEFT JOIN recovery_sources s ON s.journal_key=j.journal_key "
        "LEFT JOIN recovery_packages p ON p.hash=s.package_hash WHERE j.journal_key=?1;");
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
        "SELECT j.journal_key,j.source_path,j.display_name,j.payload,j.updated_at,"
        "s.package_hash,p.bytes,s.context FROM recovery_journals j "
        "LEFT JOIN recovery_sources s ON s.journal_key=j.journal_key "
        "LEFT JOIN recovery_packages p ON p.hash=s.package_hash ORDER BY j.updated_at,j.journal_key;");
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
    if (record.source && (record.source->packageBytes.empty() ||
        record.source->packageBytes.size() > kMaximumSourcePackageBytes ||
        record.source->context.empty() || record.source->context.size() > kMaximumSourceContextBytes ||
        !record.source->error.empty())) {
        error = "Recovery source package or context is invalid or exceeds its limit";
        return false;
    }
    if (!exec("BEGIN IMMEDIATE;", error)) return false;
    const auto fail = [&]() {
        std::string ignored;
        exec("ROLLBACK;", ignored);
        return false;
    };
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
        return fail();
    }
    bindText(statement.get(), 1, record.journalKey);
    bindText(statement.get(), 2, record.sourcePath);
    bindText(statement.get(), 3, record.displayName);
    bindBlob(statement.get(), 4, record.payload);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return fail();
    }
    if (record.source) {
        const auto hash = sourceHash(record.source->packageBytes);
        Statement package(database_, "INSERT INTO recovery_packages(hash,bytes) VALUES(?1,?2) "
            "ON CONFLICT(hash) DO UPDATE SET bytes=excluded.bytes;");
        Statement source(database_, "INSERT INTO recovery_sources(journal_key,package_hash,context) VALUES(?1,?2,?3) "
            "ON CONFLICT(journal_key) DO UPDATE SET package_hash=excluded.package_hash,context=excluded.context;");
        if (!package.get() || !source.get()) { error = sqlite3_errmsg(database_); return fail(); }
        bindText(package.get(), 1, hash);
        bindBlob(package.get(), 2, record.source->packageBytes);
        bindText(source.get(), 1, record.journalKey);
        bindText(source.get(), 2, hash);
        bindBlob(source.get(), 3, sourceHash(record.source->context) + "\n" + record.source->context);
        if (sqlite3_step(package.get()) != SQLITE_DONE || sqlite3_step(source.get()) != SQLITE_DONE) {
            error = sqlite3_errmsg(database_); return fail();
        }
    } else {
        Statement remove(database_, "DELETE FROM recovery_sources WHERE journal_key=?1;");
        if (!remove.get()) { error = sqlite3_errmsg(database_); return fail(); }
        bindText(remove.get(), 1, record.journalKey);
        if (sqlite3_step(remove.get()) != SQLITE_DONE) { error = sqlite3_errmsg(database_); return fail(); }
    }
    if (!exec("DELETE FROM recovery_packages WHERE hash NOT IN (SELECT package_hash FROM recovery_sources);", error) ||
        !exec("COMMIT;", error)) return fail();
    return true;
}

bool ChatStore::deleteRecovery(const std::string& journalKey, std::string& error) {
    if (!database_) {
        error = "local state store is not open";
        return false;
    }
    if (!exec("BEGIN IMMEDIATE;", error)) return false;
    const auto fail = [&]() { std::string ignored; exec("ROLLBACK;", ignored); return false; };
    Statement source(database_, "DELETE FROM recovery_sources WHERE journal_key=?1;");
    if (!source.get()) { error = sqlite3_errmsg(database_); return fail(); }
    bindText(source.get(), 1, journalKey);
    if (sqlite3_step(source.get()) != SQLITE_DONE) { error = sqlite3_errmsg(database_); return fail(); }
    Statement statement(database_,
                        "DELETE FROM recovery_journals WHERE journal_key=?1;");
    if (!statement.get()) {
        error = sqlite3_errmsg(database_);
        return fail();
    }
    bindText(statement.get(), 1, journalKey);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return fail();
    }
    if (!exec("DELETE FROM recovery_packages WHERE hash NOT IN (SELECT package_hash FROM recovery_sources);", error) ||
        !exec("COMMIT;", error)) return fail();
    return true;
}

}  // namespace docxstudio::app

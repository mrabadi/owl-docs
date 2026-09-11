#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace docxstudio::app {

struct StoredChatMessage {
    std::int64_t id{};
    std::string role;
    std::string text;
    std::int64_t createdAt{};
};

struct RecoveryRecord {
    std::string journalKey;
    std::string sourcePath;
    std::string displayName;
    std::string payload;
    std::int64_t updatedAt{};
};

class ChatStore final {
public:
    ChatStore() = default;
    ~ChatStore();
    ChatStore(const ChatStore&) = delete;
    ChatStore& operator=(const ChatStore&) = delete;

    bool open(const std::string& path, std::string& error);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return database_ != nullptr; }
    std::optional<std::string> threadForDocument(const std::string& documentKey) const;
    bool bindThread(const std::string& documentKey, const std::string& threadId,
                    std::string& error);
    bool appendMessage(const std::string& documentKey, const std::string& role,
                       const std::string& text, std::string& error);
    std::vector<StoredChatMessage> messages(const std::string& documentKey,
                                            std::size_t limit = 200) const;
    std::optional<RecoveryRecord> recoveryRecord(const std::string& journalKey) const;
    std::vector<RecoveryRecord> recoveryRecords() const;
    bool upsertRecovery(const RecoveryRecord& record, std::string& error);
    bool deleteRecovery(const std::string& journalKey, std::string& error);

private:
    bool exec(const char* sql, std::string& error);
    sqlite3* database_{};
};

}  // namespace docxstudio::app

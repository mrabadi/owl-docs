#pragma once

#include "docxstudio/core/operations.h"

#include <cstddef>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace docxstudio::core {

using PreviewId = NodeId;

// Session-owned history is intentionally bounded. Image payloads are shared
// across immutable Document snapshots, so repeated text edits around the same
// image consume only one payload allocation. Distinct image payloads retained
// solely for undo/redo are subject to a separate byte budget.
struct DocumentSessionLimits {
    std::size_t maximum_history_entries{256};
    std::size_t maximum_preview_branches{4};
    std::size_t maximum_retained_history_image_bytes{
        256U * 1024U * 1024U};
};

struct DocumentSnapshot {
    Revision revision;
    Document document;
};

struct PreviewSnapshot {
    PreviewId id;
    Revision base_document_revision;
    Revision revision;
    Document document;
};

struct BatchResult {
    Revision revision;
    bool changed{false};
};

struct DocumentHistoryDepths {
    std::size_t undo{};
    std::size_t redo{};
};

enum class UndoGrouping {
    separate,
    coalesce_with_previous,
};

class DocumentSession {
public:
    DocumentSession();
    explicit DocumentSession(Document document);
    DocumentSession(Document document, DocumentSessionLimits limits);

    [[nodiscard]] DocumentSnapshot snapshot() const;
    [[nodiscard]] Result<BatchResult> applyBatch(Revision expected_revision,
                                                  std::span<const Operation> operations,
                                                  UndoGrouping undo_grouping =
                                                      UndoGrouping::separate);
    [[nodiscard]] Result<BatchResult> undo(Revision expected_revision);
    [[nodiscard]] Result<BatchResult> redo(Revision expected_revision);
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] DocumentHistoryDepths historyDepths() const;

    [[nodiscard]] Result<PreviewSnapshot> createPreview(Revision expected_document_revision);
    [[nodiscard]] Result<PreviewSnapshot> previewSnapshot(PreviewId preview_id) const;
    [[nodiscard]] Result<BatchResult> applyPreviewBatch(PreviewId preview_id,
                                                         Revision expected_preview_revision,
                                                         std::span<const Operation> operations);
    [[nodiscard]] Result<BatchResult> undoPreview(PreviewId preview_id,
                                                  Revision expected_preview_revision);
    [[nodiscard]] Result<BatchResult> redoPreview(PreviewId preview_id,
                                                  Revision expected_preview_revision);
    [[nodiscard]] Result<BatchResult> acceptPreview(PreviewId preview_id,
                                                     Revision expected_document_revision,
                                                     Revision expected_preview_revision);
    [[nodiscard]] Result<void> discardPreview(PreviewId preview_id);
    [[nodiscard]] std::size_t previewCount() const;

private:
    struct HistoryEntry {
        Document before;
        Document after;
    };

    struct PreviewBranch {
        Revision base_document_revision;
        Revision revision;
        Document document;
        std::vector<HistoryEntry> undo;
        std::vector<HistoryEntry> redo;
    };

    [[nodiscard]] static Result<Document> applyOperations(const Document& base,
                                                           std::span<const Operation> operations);
    [[nodiscard]] static Error revisionConflict(Revision expected, Revision actual,
                                                 bool preview);
    [[nodiscard]] static Result<Revision> increment(Revision revision);
    void trimHistoryStack(std::vector<HistoryEntry>& stack);
    void trimRetainedHistoryImages();

    mutable std::mutex mutex_;
    Revision revision_;
    Document document_;
    DocumentSessionLimits limits_;
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::unordered_map<PreviewId, PreviewBranch, NodeIdHash> previews_;
};

}  // namespace docxstudio::core

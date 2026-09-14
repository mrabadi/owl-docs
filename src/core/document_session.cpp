#include "docxstudio/core/document_session.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace docxstudio::core {
namespace {

Result<void> applyOne(Document& document, const Operation& operation) {
    return std::visit(
        [&document](const auto& typed) -> Result<void> {
            using Type = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Type, InsertText>) {
                return document.insertText(typed.position, typed.text, typed.format);
            } else if constexpr (std::is_same_v<Type, InsertEquation>) {
                return document.insertEquation(
                    typed.position, typed.canonical_latex, typed.display,
                    typed.equation_id, typed.format);
            } else if constexpr (std::is_same_v<Type, InsertImage>) {
                return document.insertImage(
                    typed.position, typed.encoded_payload, typed.image_format,
                    typed.accessible_name, typed.width_emu, typed.height_emu,
                    typed.image_id, typed.character_format, typed.layout);
            } else if constexpr (std::is_same_v<Type, ResizeImage>) {
                return document.resizeImage(
                    typed.image_id, typed.width_emu, typed.height_emu);
            } else if constexpr (std::is_same_v<Type, SetImageLayout>) {
                return document.setImageLayout(typed.image_id, typed.layout);
            } else if constexpr (
                std::is_same_v<Type, SetImageAccessibleName>) {
                return document.setImageAccessibleName(
                    typed.image_id, typed.accessible_name);
            } else if constexpr (std::is_same_v<Type, DeleteRange>) {
                return document.deleteRange(
                    typed.range, typed.empty_paragraph_format);
            } else if constexpr (std::is_same_v<Type, ReplaceRange>) {
                return document.replaceRange(typed.range, typed.text, typed.format);
            } else if constexpr (std::is_same_v<Type, SetCharacterFormat>) {
                return document.applyCharacterFormat(typed.range, typed.delta);
            } else if constexpr (
                std::is_same_v<Type, SetParagraphMarkCharacterFormat>) {
                return document.applyParagraphMarkCharacterFormat(
                    typed.paragraph_id, typed.delta);
            } else if constexpr (std::is_same_v<Type, SetParagraphFormat>) {
                return document.applyParagraphFormat(typed.paragraph_ids, typed.delta);
            } else if constexpr (std::is_same_v<Type, SplitParagraph>) {
                return document.splitParagraph(
                    typed.position, typed.new_paragraph_id,
                    typed.new_paragraph_mark_format);
            } else if constexpr (std::is_same_v<Type, MergeWithNextParagraph>) {
                return document.mergeWithNext(typed.paragraph_id);
            } else if constexpr (std::is_same_v<Type, InsertTable>) {
                return document.insertTable(typed.before_block_id, typed.table);
            } else if constexpr (std::is_same_v<Type, SetTableCellText>) {
                return document.setTableCellText(typed.table_id, typed.row,
                                                 typed.column, typed.text,
                                                 typed.inserted_format);
            } else if constexpr (
                std::is_same_v<Type, ReplaceTableCellRange>) {
                return document.replaceTableCellRange(
                    typed.table_id, typed.row, typed.column, typed.start,
                    typed.end, typed.text, typed.format);
            } else if constexpr (
                std::is_same_v<Type, SetTableCellCharacterFormat>) {
                return document.applyTableCellCharacterFormat(
                    typed.table_id, typed.row, typed.column, typed.start,
                    typed.end, typed.delta);
            } else if constexpr (
                std::is_same_v<Type, SetTableCellParagraphFormat>) {
                return document.applyTableCellParagraphFormat(
                    typed.table_id, typed.row, typed.column, typed.delta);
            } else if constexpr (std::is_same_v<Type, AppendTableRow>) {
                return document.appendTableRow(typed.table_id, typed.cell_ids);
            } else if constexpr (std::is_same_v<Type, InsertTableRow>) {
                return document.insertTableRow(
                    typed.table_id, typed.index, typed.cell_ids,
                    typed.inheritance_source);
            } else if constexpr (std::is_same_v<Type, DeleteTableRows>) {
                return document.deleteTableRows(
                    typed.table_id, typed.index, typed.count);
            } else if constexpr (std::is_same_v<Type, InsertTableColumn>) {
                return document.insertTableColumn(
                    typed.table_id, typed.index, typed.cell_ids,
                    typed.inheritance_source);
            } else if constexpr (std::is_same_v<Type, DeleteTableColumns>) {
                return document.deleteTableColumns(
                    typed.table_id, typed.index, typed.count);
            } else if constexpr (std::is_same_v<Type, SetTableStyle>) {
                return document.setTableStyle(typed.table_id, typed.style);
            } else if constexpr (std::is_same_v<Type, MoveTable>) {
                return document.moveTable(typed.table_id, typed.before_block_id);
            } else if constexpr (std::is_same_v<Type, DeleteTable>) {
                return document.deleteTable(typed.table_id);
            }
        },
        operation);
}

using PayloadIdentity = const std::uint8_t*;

void collectPayloadIdentities(
    const Document& document,
    std::unordered_set<PayloadIdentity>& identities) {
    for (const auto& paragraph : document.paragraphs()) {
        for (const auto& image : paragraph.images()) {
            const auto bytes = image.encoded_payload.bytes();
            if (!bytes.empty()) {
                identities.insert(bytes.data());
            }
        }
    }
}

}  // namespace

DocumentSession::DocumentSession() = default;

DocumentSession::DocumentSession(Document document) : document_(std::move(document)) {}

DocumentSession::DocumentSession(Document document, DocumentSessionLimits limits)
    : document_(std::move(document)), limits_(limits) {}

void DocumentSession::trimHistoryStack(std::vector<HistoryEntry>& stack) {
    if (stack.size() <= limits_.maximum_history_entries) {
        return;
    }
    const auto removed = stack.size() - limits_.maximum_history_entries;
    stack.erase(stack.begin(),
                stack.begin() + static_cast<std::ptrdiff_t>(removed));
}

void DocumentSession::trimRetainedHistoryImages() {
    std::unordered_set<PayloadIdentity> active;
    collectPayloadIdentities(document_, active);
    for (const auto& [id, branch] : previews_) {
        static_cast<void>(id);
        collectPayloadIdentities(branch.document, active);
    }
    const auto retained_bytes = [this, &active]() {
        std::unordered_set<PayloadIdentity> retained;
        std::size_t total = 0;
        const auto collect_history_document =
            [&active, &retained, &total](const Document& document) {
                for (const auto& paragraph : document.paragraphs()) {
                    for (const auto& image : paragraph.images()) {
                        const auto bytes = image.encoded_payload.bytes();
                        if (bytes.empty() || active.contains(bytes.data()) ||
                            !retained.insert(bytes.data()).second) {
                            continue;
                        }
                        if (bytes.size() >
                            std::numeric_limits<std::size_t>::max() - total) {
                            total = std::numeric_limits<std::size_t>::max();
                        } else {
                            total += bytes.size();
                        }
                    }
                }
            };
        const auto collect_stack = [&collect_history_document](
                                       const std::vector<HistoryEntry>& stack) {
            for (const auto& entry : stack) {
                collect_history_document(entry.before);
                collect_history_document(entry.after);
            }
        };
        collect_stack(undo_);
        collect_stack(redo_);
        for (const auto& [id, branch] : previews_) {
            static_cast<void>(id);
            collect_stack(branch.undo);
            collect_stack(branch.redo);
        }
        return total;
    };

    const auto stack_retains_inactive_payload =
        [&active](const std::vector<HistoryEntry>& stack) {
            for (const auto& entry : stack) {
                for (const Document* document :
                     {&entry.before, &entry.after}) {
                    for (const auto& paragraph : document->paragraphs()) {
                        for (const auto& image : paragraph.images()) {
                            const auto bytes = image.encoded_payload.bytes();
                            if (!bytes.empty() &&
                                !active.contains(bytes.data())) {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        };
    const auto discard_oldest =
        [this, &stack_retains_inactive_payload]() {
        // Preserve the live document's undo history ahead of disposable
        // preview-internal history. Erasing the front retains a contiguous
        // chain from the current state for both undo and redo stacks. Stacks
        // that retain no budgeted payload are left intact.
        for (auto& [id, branch] : previews_) {
            static_cast<void>(id);
            if (stack_retains_inactive_payload(branch.undo)) {
                branch.undo.erase(branch.undo.begin());
                return true;
            }
            if (stack_retains_inactive_payload(branch.redo)) {
                branch.redo.erase(branch.redo.begin());
                return true;
            }
        }
        if (stack_retains_inactive_payload(redo_)) {
            redo_.erase(redo_.begin());
            return true;
        }
        if (stack_retains_inactive_payload(undo_)) {
            undo_.erase(undo_.begin());
            return true;
        }
        return false;
    };

    while (retained_bytes() >
           limits_.maximum_retained_history_image_bytes) {
        if (!discard_oldest()) {
            break;
        }
    }
}

DocumentSnapshot DocumentSession::snapshot() const {
    std::scoped_lock lock(mutex_);
    return DocumentSnapshot{revision_, document_};
}

Error DocumentSession::revisionConflict(Revision expected, Revision actual, bool preview) {
    std::ostringstream message;
    message << (preview ? "Preview" : "Document") << " revision conflict: expected "
            << expected.value() << ", actual " << actual.value();
    return Error{preview ? ErrorCode::preview_conflict : ErrorCode::revision_conflict,
                 message.str()};
}

Result<Revision> DocumentSession::increment(Revision revision) {
    const auto next = revision.next();
    if (!next) {
        return Error{ErrorCode::revision_overflow, "Revision counter exhausted"};
    }
    return *next;
}

Result<Document> DocumentSession::applyOperations(const Document& base,
                                                   std::span<const Operation> operations) {
    Document candidate = base;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto result = applyOne(candidate, operations[index]);
        if (!result) {
            auto error = result.error();
            error.message = "Operation " + std::to_string(index) + ": " + error.message;
            return error;
        }
    }
    return candidate;
}

Result<BatchResult> DocumentSession::applyBatch(Revision expected_revision,
                                                 std::span<const Operation> operations,
                                                 UndoGrouping undo_grouping) {
    std::scoped_lock lock(mutex_);
    if (expected_revision != revision_) {
        return revisionConflict(expected_revision, revision_, false);
    }
    if (operations.empty()) {
        return BatchResult{revision_, false};
    }

    auto candidate = applyOperations(document_, operations);
    if (!candidate) {
        return candidate.error();
    }
    if (candidate.value() == document_) {
        return BatchResult{revision_, false};
    }
    const auto next = increment(revision_);
    if (!next) {
        return next.error();
    }

    const bool can_coalesce =
        undo_grouping == UndoGrouping::coalesce_with_previous &&
        redo_.empty() && !undo_.empty() && undo_.back().after == document_;
    if (can_coalesce) {
        undo_.back().after = candidate.value();
    } else {
        undo_.push_back(HistoryEntry{document_, candidate.value()});
    }
    redo_.clear();
    document_ = std::move(candidate.value());
    revision_ = next.value();
    trimHistoryStack(undo_);
    trimRetainedHistoryImages();
    return BatchResult{revision_, true};
}

Result<BatchResult> DocumentSession::undo(Revision expected_revision) {
    std::scoped_lock lock(mutex_);
    if (expected_revision != revision_) {
        return revisionConflict(expected_revision, revision_, false);
    }
    if (undo_.empty()) {
        return Error{ErrorCode::history_empty, "There is no document transaction to undo"};
    }
    const auto next = increment(revision_);
    if (!next) {
        return next.error();
    }

    auto entry = std::move(undo_.back());
    undo_.pop_back();
    document_ = entry.before;
    redo_.push_back(std::move(entry));
    revision_ = next.value();
    trimHistoryStack(redo_);
    trimRetainedHistoryImages();
    return BatchResult{revision_, true};
}

Result<BatchResult> DocumentSession::redo(Revision expected_revision) {
    std::scoped_lock lock(mutex_);
    if (expected_revision != revision_) {
        return revisionConflict(expected_revision, revision_, false);
    }
    if (redo_.empty()) {
        return Error{ErrorCode::history_empty, "There is no document transaction to redo"};
    }
    const auto next = increment(revision_);
    if (!next) {
        return next.error();
    }

    auto entry = std::move(redo_.back());
    redo_.pop_back();
    document_ = entry.after;
    undo_.push_back(std::move(entry));
    revision_ = next.value();
    trimHistoryStack(undo_);
    trimRetainedHistoryImages();
    return BatchResult{revision_, true};
}

bool DocumentSession::canUndo() const {
    std::scoped_lock lock(mutex_);
    return !undo_.empty();
}

bool DocumentSession::canRedo() const {
    std::scoped_lock lock(mutex_);
    return !redo_.empty();
}

DocumentHistoryDepths DocumentSession::historyDepths() const {
    std::scoped_lock lock(mutex_);
    return {undo_.size(), redo_.size()};
}

Result<PreviewSnapshot> DocumentSession::createPreview(Revision expected_document_revision) {
    std::scoped_lock lock(mutex_);
    if (expected_document_revision != revision_) {
        return revisionConflict(expected_document_revision, revision_, false);
    }
    if (previews_.size() >= limits_.maximum_preview_branches) {
        return Error{ErrorCode::invalid_operation,
                     "The document has reached the preview-branch limit"};
    }

    auto id = PreviewId::generate();
    while (previews_.contains(id)) {
        id = PreviewId::generate();
    }
    PreviewBranch branch{revision_, Revision{}, document_, {}, {}};
    previews_.emplace(id, branch);
    return PreviewSnapshot{id, branch.base_document_revision, branch.revision, branch.document};
}

Result<PreviewSnapshot> DocumentSession::previewSnapshot(PreviewId preview_id) const {
    std::scoped_lock lock(mutex_);
    const auto found = previews_.find(preview_id);
    if (found == previews_.end()) {
        return Error{ErrorCode::preview_not_found, "Preview branch not found"};
    }
    const auto& branch = found->second;
    return PreviewSnapshot{preview_id, branch.base_document_revision, branch.revision,
                           branch.document};
}

Result<BatchResult> DocumentSession::applyPreviewBatch(PreviewId preview_id,
                                                        Revision expected_preview_revision,
                                                        std::span<const Operation> operations) {
    std::scoped_lock lock(mutex_);
    const auto found = previews_.find(preview_id);
    if (found == previews_.end()) {
        return Error{ErrorCode::preview_not_found, "Preview branch not found"};
    }
    auto& branch = found->second;
    if (expected_preview_revision != branch.revision) {
        return revisionConflict(expected_preview_revision, branch.revision, true);
    }
    if (operations.empty()) {
        return BatchResult{branch.revision, false};
    }

    auto candidate = applyOperations(branch.document, operations);
    if (!candidate) {
        return candidate.error();
    }
    if (candidate.value() == branch.document) {
        return BatchResult{branch.revision, false};
    }
    const auto next = increment(branch.revision);
    if (!next) {
        return next.error();
    }

    branch.undo.push_back(HistoryEntry{branch.document, candidate.value()});
    branch.redo.clear();
    branch.document = std::move(candidate.value());
    branch.revision = next.value();
    trimHistoryStack(branch.undo);
    trimRetainedHistoryImages();
    return BatchResult{branch.revision, true};
}

Result<BatchResult> DocumentSession::undoPreview(PreviewId preview_id,
                                                 Revision expected_preview_revision) {
    std::scoped_lock lock(mutex_);
    const auto found = previews_.find(preview_id);
    if (found == previews_.end()) {
        return Error{ErrorCode::preview_not_found, "Preview branch not found"};
    }
    auto& branch = found->second;
    if (expected_preview_revision != branch.revision) {
        return revisionConflict(expected_preview_revision, branch.revision, true);
    }
    if (branch.undo.empty()) {
        return Error{ErrorCode::history_empty, "There is no preview transaction to undo"};
    }
    const auto next = increment(branch.revision);
    if (!next) {
        return next.error();
    }

    auto entry = std::move(branch.undo.back());
    branch.undo.pop_back();
    branch.document = entry.before;
    branch.redo.push_back(std::move(entry));
    branch.revision = next.value();
    trimHistoryStack(branch.redo);
    trimRetainedHistoryImages();
    return BatchResult{branch.revision, true};
}

Result<BatchResult> DocumentSession::redoPreview(PreviewId preview_id,
                                                 Revision expected_preview_revision) {
    std::scoped_lock lock(mutex_);
    const auto found = previews_.find(preview_id);
    if (found == previews_.end()) {
        return Error{ErrorCode::preview_not_found, "Preview branch not found"};
    }
    auto& branch = found->second;
    if (expected_preview_revision != branch.revision) {
        return revisionConflict(expected_preview_revision, branch.revision, true);
    }
    if (branch.redo.empty()) {
        return Error{ErrorCode::history_empty, "There is no preview transaction to redo"};
    }
    const auto next = increment(branch.revision);
    if (!next) {
        return next.error();
    }

    auto entry = std::move(branch.redo.back());
    branch.redo.pop_back();
    branch.document = entry.after;
    branch.undo.push_back(std::move(entry));
    branch.revision = next.value();
    trimHistoryStack(branch.undo);
    trimRetainedHistoryImages();
    return BatchResult{branch.revision, true};
}

Result<BatchResult> DocumentSession::acceptPreview(PreviewId preview_id,
                                                    Revision expected_document_revision,
                                                    Revision expected_preview_revision) {
    std::scoped_lock lock(mutex_);
    const auto found = previews_.find(preview_id);
    if (found == previews_.end()) {
        return Error{ErrorCode::preview_not_found, "Preview branch not found"};
    }
    const auto& branch = found->second;
    if (expected_document_revision != revision_) {
        return revisionConflict(expected_document_revision, revision_, false);
    }
    if (expected_preview_revision != branch.revision) {
        return revisionConflict(expected_preview_revision, branch.revision, true);
    }
    if (branch.base_document_revision != revision_) {
        return Error{ErrorCode::preview_conflict,
                     "The document changed after this preview branch was created"};
    }
    if (branch.document == document_) {
        previews_.erase(found);
        trimRetainedHistoryImages();
        return BatchResult{revision_, false};
    }
    const auto next = increment(revision_);
    if (!next) {
        return next.error();
    }

    const Document accepted = branch.document;
    undo_.push_back(HistoryEntry{document_, accepted});
    redo_.clear();
    document_ = accepted;
    revision_ = next.value();
    previews_.erase(found);
    trimHistoryStack(undo_);
    trimRetainedHistoryImages();
    return BatchResult{revision_, true};
}

Result<void> DocumentSession::discardPreview(PreviewId preview_id) {
    std::scoped_lock lock(mutex_);
    if (previews_.erase(preview_id) == 0) {
        return Error{ErrorCode::preview_not_found, "Preview branch not found"};
    }
    trimRetainedHistoryImages();
    return {};
}

std::size_t DocumentSession::previewCount() const {
    std::scoped_lock lock(mutex_);
    return previews_.size();
}

}  // namespace docxstudio::core

#include "docxstudio/app/DocumentCanvas.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace docxstudio::app {
bool DocumentCanvas::planListOperations(
    const core::Document& document, const ListEditRequest& request,
    std::vector<core::Operation>& operations, QString& error) const {
    error.clear();
    const bool refresh = request.kind == ListEditRequest::Kind::refresh_numbering;
    const bool bullets = request.listKind == QStringLiteral("bullets") || request.listKind == QStringLiteral("bulleted") || request.listKind == QStringLiteral("bullet");
    if ((!refresh && request.blockIds.empty()) ||
        (request.kind == ListEditRequest::Kind::set_kind && !bullets && request.listKind != QStringLiteral("numbered") && request.listKind != QStringLiteral("none")) ||
        (request.kind == ListEditRequest::Kind::set_level && (request.level < 1 || request.level > static_cast<int>(core::kListLevelCount)))) {
        error = tr("The list request requires supported settings and body paragraph identities.");
        return false;
    }
    std::set<core::NodeId> requested(request.blockIds.begin(), request.blockIds.end());
    if (requested.size() != request.blockIds.size()) { error = tr("List paragraph identities must be unique."); return false; }
    std::vector<core::NodeId> ordered;
    std::size_t first = document.bodyBlocks().size(), last = 0;
    for (std::size_t index = 0; index < document.bodyBlocks().size(); ++index) {
        const auto& block = document.bodyBlocks()[index];
        if (refresh && requested.empty() && block.kind == core::BodyBlockKind::paragraph) ordered.push_back(block.id);
        else if (requested.contains(block.id)) {
            if (block.kind != core::BodyBlockKind::paragraph) { error = tr("Lists can target body paragraphs only."); return false; }
            ordered.push_back(block.id); first = std::min(first, index); last = index;
        }
    }
    if ((!requested.empty() && ordered.size() != requested.size()) ||
        (!refresh && (ordered.empty() || last - first + 1 != ordered.size()))) {
        error = tr("List edits require available, contiguous body paragraphs."); return false;
    }
    DocumentCanvas shadow(spelling_);
    shadow.session_ = std::make_unique<core::DocumentSession>(document);
    shadow.defaultFontFamily_ = defaultFontFamily_;
    shadow.defaultFontPointSize_ = defaultFontPointSize_;
    shadow.tabWidthSpaces_ = tabWidthSpaces_;
    shadow.defaultListLayout_ = defaultListLayout_;
    std::vector<core::Operation> planned;
    shadow.plannedOperations_ = &planned;
    QString failure;
    connect(&shadow, &DocumentCanvas::operationFailed, &shadow, [&failure](const QString& message) { failure = message; });
    bool currentBullets = false, currentNumbering = false;
    connect(&shadow, &DocumentCanvas::cursorListStateChanged, &shadow,
        [&](bool valueBullets, bool valueNumbering) { currentBullets = valueBullets; currentNumbering = valueNumbering; });
    const auto select = [&](core::NodeId begin, core::NodeId end) {
        const auto snapshot = shadow.session_->snapshot();
        const auto* paragraph = snapshot.document.findParagraph(end);
        if (!paragraph || !snapshot.document.findParagraph(begin)) return false;
        shadow.selection_ = {{begin, 0}, {end, paragraph->text().size()}};
        shadow.typingFormat_ = shadow.selectedCharacterFormat();
        shadow.emitCursorFormat();
        return true;
    };
    if (refresh) {
        std::set<core::NodeId> visited;
        for (const auto block : ordered) {
            if (visited.contains(block)) continue;
            const auto snapshot = shadow.session_->snapshot();
            const auto* paragraph = snapshot.document.findParagraph(block);
            if (!paragraph || !paragraph->format().list_id) continue;
            if (!select(block, block)) return false;
            if (!currentNumbering) continue;
            if (!shadow.resequenceNumberedList(block, false, request.numberingBaseline, &visited)) { error = failure.isEmpty() ? tr("The list could not be renumbered.") : failure; return false; }
        }
    } else if (request.kind == ListEditRequest::Kind::set_kind) {
        if (request.listKind == QStringLiteral("none")) {
            for (const auto block : ordered) {
                if (!select(block, block)) return false;
                if (currentBullets) shadow.toggleBullets();
                else if (currentNumbering) shadow.toggleNumbering();
            }
        } else {
            bool allTarget = true, allLookTarget = true;
            for (const auto block : ordered) {
                if (!select(block, block)) return false;
                const auto snapshot = shadow.session_->snapshot();
                const auto* paragraph = snapshot.document.findParagraph(block);
                const bool matches = bullets ? currentBullets : currentNumbering;
                allLookTarget = allLookTarget && matches;
                allTarget = allTarget && matches && paragraph->format().list_id.has_value();
            }
            if (!allTarget) {
                if (!select(ordered.front(), ordered.back())) return false;
                // Literal prefixes without native list identity must be
                // converted explicitly, rather than mistaken for an already
                // authored list or adopted while loading the working copy.
                if (allLookTarget) {
                    if (bullets) shadow.toggleBullets(); else shadow.toggleNumbering();
                }
                if (bullets) shadow.toggleBullets(); else shadow.toggleNumbering();
            }
        }
    } else if (request.kind == ListEditRequest::Kind::set_level) {
        for (const auto block : ordered) {
            if (!select(block, block) || !shadow.hasActiveList()) { error = tr("Only existing list paragraphs have list levels."); return false; }
            int level = shadow.activeListLevel();
            while (level != request.level) {
                if (!shadow.changeListLevel(level > request.level)) { error = tr("The list level could not be changed."); return false; }
                const int next = shadow.activeListLevel();
                if (next == level) { error = tr("The requested list level is unavailable."); return false; }
                level = next;
            }
        }
    } else if (request.kind == ListEditRequest::Kind::set_layout) {
        for (const auto block : ordered) {
            if (!select(block, block) || !shadow.setCurrentListLayout(request.layout)) { error = failure.isEmpty() ? tr("Only existing lists have list layout settings.") : failure; return false; }
        }
    } else { error = tr("The list planning request is unsupported."); return false; }
    if (!failure.isEmpty()) { error = failure; return false; }
    operations.insert(operations.end(), std::make_move_iterator(planned.begin()), std::make_move_iterator(planned.end()));
    return true;
}
}  // namespace docxstudio::app

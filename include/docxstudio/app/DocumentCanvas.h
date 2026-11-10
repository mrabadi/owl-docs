#pragma once

#include "docxstudio/core/document_session.h"
#include <QAbstractScrollArea>
#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QMarginsF>
#include <QSizeF>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

class QInputMethodEvent;
class QMimeData;
class QPlainTextEdit;
class QMouseEvent;
class QPaintEvent;
class QPdfWriter;
class QPrinter;
class QTextLayout;
class QTimer;
class QWheelEvent;

namespace docxstudio::app {

class SpellChecker;

struct ImportedInlineImagePresentation {
    ImportedInlineImagePresentation() = default;
    ImportedInlineImagePresentation(core::NodeId imageId,
                                    QImage decodedImage)
        : id(imageId), image(std::move(decodedImage)) {}

    // The semantic ImageAtom owns identity, run order, encoded bytes, format,
    // accessible name, and geometry. This presentation entry is deliberately
    // only a decoded-pixel cache keyed by that stable identity.
    core::NodeId id;
    QImage image;
};

enum class ImportedCellVerticalAlignment { top, center, bottom };

struct ImportedCellBorderPresentation {
    std::uint32_t argb{0xff000000U};
    double widthPoints{0.5};
};

struct ImportedTableCellPresentation {
    QString sourceText;
    std::vector<core::FormatRun> formats;
    std::optional<core::ParagraphAlignment> alignment;
    double spaceBeforePoints{0.0};
    double spaceAfterPoints{0.0};
    std::optional<std::uint32_t> lineSpacing;
    std::optional<core::LineSpacingRule> lineSpacingRule;
    ImportedCellVerticalAlignment verticalAlignment{
        ImportedCellVerticalAlignment::top};
    double paddingTopPoints{4.0};
    double paddingRightPoints{5.0};
    double paddingBottomPoints{4.0};
    double paddingLeftPoints{5.0};
    std::optional<std::uint32_t> fillArgb;
    std::optional<ImportedCellBorderPresentation> borderTop;
    std::optional<ImportedCellBorderPresentation> borderRight;
    std::optional<ImportedCellBorderPresentation> borderBottom;
    std::optional<ImportedCellBorderPresentation> borderLeft;
};

struct ImportedTablePresentation {
    core::NodeId tableId;
    std::optional<core::ParagraphAlignment> alignment;
    std::vector<double> columnWidthsPoints;
    // When supplied by the DOCX importer, identities prevent stale ordinal
    // presentation data from being reused after structural table edits.
    std::vector<core::NodeId> cellIds;
    std::vector<ImportedTableCellPresentation> cells;
    std::optional<core::TableStyle> sourceSemanticStyle;
    std::optional<std::string> sourceStyleId;
};

struct DocumentSearchOptions {
    bool caseSensitive{false};
    bool wholeWord{false};

    auto operator<=>(const DocumentSearchOptions&) const = default;
};

struct BodyParagraphSearchHit {
    core::NodeId paragraphId;
    std::size_t startUtf16{};
    std::size_t endUtf16{};

    auto operator<=>(const BodyParagraphSearchHit&) const = default;
};

struct TableCellSearchHit {
    core::NodeId tableId;
    core::NodeId cellId;
    std::size_t row{};
    std::size_t column{};
    std::size_t startUtf16{};
    std::size_t endUtf16{};

    auto operator<=>(const TableCellSearchHit&) const = default;
};

// Search hits carry the document revision that produced their UTF-16 ranges.
// Activation and replacement reject stale hits instead of guessing after an
// intervening edit or table-structure change.
struct DocumentSearchHit {
    core::Revision revision;
    // Live and preview revisions advance independently and can have the same
    // numeric value. Branch identity prevents a preview hit from becoming a
    // coincidentally valid live edit target after accept/discard.
    std::optional<core::PreviewId> previewId;
    std::variant<BodyParagraphSearchHit, TableCellSearchHit> target;

    auto operator<=>(const DocumentSearchHit&) const = default;
};

struct DocumentSearchMatch {
    DocumentSearchHit hit;
    QString containerText;
    // One-based paragraph or table ordinal, depending on hit.target. It is
    // computed from the same visible snapshot as the hit and container text,
    // so preview insertions/reordering cannot produce live-document labels.
    std::size_t containerOrdinal{};
    // Zero-based ordinal across every searchable body paragraph and table
    // cell. Navigation compares this once-computed value with the caret unit
    // instead of rescanning the complete document for every match.
    std::size_t searchUnitOrdinal{};

    auto operator<=>(const DocumentSearchMatch&) const = default;
};

struct PageLayoutSettings {
    double widthPoints{612.0};
    double heightPoints{792.0};
    double marginTopPoints{72.0};
    double marginRightPoints{72.0};
    double marginBottomPoints{72.0};
    double marginLeftPoints{72.0};
    auto operator<=>(const PageLayoutSettings&) const = default;
};

struct ListEditRequest {
    enum class Kind { set_kind, set_level, set_layout, refresh_numbering };
    Kind kind{Kind::set_kind};
    std::vector<core::NodeId> blockIds;
    QString listKind{QStringLiteral("numbered")};
    int level{1};
    core::ListLayout layout;
    // Borrowed only for this synchronous plan; retains authored starting
    // counters when a structural edit changes the first list item.
    const core::Document* numberingBaseline{nullptr};
};
struct TableCellRange {
    core::NodeId tableId;
    core::NodeId cellId;
    std::size_t row{0}, column{0}, start{0}, end{0};
};
struct StorySelection {
    bool footer{false};
    int region{0};
    // UTF-16 offsets within the active left/center/right region.
    std::size_t start{0}, end{0};
};

class DocumentCanvas final : public QAbstractScrollArea {
    Q_OBJECT

public:
    static constexpr int kMinimumZoomPercent = 25;
    static constexpr int kMaximumZoomPercent = 400;

    explicit DocumentCanvas(SpellChecker& spelling, QWidget* parent = nullptr);
    DocumentCanvas(SpellChecker& spelling, core::DocumentSessionLimits limits,
                   QWidget* parent = nullptr);
    ~DocumentCanvas() override;

    void setDocument(core::Document document);
    void setImportedPresentation(
        std::vector<ImportedInlineImagePresentation> images,
        std::vector<ImportedTablePresentation> tables);
    void setEditorDefaults(const QString& fontFamily, double fontPointSize,
                           int tabWidthSpaces);
    void setDefaultListLayout(const core::ListLayout& layout);
    QString defaultFontFamily() const { return defaultFontFamily_; }
    double defaultFontPointSize() const noexcept { return defaultFontPointSize_; }
    int tabWidthSpaces() const noexcept { return tabWidthSpaces_; }
    const core::ListLayout& defaultListLayout() const noexcept {
        return defaultListLayout_;
    }
    core::DocumentSnapshot snapshot() const;
    core::Range selection() const;
    QString selectedText() const;
    QString outlineText(std::size_t maxCharacters = 12000) const;

    bool isModified() const noexcept { return modified_; }
    bool hasNonTextChanges() const noexcept { return nonTextModified_; }
    bool hasPageLayoutChanges() const noexcept { return pageLayoutModified_; }
    void markSaved() noexcept;
    void markRecovered(bool nonTextChanged = true, bool pageLayoutChanged = true);
    void setZoomPercent(int percent);
    void fitToWidth();
    int zoomPercent() const noexcept { return zoomPercent_; }
    double pageWidthPoints() const noexcept { return previewOriginalLayout_ ? previewOriginalLayout_->widthPoints : pageWidthPoints_; }
    double pageHeightPoints() const noexcept { return previewOriginalLayout_ ? previewOriginalLayout_->heightPoints : pageHeightPoints_; }
    double marginTopPoints() const noexcept { return previewOriginalLayout_ ? previewOriginalLayout_->marginTopPoints : marginTopPoints_; }
    double marginRightPoints() const noexcept { return previewOriginalLayout_ ? previewOriginalLayout_->marginRightPoints : marginRightPoints_; }
    double marginBottomPoints() const noexcept { return previewOriginalLayout_ ? previewOriginalLayout_->marginBottomPoints : marginBottomPoints_; }
    double marginLeftPoints() const noexcept { return previewOriginalLayout_ ? previewOriginalLayout_->marginLeftPoints : marginLeftPoints_; }
    int pageCount() const;
    // Maximum rendered line count per story region across the document pages.
    static std::array<int, 3> measureStoryLines(
        const core::Document& document, bool footer,
        const QSizeF& pageSizePoints, const QMarginsF& marginsPoints,
        const QString& fontFamily, double fontPointSize, int pageCount = 1);
    // Monotonic diagnostic for proving that screen, print, and PDF consume
    // an already-computed pagination result instead of silently repaginating.
    std::uint64_t layoutGeneration() const;
    int currentPageNumber() const;
    int paragraphPageNumber(core::NodeId paragraphId) const;

    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void pasteTextOnly();
    void selectAll();
    void insertText(const QString& text);
    // Inserts a bounded PNG/JPEG as an inline picture at the current body
    // selection. The picture participates in line layout and remains anchored
    // to that text boundary as surrounding content changes.
    bool insertInlineImage(std::vector<std::uint8_t> encodedBytes,
                           const QString& accessibleName);
    // Inserts into the currently active on-page header/footer region. Returns
    // false when header/footer editing is not active or the image is invalid.
    bool insertHeaderFooterImage(std::vector<std::uint8_t> encodedBytes,
                                 const QString& accessibleName);
    std::optional<QByteArray> selectedExcalidrawScene() const;
    bool replaceSelectedExcalidrawFigure(
        std::vector<std::uint8_t> encodedPng);
    bool selectInlineImage(core::NodeId imageId);
    bool deleteSelectedInlineImage();
    bool resizeSelectedInlineImage(double widthPoints, double heightPoints);
    std::optional<core::NodeId> selectedInlineImageId() const;
    std::optional<core::ImageLayout> selectedImageLayout() const;
    QString selectedImageAccessibleName() const;
    bool setSelectedImageLayout(const core::ImageLayout& layout);
    bool setSelectedImageAccessibleName(const QString& accessibleName);
    void showSelectedImageSizeDialog();
    void showSelectedImageAltTextDialog();
    void showSelectedImageLayoutDialog();
    bool insertEquation(const QString& latex, bool display = false);
    bool selectEquation(core::NodeId equationId);
    std::optional<core::NodeId> selectedEquationId() const;
    bool replaceSelectedEquation(const QString& latex, bool display);
    void showEquationEditor();
    QRectF equationBounds(core::NodeId equationId) const;
    bool navigateToParagraph(core::NodeId paragraphId);
    bool insertNote(const QString& text, core::NoteKind kind);
    bool updateNote(core::NodeId id, const QString& text);
    bool insertCaption(core::CaptionLabel label, const QString& text);
    bool insertBibliographyEntry(const QString& text);
    bool insertCitation(core::NodeId entryId, const QString& locator = {});
    bool insertCrossReference(core::NodeId targetId, bool fullText = false);
    bool insertTableOfContents(int maximumLevel = 3);
    void showReferencesEditor(const QString& kind);
    bool insertTable(std::size_t rows, std::size_t columns, bool headerRow);
    bool insertTableRow(bool after);
    bool insertTableColumn(bool after);
    bool deleteSelectedTableRows();
    bool deleteSelectedTableColumns();
    bool setTableStyle(const QString& styleKey);
    bool activateTableCell(core::NodeId tableId, std::size_t row,
                           std::size_t column, std::size_t utf16Offset = 0);
    // Selects the inclusive rectangular cell range. This is distinct from an
    // insertion caret inside one cell and from selecting the table object.
    bool selectTableCells(core::NodeId tableId,
                          std::size_t anchorRow, std::size_t anchorColumn,
                          std::size_t focusRow, std::size_t focusColumn);
    bool selectTable(core::NodeId tableId);
    std::vector<core::NodeId> selectedTableCellIds() const;
    std::optional<TableCellRange> selectedTableCellRange() const;
    bool moveSelectedTable(bool forward);
    std::optional<core::NodeId> selectedTableId() const noexcept {
        return selectedTable_;
    }
    QRectF tableCellViewportRect(core::NodeId tableId, std::size_t row, std::size_t column) const;
    // Fits to the candidate's actual cell writing area without changing the
    // live canvas. Invalid dimensions, layout or stable targets return nullopt.
    std::optional<QSizeF> fitTableCellImageSize(
        const core::Document& document, core::NodeId tableId,
        core::NodeId cellId, QSizeF desiredPoints,
        std::optional<PageLayoutSettings> proposedLayout = std::nullopt) const;
    bool setSelectedTableCellAlignment(core::ParagraphAlignment horizontal, core::TableCellVerticalAlignment vertical);
    std::optional<std::pair<core::ParagraphAlignment, core::TableCellVerticalAlignment>> selectedTableCellAlignment() const;
    bool setSelectedTableBorders(const QString& preset, double widthPoints, const QColor& color);
    bool setSelectedTableColumnWidth(double points);
    bool setSelectedTableRowMinimumHeight(double points);
    bool distributeSelectedTableColumns();
    bool distributeSelectedTableRows();
    double selectedTableColumnWidthPoints() const;
    double selectedTableRowMinimumHeightPoints() const;
    void toggleBullets();
    void toggleNumbering();
    bool hasActiveList() const;
    int activeListLevel() const;
    std::optional<core::ListLayout> currentListLayout() const;
    bool setCurrentListLayout(const core::ListLayout& layout);
    bool changeListLevel(bool outdent);
    void insertPageBreak();
    QString headerText() const;
    QString footerText() const;
    bool setHeaderText(const QString& text);
    bool setFooterText(const QString& text);
    bool setHeaderFooterText(const QString& header, const QString& footer);
    void beginHeaderFooterEditing(bool footer = false, int section = 0,
                                  int pageIndex = 0);
    void endHeaderFooterEditing();
    bool isHeaderFooterEditing() const noexcept {
        return headerFooterEditing_;
    }
    std::optional<StorySelection> activeStorySelection() const;
    // The canvas owns these controls even when the workspace hosts them in
    // its contextual ribbon. Editing itself remains in the page viewport.
    QWidget* headerFooterControls();
    void applyCharacterFormat(const core::CharacterFormatDelta& delta);
    void applyParagraphFormat(const core::ParagraphFormatDelta& delta);
    void applyParagraphStyle(const QString& styleId);
    // Produces the same provenance-aware semantic operations used by the UI
    // for an arbitrary isolated document. The Codex bridge uses this while
    // constructing preview branches, so AI and direct editor style changes
    // cannot drift in formatting or configured-default behavior.
    [[nodiscard]] std::vector<core::Operation> planParagraphStyleOperations(
        const core::Document& document,
        const std::vector<core::NodeId>& paragraphIds,
        const core::ParagraphStyleDefinition& target,
        bool ensureTargetProvenance = false) const;
    // Plans through the actual UI list implementation on a private canvas;
    // the supplied document and this visible canvas are never changed.
    [[nodiscard]] bool planListOperations(
        const core::Document& document, const ListEditRequest& request,
        std::vector<core::Operation>& operations, QString& error) const;
    QString currentParagraphStyleId() const;
    bool paragraphStylesAvailable() const noexcept;
    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void toggleStrike();
    void setBaseline(core::BaselinePosition baseline);
    void toggleBaseline(core::BaselinePosition baseline);
    void setFontFamily(const QString& family);
    void setFontPointSize(double points);
    void setForeground(const QColor& color);
    void setHighlight(const QColor& color);
    void clearHighlight();
    void beginColorAdjustment();
    void endColorAdjustment() noexcept;
    QColor currentTextColor() const;
    QColor currentHighlightColor() const;
    QString currentFontFamily() const;
    double currentFontPointSize() const;
    void refreshCursorFormat();
    void setAlignment(core::ParagraphAlignment alignment);
    void setMarginsPoints(double top, double right, double bottom, double left);
    void setPageSizePoints(double width, double height);
    void setImportedPageLayout(double width, double height,
                               double top, double right,
                               double bottom, double left);
    void toggleOrientation();

    std::vector<DocumentSearchHit> searchHits(
        const QString& needle,
        DocumentSearchOptions options = {}) const;
    // Returns hits and their containing text from one immutable snapshot so
    // navigation panes can build many snippets without copying the document
    // once per result.
    std::vector<DocumentSearchMatch> searchMatches(
        const QString& needle,
        DocumentSearchOptions options = {}) const;
    // Returns the complete searchable paragraph/cell text for a valid hit in
    // the currently displayed branch. Callers can build snippets without
    // mixing preview targets with a live-document snapshot.
    std::optional<QString> searchHitContainerText(
        const DocumentSearchHit& hit) const;
    std::optional<DocumentSearchHit> currentSearchHit() const;
    bool activateSearchHit(const DocumentSearchHit& hit);
    std::optional<DocumentSearchHit> findNextHit(
        const QString& needle,
        DocumentSearchOptions options = {});
    std::optional<DocumentSearchHit> findPreviousHit(
        const QString& needle,
        DocumentSearchOptions options = {});
    bool replaceSearchHit(const DocumentSearchHit& hit,
                          const QString& replacement);
    bool replaceCurrent(const QString& needle, const QString& replacement,
                        DocumentSearchOptions options = {});
    int replaceAllMatches(const QString& needle, const QString& replacement,
                          DocumentSearchOptions options = {});

    // Compatibility wrappers for the original find dialog. New UI should use
    // the typed API above so whole-word options and stable table-cell targets
    // remain available.
    bool findNext(const QString& needle, bool caseSensitive = false);
    int replaceAll(const QString& needle, const QString& replacement,
                   bool caseSensitive = false);
    bool exportPdf(const QString& path, QString& error);
    bool configurePrinter(QPrinter& printer, QString& error) const;
    bool printTo(QPrinter& printer, QString& error);
    bool createOperationsPreview(core::Revision expectedRevision,
                                 const std::vector<core::Operation>& operations,
                                 const QString& label,
                                 QString& summary,
                                 QString& error,
                                 std::optional<PageLayoutSettings> proposedLayout = std::nullopt);
    bool createReplacementPreview(const QString& replacement, QString& summary, QString& error);
    bool acceptPreview(QString& error);
    void discardPreview();
    bool hasPreview() const noexcept { return previewId_.has_value(); }
    QString previewIdString() const {
        return previewId_ ? QString::fromStdString(previewId_->toString()) : QString();
    }

signals:
    void documentChanged(qulonglong revision);
    void selectionChanged();
    void cursorFormatChanged(const QString& family, double pointSize,
                             const QColor& textColor);
    void cursorHighlightChanged(const QColor& highlightColor);
    void cursorStyleChanged(bool bold, bool italic, bool underline,
                            bool strike, bool superscript, bool subscript);
    // Empty means that the current selection spans multiple paragraph styles
    // or that paragraph styles are not applicable (for example, a table
    // object selection). An unstyled body paragraph reports Normal.
    void cursorParagraphStyleChanged(const QString& styleId);
    void cursorListStateChanged(bool bullets, bool numbering);
    void cursorListContextChanged(bool active, int oneBasedLevel);
    void listPropertiesRequested();
    void editExcalidrawFigureRequested();
    void pageStatusChanged(int currentPage, int pageCount, int wordCount);
    void zoomChanged(int percent);
    // The displayed branch changed between the live document and an isolated
    // Codex preview even when the live document revision did not change.
    void previewStateChanged(bool active);
    void operationFailed(const QString& message);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    bool focusNextPrevChild(bool next) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct VisualLine;
    struct ParagraphVisual;
    struct EquationVisual;
    struct NoteVisual;
    struct TocVisual;
    struct TableCellVisual;
    struct TableRowVisual;
    struct TableVisual;
    struct BlockPlacement;
    struct Hit;
    enum class ImageResizeHandle {
        right,
        bottom_right,
        bottom,
    };
    struct ImageResizeDrag {
        core::NodeId imageId;
        ImageResizeHandle handle{ImageResizeHandle::bottom_right};
        QPointF startPoint;
        QRectF originalRect;
        QRectF previewRect;
        int pageIndex{};
        double originalWidthPoints{};
        double originalHeightPoints{};
    };
    struct TableCursor {
        core::NodeId tableId;
        std::size_t row{};
        std::size_t column{};
        std::size_t utf16Offset{};

        auto operator<=>(const TableCursor&) const = default;
    };
    struct TableBoundary {
        core::NodeId tableId;
        bool column{true};
        std::size_t index{};
    };
    struct TableResizeDrag {
        TableBoundary boundary;
        QPointF startViewportPoint;
        core::TableGeometry original;
        core::TableGeometry resolved;
        core::TableGeometry preview;
    };
    struct PendingSpellingWord {
        enum class Container { bodyParagraph, tableCell };

        Container container{Container::bodyParagraph};
        core::NodeId ownerId;
        std::size_t row{};
        std::size_t column{};
        std::size_t start{};
        std::size_t end{};
    };
    struct TableCellSelection {
        core::NodeId tableId;
        std::size_t anchorRow{};
        std::size_t anchorColumn{};
        std::size_t focusRow{};
        std::size_t focusColumn{};

        auto operator<=>(const TableCellSelection&) const = default;
    };
    struct LineAffinity {
        core::NodeId paragraphId;
        int textStart{};
    };
    struct CursorState {
        core::Range selection;
        core::CharacterFormat typingFormat;
        // Direct-property intent is distinct from the effective insertion
        // format. In particular, an explicit black/false/clear can equal the
        // current style baseline and still has to survive a later style
        // transition.
        core::CharacterFormatMask typingOverrideMask;
        std::optional<TableCursor> tableCursor;
        std::optional<std::size_t> tableSelectionAnchor;
        std::optional<TableCellSelection> tableCellSelection;
        std::optional<core::NodeId> selectedTable;
        std::optional<LineAffinity> lineAffinity;
        std::optional<double> preferredVerticalX;
        double pageWidthPoints;
        double pageHeightPoints;
        double marginTopPoints;
        double marginRightPoints;
        double marginBottomPoints;
        double marginLeftPoints;
        std::uint64_t stateId;
        std::uint64_t nonTextStateId;
        std::uint64_t pageLayoutStateId;
    };
    struct CursorHistoryEntry {
        CursorState before;
        CursorState after;
        bool documentTransaction;
    };

    void invalidateLayout();
    void ensureLayout() const;
    void rebuildLayout() const;
    core::DocumentSnapshot visibleDocumentSnapshot() const;
    void updateScrollBars() const;
    void updateStatus();
    void revealCursor();
    void emitCursorFormat();
    CursorState captureEditorState() const;
    void restoreEditorState(const CursorState& state);
    void updateDirtyFlags();
    void synchronizeCursorHistory();
    void recordLayoutChange(const CursorState& before);
    bool insertInlineImageWithGeometry(
        std::vector<std::uint8_t> encodedBytes,
        const QString& accessibleName,
        std::optional<std::int64_t> widthEmu,
        std::optional<std::int64_t> heightEmu,
        core::ImageLayout layout = {});
    const QImage* decodedInlineImage(const core::ImageAtom& image) const;
    void reconcileDecodedImageCache();
    std::optional<std::pair<core::Position, core::ImageAtom>>
    selectedInlineImage() const;
    bool rejectLiveEditDuringPreview();
    bool insertAcademicField(core::FieldKind kind, core::NodeId target,
                             const QString& locator = {}, bool fullText = false);
    void endTypingGroup() noexcept;
    void resetVerticalNavigation() noexcept;
    void clearPendingSpellingWord() noexcept;
    void setPendingSpellingWordFromTypedText(const QString& insertedText);
    void refreshPendingSpellingWordAfterEdit();
    void commitPendingSpellingWordIfCaretLeft();
    bool suppressSpellingWord(PendingSpellingWord::Container container,
                              core::NodeId ownerId, std::size_t row,
                              std::size_t column, std::size_t start,
                              std::size_t end) const noexcept;
    bool apply(std::vector<core::Operation> operations,
               std::optional<core::Position> resultingCursor = std::nullopt,
               std::optional<core::CharacterFormat> resultingTypingFormat =
                   std::nullopt,
               bool coalesceTyping = false,
               std::optional<core::Range> resultingSelection = std::nullopt,
               bool coalesceWithPrevious = false,
               bool updateTableSelection = false,
               std::optional<TableCursor> resultingTableCursor = std::nullopt,
               std::optional<core::NodeId> resultingSelectedTable = std::nullopt,
               std::optional<LineAffinity> resultingLineAffinity = std::nullopt,
               std::optional<core::CharacterFormatMask>
                   resultingTypingOverrideMask = std::nullopt);
    bool replaceTableCellText(const QString& text, bool coalesceTyping);
    bool moveActiveTableCell(bool forward);
    bool deleteSelectedTable();
    bool clearSelectedTableCells();
    bool hasClipboardSelection() const noexcept;
    std::vector<std::pair<std::size_t, std::size_t>>
    selectedTableCells(const core::Table& table) const;
    bool tableCellIsSelected(core::NodeId tableId, std::size_t row,
                             std::size_t column) const noexcept;
    void applyCharacterFormatInternal(const core::CharacterFormatDelta& delta,
                                      bool coalesceWithPrevious);
    void applyParagraphStyleInternal(const QString& styleId,
                                     bool coalesceWithPrevious);
    void insertParagraphBreak();
    void replaceSelection(
        const QString& text, bool coalesceTyping = false,
        std::optional<std::string> resultingParagraphStyle = std::nullopt);
    bool continuePlainTextList();
    bool resequenceNumberedList(core::NodeId paragraphId,
                                bool coalesceWithPrevious,
                                const core::Document* numberingBaseline = nullptr,
                                std::set<core::NodeId>* visitedRun = nullptr);
    void togglePlainTextList(bool numbered);
    bool handlePlainTextListBackspace();
    void deleteBackward(bool byWord = false);
    void deleteForward(bool byWord = false);
    void moveHorizontal(bool forward, bool extend, bool byWord = false);
    void moveVertical(bool down, bool extend);
    Hit hitTest(const QPoint& viewportPoint) const;
    core::TableGeometry resolvedTableGeometry(core::NodeId tableId) const;
    void cancelTableResize();
    const VisualLine* visualLineForCaret() const;
    QRectF caretRectInContent() const;
    std::vector<core::NodeId> selectedParagraphIds() const;
    core::CharacterFormat currentCharacterFormat() const;
    core::CharacterFormat selectedCharacterFormat() const;
    core::CharacterFormatMask selectedCharacterOverrideMask() const;
    core::CharacterFormat activeCharacterFormat() const;
    int paragraphIndex(core::NodeId id) const;
    QString paragraphText(core::NodeId id) const;
    void setCursor(core::Position position, bool extend,
                   bool preserveVerticalNavigation = false);
    void selectRange(core::Range range, const LineAffinity& lineAffinity);
    QString wordAt(const core::Position& position, core::Range* range = nullptr) const;
    void renderPage(QPainter& painter, int pageIndex, const QPointF& origin, double scale,
                    bool decorations) const;
    struct StoryRegionHit {
        bool footer{false};
        int section{0};
        int pageIndex{0};
    };
    std::optional<StoryRegionHit> storyRegionAt(
        const QPoint& viewportPoint) const;
    std::pair<double, double> storyBodyMargins(const core::Document& document, int pages) const;
    QRectF storyRegion(bool footer, int region) const;
    void ensureStoryEditors();
    void loadStoryEditors(bool footer);
    void updateStoryEditorGeometry();
    void applyStoryEditorText();
    bool pasteStoryImage(QPlainTextEdit* editor, const QMimeData* mime,
                         const QString& accessibleNameOverride = {});
    static std::array<QString, 3> splitStorySections(const QString& text);
    static QString joinStorySections(
        const std::array<QPlainTextEdit*, 3>& editors);

    SpellChecker& spelling_;
    std::unique_ptr<core::DocumentSession> session_;
    std::vector<core::Operation>* plannedOperations_{nullptr};
    core::Range selection_;
    core::CharacterFormat typingFormat_;
    core::CharacterFormatMask typingOverrideMask_;
    bool modified_{false};
    bool nonTextModified_{false};
    bool pageLayoutModified_{false};
    bool selecting_{false};
    int zoomPercent_{100};
    int zoomWheelAngleRemainder_{0};
    int zoomWheelPixelRemainder_{0};
    double pageWidthPoints_{612.0};
    double pageHeightPoints_{792.0};
    double marginTopPoints_{72.0};
    double marginRightPoints_{72.0};
    double marginBottomPoints_{72.0};
    double marginLeftPoints_{72.0};
    QString defaultFontFamily_{QStringLiteral("Helvetica")};
    double defaultFontPointSize_{11.0};
    int tabWidthSpaces_{4};
    core::ListLayout defaultListLayout_;
    std::array<QPlainTextEdit*, 3> headerEditors_{};
    std::array<QPlainTextEdit*, 3> footerEditors_{};
    QPointer<QWidget> storyTools_;
    bool headerFooterEditing_{false};
    bool activeStoryIsFooter_{false};
    int activeStoryRegion_{0};
    int activeStoryPage_{0};
    bool loadingStoryEditors_{false};
    bool storyEditHasTransaction_{false};

    mutable core::Revision layoutRevision_;
    mutable bool layoutValid_{false};
    mutable std::vector<std::unique_ptr<NoteVisual>> noteVisuals_;
    mutable std::vector<std::unique_ptr<TocVisual>> tocVisuals_;
    mutable bool layoutIsPreview_{false};
    mutable int pageCount_{1};
    mutable double bodyMarginTopPoints_{72.0};
    mutable double bodyMarginBottomPoints_{72.0};
    mutable int storyPaginationPass_{0};
    mutable std::uint64_t layoutGeneration_{0};
    mutable std::vector<std::unique_ptr<ParagraphVisual>> visuals_;
    mutable std::vector<std::unique_ptr<TableVisual>> tableVisuals_;
    mutable std::vector<BlockPlacement> blockPlacements_;
    mutable std::map<core::NodeId, QImage> decodedImages_;
    mutable std::map<core::NodeId, core::EncodedImagePayload> decodedImagePayloads_;
    mutable std::int64_t decodedImageBytes_{0};
    std::vector<ImportedTablePresentation> importedTables_;
    std::optional<core::PreviewId> previewId_;
    core::Revision previewRevision_;
    std::optional<core::Position> previewCursor_;
    std::vector<core::Operation> previewOperations_;
    bool previewHasNonTextChanges_{false};
    std::optional<PageLayoutSettings> previewOriginalLayout_;
    std::optional<CursorState> previewOriginalCursorState_;
    PageLayoutSettings renderedPageLayout() const noexcept;
    void useRenderedPageLayout(const PageLayoutSettings& layout) noexcept;
    QTimer* typingGroupTimer_{nullptr};
    bool typingGroupActive_{false};
    bool colorAdjustmentActive_{false};
    std::optional<core::Revision> colorAdjustmentLastRevision_;
    std::optional<PendingSpellingWord> pendingSpellingWord_;
    QElapsedTimer multiClickTimer_;
    QPoint lastDoubleClickPosition_;
    bool tripleClickArmed_{false};
    std::optional<TableCursor> tableCursor_;
    std::optional<std::size_t> tableSelectionAnchor_;
    std::optional<TableCellSelection> tableCellSelection_;
    std::optional<TableCursor> tableMouseSelectionAnchor_;
    std::optional<core::NodeId> selectedTable_;
    bool draggingTable_{false};
    bool tableDropTargetValid_{false};
    std::optional<core::NodeId> tableDropBefore_;
    std::optional<ImageResizeDrag> imageResizeDrag_;
    std::optional<TableResizeDrag> tableResizeDrag_;
    std::optional<LineAffinity> lineAffinity_;
    std::optional<double> preferredVerticalX_;
    std::vector<CursorHistoryEntry> undoCursorHistory_;
    std::vector<CursorHistoryEntry> redoCursorHistory_;
    std::uint64_t currentStateId_{0};
    std::uint64_t savedStateId_{0};
    std::uint64_t nextStateId_{1};
    std::uint64_t currentNonTextStateId_{0};
    std::uint64_t savedNonTextStateId_{0};
    std::uint64_t nextNonTextStateId_{1};
    std::uint64_t currentPageLayoutStateId_{0};
    std::uint64_t savedPageLayoutStateId_{0};
    std::uint64_t nextPageLayoutStateId_{1};
};

}  // namespace docxstudio::app

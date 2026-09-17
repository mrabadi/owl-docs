#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ChatDock.h"
#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/SpellChecker.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFile>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolButton>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString textOf(const docxstudio::core::DocumentSnapshot& snapshot) {
    const auto& text = snapshot.document.paragraphs().front().text();
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

QStringList paragraphTexts(const docxstudio::core::DocumentSnapshot& snapshot) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(snapshot.document.paragraphs().size()));
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(QString::fromUtf16(
            paragraph.text().data(), static_cast<qsizetype>(paragraph.text().size())));
    }
    return result;
}

void sendKey(docxstudio::app::DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

docxstudio::core::Document spacedDocument(docxstudio::core::LineSpacingRule rule,
                                          std::int64_t spacingPoints) {
    std::vector<docxstudio::core::Paragraph> paragraphs;
    paragraphs.reserve(100);
    for (int index = 0; index < 100; ++index) {
        auto paragraph = docxstudio::core::Paragraph::create(u"Line");
        check(static_cast<bool>(paragraph), "could not create line-spacing paragraph");
        paragraphs.push_back(std::move(paragraph.value()));
    }

    auto created = docxstudio::core::Document::create(std::move(paragraphs));
    check(static_cast<bool>(created), "could not create line-spacing document");
    auto document = std::move(created.value());

    std::vector<docxstudio::core::NodeId> paragraphIds;
    paragraphIds.reserve(document.paragraphs().size());
    docxstudio::core::CharacterFormatDelta characterFormat;
    characterFormat.font_size_half_points =
        docxstudio::core::PropertyDelta<std::int32_t>::set(48);
    for (const auto& paragraph : document.paragraphs()) {
        paragraphIds.push_back(paragraph.id());
        const auto applied = document.applyCharacterFormat(
            {{paragraph.id(), 0}, {paragraph.id(), paragraph.text().size()}},
            characterFormat);
        check(static_cast<bool>(applied), "could not apply line-spacing test font");
    }

    docxstudio::core::ParagraphFormatDelta paragraphFormat;
    paragraphFormat.line_spacing_emu =
        docxstudio::core::PropertyDelta<std::int64_t>::set(spacingPoints * 12700);
    paragraphFormat.line_spacing_rule =
        docxstudio::core::PropertyDelta<docxstudio::core::LineSpacingRule>::set(rule);
    const auto applied = document.applyParagraphFormat(paragraphIds, paragraphFormat);
    check(static_cast<bool>(applied), "could not apply line-spacing test format");
    return document;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);

    canvas.insertText(QStringLiteral("Helo world"));
    check(textOf(canvas.snapshot()) == QStringLiteral("Helo world"),
          "typing did not update the semantic document");

    canvas.selectAll();
    QString summary;
    QString error;
    check(canvas.createReplacementPreview(QStringLiteral("Hello world"), summary, error),
          "could not create isolated preview");
    check(textOf(canvas.snapshot()) == QStringLiteral("Helo world"),
          "preview changed the live document");
    check(canvas.acceptPreview(error), "could not accept preview");
    check(textOf(canvas.snapshot()) == QStringLiteral("Hello world"),
          "accepted preview did not update the document");
    canvas.undo();
    check(textOf(canvas.snapshot()) == QStringLiteral("Helo world"),
          "accepted preview was not one undo transaction");

    {
        docxstudio::app::DocumentCanvas styled(spelling);
        styled.insertText(QStringLiteral("Quarterly results"));
        styled.applyParagraphStyle(QStringLiteral("Heading1"));
        auto snapshot = styled.snapshot();
        check(snapshot.document.paragraphs().front().styleId() ==
                  std::optional<std::string>{"Heading1"},
              "Heading 1 did not set native paragraph-style identity");
        const auto headingFormat =
            snapshot.document.paragraphs().front().characterFormatAt(1);
        check(headingFormat.font_size_half_points == 32 &&
                  headingFormat.bold == true &&
                  headingFormat.foreground_argb == 0xffe95420U,
              "Heading 1 did not apply its visible catalog baseline");
        check(snapshot.document.paragraphs().front().format().keep_with_next ==
                  true &&
                  snapshot.document.paragraphs().front().format()
                          .space_before_emu == 12 * 12700,
              "Heading 1 did not apply its paragraph baseline");

        const auto beforeEnterRevision = snapshot.revision;
        int enterChangeSignals = 0;
        qulonglong enterSignalRevision = 0;
        const auto enterChangeConnection = QObject::connect(
            &styled, &docxstudio::app::DocumentCanvas::documentChanged,
            [&](qulonglong revision) {
                ++enterChangeSignals;
                enterSignalRevision = revision;
            });
        sendKey(styled, Qt::Key_Return);
        QObject::disconnect(enterChangeConnection);
        snapshot = styled.snapshot();
        check(snapshot.revision.value() == beforeEnterRevision.value() + 1,
              "heading Enter used more than one live document revision");
        check(enterChangeSignals == 1 &&
                  enterSignalRevision == snapshot.revision.value(),
              "heading Enter emitted an intermediate documentChanged state");
        check(snapshot.document.paragraphs().size() == 2 &&
                  snapshot.document.paragraphs()[0].styleId() ==
                      std::optional<std::string>{"Heading1"} &&
                  snapshot.document.paragraphs()[1].styleId() ==
                      std::optional<std::string>{"Normal"},
              "Enter after a heading did not use its native next style");
        const auto nextMark = snapshot.document.paragraphs()[1]
                                  .paragraphMarkCharacterFormat();
        check(nextMark.font_size_half_points == 22 &&
                  nextMark.bold == false &&
                  nextMark.foreground_argb == 0xff000000U,
              "next-style paragraph retained heading insertion formatting");
        styled.insertText(QStringLiteral("Body"));
        const auto bodyFormat = styled.snapshot().document.paragraphs()[1]
                                    .characterFormatAt(1);
        check(bodyFormat.font_size_half_points == 22 &&
                  bodyFormat.bold == false &&
                  bodyFormat.foreground_argb == 0xff000000U,
              "typing in the next-style paragraph lost its Normal baseline");
        styled.undo();
        check(styled.snapshot().document.paragraphs()[1].text().empty(),
              "typing after a heading could not be undone independently");
        styled.undo();
        check(styled.snapshot().document.paragraphs().size() == 1 &&
                  styled.snapshot().document.paragraphs().front().styleId() ==
                      std::optional<std::string>{"Heading1"},
              "paragraph break plus next-style transition was not one undo transaction");

        sendKey(styled, Qt::Key_Return, Qt::ShiftModifier);
        snapshot = styled.snapshot();
        check(snapshot.document.paragraphs().size() == 1 &&
                  snapshot.document.paragraphs().front().styleId() ==
                      std::optional<std::string>{"Heading1"} &&
                  snapshot.document.paragraphs().front().text().back() ==
                      QChar::LineSeparator,
              "Shift+Enter changed paragraph identity or created a new style");
    }

    {
        docxstudio::app::DocumentCanvas styledTyping(spelling);
        styledTyping.insertText(QStringLiteral("Styled heading"));
        styledTyping.applyParagraphStyle(QStringLiteral("Heading1"));
        const auto beforeEnter = styledTyping.snapshot();
        styledTyping.setForeground(QColor(QStringLiteral("#cc0000")));
        check(styledTyping.snapshot().revision == beforeEnter.revision,
              "transient heading color unexpectedly changed the document");

        sendKey(styledTyping, Qt::Key_Return);
        auto snapshot = styledTyping.snapshot();
        check(snapshot.revision.value() == beforeEnter.revision.value() + 1 &&
                  snapshot.document.paragraphs().size() == 2 &&
                  snapshot.document.paragraphs()[1].styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  snapshot.document.paragraphs()[1]
                          .paragraphMarkCharacterFormat().foreground_argb ==
                      0xffcc0000U &&
                  snapshot.document.paragraphs()[1].styleProvenance() &&
                  snapshot.document.paragraphs()[1].styleProvenance()
                          ->paragraph_mark_overrides.foreground_argb,
              "heading Enter erased a transient direct typing color");

        styledTyping.undo();
        snapshot = styledTyping.snapshot();
        check(snapshot.document.paragraphs().size() == 1 &&
                  snapshot.document.paragraphs().front().styleId() ==
                      std::optional<std::string>{"Heading1"} &&
                  snapshot.document.paragraphs().front().text() ==
                      u"Styled heading",
              "undo did not atomically restore the heading before Enter");
        styledTyping.redo();
        snapshot = styledTyping.snapshot();
        check(snapshot.document.paragraphs().size() == 2 &&
                  snapshot.document.paragraphs()[1]
                          .paragraphMarkCharacterFormat().foreground_argb ==
                      0xffcc0000U &&
                  snapshot.document.paragraphs()[1].styleProvenance()
                          ->paragraph_mark_overrides.foreground_argb,
              "redo did not restore the styled paragraph break atomically");

        styledTyping.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = styledTyping.snapshot();
        check(snapshot.document.paragraphs()[1].styleId() ==
                  std::optional<std::string>{"Heading2"} &&
                  snapshot.document.paragraphs()[1]
                          .paragraphMarkCharacterFormat().foreground_argb ==
                      0xffcc0000U &&
                  snapshot.document.paragraphs()[1].styleProvenance()
                          ->paragraph_mark_overrides.foreground_argb,
              "a later style transition erased the direct typing color");
    }

    {
        docxstudio::app::DocumentCanvas selectedHeading(spelling);
        selectedHeading.insertText(QStringLiteral("Heading replacement"));
        selectedHeading.applyParagraphStyle(QStringLiteral("Heading1"));
        sendKey(selectedHeading, Qt::Key_Left,
                Qt::ControlModifier | Qt::ShiftModifier);
        const auto beforeEnter = selectedHeading.snapshot();
        int changeSignals = 0;
        const auto changeConnection = QObject::connect(
            &selectedHeading,
            &docxstudio::app::DocumentCanvas::documentChanged,
            [&](qulonglong) { ++changeSignals; });
        sendKey(selectedHeading, Qt::Key_Return);
        QObject::disconnect(changeConnection);

        const auto afterEnter = selectedHeading.snapshot();
        check(afterEnter.revision.value() == beforeEnter.revision.value() + 1 &&
                  changeSignals == 1,
              "heading Enter with a replacement selection was not atomic");
        check(paragraphTexts(afterEnter) ==
                  QStringList{QStringLiteral("Heading "), QString{}},
              "heading Enter did not replace the selection before splitting");
        check(afterEnter.document.paragraphs()[0].styleId() ==
                  std::optional<std::string>{"Heading1"} &&
                  afterEnter.document.paragraphs()[1].styleId() ==
                  std::optional<std::string>{"Normal"},
              "heading Enter lost style identity around a replacement");
        selectedHeading.undo();
        const auto restored = selectedHeading.snapshot();
        check(restored.document.paragraphs().size() == 1 &&
                  restored.document.paragraphs().front().text() ==
                      u"Heading replacement" &&
                  restored.document.paragraphs().front().styleId() ==
                      std::optional<std::string>{"Heading1"},
              "one Undo did not restore an atomic heading replacement");
    }

    {
        docxstudio::app::DocumentCanvas directOverride(spelling);
        directOverride.insertText(QStringLiteral("Important"));
        directOverride.selectAll();
        directOverride.setForeground(QColor(QStringLiteral("#cc0000")));
        directOverride.applyParagraphStyle(QStringLiteral("Heading2"));
        const auto paragraph =
            directOverride.snapshot().document.paragraphs().front();
        const auto format = paragraph.characterFormatAt(1);
        check(paragraph.styleId() ==
                  std::optional<std::string>{"Heading2"} &&
                  format.font_size_half_points == 26 &&
                  format.bold == true &&
                  format.foreground_argb == 0xffcc0000U,
              "style application erased a direct text-color override");
        directOverride.applyParagraphStyle(QStringLiteral("Normal"));
        const auto normalized =
            directOverride.snapshot().document.paragraphs().front();
        check(normalized.styleId() == std::optional<std::string>{"Normal"} &&
                  normalized.characterFormatAt(1).font_size_half_points == 22 &&
                  normalized.characterFormatAt(1).bold == false &&
                  normalized.characterFormatAt(1).foreground_argb ==
                      0xffcc0000U,
              "returning to Normal did not retain only the direct override");
    }

    {
        using namespace docxstudio::core;
        CharacterFormatDelta exactDefaults;
        exactDefaults.foreground_argb =
            PropertyDelta<std::uint32_t>::set(0xff000000U);
        exactDefaults.bold = PropertyDelta<bool>::set(false);
        exactDefaults.highlight_argb =
            PropertyDelta<std::uint32_t>::clear();

        docxstudio::app::DocumentCanvas selectedExact(spelling);
        selectedExact.insertText(QStringLiteral("Selected"));
        selectedExact.selectAll();
        const auto beforeFormat = selectedExact.snapshot();
        selectedExact.applyCharacterFormat(exactDefaults);
        auto snapshot = selectedExact.snapshot();
        const auto& formatted = snapshot.document.paragraphs().front();
        check(snapshot.revision.value() == beforeFormat.revision.value() + 1 &&
                  formatted.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  formatted.styleProvenance() &&
                  formatted.characterFormatAt(1).foreground_argb ==
                      0xff000000U &&
                  formatted.characterFormatAt(1).bold == false &&
                  formatted.styleOverrideMaskAt(1).foreground_argb &&
                  formatted.styleOverrideMaskAt(1).bold &&
                  formatted.styleOverrideMaskAt(1).highlight_argb,
              "equal-value selected formatting lacks exact Normal provenance");
        selectedExact.undo();
        snapshot = selectedExact.snapshot();
        check(!snapshot.document.paragraphs().front().styleId() &&
                  !snapshot.document.paragraphs().front().styleProvenance(),
              "one Undo did not remove lazy selected-format provenance");
        selectedExact.redo();
        selectedExact.applyParagraphStyle(QStringLiteral("Heading1"));
        selectedExact.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = selectedExact.snapshot();
        const auto& transitioned = snapshot.document.paragraphs().front();
        check(transitioned.characterFormatAt(1).foreground_argb ==
                  0xff000000U &&
                  transitioned.characterFormatAt(1).bold == false &&
                  transitioned.styleOverrideMaskAt(1).foreground_argb &&
                  transitioned.styleOverrideMaskAt(1).bold &&
                  transitioned.styleOverrideMaskAt(1).highlight_argb,
              "two style transitions erased equal-value selected overrides");

        docxstudio::app::DocumentCanvas emptyExact(spelling);
        const auto beforeEmptyFormat = emptyExact.snapshot();
        emptyExact.applyCharacterFormat(exactDefaults);
        snapshot = emptyExact.snapshot();
        const auto& emptyParagraph = snapshot.document.paragraphs().front();
        check(snapshot.revision.value() ==
                  beforeEmptyFormat.revision.value() + 1 &&
                  emptyParagraph.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  emptyParagraph.styleProvenance() &&
                  emptyParagraph.paragraphMarkCharacterFormat()
                          .foreground_argb == 0xff000000U &&
                  emptyParagraph.paragraphMarkCharacterFormat().bold == false &&
                  emptyParagraph.styleProvenance()
                          ->paragraph_mark_overrides.foreground_argb &&
                  emptyParagraph.styleProvenance()
                          ->paragraph_mark_overrides.bold &&
                  emptyParagraph.styleProvenance()
                          ->paragraph_mark_overrides.highlight_argb,
              "equal-value empty-mark formatting lacks exact provenance");
        emptyExact.undo();
        check(!emptyExact.snapshot().document.paragraphs().front().styleId() &&
                  !emptyExact.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "one Undo did not remove lazy empty-mark provenance");
        emptyExact.redo();
        emptyExact.applyParagraphStyle(QStringLiteral("Heading1"));
        emptyExact.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = emptyExact.snapshot();
        const auto& transitionedMark = snapshot.document.paragraphs().front();
        check(transitionedMark.paragraphMarkCharacterFormat()
                      .foreground_argb == 0xff000000U &&
                  transitionedMark.paragraphMarkCharacterFormat().bold ==
                      false &&
                  transitionedMark.styleProvenance()
                      ->paragraph_mark_overrides.foreground_argb &&
                  transitionedMark.styleProvenance()
                      ->paragraph_mark_overrides.bold &&
                  transitionedMark.styleProvenance()
                      ->paragraph_mark_overrides.highlight_argb,
              "two style transitions erased equal-value mark overrides");

        docxstudio::app::DocumentCanvas paragraphExact(spelling);
        paragraphExact.insertText(QStringLiteral("Paragraph"));
        ParagraphFormatDelta exactParagraphDefaults;
        exactParagraphDefaults.alignment =
            PropertyDelta<ParagraphAlignment>::set(
                ParagraphAlignment::left);
        exactParagraphDefaults.keep_with_next =
            PropertyDelta<bool>::set(false);
        const auto beforeParagraphFormat = paragraphExact.snapshot();
        paragraphExact.applyParagraphFormat(exactParagraphDefaults);
        snapshot = paragraphExact.snapshot();
        const auto& directParagraph = snapshot.document.paragraphs().front();
        check(snapshot.revision.value() ==
                  beforeParagraphFormat.revision.value() + 1 &&
                  directParagraph.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  directParagraph.styleProvenance() &&
                  directParagraph.styleProvenance()
                          ->paragraph_overrides.alignment &&
                  directParagraph.styleProvenance()
                          ->paragraph_overrides.keep_with_next,
              "equal-value paragraph formatting lacks exact provenance");
        paragraphExact.undo();
        check(!paragraphExact.snapshot().document.paragraphs().front()
                       .styleId() &&
                  !paragraphExact.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "one Undo did not remove lazy paragraph-format provenance");
        paragraphExact.redo();
        paragraphExact.applyParagraphStyle(QStringLiteral("Heading1"));
        paragraphExact.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = paragraphExact.snapshot();
        const auto& transitionedParagraph =
            snapshot.document.paragraphs().front();
        check(transitionedParagraph.format().alignment ==
                  ParagraphAlignment::left &&
                  transitionedParagraph.format().keep_with_next == false &&
                  transitionedParagraph.styleProvenance()
                      ->paragraph_overrides.alignment &&
                  transitionedParagraph.styleProvenance()
                      ->paragraph_overrides.keep_with_next,
              "two style transitions erased equal-value paragraph overrides");
    }

    {
        using namespace docxstudio::core;
        CharacterFormatDelta exactTyping;
        exactTyping.foreground_argb =
            PropertyDelta<std::uint32_t>::set(0xff000000U);
        exactTyping.bold = PropertyDelta<bool>::set(false);
        exactTyping.highlight_argb =
            PropertyDelta<std::uint32_t>::clear();

        docxstudio::app::DocumentCanvas collapsedExact(spelling);
        collapsedExact.insertText(QStringLiteral("A"));
        const auto beforeTransient = collapsedExact.snapshot();
        collapsedExact.applyCharacterFormat(exactTyping);
        check(collapsedExact.snapshot().revision == beforeTransient.revision &&
                  !collapsedExact.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "collapsed typing choice created an invisible undo item");
        collapsedExact.insertText(QStringLiteral("B"));
        auto snapshot = collapsedExact.snapshot();
        const auto& inserted = snapshot.document.paragraphs().front();
        check(snapshot.revision.value() ==
                  beforeTransient.revision.value() + 1 &&
                  inserted.text() == u"AB" && inserted.styleProvenance() &&
                  inserted.styleOverrideMaskAt(2).foreground_argb &&
                  inserted.styleOverrideMaskAt(2).bold &&
                  inserted.styleOverrideMaskAt(2).highlight_argb,
              "collapsed equal-value typing intent was not attached atomically");
        collapsedExact.undo();
        snapshot = collapsedExact.snapshot();
        check(snapshot.document.paragraphs().front().text() == u"A" &&
                  !snapshot.document.paragraphs().front().styleId() &&
                  !snapshot.document.paragraphs().front().styleProvenance(),
              "one Undo did not restore the pre-typing implicit paragraph");
        collapsedExact.redo();
        collapsedExact.applyParagraphStyle(QStringLiteral("Heading1"));
        collapsedExact.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = collapsedExact.snapshot();
        const auto& transitioned = snapshot.document.paragraphs().front();
        check(transitioned.characterFormatAt(2).foreground_argb ==
                  0xff000000U &&
                  transitioned.characterFormatAt(2).bold == false &&
                  transitioned.styleOverrideMaskAt(2).foreground_argb &&
                  transitioned.styleOverrideMaskAt(2).bold &&
                  transitioned.styleOverrideMaskAt(2).highlight_argb,
              "style transitions erased collapsed equal-value typing intent");

        docxstudio::app::DocumentCanvas pastedExact(spelling);
        pastedExact.insertText(QStringLiteral("replace"));
        pastedExact.selectAll();
        pastedExact.applyCharacterFormat(exactTyping);
        QApplication::clipboard()->setText(QStringLiteral("pasted"));
        pastedExact.pasteTextOnly();
        auto pasted = pastedExact.snapshot();
        const auto& pastedParagraph = pasted.document.paragraphs().front();
        check(pastedParagraph.text() == u"pasted" &&
                  pastedParagraph.styleOverrideMaskAt(1).foreground_argb &&
                  pastedParagraph.styleOverrideMaskAt(1).bold &&
                  pastedParagraph.styleOverrideMaskAt(1).highlight_argb,
              "text-only replacement paste lost exact typing overrides");
        pastedExact.applyParagraphStyle(QStringLiteral("Heading1"));
        pastedExact.applyParagraphStyle(QStringLiteral("Heading2"));
        pasted = pastedExact.snapshot();
        check(pasted.document.paragraphs().front()
                      .characterFormatAt(1).foreground_argb == 0xff000000U &&
                  pasted.document.paragraphs().front()
                      .characterFormatAt(1).bold == false,
              "style transitions erased text-only paste overrides");

        docxstudio::app::DocumentCanvas previewInsertion(spelling);
        previewInsertion.insertText(QStringLiteral("A"));
        const auto beforePreviewInsertion = previewInsertion.snapshot();
        previewInsertion.applyCharacterFormat(exactTyping);
        check(previewInsertion.snapshot().revision ==
                  beforePreviewInsertion.revision &&
                  !previewInsertion.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "collapsed preview typing choice created an invisible edit");
        QString previewSummary;
        QString previewError;
        int previewChangeSignals = 0;
        const auto previewChangeConnection = QObject::connect(
            &previewInsertion,
            &docxstudio::app::DocumentCanvas::documentChanged,
            [&](qulonglong) { ++previewChangeSignals; });
        check(previewInsertion.createReplacementPreview(
                  QStringLiteral("B"), previewSummary, previewError),
              "could not create provenance-aware insertion preview");
        const auto duringPreviewInsertion = previewInsertion.snapshot();
        check(duringPreviewInsertion.revision ==
                  beforePreviewInsertion.revision &&
                  duringPreviewInsertion.document ==
                      beforePreviewInsertion.document &&
                  previewChangeSignals == 0,
              "creating an insertion preview changed the live revision");
        check(previewInsertion.acceptPreview(previewError),
              "could not accept provenance-aware insertion preview");
        QObject::disconnect(previewChangeConnection);
        snapshot = previewInsertion.snapshot();
        const auto& previewInserted =
            snapshot.document.paragraphs().front();
        check(snapshot.revision.value() ==
                  beforePreviewInsertion.revision.value() + 1 &&
                  previewChangeSignals == 1 && previewInserted.text() == u"AB" &&
                  previewInserted.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  previewInserted.styleProvenance() &&
                  previewInserted.styleOverrideMaskAt(2).foreground_argb &&
                  previewInserted.styleOverrideMaskAt(2).bold &&
                  previewInserted.styleOverrideMaskAt(2).highlight_argb,
              "accepted insertion preview lost atomic Normal provenance");
        previewInsertion.undo();
        snapshot = previewInsertion.snapshot();
        check(snapshot.document.paragraphs().front().text() == u"A" &&
                  !snapshot.document.paragraphs().front().styleId() &&
                  !snapshot.document.paragraphs().front().styleProvenance(),
              "one Undo did not remove the preview and lazy provenance");
        previewError.clear();
        check(!previewInsertion.createReplacementPreview(
                  QString{}, previewSummary, previewError) &&
                  !previewInsertion.hasPreview() &&
                  !previewInsertion.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "an empty insertion preview materialized typing provenance");

        docxstudio::app::DocumentCanvas previewReplacement(spelling);
        previewReplacement.insertText(QStringLiteral("replace"));
        previewReplacement.selectAll();
        previewReplacement.applyCharacterFormat(exactTyping);
        const auto beforePreviewReplacement = previewReplacement.snapshot();
        check(previewReplacement.createReplacementPreview(
                  QStringLiteral("preview"), previewSummary, previewError),
              "could not create exact-format replacement preview");
        const auto duringPreviewReplacement = previewReplacement.snapshot();
        check(duringPreviewReplacement.revision ==
                  beforePreviewReplacement.revision &&
                  duringPreviewReplacement.document ==
                      beforePreviewReplacement.document,
              "replacement preview changed its live source document");
        check(previewReplacement.acceptPreview(previewError),
              "could not accept exact-format replacement preview");
        snapshot = previewReplacement.snapshot();
        const auto& previewReplaced =
            snapshot.document.paragraphs().front();
        check(snapshot.revision.value() ==
                  beforePreviewReplacement.revision.value() + 1 &&
                  previewReplaced.text() == u"preview" &&
                  previewReplaced.styleOverrideMaskAt(1).foreground_argb &&
                  previewReplaced.styleOverrideMaskAt(1).bold &&
                  previewReplaced.styleOverrideMaskAt(1).highlight_argb,
              "replacement preview discarded equal-value direct formatting");
        previewReplacement.undo();
        snapshot = previewReplacement.snapshot();
        check(snapshot.document == beforePreviewReplacement.document,
              "one Undo did not restore the pre-preview formatted selection");
    }

    {
        using namespace docxstudio::core;
        const auto* headingOne = findBuiltInParagraphStyle("Heading1");
        const auto* headingTwo = findBuiltInParagraphStyle("Heading2");
        check(headingOne && headingTwo,
              "paragraph-style merge fixture lacks built-in styles");
        const auto firstId = NodeId::generate();
        const auto secondId = NodeId::generate();
        auto first = Paragraph::create(
            u"ABC", firstId, headingOne->character_format,
            std::string{"Heading1"});
        auto second = Paragraph::create(
            u"xyz", secondId, headingTwo->character_format,
            std::string{"Heading2"});
        check(first && second,
              "could not create imported-style merge fixture");
        auto created = Document::create({first.value(), second.value()});
        check(static_cast<bool>(created),
              "could not create imported-style merge document");
        auto document = std::move(created.value());
        check(static_cast<bool>(document.applyCharacterFormat(
                  {{firstId, 0}, {firstId, 3}},
                  headingOne->characterBaselineDelta())) &&
                  static_cast<bool>(document.applyCharacterFormat(
                      {{secondId, 0}, {secondId, 3}},
                      headingTwo->characterBaselineDelta())),
              "could not resolve imported style baselines");
        ParagraphStyleProvenance firstProvenance;
        firstProvenance.inherited_character_format =
            headingOne->character_format;
        firstProvenance.inherited_paragraph_mark_character_format =
            headingOne->character_format;
        firstProvenance.inherited_paragraph_format =
            headingOne->paragraph_format;
        ParagraphStyleProvenance secondProvenance;
        secondProvenance.inherited_character_format =
            headingTwo->character_format;
        secondProvenance.inherited_paragraph_mark_character_format =
            headingTwo->character_format;
        secondProvenance.inherited_paragraph_format =
            headingTwo->paragraph_format;
        check(static_cast<bool>(document.setParagraphStyleProvenance(
                  firstId, firstProvenance)) &&
                  static_cast<bool>(document.setParagraphStyleProvenance(
                      secondId, secondProvenance)),
              "could not attach imported style provenance");
        CharacterFormatDelta explicitBold;
        explicitBold.bold = PropertyDelta<bool>::set(true);
        check(static_cast<bool>(document.applyCharacterFormat(
                  {{secondId, 1}, {secondId, 2}}, explicitBold)),
              "could not add equal-by-value direct override");
        CharacterFormatDelta explicitOrange;
        explicitOrange.foreground_argb =
            PropertyDelta<std::uint32_t>::set(0xffe95420U);
        check(static_cast<bool>(document.applyCharacterFormat(
                  {{secondId, 2}, {secondId, 3}}, explicitOrange)),
              "could not add direct color override");
        check(static_cast<bool>(document.mergeWithNext(firstId)),
              "could not merge differently styled paragraphs");

        docxstudio::app::DocumentCanvas mergedStyles(spelling);
        mergedStyles.setDocument(std::move(document));
        mergedStyles.selectAll();
        mergedStyles.applyParagraphStyle(QStringLiteral("Normal"));
        auto paragraph =
            mergedStyles.snapshot().document.paragraphs().front();
        check(paragraph.styleId() ==
                  std::optional<std::string>{"Normal"} &&
                  paragraph.styleProvenance().has_value() &&
                  paragraph.styleProvenance()
                          ->inherited_character_format.font_size_half_points ==
                      22,
              "style transition did not attach the target provenance");
        check(paragraph.characterFormatAt(1).font_size_half_points == 22 &&
                  paragraph.characterFormatAt(1).bold == false &&
                  paragraph.characterFormatAt(1).foreground_argb ==
                      0xff000000U,
              "merged first-style text did not rebase to Normal");
        check(paragraph.characterFormatAt(4).font_size_half_points == 26 &&
                  paragraph.characterFormatAt(4).bold == false &&
                  paragraph.characterFormatAt(4).foreground_argb ==
                      0xff77216fU,
              "removed paragraph's inherited appearance was not preserved");
        check(paragraph.characterFormatAt(5).bold == true &&
                  paragraph.styleOverrideMaskAt(5).bold,
              "equal-by-value direct override was not preserved after merge");
        check(paragraph.characterFormatAt(6).foreground_argb ==
                      0xffe95420U &&
                  paragraph.styleOverrideMaskAt(6).foreground_argb,
              "direct value matching the retained style was not preserved");

        // Heading 2's baseline bold value equals the explicit bold override.
        // The override must remain distinguishable so the following Normal
        // transition cannot clear it.
        mergedStyles.applyParagraphStyle(QStringLiteral("Heading2"));
        paragraph = mergedStyles.snapshot().document.paragraphs().front();
        check(paragraph.styleProvenance().has_value() &&
                  paragraph.styleOverrideMaskAt(5).bold &&
                  paragraph.characterFormatAt(5).bold == true,
              "equal-by-value override lost directness at target baseline");
        mergedStyles.applyParagraphStyle(QStringLiteral("Normal"));
        paragraph = mergedStyles.snapshot().document.paragraphs().front();
        check(paragraph.characterFormatAt(5).bold == true &&
                  paragraph.styleOverrideMaskAt(5).bold &&
                  paragraph.characterFormatAt(6).foreground_argb ==
                      0xffe95420U,
              "second style transition erased preserved direct formatting");
    }

    {
        using namespace docxstudio::core;
        CharacterFormat importedBaseline;
        importedBaseline.font_size_half_points = 32;
        importedBaseline.bold = true;
        importedBaseline.foreground_argb = 0xffe95420U;
        const auto paragraphId = NodeId::generate();
        auto imported = Paragraph::create(
            u"Body", paragraphId, importedBaseline,
            std::string{"Heading1"});
        check(static_cast<bool>(imported),
              "could not create imported typing fixture");
        auto created = Document::create({imported.value()});
        check(static_cast<bool>(created),
              "could not create imported typing document");
        auto document = std::move(created.value());
        CharacterFormatDelta baselineDelta;
        baselineDelta.font_size_half_points =
            PropertyDelta<std::int32_t>::set(32);
        baselineDelta.bold = PropertyDelta<bool>::set(true);
        baselineDelta.foreground_argb =
            PropertyDelta<std::uint32_t>::set(0xffe95420U);
        check(static_cast<bool>(document.applyCharacterFormat(
                  {{paragraphId, 0}, {paragraphId, 4}}, baselineDelta)),
              "could not resolve imported typing baseline");
        ParagraphStyleProvenance provenance;
        provenance.inherited_character_format = importedBaseline;
        provenance.inherited_paragraph_mark_character_format =
            importedBaseline;
        provenance.inherited_paragraph_format =
            findBuiltInParagraphStyle("Heading1")->paragraph_format;
        check(static_cast<bool>(document.setParagraphStyleProvenance(
                  paragraphId, provenance)),
              "could not attach imported typing provenance");

        docxstudio::app::DocumentCanvas typedOverride(spelling);
        typedOverride.setDocument(std::move(document));
        typedOverride.setForeground(QColor(QStringLiteral("#b00020")));
        typedOverride.insertText(QStringLiteral("X"));
        auto paragraph =
            typedOverride.snapshot().document.paragraphs().front();
        check(paragraph.text() == u"XBody" &&
                  paragraph.characterFormatAt(1).foreground_argb ==
                      0xffb00020U &&
                  paragraph.styleOverrideMaskAt(1).foreground_argb,
              "typed direct format lacked imported-style override provenance");
        typedOverride.applyParagraphStyle(QStringLiteral("Heading2"));
        typedOverride.applyParagraphStyle(QStringLiteral("Normal"));
        paragraph = typedOverride.snapshot().document.paragraphs().front();
        check(paragraph.characterFormatAt(1).foreground_argb ==
                      0xffb00020U &&
                  paragraph.styleOverrideMaskAt(1).foreground_argb,
              "typed override did not survive successive style transitions");
    }

    {
        using namespace docxstudio::core;
        const auto* headingOne = findBuiltInParagraphStyle("Heading1");
        const auto* headingTwo = findBuiltInParagraphStyle("Heading2");
        check(headingOne && headingTwo,
              "empty-mark style fixture lacks built-in styles");
        const auto retainedId = NodeId::generate();
        const auto removedId = NodeId::generate();
        auto retained = Paragraph::create(
            u"", retainedId, {}, std::string{"Heading1"});
        auto removed = Paragraph::create(
            u"x", removedId, headingTwo->character_format,
            std::string{"Heading2"});
        check(retained && removed,
              "could not create empty-mark style fixture");
        auto created = Document::create(
            {retained.value(), removed.value()});
        check(static_cast<bool>(created),
              "could not create empty-mark style document");
        auto document = std::move(created.value());
        check(static_cast<bool>(document.applyCharacterFormat(
                  {{removedId, 0}, {removedId, 1}},
                  headingTwo->characterBaselineDelta())),
              "could not resolve removed paragraph style");
        ParagraphStyleProvenance retainedProvenance;
        retainedProvenance.inherited_character_format =
            headingOne->character_format;
        retainedProvenance.inherited_paragraph_mark_character_format =
            headingOne->character_format;
        retainedProvenance.inherited_paragraph_format =
            headingOne->paragraph_format;
        ParagraphStyleProvenance removedProvenance;
        removedProvenance.inherited_character_format =
            headingTwo->character_format;
        removedProvenance.inherited_paragraph_mark_character_format =
            headingTwo->character_format;
        removedProvenance.inherited_paragraph_format =
            headingTwo->paragraph_format;
        CharacterFormatMask explicitEqualBold;
        explicitEqualBold.bold = true;
        removedProvenance.character_overrides = {
            {0, 1, explicitEqualBold}};
        check(static_cast<bool>(document.setParagraphStyleProvenance(
                  retainedId, retainedProvenance)) &&
                  static_cast<bool>(document.setParagraphStyleProvenance(
                      removedId, removedProvenance)) &&
                  static_cast<bool>(document.deleteRange(
                      {{retainedId, 0}, {removedId, 1}})),
              "could not create cross-paragraph empty survivor");

        docxstudio::app::DocumentCanvas emptyMark(spelling);
        emptyMark.setDocument(std::move(document));
        emptyMark.applyParagraphStyle(QStringLiteral("Normal"));
        emptyMark.applyParagraphStyle(QStringLiteral("Heading2"));
        emptyMark.applyParagraphStyle(QStringLiteral("Normal"));
        auto paragraph = emptyMark.snapshot().document.paragraphs().front();
        check(paragraph.text().empty() &&
                  paragraph.paragraphMarkCharacterFormat()
                          .font_size_half_points == 26 &&
                  paragraph.paragraphMarkCharacterFormat().bold == true &&
                  paragraph.paragraphMarkCharacterFormat()
                          .foreground_argb == 0xff77216fU &&
                  paragraph.styleProvenance()
                      ->paragraph_mark_overrides.bold &&
                  paragraph.styleProvenance()
                      ->paragraph_mark_overrides.font_size_half_points &&
                  paragraph.styleProvenance()
                      ->paragraph_mark_overrides.foreground_argb,
              "empty survivor lost rebased mark provenance across styles");
        emptyMark.insertText(QStringLiteral("x"));
        paragraph = emptyMark.snapshot().document.paragraphs().front();
        check(paragraph.characterFormatAt(1).font_size_half_points == 26 &&
                  paragraph.characterFormatAt(1).bold == true &&
                  paragraph.characterFormatAt(1).foreground_argb ==
                      0xff77216fU,
              "typing after an empty-mark transition lost its formatting");
    }

    {
        docxstudio::app::DocumentCanvas customizedDefaults(spelling);
        customizedDefaults.setEditorDefaults(
            QStringLiteral("Liberation Serif"), 14.0, 4);
        customizedDefaults.insertText(QStringLiteral("Configured body"));
        customizedDefaults.applyParagraphStyle(QStringLiteral("Heading1"));
        customizedDefaults.applyParagraphStyle(QStringLiteral("Normal"));
        auto paragraph =
            customizedDefaults.snapshot().document.paragraphs().front();
        auto format = paragraph.characterFormatAt(1);
        check(format.font_family == "Liberation Serif" &&
                  format.font_size_half_points == 28,
              "Normal ignored the configured editor font or size");

        customizedDefaults.applyParagraphStyle(QStringLiteral("Heading2"));
        customizedDefaults.applyParagraphStyle(QStringLiteral("NoSpacing"));
        paragraph = customizedDefaults.snapshot().document.paragraphs().front();
        format = paragraph.characterFormatAt(1);
        check(format.font_family == "Liberation Serif" &&
                  format.font_size_half_points == 28,
              "No Spacing ignored the configured editor font or size");
    }

    {
        docxstudio::app::DocumentCanvas bullet(spelling);
        bullet.insertText(QStringLiteral("\u2022 first"));
        sendKey(bullet, Qt::Key_Return);
        check(paragraphTexts(bullet.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\tfirst"), QStringLiteral("\u2022\t")},
              "Enter did not continue a plain-text bullet");
    }

    {
        docxstudio::app::DocumentCanvas toolbarList(spelling);
        toolbarList.insertText(QStringLiteral("first"));
        toolbarList.toggleBullets();
        check(paragraphTexts(toolbarList.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\tfirst")},
              "Bullets did not convert the current paragraph into a list item");
        sendKey(toolbarList, Qt::Key_Return);
        check(paragraphTexts(toolbarList.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\tfirst"),
                              QStringLiteral("\u2022\t")},
              "Enter did not continue a list created from the Bullets command");
        toolbarList.toggleBullets();
        check(paragraphTexts(toolbarList.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\tfirst"), QString()},
              "Bullets on an existing bullet inserted a duplicate instead of toggling it off");
        toolbarList.toggleNumbering();
        check(paragraphTexts(toolbarList.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\tfirst"),
                              QStringLiteral("1.\t")},
              "Numbering did not convert the current paragraph into a numbered item");
    }

    {
        docxstudio::app::DocumentCanvas selectedLists(spelling);
        selectedLists.insertText(QStringLiteral("alpha\nbeta"));
        selectedLists.selectAll();
        selectedLists.toggleBullets();
        check(paragraphTexts(selectedLists.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\talpha"),
                              QStringLiteral("\u2022\tbeta")},
              "Bullets did not format every selected paragraph");
        selectedLists.toggleNumbering();
        check(paragraphTexts(selectedLists.snapshot()) ==
                  QStringList{QStringLiteral("1.\talpha"),
                              QStringLiteral("2.\tbeta")},
              "Numbering did not convert and sequence selected bullet items");
        selectedLists.toggleNumbering();
        check(paragraphTexts(selectedLists.snapshot()) ==
                  QStringList{QStringLiteral("alpha"), QStringLiteral("beta")},
              "Numbering did not toggle off across selected list items");
        selectedLists.undo();
        check(paragraphTexts(selectedLists.snapshot()) ==
                  QStringList{QStringLiteral("1.\talpha"),
                              QStringLiteral("2.\tbeta")},
              "one Undo did not restore the selected numbered list transaction");
    }

    {
        docxstudio::app::DocumentCanvas numbered(spelling);
        numbered.insertText(QStringLiteral("9. ninth"));
        sendKey(numbered, Qt::Key_Return);
        check(paragraphTexts(numbered.snapshot()) ==
                  QStringList{QStringLiteral("9.\tninth"), QStringLiteral("10.\t")},
              "decimal list continuation did not increment 9 to 10");
    }

    {
        docxstudio::app::DocumentCanvas spacedNumber(spelling);
        spacedNumber.insertText(QStringLiteral("    9. nested"));
        sendKey(spacedNumber, Qt::Key_Return);
        check(paragraphTexts(spacedNumber.snapshot()) ==
                  QStringList{QStringLiteral("    I.\tnested"),
                              QStringLiteral("    J.\t")},
              "nested numbering did not normalize to uppercase alphabetic");
    }

    {
        docxstudio::app::DocumentCanvas splitNumber(spelling);
        splitNumber.insertText(QStringLiteral("9. ab"));
        sendKey(splitNumber, Qt::Key_Left);
        sendKey(splitNumber, Qt::Key_Return);
        check(paragraphTexts(splitNumber.snapshot()) ==
                  QStringList{QStringLiteral("9.\ta"), QStringLiteral("10.\tb")},
              "Enter inside a numbered item did not prefix the split-off text");
    }

    {
        docxstudio::app::DocumentCanvas replaceBulletText(spelling);
        replaceBulletText.insertText(QStringLiteral("\u2022 hello"));
        sendKey(replaceBulletText, Qt::Key_Left, Qt::ControlModifier |
                                                    Qt::ShiftModifier);
        sendKey(replaceBulletText, Qt::Key_Return);
        check(paragraphTexts(replaceBulletText.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\t"), QStringLiteral("\u2022\t")},
              "Enter after selecting bullet text did not continue the list");
    }

    {
        docxstudio::app::DocumentCanvas nestedBullet(spelling);
        nestedBullet.insertText(QStringLiteral("\u2022 child"));
        sendKey(nestedBullet, Qt::Key_Tab);
        check(textOf(nestedBullet.snapshot()) == QStringLiteral("    \u25e6\tchild"),
              "Tab did not indent and cycle a bullet marker");
        sendKey(nestedBullet, Qt::Key_Return);
        check(paragraphTexts(nestedBullet.snapshot()) ==
                  QStringList{QStringLiteral("    \u25e6\tchild"),
                              QStringLiteral("    \u25e6\t")},
              "Enter did not continue the nested bullet prefix");
        nestedBullet.insertText(QStringLiteral("grandchild"));
        sendKey(nestedBullet, Qt::Key_Tab);
        check(paragraphTexts(nestedBullet.snapshot()).back() ==
                  QStringLiteral("        \u25aa\tgrandchild"),
              "second-level Tab did not advance the bullet cycle");
        sendKey(nestedBullet, Qt::Key_Tab, Qt::ShiftModifier);
        check(paragraphTexts(nestedBullet.snapshot()).back() ==
                  QStringLiteral("    \u25e6\tgrandchild"),
              "Shift+Tab did not outdent to the preceding bullet level");
        sendKey(nestedBullet, Qt::Key_Backtab, Qt::ShiftModifier);
        check(paragraphTexts(nestedBullet.snapshot()).back() ==
                  QStringLiteral("\u2022\tgrandchild"),
              "Backtab did not outdent a bullet to its parent level");
    }

    {
        docxstudio::app::DocumentCanvas selectedBullets(spelling);
        selectedBullets.insertText(
            QStringLiteral("\u2022 alpha\n\u2022 beta"));
        selectedBullets.selectAll();
        sendKey(selectedBullets, Qt::Key_Tab);
        check(paragraphTexts(selectedBullets.snapshot()) ==
                  QStringList{QStringLiteral("    \u25e6\talpha"),
                              QStringLiteral("    \u25e6\tbeta")},
              "Tab did not indent every selected bullet as a list unit");
        sendKey(selectedBullets, Qt::Key_Backtab, Qt::ShiftModifier);
        check(paragraphTexts(selectedBullets.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\talpha"),
                              QStringLiteral("\u2022\tbeta")},
              "Shift+Tab did not outdent every selected bullet as a list unit");
        selectedBullets.undo();
        check(paragraphTexts(selectedBullets.snapshot()) ==
                  QStringList{QStringLiteral("    \u25e6\talpha"),
                              QStringLiteral("    \u25e6\tbeta")},
              "selected bullet outdent was not one undoable transaction");
    }

    {
        docxstudio::app::DocumentCanvas selectedNumbers(spelling);
        selectedNumbers.insertText(QStringLiteral("one\ntwo\nthree"));
        selectedNumbers.selectAll();
        selectedNumbers.toggleNumbering();
        sendKey(selectedNumbers, Qt::Key_Tab);
        check(paragraphTexts(selectedNumbers.snapshot()) ==
                  QStringList{QStringLiteral("    A.\tone"),
                              QStringLiteral("    B.\ttwo"),
                              QStringLiteral("    C.\tthree")},
              "Tab did not number selected nested items as A, B, C");
        sendKey(selectedNumbers, Qt::Key_Backtab, Qt::ShiftModifier);
        check(paragraphTexts(selectedNumbers.snapshot()) ==
                  QStringList{QStringLiteral("1.\tone"),
                              QStringLiteral("2.\ttwo"),
                              QStringLiteral("3.\tthree")},
              "Shift+Tab did not restore selected items to decimal numbering");
        selectedNumbers.undo();
        check(paragraphTexts(selectedNumbers.snapshot()) ==
                  QStringList{QStringLiteral("    A.\tone"),
                              QStringLiteral("    B.\ttwo"),
                              QStringLiteral("    C.\tthree")},
              "selected numbered-list outdent was not one undoable transaction");
    }

    {
        docxstudio::app::DocumentCanvas nestedNumber(spelling);
        nestedNumber.toggleNumbering();
        nestedNumber.insertText(QStringLiteral("parent"));
        sendKey(nestedNumber, Qt::Key_Return);
        sendKey(nestedNumber, Qt::Key_Tab);
        check(paragraphTexts(nestedNumber.snapshot()).back() ==
                  QStringLiteral("    A.\t"),
              "Tab did not start a nested uppercase-alphabetic sequence at A");
        nestedNumber.insertText(QStringLiteral("nested"));
        sendKey(nestedNumber, Qt::Key_Return);
        check(paragraphTexts(nestedNumber.snapshot()) ==
                  QStringList{QStringLiteral("1.\tparent"),
                              QStringLiteral("    A.\tnested"),
                              QStringLiteral("    B.\t")},
              "nested uppercase-alphabetic continuation was incorrect");
        sendKey(nestedNumber, Qt::Key_Tab, Qt::ShiftModifier);
        check(paragraphTexts(nestedNumber.snapshot()).back() == QStringLiteral("2.\t"),
              "Shift+Tab did not continue the parent decimal sequence");
    }

    {
        docxstudio::app::DocumentCanvas numberedLevels(spelling);
        numberedLevels.insertText(QStringLiteral("1. nested through ten levels"));
        const QStringList expectedMarkers{
            QStringLiteral("    A.\t"),
            QStringLiteral("        I.\t"),
            QStringLiteral("            a.\t"),
            QStringLiteral("                i.\t"),
            QStringLiteral("                    1.\t"),
            QStringLiteral("                        A.\t"),
            QStringLiteral("                            I.\t"),
            QStringLiteral("                                a.\t"),
            QStringLiteral("                                    i.\t"),
        };
        for (const QString& prefix : expectedMarkers) {
            sendKey(numberedLevels, Qt::Key_Tab);
            check(textOf(numberedLevels.snapshot()).startsWith(prefix),
                  "numbered-list nesting did not follow the five-style cycle");
        }
        sendKey(numberedLevels, Qt::Key_Tab);
        check(textOf(numberedLevels.snapshot()).startsWith(
                  expectedMarkers.back()),
              "numbered-list nesting advanced beyond level 10");
        sendKey(numberedLevels, Qt::Key_Return);
        check(paragraphTexts(numberedLevels.snapshot()).back() ==
                  QStringLiteral("                                    ii.\t"),
              "lowercase-Roman continuation failed at level 10");
        sendKey(numberedLevels, Qt::Key_Backtab, Qt::ShiftModifier);
        check(paragraphTexts(numberedLevels.snapshot()).back() ==
                  QStringLiteral("                                a.\t"),
              "outdenting did not convert lowercase Roman to lowercase alphabetic");
    }

    {
        docxstudio::app::DocumentCanvas alphabeticBoundary(spelling);
        alphabeticBoundary.insertText(QStringLiteral("    Z. alphabetic"));
        sendKey(alphabeticBoundary, Qt::Key_Return);
        check(paragraphTexts(alphabeticBoundary.snapshot()).back() ==
                  QStringLiteral("    AA.\t"),
              "uppercase alphabetic numbering did not continue from Z to AA");

        docxstudio::app::DocumentCanvas upperRoman(spelling);
        upperRoman.insertText(QStringLiteral("        III. roman"));
        sendKey(upperRoman, Qt::Key_Return);
        check(paragraphTexts(upperRoman.snapshot()).back() ==
                  QStringLiteral("        IV.\t"),
              "uppercase Roman numbering did not continue from III to IV");

        docxstudio::app::DocumentCanvas lowerAlphabetic(spelling);
        lowerAlphabetic.insertText(QStringLiteral("            z. alphabetic"));
        sendKey(lowerAlphabetic, Qt::Key_Return);
        check(paragraphTexts(lowerAlphabetic.snapshot()).back() ==
                  QStringLiteral("            aa.\t"),
              "lowercase alphabetic numbering did not continue from z to aa");

        docxstudio::app::DocumentCanvas lowerRoman(spelling);
        lowerRoman.insertText(QStringLiteral("                iii. roman"));
        sendKey(lowerRoman, Qt::Key_Return);
        check(paragraphTexts(lowerRoman.snapshot()).back() ==
                  QStringLiteral("                iv.\t"),
              "lowercase Roman numbering did not continue from iii to iv");
    }

    {
        docxstudio::app::DocumentCanvas emptyBullet(spelling);
        emptyBullet.insertText(QStringLiteral("\u2022 "));
        sendKey(emptyBullet, Qt::Key_Return);
        check(paragraphTexts(emptyBullet.snapshot()) == QStringList{QString()},
              "Enter on an empty bullet did not exit the list");
    }

    {
        docxstudio::app::DocumentCanvas emptyNumber(spelling);
        emptyNumber.insertText(QStringLiteral("12. "));
        sendKey(emptyNumber, Qt::Key_Return);
        check(paragraphTexts(emptyNumber.snapshot()) == QStringList{QString()},
              "Enter on an empty numbered marker did not exit the list");
        emptyNumber.insertText(QStringLiteral("12. "));
        sendKey(emptyNumber, Qt::Key_Backspace);
        check(paragraphTexts(emptyNumber.snapshot()) == QStringList{QString()},
              "Backspace on an empty numbered marker did not remove it");
    }

    {
        docxstudio::app::DocumentCanvas protectedBulletPrefix(spelling);
        protectedBulletPrefix.insertText(QStringLiteral("\u2022 first"));
        sendKey(protectedBulletPrefix, Qt::Key_Left, Qt::ControlModifier);
        sendKey(protectedBulletPrefix, Qt::Key_Backspace);
        check(paragraphTexts(protectedBulletPrefix.snapshot()) ==
                  QStringList{QStringLiteral("first")},
              "Backspace at bullet content start corrupted the marker");

        docxstudio::app::DocumentCanvas nestedPrefix(spelling);
        nestedPrefix.insertText(QStringLiteral("\t\u25e6 child"));
        sendKey(nestedPrefix, Qt::Key_Left, Qt::ControlModifier);
        sendKey(nestedPrefix, Qt::Key_Backspace);
        check(paragraphTexts(nestedPrefix.snapshot()) ==
                  QStringList{QStringLiteral("\u2022\tchild")},
              "Backspace at nested bullet content start did not outdent it");
    }

    {
        docxstudio::app::DocumentCanvas mergeBackward(spelling);
        mergeBackward.insertText(
            QStringLiteral("\u2022 first\n\u2022 second"));
        sendKey(mergeBackward, Qt::Key_Home);
        sendKey(mergeBackward, Qt::Key_Backspace);
        check(paragraphTexts(mergeBackward.snapshot()) ==
                  QStringList{QStringLiteral("\u2022 first"),
                              QStringLiteral("second")},
              "Backspace at bullet content start corrupted the list prefix");
        mergeBackward.undo();
        check(paragraphTexts(mergeBackward.snapshot()) ==
                  QStringList{QStringLiteral("\u2022 first"),
                              QStringLiteral("\u2022 second")},
              "removing a bullet marker was not one undo transaction");
        mergeBackward.insertText(QStringLiteral("X"));
        check(paragraphTexts(mergeBackward.snapshot()).back() ==
                  QStringLiteral("\u2022 Xsecond"),
              "Home allowed typing before or inside a bullet marker");

        docxstudio::app::DocumentCanvas mergeForward(spelling);
        mergeForward.insertText(
            QStringLiteral("\u2022 first\n\u2022 second"));
        sendKey(mergeForward, Qt::Key_Home, Qt::ControlModifier);
        sendKey(mergeForward, Qt::Key_End);
        sendKey(mergeForward, Qt::Key_Delete);
        check(paragraphTexts(mergeForward.snapshot()) ==
                  QStringList{QStringLiteral("\u2022 firstsecond")},
              "Delete between bullet items left a second marker mid-line");
    }

    {
        docxstudio::app::DocumentCanvas normalKeys(spelling);
        normalKeys.insertText(QStringLiteral("ordinary"));
        sendKey(normalKeys, Qt::Key_Return);
        check(paragraphTexts(normalKeys.snapshot()) ==
                  QStringList{QStringLiteral("ordinary"), QString()},
              "normal Enter no longer creates an ordinary paragraph");
        normalKeys.insertText(QStringLiteral("text"));
        sendKey(normalKeys, Qt::Key_Tab);
        check(paragraphTexts(normalKeys.snapshot()).back() == QStringLiteral("text\t"),
              "Tab outside a list no longer inserts a literal tab");
    }

    docxstudio::app::ChatDock chat;
    chat.setModels({QStringLiteral("gpt-default"),
                    QStringLiteral("gpt-5.6-luna")});
    check(chat.selectedModel() == QStringLiteral("gpt-5.6-luna"),
          "Codex chat did not prefer gpt-5.6-luna when advertised");
    chat.setEfforts({QStringLiteral("medium"), QStringLiteral("low")});
    check(chat.selectedEffort() == QStringLiteral("low"),
          "Codex chat did not prefer low reasoning when advertised");
    const QString tierDetails = QStringLiteral(
        "Standard: Normal speed\nFast: May use additional credits");
    chat.setServiceTiers({QStringLiteral("fast"), QStringLiteral("default")},
                         QStringLiteral("default"), tierDetails);
    check(chat.selectedServiceTier() == QStringLiteral("default"),
          "Standard service tier was not selected");
    chat.setServiceTiers({QStringLiteral("fast")}, QString(), tierDetails);
    check(chat.selectedServiceTier().isEmpty(),
          "Fast service tier was selected without explicit user action");
    chat.setServiceTiers({QStringLiteral("priority"), QStringLiteral("fast")},
                         QStringLiteral("priority"), tierDetails);
    check(chat.selectedServiceTier() == QStringLiteral("priority"),
          "server-advertised nonstandard default service tier was not selected");
    chat.setServiceTiers({QStringLiteral("fast")}, QStringLiteral("fast"),
                         tierDetails);
    check(chat.selectedServiceTier().isEmpty(),
          "Fast service tier was auto-selected from the catalog default");
    auto* chatInput = chat.findChild<QPlainTextEdit*>(QStringLiteral("codex.input"));
    auto* chatSend = chat.findChild<QPushButton*>(QStringLiteral("codex.send"));
    check(chatInput != nullptr && chatSend != nullptr,
          "Codex chat input controls are not discoverable");
    chat.setConnected(true);
    chat.setBusy(true);
    check(chatInput != nullptr && !chatInput->isEnabled() &&
              chatSend != nullptr && !chatSend->isEnabled(),
          "Codex chat controls stayed active during a turn");
    chat.setBusy(false);
    check(chatInput != nullptr && chatInput->isEnabled() &&
              chatSend != nullptr && chatSend->isEnabled(),
          "Codex chat did not re-enable after a completed turn");
    chat.setConnected(false);
    check(chatInput != nullptr && !chatInput->isEnabled() &&
              chatSend != nullptr && !chatSend->isEnabled(),
          "Codex chat controls stayed active after disconnecting");

    docxstudio::app::CommandRegistry iconCommands;
    const auto addIconCommand = [&iconCommands](const QString& id,
                                                const QString& text) {
        iconCommands.add(id, text, QKeySequence(), [] {});
    };
    addIconCommand(QStringLiteral("edit.undo"), QStringLiteral("Undo"));
    addIconCommand(QStringLiteral("edit.paste"), QStringLiteral("Paste"));
    addIconCommand(QStringLiteral("format.bold"), QStringLiteral("Bold"));
    addIconCommand(QStringLiteral("paragraph.bullets"), QStringLiteral("Bullets"));
    addIconCommand(QStringLiteral("paragraph.alignLeft"), QStringLiteral("Align Left"));
    addIconCommand(QStringLiteral("insert.table"), QStringLiteral("Table"));
    addIconCommand(QStringLiteral("review.spelling"), QStringLiteral("Spelling"));
    docxstudio::app::RibbonWidget iconRibbon(iconCommands);
    const auto* marginPresets = iconRibbon.findChild<QComboBox*>(
        QStringLiteral("ribbon.margins"));
    check(marginPresets != nullptr,
          "Layout ribbon has no margin preset control");
    for (const auto* preset : {"normal", "narrow", "moderate", "wide",
                               "office2003", "custom"}) {
        check(marginPresets->findData(QString::fromLatin1(preset)) >= 0,
              "Layout ribbon is missing a standard margin choice");
    }
    for (const auto* rawId : {"edit.undo", "edit.paste", "format.bold",
                              "paragraph.bullets", "paragraph.alignLeft",
                              "insert.table", "review.spelling"}) {
        const QString id = QString::fromLatin1(rawId);
        const auto* action = iconCommands.action(id);
        check(action != nullptr && !action->icon().isNull(),
              "key ribbon action has no icon");
        check(!action->text().isEmpty() && !action->toolTip().isEmpty(),
              "key ribbon action lost its text or tooltip");
        const auto* button = iconRibbon.findChild<QToolButton*>(
            QStringLiteral("ribbonButton.%1").arg(id));
        check(button != nullptr && !button->icon().isNull(),
              "key ribbon button has no icon");
        check(!button->accessibleName().isEmpty() && !button->toolTip().isEmpty(),
              "icon-only ribbon button has no accessible name or tooltip");
    }
    for (const auto* rawId : {"format.textColor", "format.highlightColor"}) {
        const auto* button = iconRibbon.findChild<QToolButton*>(
            QStringLiteral("ribbonButton.%1").arg(QString::fromLatin1(rawId)));
        check(button != nullptr && !button->icon().isNull(),
              "color ribbon button has no icon");
        check(!button->accessibleName().isEmpty() && !button->toolTip().isEmpty(),
              "color ribbon button has no accessible name or tooltip");
    }
    const auto* textColorButton = iconRibbon.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.format.textColor"));
    const auto* highlightButton = iconRibbon.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.format.highlightColor"));
    check(textColorButton->accessibleName() == QStringLiteral("Text color") &&
              textColorButton->toolTip() == QStringLiteral("Text color"),
          "text color icon has the wrong accessible label");
    check(highlightButton->accessibleName() == QStringLiteral("Highlight") &&
              highlightButton->toolTip() == QStringLiteral("Highlight"),
          "highlight icon has the wrong accessible label");
    const qint64 textColorBefore = textColorButton->icon().cacheKey();
    const qint64 highlightBefore = highlightButton->icon().cacheKey();
    iconRibbon.setTextColor(QColor(QStringLiteral("#336699")));
    iconRibbon.setHighlightColor(QColor(QStringLiteral("#99cc33")));
    check(textColorButton->icon().cacheKey() != textColorBefore &&
              highlightButton->icon().cacheKey() != highlightBefore,
          "color icon swatches did not update");
    check(textColorButton->menu() && highlightButton->menu() &&
              textColorButton->popupMode() == QToolButton::InstantPopup &&
              highlightButton->popupMode() == QToolButton::InstantPopup,
          "color controls are not immediate popup palettes");
    QColor selectedTextColor;
    QColor selectedHighlightColor;
    bool clearHighlightRequested = false;
    QObject::connect(&iconRibbon, &docxstudio::app::RibbonWidget::textColorSelected,
                     [&selectedTextColor](const QColor& color) {
        selectedTextColor = color;
    });
    QObject::connect(&iconRibbon,
                     &docxstudio::app::RibbonWidget::highlightColorSelected,
                     [&selectedHighlightColor](const QColor& color) {
        selectedHighlightColor = color;
    });
    QObject::connect(&iconRibbon,
                     &docxstudio::app::RibbonWidget::clearHighlightRequested,
                     [&clearHighlightRequested] { clearHighlightRequested = true; });
    auto* textPaletteChoice = iconRibbon.findChild<QAction*>(
        QStringLiteral("ribbonColor.text.2"));
    auto* highlightPaletteChoice = iconRibbon.findChild<QAction*>(
        QStringLiteral("ribbonColor.highlight.0"));
    auto* noHighlightChoice = iconRibbon.findChild<QAction*>(
        QStringLiteral("ribbonColor.highlight.none"));
    check(textPaletteChoice && highlightPaletteChoice && noHighlightChoice,
          "color palettes are missing immediate or No Highlight choices");
    textPaletteChoice->trigger();
    highlightPaletteChoice->trigger();
    noHighlightChoice->trigger();
    check(selectedTextColor == QColor(QStringLiteral("#ff0000")) &&
              selectedHighlightColor == QColor(QStringLiteral("#fff200")) &&
              clearHighlightRequested,
          "a palette click was not applied immediately");
    const qint64 coloredHighlight = highlightButton->icon().cacheKey();
    iconRibbon.setHighlightColor({});
    check(highlightButton->icon().cacheKey() != coloredHighlight,
          "No Highlight did not update the ribbon swatch");

    QTemporaryDir temporary;
    check(temporary.isValid(), "could not create temporary directory");
    const QString pdf = temporary.filePath(QStringLiteral("document.pdf"));
    check(canvas.exportPdf(pdf, error), "PDF export failed");
    QFile output(pdf);
    check(output.open(QIODevice::ReadOnly), "could not read exported PDF");
    check(output.read(5) == QByteArrayLiteral("%PDF-"), "export was not a PDF file");
    output.close();

    const QString pdftotext = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (!pdftotext.isEmpty()) {
        QProcess extractor;
        extractor.start(pdftotext, {pdf, QStringLiteral("-")});
        check(extractor.waitForStarted(5000), "could not start pdftotext");
        check(extractor.waitForFinished(10000), "pdftotext did not finish");
        check(extractor.exitStatus() == QProcess::NormalExit && extractor.exitCode() == 0,
              "pdftotext could not extract the exported PDF");
        const QString extracted = QString::fromUtf8(extractor.readAllStandardOutput());
        check(extracted.contains(QStringLiteral("Helo world")),
              "exported PDF text was not searchable/extractable");
    } else {
        std::cout << "pdftotext unavailable; PDF text extraction check skipped\n";
    }

    docxstudio::app::DocumentCanvas spacingCanvas(spelling);
    int pageCount = 0;
    QObject::connect(&spacingCanvas, &docxstudio::app::DocumentCanvas::pageStatusChanged,
                     [&pageCount](int, int pages, int) { pageCount = pages; });
    const auto pagesFor = [&](docxstudio::core::LineSpacingRule rule,
                              std::int64_t spacingPoints) {
        pageCount = 0;
        spacingCanvas.setDocument(spacedDocument(rule, spacingPoints));
        check(pageCount > 0, "line-spacing layout did not report a page count");
        return pageCount;
    };
    const int automaticSingle =
        pagesFor(docxstudio::core::LineSpacingRule::automatic, 12);
    const int automaticDouble =
        pagesFor(docxstudio::core::LineSpacingRule::automatic, 24);
    const int atLeastTwelve =
        pagesFor(docxstudio::core::LineSpacingRule::at_least, 12);
    const int exactTwelve =
        pagesFor(docxstudio::core::LineSpacingRule::exact, 12);
    check(automaticDouble > automaticSingle,
          "automatic double spacing did not scale the natural line advance");
    check(atLeastTwelve == automaticSingle,
          "at-least spacing did not retain the larger natural line advance");
    check(exactTwelve < atLeastTwelve,
          "exact spacing did not use the requested advance");

    std::cout << "app smoke tests passed\n";
    return 0;
}

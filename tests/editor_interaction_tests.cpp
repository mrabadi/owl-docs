#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/SpellChecker.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QEventLoop>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using docxstudio::app::DocumentCanvas;
using docxstudio::core::CharacterFormat;
using docxstudio::core::DocumentSnapshot;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString fromUtf16(const std::u16string& text) {
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

QStringList paragraphTexts(const DocumentSnapshot& snapshot) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(snapshot.document.paragraphs().size()));
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(fromUtf16(paragraph.text()));
    }
    return result;
}

QString onlyText(const DocumentCanvas& canvas) {
    const auto snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().size() == 1,
          "test expected exactly one paragraph");
    return fromUtf16(snapshot.document.paragraphs().front().text());
}

CharacterFormat formatAt(const DocumentCanvas& canvas, std::size_t offset) {
    const auto snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().size() == 1,
          "format test expected exactly one paragraph");
    return snapshot.document.paragraphs().front().characterFormatAt(offset);
}

docxstudio::core::Document documentWithText(const QString& text) {
    auto paragraph = docxstudio::core::Paragraph::create(text.toStdU16String());
    check(static_cast<bool>(paragraph), "could not create test paragraph");
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{std::move(paragraph.value())});
    check(static_cast<bool>(document), "could not create test document");
    return std::move(document.value());
}

docxstudio::core::Document documentWithParagraphs(const QStringList& texts) {
    std::vector<docxstudio::core::Paragraph> paragraphs;
    paragraphs.reserve(static_cast<std::size_t>(texts.size()));
    for (const auto& text : texts) {
        auto paragraph = docxstudio::core::Paragraph::create(
            text.toStdU16String());
        check(static_cast<bool>(paragraph),
              "could not create list test paragraph");
        paragraphs.push_back(std::move(paragraph.value()));
    }
    auto document = docxstudio::core::Document::create(
        std::move(paragraphs));
    check(static_cast<bool>(document),
          "could not create multi-paragraph test document");
    return std::move(document.value());
}

docxstudio::core::Document documentWithColor(const QString& text,
                                             std::size_t start,
                                             std::size_t end,
                                             std::uint32_t argb) {
    auto paragraph = docxstudio::core::Paragraph::create(text.toStdU16String());
    check(static_cast<bool>(paragraph), "could not create colored paragraph");
    const auto paragraphId = paragraph.value().id();
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{std::move(paragraph.value())});
    check(static_cast<bool>(document), "could not create colored document");
    docxstudio::core::CharacterFormatDelta delta;
    delta.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(argb);
    const auto formatted = document.value().applyCharacterFormat(
        {{paragraphId, start}, {paragraphId, end}}, delta);
    check(static_cast<bool>(formatted), "could not color document range");
    return std::move(document.value());
}

docxstudio::core::Document paragraphFlowDocument(bool explicitDefaults) {
    std::vector<docxstudio::core::Paragraph> paragraphs;
    paragraphs.reserve(120);
    for (int index = 0; index < 120; ++index) {
        auto paragraph = docxstudio::core::Paragraph::create(u"Line");
        check(static_cast<bool>(paragraph),
              "could not create default-spacing test paragraph");
        paragraphs.push_back(std::move(paragraph.value()));
    }

    auto created = docxstudio::core::Document::create(std::move(paragraphs));
    check(static_cast<bool>(created),
          "could not create default-spacing test document");
    auto document = std::move(created.value());
    if (!explicitDefaults) {
        return document;
    }

    std::vector<docxstudio::core::NodeId> paragraphIds;
    paragraphIds.reserve(document.paragraphs().size());
    for (const auto& paragraph : document.paragraphs()) {
        paragraphIds.push_back(paragraph.id());
    }
    docxstudio::core::ParagraphFormatDelta format;
    format.space_after_emu =
        docxstudio::core::PropertyDelta<std::int64_t>::set(0);
    format.line_spacing_emu =
        docxstudio::core::PropertyDelta<std::int64_t>::set(12 * 12700);
    format.line_spacing_rule =
        docxstudio::core::PropertyDelta<docxstudio::core::LineSpacingRule>::set(
            docxstudio::core::LineSpacingRule::automatic);
    const auto applied = document.applyParagraphFormat(paragraphIds, format);
    check(static_cast<bool>(applied),
          "could not apply explicit single-spacing defaults");
    return document;
}

void sendKey(DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

void sendTextKey(DocumentCanvas& canvas, Qt::Key key, const QString& text,
                 Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(&canvas, &event);
}

void sendMouseEvent(DocumentCanvas& canvas, QEvent::Type type,
                    const QPoint& position, Qt::MouseButton button,
                    Qt::MouseButtons buttons) {
    const QPoint globalPosition = canvas.viewport()->mapToGlobal(position);
    QMouseEvent event(type, QPointF(position), QPointF(position),
                      QPointF(globalPosition), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(canvas.viewport(), &event);
}

QRect inputMethodCursorRect(DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorRectangle);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorRectangle).toRect();
}

QImage renderedViewport(DocumentCanvas& canvas) {
    QApplication::processEvents();
    return canvas.viewport()->grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
}

int spellingDecorationPixels(const QImage& image, int tolerance = 40) {
    const QColor target(QStringLiteral("#d92d20"));
    int result = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - target.red()) <= tolerance &&
                std::abs(pixel.green() - target.green()) <= tolerance &&
                std::abs(pixel.blue() - target.blue()) <= tolerance) {
                ++result;
            }
        }
    }
    return result;
}

void processEventsFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void checkCollapsedCaret(const DocumentCanvas& canvas,
                         docxstudio::core::NodeId paragraphId,
                         std::size_t offset,
                         const char* message) {
    const auto selection = canvas.selection();
    check(selection.anchor == selection.focus, message);
    check(selection.focus.paragraph_id == paragraphId, message);
    check(selection.focus.utf16_offset == offset, message);
}

void commitInputMethodText(DocumentCanvas& canvas, const QString& text) {
    QInputMethodEvent event;
    event.setCommitString(text);
    QApplication::sendEvent(&canvas, &event);
    check(event.isAccepted(), "IME commit event was not accepted");
}

void testDefaultAndPlainTyping(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    check(canvas.currentTextColor().rgba() == QColor(Qt::black).rgba(),
          "new document did not report black as its current text color");

    canvas.insertText(QStringLiteral("plain"));
    check(canvas.currentTextColor().rgba() == QColor(Qt::black).rgba(),
          "plain typing changed the current text color from black");
    canvas.selectAll();
    check(canvas.currentTextColor().rgba() == QColor(Qt::black).rgba(),
          "default-black selected text was not reported as black");
    const auto snapshot = canvas.snapshot();
    const auto& paragraph = snapshot.document.paragraphs().front();
    check(fromUtf16(paragraph.text()) == QStringLiteral("plain"),
          "plain typing did not reach the semantic model");
    check(paragraph.characterFormats().empty(),
          "plain typing stored an unnecessary explicit character format");
    check(!paragraph.characterFormatAt(1).foreground_argb.has_value(),
          "plain typing stored an explicit foreground color");
}

void testEditorDefaultsAffectLayout(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas fourSpaces(spelling);
    fourSpaces.resize(900, 400);
    fourSpaces.show();
    fourSpaces.setFocus();
    QApplication::processEvents();
    const QRect fourStart = inputMethodCursorRect(fourSpaces);
    sendKey(fourSpaces, Qt::Key_Tab);
    QApplication::processEvents();
    const QRect fourEnd = inputMethodCursorRect(fourSpaces);
    const int fourAdvance = fourEnd.x() - fourStart.x();

    DocumentCanvas eightSpaces(spelling);
    eightSpaces.setEditorDefaults(QStringLiteral("DejaVu Sans"), 22.0, 8);
    check(!eightSpaces.isModified(),
          "changing editor defaults dirtied an otherwise untouched document");
    eightSpaces.resize(900, 400);
    eightSpaces.show();
    eightSpaces.setFocus();
    QApplication::processEvents();
    const QRect eightStart = inputMethodCursorRect(eightSpaces);
    sendKey(eightSpaces, Qt::Key_Tab);
    QApplication::processEvents();
    const QRect eightEnd = inputMethodCursorRect(eightSpaces);
    const int eightAdvance = eightEnd.x() - eightStart.x();

    check(fourSpaces.tabWidthSpaces() == 4 &&
              eightSpaces.tabWidthSpaces() == 8,
          "the canvas did not retain the configured tab sizes");
    if (!(eightAdvance > fourAdvance * 2)) {
        std::cerr << "Tab advances: four-space=" << fourAdvance
                  << ", eight-space=" << eightAdvance << '\n';
    }
    check(eightAdvance > fourAdvance * 2,
          "the configured tab size was not measured in font-space equivalents");
    check(eightEnd.height() > fourEnd.height(),
          "the configured default font size did not affect document layout");
    check(eightSpaces.currentFontFamily() == QStringLiteral("DejaVu Sans") &&
              eightSpaces.currentFontPointSize() == 22.0,
          "the active editor defaults were not reported to the ribbon");
    fourSpaces.hide();
    eightSpaces.hide();
}

void testDefaultParagraphFlow(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas implicitDefaults(spelling);
    DocumentCanvas explicitDefaults(spelling);
    int implicitPages = 0;
    int explicitPages = 0;
    QObject::connect(
        &implicitDefaults, &DocumentCanvas::pageStatusChanged,
        [&implicitPages](int, int pages, int) { implicitPages = pages; });
    QObject::connect(
        &explicitDefaults, &DocumentCanvas::pageStatusChanged,
        [&explicitPages](int, int pages, int) { explicitPages = pages; });

    implicitDefaults.setDocument(paragraphFlowDocument(false));
    explicitDefaults.setDocument(paragraphFlowDocument(true));
    const auto implicitSnapshot = implicitDefaults.snapshot();
    const auto& format = implicitSnapshot.document.paragraphs().front().format();
    check(format.space_after_emu.value_or(0) == 0,
          "a default paragraph has nonzero after-spacing");
    check(format.line_spacing_rule.value_or(
              docxstudio::core::LineSpacingRule::automatic) ==
              docxstudio::core::LineSpacingRule::automatic &&
              format.line_spacing_emu.value_or(12 * 12700) == 12 * 12700,
          "a default paragraph is not single-spaced");
    check(implicitPages > 0 && implicitPages == explicitPages,
          "default layout adds spacing beyond explicit single/zero-after flow");
}

void testDefaultTextRendersBlack(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 500);
    canvas.insertText(QStringLiteral("hello world"));
    canvas.show();
    canvas.clearFocus();
    QApplication::processEvents();

    const QImage image = canvas.viewport()->grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    check(!image.isNull(), "could not render the document canvas offscreen");

    // Restrict the scan to the first line inside the white page, excluding
    // the gray page border, shadow, scroll bars, and caret. Unformatted text
    // used to inherit that border/squiggle pen and therefore had no neutral
    // near-black glyph pixels here.
    int neutralDarkPixels = 0;
    const int left = std::min(120, image.width());
    const int right = std::min(430, image.width());
    const int top = std::min(105, image.height());
    const int bottom = std::min(180, image.height());
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const QColor pixel = QColor::fromRgba(image.pixel(x, y));
            const int maximum = std::max({pixel.red(), pixel.green(), pixel.blue()});
            const int minimum = std::min({pixel.red(), pixel.green(), pixel.blue()});
            if (maximum < 90 && maximum - minimum < 12) {
                ++neutralDarkPixels;
            }
        }
    }
    check(neutralDarkPixels > 30,
          "unformatted glyphs did not render in stable near-black pixels");
    canvas.hide();
}

void testSpellcheckCommitsCompletedWords(
    docxstudio::app::SpellChecker& spelling) {
    check(spelling.available(),
          "Hunspell is unavailable for the spellcheck timing regression");
    check(!spelling.isCorrect(QStringLiteral("teh")),
          "the spellcheck timing fixture is unexpectedly accepted");
    const auto redPixels = [](DocumentCanvas& canvas) {
        return spellingDecorationPixels(renderedViewport(canvas));
    };
    const auto type = [](DocumentCanvas& canvas, const QString& text) {
        for (const auto character : text) {
            sendTextKey(canvas, Qt::Key_unknown, QString(character));
        }
    };
    const auto prepare = [](DocumentCanvas& canvas) {
        canvas.resize(900, 500);
        canvas.show();
        canvas.setFocus();
        QApplication::processEvents();
    };

    // Type before existing text so the caret can move both within and off the
    // pending token without first committing it with a delimiter.
    DocumentCanvas navigation(spelling);
    navigation.setDocument(documentWithText(QStringLiteral(" good")));
    prepare(navigation);
    type(navigation, QStringLiteral("teh"));
    check(onlyText(navigation) == QStringLiteral("teh good") &&
              redPixels(navigation) <= 2,
          "a misspelled body word was underlined while it was being typed");
    processEventsFor(1100);
    check(redPixels(navigation) <= 2,
          "typing-group idle time incorrectly committed a pending word");
    sendKey(navigation, Qt::Key_Left);
    check(redPixels(navigation) <= 2,
          "moving within a pending body word exposed its squiggle");
    sendKey(navigation, Qt::Key_Right);
    check(redPixels(navigation) <= 2,
          "returning to the typed word's end committed it prematurely");
    sendKey(navigation, Qt::Key_Right);
    check(redPixels(navigation) > 2,
          "moving the caret off a misspelled body word did not commit it");
    sendKey(navigation, Qt::Key_Left);
    check(redPixels(navigation) > 2,
          "revisiting a committed misspelling incorrectly hid its squiggle");
    navigation.hide();

    DocumentCanvas delimiter(spelling);
    prepare(delimiter);
    type(delimiter, QStringLiteral("teh"));
    check(redPixels(delimiter) <= 2,
          "a pending typo was decorated before delimiter input");
    sendTextKey(delimiter, Qt::Key_Comma, QStringLiteral(","));
    check(redPixels(delimiter) > 2,
          "punctuation did not commit a completed misspelled word");
    delimiter.hide();

    DocumentCanvas apostrophe(spelling);
    prepare(apostrophe);
    type(apostrophe, QStringLiteral("teh'"));
    check(redPixels(apostrophe) <= 2,
          "a trailing apostrophe prematurely committed a word mid-typing");
    sendTextKey(apostrophe, Qt::Key_Space, QStringLiteral(" "));
    check(redPixels(apostrophe) > 2,
          "a space did not commit a word ending in an apostrophe");
    apostrophe.hide();

    DocumentCanvas newline(spelling);
    prepare(newline);
    type(newline, QStringLiteral("teh"));
    sendKey(newline, Qt::Key_Return);
    check(redPixels(newline) > 2,
          "a paragraph break did not commit a misspelled word");
    newline.hide();

    DocumentCanvas tab(spelling);
    prepare(tab);
    type(tab, QStringLiteral("teh"));
    sendKey(tab, Qt::Key_Tab);
    check(redPixels(tab) > 2,
          "Tab did not commit a misspelled word");
    tab.hide();

    DocumentCanvas focus(spelling);
    prepare(focus);
    type(focus, QStringLiteral("teh"));
    check(redPixels(focus) <= 2,
          "focus fixture was committed before focus changed");
    focus.clearFocus();
    QApplication::processEvents();
    check(redPixels(focus) > 2,
          "leaving the editor did not commit a misspelled word");
    focus.hide();

    DocumentCanvas pasted(spelling);
    prepare(pasted);
    QApplication::clipboard()->setText(QStringLiteral("teh"));
    pasted.paste();
    check(redPixels(pasted) > 2,
          "pasted misspelling was treated as actively typed text");
    pasted.hide();

    DocumentCanvas ime(spelling);
    prepare(ime);
    commitInputMethodText(ime, QStringLiteral("teh"));
    check(redPixels(ime) <= 2,
          "an in-progress IME-committed word was underlined immediately");
    commitInputMethodText(ime, QStringLiteral(" "));
    check(redPixels(ime) > 2,
          "IME delimiter input did not commit the preceding word");
    ime.hide();

    DocumentCanvas history(spelling);
    prepare(history);
    type(history, QStringLiteral("teh"));
    history.undo();
    history.redo();
    check(onlyText(history) == QStringLiteral("teh") &&
              redPixels(history) > 2,
          "redo restored an obsolete active-typing spellcheck state");
    history.hide();

    DocumentCanvas correct(spelling);
    prepare(correct);
    type(correct, QStringLiteral("the "));
    check(redPixels(correct) <= 2,
          "spellcheck decorated a correctly spelled completed word");
    correct.hide();

    DocumentCanvas programmatic(spelling);
    programmatic.insertText(QStringLiteral("teh"));
    prepare(programmatic);
    check(redPixels(programmatic) > 2,
          "programmatic text insertion was mistaken for live typing");
    const QRect caret = inputMethodCursorRect(programmatic);
    sendMouseEvent(programmatic, QEvent::MouseButtonPress,
                   caret.center() + QPoint(8, 0), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(programmatic, QEvent::MouseButtonRelease,
                   caret.center() + QPoint(8, 0), Qt::LeftButton,
                   Qt::NoButton);
    check(redPixels(programmatic) > 2,
          "clicking a committed misspelling hid its squiggle");
    programmatic.hide();
}

void testColorTyping(docxstudio::app::SpellChecker& spelling) {
    const QColor selectedColor(QStringLiteral("#336699"));
    DocumentCanvas replacement(spelling);
    replacement.insertText(QStringLiteral("replace me"));
    replacement.selectAll();
    replacement.setForeground(selectedColor);
    replacement.insertText(QStringLiteral("colored"));
    check(onlyText(replacement) == QStringLiteral("colored"),
          "typing did not replace the formatted selection");
    check(formatAt(replacement, 1).foreground_argb ==
              static_cast<std::uint32_t>(selectedColor.rgba()),
          "replacement text did not retain the selected foreground color");

    const QColor caretColor(QStringLiteral("#8a2be2"));
    DocumentCanvas caret(spelling);
    caret.insertText(QStringLiteral("A"));
    caret.selectAll();
    caret.setForeground(caretColor);
    sendKey(caret, Qt::Key_Home);
    check(caret.currentTextColor().rgba() == caretColor.rgba(),
          "caret did not expose the explicit color of adjacent text");
    caret.insertText(QStringLiteral("B"));
    check(onlyText(caret) == QStringLiteral("BA"),
          "caret insertion occurred at the wrong position");
    check(formatAt(caret, 0).foreground_argb ==
              static_cast<std::uint32_t>(caretColor.rgba()),
          "new text did not inherit the explicit color selected by the caret");
}

void testHighlightRemoval(docxstudio::app::SpellChecker& spelling) {
    const QColor yellow(QStringLiteral("#fff200"));
    const QColor red(QStringLiteral("#c00000"));
    DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("marked"));
    canvas.selectAll();
    canvas.setHighlight(yellow);
    canvas.setForeground(red);
    check(formatAt(canvas, 1).highlight_argb ==
              static_cast<std::uint32_t>(yellow.rgba()) &&
              formatAt(canvas, 1).foreground_argb ==
                  static_cast<std::uint32_t>(red.rgba()),
          "changing font color did not preserve the existing highlight");

    canvas.clearHighlight();
    check(!formatAt(canvas, 1).highlight_argb.has_value(),
          "No Highlight did not remove highlighting from the selection");
    check(formatAt(canvas, 1).foreground_argb ==
              static_cast<std::uint32_t>(red.rgba()),
          "No Highlight also removed the selected font color");
    check(!canvas.currentHighlightColor().isValid(),
          "cleared highlighting was still reported at the selection");

    canvas.undo();
    check(formatAt(canvas, 1).highlight_argb ==
              static_cast<std::uint32_t>(yellow.rgba()) &&
              formatAt(canvas, 1).foreground_argb ==
                  static_cast<std::uint32_t>(red.rgba()),
          "one Undo did not restore only the removed highlight");
    canvas.redo();
    check(!formatAt(canvas, 1).highlight_argb.has_value() &&
              formatAt(canvas, 1).foreground_argb ==
                  static_cast<std::uint32_t>(red.rgba()),
          "Redo did not remove only the highlight");

    DocumentCanvas typing(spelling);
    typing.setHighlight(yellow);
    typing.clearHighlight();
    typing.insertText(QStringLiteral("plain"));
    check(!formatAt(typing, 1).highlight_argb.has_value(),
          "No Highlight at a collapsed caret did not clear future typing");
}

void testColorAdjustmentUndoGrouping(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("paint"));
    canvas.selectAll();
    canvas.beginColorAdjustment();
    canvas.setForeground(QColor(QStringLiteral("#123456")));
    canvas.setForeground(QColor(QStringLiteral("#223344")));
    canvas.setForeground(QColor(QStringLiteral("#334455")));
    canvas.endColorAdjustment();
    check(formatAt(canvas, 1).foreground_argb == 0xff334455U,
          "live color adjustment did not retain its final color");
    canvas.undo();
    check(!formatAt(canvas, 1).foreground_argb.has_value(),
          "one Undo did not revert the complete live color adjustment");
    canvas.redo();
    check(formatAt(canvas, 1).foreground_argb == 0xff334455U,
          "Redo did not restore the complete live color adjustment");

    DocumentCanvas saveBoundary(spelling);
    saveBoundary.insertText(QStringLiteral("paint"));
    saveBoundary.markSaved();
    saveBoundary.selectAll();
    saveBoundary.beginColorAdjustment();
    saveBoundary.setForeground(QColor(QStringLiteral("#aa0000")));
    saveBoundary.markSaved();
    saveBoundary.setForeground(QColor(QStringLiteral("#0000aa")));
    saveBoundary.endColorAdjustment();
    saveBoundary.undo();
    check(formatAt(saveBoundary, 1).foreground_argb == 0xffaa0000U &&
              !saveBoundary.isModified(),
          "a live color adjustment crossed a saved-document undo boundary");
}

void testImportedColorInheritance(docxstudio::app::SpellChecker& spelling) {
    const QColor blue(QStringLiteral("#336699"));

    DocumentCanvas insertion(spelling);
    insertion.setDocument(documentWithColor(
        QStringLiteral("colored"), 0, 7,
        static_cast<std::uint32_t>(blue.rgba())));
    check(insertion.currentTextColor().rgba() == blue.rgba(),
          "imported colored run was not exposed as the active typing color");
    insertion.insertText(QStringLiteral("X"));
    check(onlyText(insertion) == QStringLiteral("Xcolored"),
          "typing into an imported colored run inserted at the wrong place");
    check(formatAt(insertion, 1).foreground_argb ==
              static_cast<std::uint32_t>(blue.rgba()),
          "typing did not inherit the imported run color");
    check(!insertion.hasNonTextChanges(),
          "inherited colored typing was incorrectly classified as a format edit");

    DocumentCanvas preview(spelling);
    preview.setDocument(documentWithColor(
        QStringLiteral("colored"), 0, 7,
        static_cast<std::uint32_t>(blue.rgba())));
    preview.selectAll();
    QString summary;
    QString error;
    check(preview.createReplacementPreview(
              QStringLiteral("replacement"), summary, error),
          "could not preview replacement of an imported colored run");
    check(preview.acceptPreview(error),
          "could not accept replacement of an imported colored run");
    check(formatAt(preview, 1).foreground_argb ==
              static_cast<std::uint32_t>(blue.rgba()),
          "preview replacement discarded the imported run color");
    check(!preview.hasNonTextChanges(),
          "format-preserving preview was incorrectly classified as a format edit");

    DocumentCanvas boundary(spelling);
    boundary.setDocument(documentWithColor(
        QStringLiteral("Atarget"), 1, 7,
        static_cast<std::uint32_t>(blue.rgba())));
    check(boundary.replaceAll(QStringLiteral("target"), QStringLiteral("word")) == 1,
          "boundary replace-all did not replace its match");
    check(onlyText(boundary) == QStringLiteral("Aword"),
          "boundary replace-all produced incorrect text");
    check(!formatAt(boundary, 1).foreground_argb.has_value(),
          "boundary replace-all recolored the preceding default-black character");
    check(formatAt(boundary, 2).foreground_argb ==
              static_cast<std::uint32_t>(blue.rgba()),
          "boundary replace-all inherited the preceding run instead of the match color");
    check(!boundary.hasNonTextChanges(),
          "uniform format-preserving replace-all was classified as a format edit");
}

void testCollapsedBoldToggle(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.toggleBold();
    canvas.insertText(QStringLiteral("B"));
    check(formatAt(canvas, 0).bold == true,
          "first collapsed-caret bold toggle did not affect typed text");

    canvas.toggleBold();
    canvas.insertText(QStringLiteral("n"));
    check(onlyText(canvas) == QStringLiteral("Bn"),
          "bold-toggle typing produced incorrect text");
    check(formatAt(canvas, 2).bold == false,
          "second collapsed-caret bold toggle did not turn bold off");
}

void testBaselineToggle(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.toggleBaseline(docxstudio::core::BaselinePosition::superscript);
    canvas.insertText(QStringLiteral("x"));
    check(formatAt(canvas, 1).baseline ==
              docxstudio::core::BaselinePosition::superscript,
          "superscript toggle did not affect typed text");
    canvas.toggleBaseline(docxstudio::core::BaselinePosition::superscript);
    canvas.insertText(QStringLiteral("n"));
    check(formatAt(canvas, 2).baseline ==
              docxstudio::core::BaselinePosition::normal,
          "second superscript toggle did not return to baseline");
}

void testFontSizeCommit(docxstudio::app::SpellChecker& spelling) {
    docxstudio::app::CommandRegistry commands;
    docxstudio::app::RibbonWidget ribbon(commands);
    auto* size = ribbon.findChild<QComboBox*>(
        QStringLiteral("ribbon.fontSize"));
    check(size != nullptr, "ribbon font-size editor is missing");

    DocumentCanvas canvas(spelling);
    canvas.setDocument(documentWithText(QStringLiteral("size me")));
    canvas.selectAll();
    QObject::connect(&ribbon, &docxstudio::app::RibbonWidget::fontPointSizeRequested,
                     &canvas, &DocumentCanvas::setFontPointSize);
    size->setEditText(QString());
    size->setEditText(QStringLiteral("2"));
    size->setEditText(QStringLiteral("24"));
    check(!formatAt(canvas, 1).font_size_half_points.has_value(),
          "partial multi-digit font-size typing changed the document");
    check(QMetaObject::invokeMethod(
              size, "textActivated", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("24"))),
          "could not commit the ribbon font size");
    check(formatAt(canvas, 1).font_size_half_points == 48,
          "committed ribbon font size was not applied");
    canvas.undo();
    check(!formatAt(canvas, 1).font_size_half_points.has_value(),
          "one Undo did not restore the size before a multi-digit entry");
}

void testBreakKeys(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas lineBreak(spelling);
    lineBreak.insertText(QStringLiteral("first"));
    sendKey(lineBreak, Qt::Key_Return, Qt::ShiftModifier);
    lineBreak.insertText(QStringLiteral("second"));
    check(lineBreak.snapshot().document.paragraphs().size() == 1,
          "Shift+Enter created a paragraph instead of a line separator");
    check(onlyText(lineBreak) == QStringLiteral("first\u2028second"),
          "Shift+Enter did not insert U+2028 into the semantic text");

    DocumentCanvas bulletLineBreak(spelling);
    bulletLineBreak.resize(900, 500);
    bulletLineBreak.show();
    bulletLineBreak.setFocus();
    bulletLineBreak.toggleBullets();
    QApplication::processEvents();
    const QRect bulletTextStart = inputMethodCursorRect(bulletLineBreak);
    bulletLineBreak.insertText(QStringLiteral("first"));
    QApplication::processEvents();
    const QRect bulletLineBefore = inputMethodCursorRect(bulletLineBreak);
    sendKey(bulletLineBreak, Qt::Key_Return, Qt::ShiftModifier);
    QApplication::processEvents();
    const QRect bulletContinuation = inputMethodCursorRect(bulletLineBreak);
    check(bulletContinuation.y() >= bulletLineBefore.bottom(),
          "Shift+Enter left the bullet caret painted on the preceding line");
    check(std::abs(bulletContinuation.x() - bulletTextStart.x()) <= 2,
          "Shift+Enter did not align the continuation caret with bullet text");
    commitInputMethodText(bulletLineBreak, QStringLiteral("second"));
    QApplication::processEvents();
    check(inputMethodCursorRect(bulletLineBreak).y() == bulletContinuation.y(),
          "typing after Shift+Enter jumped back to the preceding bullet line");
    check(bulletLineBreak.snapshot().document.paragraphs().size() == 1,
          "Shift+Enter split a bullet into separate list items");
    check(onlyText(bulletLineBreak) ==
              QStringLiteral("\u2022\tfirst\u2028second"),
          "Shift+Enter did not keep the continuation inside the same bullet");
    bulletLineBreak.hide();

    DocumentCanvas continuedBullet(spelling);
    continuedBullet.toggleBullets();
    continuedBullet.insertText(QStringLiteral("first"));
    sendKey(continuedBullet, Qt::Key_Return);
    check(paragraphTexts(continuedBullet.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tfirst"),
                          QStringLiteral("\u2022\t")},
          "Enter did not continue the current bullet");
    sendKey(continuedBullet, Qt::Key_Return);
    check(paragraphTexts(continuedBullet.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tfirst"), QString()},
          "Enter on an empty bullet did not exit the list");

    DocumentCanvas toggledBullet(spelling);
    toggledBullet.insertText(QStringLiteral("item"));
    toggledBullet.toggleBullets();
    check(onlyText(toggledBullet) == QStringLiteral("\u2022\titem"),
          "Bullets did not add one marker to ordinary text");
    toggledBullet.toggleBullets();
    check(onlyText(toggledBullet) == QStringLiteral("item"),
          "Bullets added a second marker instead of toggling the list off");

    DocumentCanvas pageBreak(spelling);
    pageBreak.insertText(QStringLiteral("before"));
    sendKey(pageBreak, Qt::Key_Return, Qt::ControlModifier);
    auto snapshot = pageBreak.snapshot();
    check(paragraphTexts(snapshot) ==
              QStringList{QStringLiteral("before"), QString()},
          "Ctrl+Enter did not split the paragraph at the caret");
    check(snapshot.document.paragraphs()[1].format().page_break_before == true,
          "Ctrl+Enter did not mark the new paragraph as page-break-before");

    pageBreak.undo();
    check(paragraphTexts(pageBreak.snapshot()) == QStringList{QStringLiteral("before")},
          "one undo did not remove the complete Ctrl+Enter page break");
    pageBreak.redo();
    snapshot = pageBreak.snapshot();
    check(paragraphTexts(snapshot) ==
              QStringList{QStringLiteral("before"), QString()},
          "redo did not restore the Ctrl+Enter paragraph split");
    check(snapshot.document.paragraphs()[1].format().page_break_before == true,
          "redo did not restore page-break-before formatting");
    pageBreak.insertText(QStringLiteral("after"));
    check(paragraphTexts(pageBreak.snapshot()) ==
              QStringList{QStringLiteral("before"), QStringLiteral("after")},
          "redo did not restore the caret after the page break");
}

void testSemanticListProperties(docxstudio::app::SpellChecker& spelling) {
    using docxstudio::core::ListLayout;

    DocumentCanvas continued(spelling);
    continued.toggleBullets();
    continued.insertText(QStringLiteral("first"));
    auto firstSnapshot = continued.snapshot();
    const auto& firstFormat = firstSnapshot.document.paragraphs().front().format();
    check(firstFormat.list_id && firstFormat.list_level == std::uint8_t{0} &&
              firstFormat.list_layout,
          "a ribbon-created list has no semantic identity, level, or layout");
    for (std::size_t level = 0;
         level < docxstudio::core::kListLevelCount; ++level) {
        check(firstFormat.list_layout->levels[level].bullet_indent_spaces ==
                  static_cast<std::int32_t>(level * 4) &&
                  firstFormat.list_layout->levels[level].text_indent_spaces == 2,
              "semantic list defaults are not 0,4,...,36 plus a two-space gap");
    }
    const auto listId = *firstFormat.list_id;
    sendKey(continued, Qt::Key_Return);
    auto continuedSnapshot = continued.snapshot();
    check(continuedSnapshot.document.paragraphs().size() == 2 &&
              continuedSnapshot.document.paragraphs()[1].format().list_id ==
                  listId,
          "Enter did not keep the next item in the same semantic list");

    for (int level = 1; level <= 12; ++level) {
        sendKey(continued, Qt::Key_Tab);
    }
    continuedSnapshot = continued.snapshot();
    check(continuedSnapshot.document.paragraphs()[1].format().list_level ==
              std::uint8_t{9} &&
              continued.activeListLevel() == 10,
          "Tab did not stop at the supported tenth list level");
    check(paragraphTexts(continuedSnapshot).back().startsWith(
              QString(36, QLatin1Char(' ')) +
              QStringLiteral("\u2022\t")),
          "level ten did not use the configured 36-space bullet position");

    DocumentCanvas separateLists(spelling);
    separateLists.setDocument(documentWithParagraphs({
        QStringLiteral("\u2022 first"), QStringLiteral("\u2022 second"),
        QStringLiteral("between"), QStringLiteral("\u2022 other"),
        QStringLiteral("\u2022 last")}));
    auto before = separateLists.snapshot();
    const auto firstId = before.document.paragraphs()[0].format().list_id;
    const auto secondId = before.document.paragraphs()[1].format().list_id;
    const auto otherId = before.document.paragraphs()[3].format().list_id;
    check(firstId && firstId == secondId && otherId && firstId != otherId,
          "contiguous lists were not assigned distinct stable identities");

    ListLayout custom;
    custom.levels[0].bullet_indent_spaces = 2;
    custom.levels[0].text_indent_spaces = 3;
    check(separateLists.setCurrentListLayout(custom),
          "list properties could not be applied");
    auto after = separateLists.snapshot();
    check(after.document.paragraphs()[0].format().list_layout == custom &&
              after.document.paragraphs()[1].format().list_layout == custom,
          "list properties did not reach every item in the selected list");
    check(after.document.paragraphs()[3].format().list_layout == ListLayout{} &&
              after.document.paragraphs()[4].format().list_layout == ListLayout{},
          "list properties leaked into a separate list");
    check(paragraphTexts(after)[0] == QStringLiteral("  \u2022\tfirst") &&
              paragraphTexts(after)[1] == QStringLiteral("  \u2022\tsecond") &&
              paragraphTexts(after)[3] == QStringLiteral("\u2022 other"),
          "custom bullet positions did not remain scoped to the selected list");
    separateLists.undo();
    auto undone = separateLists.snapshot();
    check(undone.document.paragraphs()[0].format().list_layout == ListLayout{} &&
              paragraphTexts(undone)[0] == QStringLiteral("\u2022 first"),
          "one Undo did not restore the complete list-property edit");

    DocumentCanvas alignedNumbers(spelling);
    alignedNumbers.resize(900, 500);
    alignedNumbers.show();
    alignedNumbers.setFocus();
    alignedNumbers.insertText(QStringLiteral("9. ninth"));
    sendKey(alignedNumbers, Qt::Key_Return);
    QApplication::processEvents();
    sendKey(alignedNumbers, Qt::Key_Home, Qt::ControlModifier);
    QApplication::processEvents();
    const QRect nineStart = inputMethodCursorRect(alignedNumbers);
    sendKey(alignedNumbers, Qt::Key_End, Qt::ControlModifier);
    sendKey(alignedNumbers, Qt::Key_Home);
    QApplication::processEvents();
    const QRect tenStart = inputMethodCursorRect(alignedNumbers);
    check(std::abs(nineStart.x() - tenStart.x()) <= 2,
          "one- and two-digit list markers do not share a text tab stop");
    alignedNumbers.hide();

    DocumentCanvas replaceWholeItem(spelling);
    replaceWholeItem.toggleBullets();
    replaceWholeItem.insertText(QStringLiteral("replace me"));
    replaceWholeItem.selectAll();
    replaceWholeItem.insertText(QStringLiteral("plain"));
    auto replaced = replaceWholeItem.snapshot();
    check(!replaced.document.paragraphs().front().format().list_id &&
              !replaceWholeItem.hasActiveList(),
          "replacing a complete list item left hidden list metadata behind");
    sendKey(replaceWholeItem, Qt::Key_Return);
    check(paragraphTexts(replaceWholeItem.snapshot()) ==
              QStringList{QStringLiteral("plain"), QString()},
          "Enter continued a list after its complete marker was replaced");

    DocumentCanvas wrapped(spelling);
    wrapped.resize(900, 500);
    wrapped.show();
    wrapped.setFocus();
    wrapped.toggleBullets();
    wrapped.insertText(QString(180, QLatin1Char('w')));
    sendKey(wrapped, Qt::Key_Home);
    QApplication::processEvents();
    const QRect wrappedFirst = inputMethodCursorRect(wrapped);
    sendKey(wrapped, Qt::Key_Down);
    QApplication::processEvents();
    const QRect wrappedNext = inputMethodCursorRect(wrapped);
    check(wrappedNext.y() > wrappedFirst.y() &&
              std::abs(wrappedNext.x() - wrappedFirst.x()) <= 2,
          "wrapped bullet text did not align with the shared text tab stop");
    wrapped.hide();
}

void testAdversarialListEditing(docxstudio::app::SpellChecker& spelling) {
    using docxstudio::core::ListLayout;

    // Enter in the middle of an item must split it into two items in the same
    // list, and the entire structural edit must be one undo transaction.
    DocumentCanvas split(spelling);
    split.toggleBullets();
    split.insertText(QStringLiteral("alpha omega"));
    sendKey(split, Qt::Key_Home, Qt::ControlModifier);
    for (int index = 0; index < 5; ++index) sendKey(split, Qt::Key_Right);
    sendKey(split, Qt::Key_Return);
    auto splitSnapshot = split.snapshot();
    check(paragraphTexts(splitSnapshot) ==
              QStringList{QStringLiteral("\u2022\talpha"),
                          QStringLiteral("\u2022\t omega")},
          "Enter in the middle of a bullet did not split the item correctly");
    check(splitSnapshot.document.paragraphs()[0].format().list_id &&
              splitSnapshot.document.paragraphs()[0].format().list_id ==
                  splitSnapshot.document.paragraphs()[1].format().list_id &&
              splitSnapshot.document.paragraphs()[0].format().list_layout ==
                  splitSnapshot.document.paragraphs()[1].format().list_layout,
          "a split bullet did not retain its list identity and properties");
    split.undo();
    check(paragraphTexts(split.snapshot()) ==
              QStringList{QStringLiteral("\u2022\talpha omega")},
          "one Undo did not restore a bullet split by Enter");
    split.redo();
    check(paragraphTexts(split.snapshot()) ==
              QStringList{QStringLiteral("\u2022\talpha"),
                          QStringLiteral("\u2022\t omega")},
          "Redo did not restore a bullet split by Enter");

    // Exiting an empty item clears all semantic list metadata, while Undo
    // restores both the marker and metadata together.
    DocumentCanvas emptyExit(spelling);
    emptyExit.toggleBullets();
    emptyExit.insertText(QStringLiteral("kept"));
    sendKey(emptyExit, Qt::Key_Return);
    const auto beforeExit = emptyExit.snapshot();
    const auto emptyListId = beforeExit.document.paragraphs()[1].format().list_id;
    check(emptyListId.has_value(),
          "the empty continued item lost its semantic list identity");
    sendKey(emptyExit, Qt::Key_Return);
    auto afterExit = emptyExit.snapshot();
    check(paragraphTexts(afterExit) ==
              QStringList{QStringLiteral("\u2022\tkept"), QString()} &&
              !afterExit.document.paragraphs()[1].format().list_id &&
              !afterExit.document.paragraphs()[1].format().list_level &&
              !afterExit.document.paragraphs()[1].format().list_layout,
          "Enter on an empty item left hidden list metadata behind");
    emptyExit.undo();
    auto restoredEmpty = emptyExit.snapshot();
    check(paragraphTexts(restoredEmpty).back() == QStringLiteral("\u2022\t") &&
              restoredEmpty.document.paragraphs()[1].format().list_id ==
                  emptyListId,
          "Undo did not restore an exited empty list item atomically");
    emptyExit.redo();
    check(!emptyExit.snapshot().document.paragraphs()[1].format().list_id,
          "Redo did not clear empty-item list metadata");

    // Tab/Shift+Tab should transform every selected item together, preserve a
    // single list identity, and undo/redo as one operation.
    DocumentCanvas selected(spelling);
    selected.setDocument(documentWithParagraphs(
        {QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}));
    selected.selectAll();
    selected.toggleBullets();
    auto levelZero = selected.snapshot();
    const auto selectedListId = levelZero.document.paragraphs()[0].format().list_id;
    check(selectedListId &&
              std::all_of(levelZero.document.paragraphs().begin(),
                          levelZero.document.paragraphs().end(),
                          [&selectedListId](const auto& paragraph) {
                              return paragraph.format().list_id == selectedListId &&
                                     paragraph.format().list_level == std::uint8_t{0};
                          }),
          "converting selected paragraphs did not create one semantic list");
    sendKey(selected, Qt::Key_Tab);
    auto levelOne = selected.snapshot();
    check(std::all_of(levelOne.document.paragraphs().begin(),
                      levelOne.document.paragraphs().end(),
                      [&selectedListId](const auto& paragraph) {
                          return paragraph.format().list_id == selectedListId &&
                                 paragraph.format().list_level == std::uint8_t{1};
                      }) &&
              paragraphTexts(levelOne) ==
                  QStringList{QStringLiteral("    \u25e6\tone"),
                              QStringLiteral("    \u25e6\ttwo"),
                              QStringLiteral("    \u25e6\tthree")},
          "Tab did not indent every selected list item as one list");
    selected.undo();
    check(paragraphTexts(selected.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tone"),
                          QStringLiteral("\u2022\ttwo"),
                          QStringLiteral("\u2022\tthree")},
          "one Undo did not restore a multi-item list indentation");
    selected.redo();
    sendKey(selected, Qt::Key_Tab, Qt::ShiftModifier);
    check(paragraphTexts(selected.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tone"),
                          QStringLiteral("\u2022\ttwo"),
                          QStringLiteral("\u2022\tthree")},
          "Shift+Tab did not outdent every selected list item");

    // Properties are list-scoped even when a selection spans multiple items,
    // and a level change must use that list's custom level geometry.
    ListLayout selectedLayout;
    selectedLayout.levels[0].bullet_indent_spaces = 3;
    selectedLayout.levels[0].text_indent_spaces = 5;
    selectedLayout.levels[1].bullet_indent_spaces = 11;
    selectedLayout.levels[1].text_indent_spaces = 2;
    check(selected.setCurrentListLayout(selectedLayout),
          "properties could not be applied to a selected multi-item list");
    const auto selectedProperties = selected.snapshot();
    check(std::all_of(selectedProperties.document.paragraphs().begin(),
                      selectedProperties.document.paragraphs().end(),
                      [&selectedLayout](const auto& paragraph) {
                          return paragraph.format().list_layout == selectedLayout;
                      }),
          "selected-list properties did not reach every selected item");
    sendKey(selected, Qt::Key_Tab);
    check(paragraphTexts(selected.snapshot()) ==
              QStringList{QStringLiteral("           \u25e6\tone"),
                          QStringLiteral("           \u25e6\ttwo"),
                          QStringLiteral("           \u25e6\tthree")},
          "Tab did not use the custom bullet position for the target level");

    // Backspace at the content start outdents nested items first. At level one
    // it removes the marker and list metadata without deleting user text.
    DocumentCanvas backspace(spelling);
    backspace.toggleBullets();
    backspace.insertText(QStringLiteral("body"));
    sendKey(backspace, Qt::Key_Tab);
    sendKey(backspace, Qt::Key_Home);
    sendKey(backspace, Qt::Key_Backspace);
    check(onlyText(backspace) == QStringLiteral("\u2022\tbody") &&
              backspace.snapshot().document.paragraphs()[0].format().list_level ==
                  std::uint8_t{0},
          "Backspace at nested-list content did not outdent first");
    sendKey(backspace, Qt::Key_Backspace);
    const auto removedMarker = backspace.snapshot();
    check(onlyText(backspace) == QStringLiteral("body") &&
              !removedMarker.document.paragraphs()[0].format().list_id &&
              !removedMarker.document.paragraphs()[0].format().list_level &&
              !removedMarker.document.paragraphs()[0].format().list_layout,
          "Backspace at level-one content did not remove list semantics");
    backspace.undo();
    check(onlyText(backspace) == QStringLiteral("\u2022\tbody") &&
              backspace.snapshot().document.paragraphs()[0].format().list_id,
          "Undo did not atomically restore a marker removed by Backspace");

    DocumentCanvas controlBackspace(spelling);
    controlBackspace.toggleBullets();
    controlBackspace.insertText(QStringLiteral("body"));
    sendKey(controlBackspace, Qt::Key_Home);
    sendKey(controlBackspace, Qt::Key_Backspace, Qt::ControlModifier);
    const auto controlBackspaceSnapshot = controlBackspace.snapshot();
    check(onlyText(controlBackspace) == QStringLiteral("body") &&
              !controlBackspaceSnapshot.document.paragraphs()[0]
                   .format().list_id,
          "Ctrl+Backspace at list content left invisible list metadata");

    // Delete at an item boundary merges with the next item without preserving
    // a second marker, and Undo restores both original items.
    DocumentCanvas boundaryDelete(spelling);
    boundaryDelete.toggleBullets();
    boundaryDelete.insertText(QStringLiteral("one"));
    sendKey(boundaryDelete, Qt::Key_Return);
    boundaryDelete.insertText(QStringLiteral("two"));
    sendKey(boundaryDelete, Qt::Key_Home, Qt::ControlModifier);
    sendKey(boundaryDelete, Qt::Key_End);
    sendKey(boundaryDelete, Qt::Key_Delete);
    check(paragraphTexts(boundaryDelete.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tonetwo")},
          "Delete at a list boundary retained the next item's marker");
    boundaryDelete.undo();
    check(paragraphTexts(boundaryDelete.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tone"),
                          QStringLiteral("\u2022\ttwo")},
          "one Undo did not restore list items merged with Delete");

    // Pasting or programmatically inserting plain multi-line text while in a
    // list should create real list items, not paragraphs with invisible list
    // metadata and no marker.
    DocumentCanvas multiline(spelling);
    multiline.toggleBullets();
    multiline.insertText(QStringLiteral("first"));
    multiline.insertText(QStringLiteral("\nsecond\nthird"));
    const auto multilineSnapshot = multiline.snapshot();
    const auto multilineListId =
        multilineSnapshot.document.paragraphs()[0].format().list_id;
    check(paragraphTexts(multilineSnapshot) ==
              QStringList{QStringLiteral("\u2022\tfirst"),
                          QStringLiteral("\u2022\tsecond"),
                          QStringLiteral("\u2022\tthird")} &&
              multilineListId &&
              std::all_of(multilineSnapshot.document.paragraphs().begin(),
                          multilineSnapshot.document.paragraphs().end(),
                          [&multilineListId](const auto& paragraph) {
                              return paragraph.format().list_id == multilineListId;
                          }),
          "multi-line insertion in a list did not create visible list items");
    multiline.undo();
    check(paragraphTexts(multiline.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tfirst")},
          "one Undo did not remove a multi-line list insertion");

    DocumentCanvas numberedMultiline(spelling);
    numberedMultiline.insertText(QStringLiteral("9. ninth"));
    numberedMultiline.insertText(QStringLiteral("\ntenth\neleventh"));
    const auto numberedMultilineSnapshot = numberedMultiline.snapshot();
    check(paragraphTexts(numberedMultilineSnapshot) ==
              QStringList{QStringLiteral("9.\tninth"),
                          QStringLiteral("10.\ttenth"),
                          QStringLiteral("11.\televenth")},
          "multi-line insertion did not increment numbered-list markers");
    const auto numberedListId =
        numberedMultilineSnapshot.document.paragraphs()[0].format().list_id;
    check(numberedListId &&
              std::all_of(
                  numberedMultilineSnapshot.document.paragraphs().begin(),
                  numberedMultilineSnapshot.document.paragraphs().end(),
                  [&numberedListId](const auto& paragraph) {
                      return paragraph.format().list_id == numberedListId;
                  }),
          "multi-line insertion did not adopt a typed numbered list");

    DocumentCanvas nestedNumberedMultiline(spelling);
    nestedNumberedMultiline.insertText(QStringLiteral("    9. ninth"));
    nestedNumberedMultiline.insertText(QStringLiteral("\ntenth\neleventh"));
    check(paragraphTexts(nestedNumberedMultiline.snapshot()) ==
              QStringList{QStringLiteral("    I.\tninth"),
                          QStringLiteral("    J.\ttenth"),
                          QStringLiteral("    K.\televenth")},
          "multi-line insertion left mixed decimal and alphabetic markers at a nested level");

    DocumentCanvas romanNumberedMultiline(spelling);
    romanNumberedMultiline.insertText(QStringLiteral("        III. third"));
    romanNumberedMultiline.insertText(QStringLiteral("\nfourth\nfifth"));
    check(paragraphTexts(romanNumberedMultiline.snapshot()) ==
              QStringList{QStringLiteral("        III.\tthird"),
                          QStringLiteral("        IV.\tfourth"),
                          QStringLiteral("        V.\tfifth")},
          "multi-line insertion did not continue uppercase Roman markers");

    DocumentCanvas initialismProse(spelling);
    initialismProse.insertText(QStringLiteral("I. am ordinary prose"));
    sendKey(initialismProse, Qt::Key_Return);
    check(paragraphTexts(initialismProse.snapshot()) ==
              QStringList{QStringLiteral("I. am ordinary prose"), QString()},
          "ordinary prose beginning with I. was misclassified as a numbered list");

    DocumentCanvas hierarchicalResequence(spelling);
    hierarchicalResequence.toggleNumbering();
    hierarchicalResequence.insertText(QStringLiteral("one"));
    sendKey(hierarchicalResequence, Qt::Key_Return);
    hierarchicalResequence.insertText(QStringLiteral("two"));
    sendKey(hierarchicalResequence, Qt::Key_Return);
    hierarchicalResequence.insertText(QStringLiteral("three"));
    sendKey(hierarchicalResequence, Qt::Key_Home, Qt::ControlModifier);
    sendKey(hierarchicalResequence, Qt::Key_Down);
    sendKey(hierarchicalResequence, Qt::Key_Tab);
    check(paragraphTexts(hierarchicalResequence.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("    A.\ttwo"),
                          QStringLiteral("2.\tthree")},
          "indenting a numbered item did not start A and resequence its later sibling");
    hierarchicalResequence.undo();
    check(paragraphTexts(hierarchicalResequence.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("2.\ttwo"),
                          QStringLiteral("3.\tthree")},
          "one Undo did not restore a hierarchical numbering change");
    hierarchicalResequence.redo();
    check(paragraphTexts(hierarchicalResequence.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("    A.\ttwo"),
                          QStringLiteral("2.\tthree")},
          "Redo did not restore a hierarchical numbering change");

    DocumentCanvas outdentedParent(spelling);
    outdentedParent.toggleNumbering();
    outdentedParent.insertText(QStringLiteral("parent"));
    sendKey(outdentedParent, Qt::Key_Return);
    sendKey(outdentedParent, Qt::Key_Tab);
    outdentedParent.insertText(QStringLiteral("first child"));
    sendKey(outdentedParent, Qt::Key_Return);
    outdentedParent.insertText(QStringLiteral("second child"));
    sendKey(outdentedParent, Qt::Key_Home, Qt::ControlModifier);
    sendKey(outdentedParent, Qt::Key_Down);
    sendKey(outdentedParent, Qt::Key_Backtab, Qt::ShiftModifier);
    check(paragraphTexts(outdentedParent.snapshot()) ==
              QStringList{QStringLiteral("1.\tparent"),
                          QStringLiteral("2.\tfirst child"),
                          QStringLiteral("    A.\tsecond child")},
          "outdenting a child did not restart its newly nested child at A");
    outdentedParent.undo();
    check(paragraphTexts(outdentedParent.snapshot()) ==
              QStringList{QStringLiteral("1.\tparent"),
                          QStringLiteral("    A.\tfirst child"),
                          QStringLiteral("    B.\tsecond child")},
          "one Undo did not restore the list before the child outdent");

    DocumentCanvas enterInNumberedMiddle(spelling);
    enterInNumberedMiddle.toggleNumbering();
    enterInNumberedMiddle.insertText(QStringLiteral("one"));
    sendKey(enterInNumberedMiddle, Qt::Key_Return);
    enterInNumberedMiddle.insertText(QStringLiteral("two"));
    sendKey(enterInNumberedMiddle, Qt::Key_Return);
    enterInNumberedMiddle.insertText(QStringLiteral("three"));
    sendKey(enterInNumberedMiddle, Qt::Key_Home, Qt::ControlModifier);
    sendKey(enterInNumberedMiddle, Qt::Key_End);
    sendKey(enterInNumberedMiddle, Qt::Key_Return);
    check(paragraphTexts(enterInNumberedMiddle.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"), QStringLiteral("2.\t"),
                          QStringLiteral("3.\ttwo"),
                          QStringLiteral("4.\tthree")},
          "Enter in the middle of a numbered list left duplicate ordinals");
    enterInNumberedMiddle.undo();
    check(paragraphTexts(enterInNumberedMiddle.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("2.\ttwo"),
                          QStringLiteral("3.\tthree")},
          "one Undo did not reverse Enter and numbered-list resequencing");

    DocumentCanvas pasteInNumberedMiddle(spelling);
    pasteInNumberedMiddle.toggleNumbering();
    pasteInNumberedMiddle.insertText(QStringLiteral("one"));
    sendKey(pasteInNumberedMiddle, Qt::Key_Return);
    pasteInNumberedMiddle.insertText(QStringLiteral("two"));
    sendKey(pasteInNumberedMiddle, Qt::Key_Home, Qt::ControlModifier);
    sendKey(pasteInNumberedMiddle, Qt::Key_End);
    pasteInNumberedMiddle.insertText(QStringLiteral("\ninserted"));
    check(paragraphTexts(pasteInNumberedMiddle.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("2.\tinserted"),
                          QStringLiteral("3.\ttwo")},
          "multi-line insertion in the middle left duplicate numbered markers");
    pasteInNumberedMiddle.undo();
    check(paragraphTexts(pasteInNumberedMiddle.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("2.\ttwo")},
          "one Undo did not reverse a numbered multi-line insertion and resequence");

    DocumentCanvas deleteNumberedBoundary(spelling);
    deleteNumberedBoundary.toggleNumbering();
    deleteNumberedBoundary.insertText(QStringLiteral("one"));
    sendKey(deleteNumberedBoundary, Qt::Key_Return);
    deleteNumberedBoundary.insertText(QStringLiteral("two"));
    sendKey(deleteNumberedBoundary, Qt::Key_Return);
    deleteNumberedBoundary.insertText(QStringLiteral("three"));
    sendKey(deleteNumberedBoundary, Qt::Key_Home, Qt::ControlModifier);
    sendKey(deleteNumberedBoundary, Qt::Key_End);
    sendKey(deleteNumberedBoundary, Qt::Key_Delete);
    check(paragraphTexts(deleteNumberedBoundary.snapshot()) ==
              QStringList{QStringLiteral("1.\tonetwo"),
                          QStringLiteral("2.\tthree")},
          "Delete at a numbered-list boundary did not resequence later items");
    deleteNumberedBoundary.undo();
    check(paragraphTexts(deleteNumberedBoundary.snapshot()) ==
              QStringList{QStringLiteral("1.\tone"),
                          QStringLiteral("2.\ttwo"),
                          QStringLiteral("3.\tthree")},
          "one Undo did not reverse a numbered boundary merge and resequence");

    // Replacing a selection spanning several items with Enter follows the
    // starting item's list semantics and leaves a usable empty item.
    DocumentCanvas spanning(spelling);
    spanning.toggleBullets();
    spanning.insertText(QStringLiteral("one"));
    sendKey(spanning, Qt::Key_Return);
    spanning.insertText(QStringLiteral("two"));
    sendKey(spanning, Qt::Key_Return);
    spanning.insertText(QStringLiteral("three"));
    sendKey(spanning, Qt::Key_Home, Qt::ControlModifier);
    sendKey(spanning, Qt::Key_Right);
    sendKey(spanning, Qt::Key_End,
            Qt::ControlModifier | Qt::ShiftModifier);
    sendKey(spanning, Qt::Key_Return);
    const auto spanningSnapshot = spanning.snapshot();
    check(paragraphTexts(spanningSnapshot) ==
              QStringList{QStringLiteral("\u2022\to"),
                          QStringLiteral("\u2022\t")} &&
              spanningSnapshot.document.paragraphs()[0].format().list_id ==
                  spanningSnapshot.document.paragraphs()[1].format().list_id,
          "Enter over a multi-item selection left an unmarked or detached item");
    sendKey(spanning, Qt::Key_Return);
    check(paragraphTexts(spanning.snapshot()) ==
              QStringList{QStringLiteral("\u2022\to"), QString()},
          "the empty item created from a spanning Enter could not exit the list");

    // Removing the marker from a middle item creates two visible lists. A
    // later property edit on either side must not jump across the intervening
    // ordinary paragraph merely because the old items once shared an ID.
    DocumentCanvas visiblySplit(spelling);
    visiblySplit.toggleBullets();
    visiblySplit.insertText(QStringLiteral("one"));
    sendKey(visiblySplit, Qt::Key_Return);
    visiblySplit.insertText(QStringLiteral("two"));
    sendKey(visiblySplit, Qt::Key_Return);
    visiblySplit.insertText(QStringLiteral("three"));
    sendKey(visiblySplit, Qt::Key_Home, Qt::ControlModifier);
    sendKey(visiblySplit, Qt::Key_End);
    sendKey(visiblySplit, Qt::Key_Right);
    visiblySplit.toggleBullets();
    check(paragraphTexts(visiblySplit.snapshot()) ==
              QStringList{QStringLiteral("\u2022\tone"), QStringLiteral("two"),
                          QStringLiteral("\u2022\tthree")},
          "toggling a middle list item off did not create an ordinary paragraph");
    sendKey(visiblySplit, Qt::Key_Home, Qt::ControlModifier);
    ListLayout firstSegmentLayout;
    firstSegmentLayout.levels[0].bullet_indent_spaces = 7;
    firstSegmentLayout.levels[0].text_indent_spaces = 2;
    check(visiblySplit.setCurrentListLayout(firstSegmentLayout),
          "properties could not be applied after splitting a visible list");
    const auto visiblySplitSnapshot = visiblySplit.snapshot();
    check(visiblySplitSnapshot.document.paragraphs()[0].format().list_layout ==
              firstSegmentLayout &&
              visiblySplitSnapshot.document.paragraphs()[2].format().list_layout ==
                  ListLayout{} &&
              visiblySplitSnapshot.document.paragraphs()[0].format().list_id !=
                  visiblySplitSnapshot.document.paragraphs()[2].format().list_id &&
              paragraphTexts(visiblySplitSnapshot)[2] ==
                  QStringLiteral("\u2022\tthree"),
          "list properties crossed an ordinary paragraph into another visible list");
}

void testMouseClickSelection(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 500);
    canvas.setDocument(
        documentWithText(QStringLiteral("first line\u2028second line")));
    canvas.show();
    QApplication::processEvents();

    sendKey(canvas, Qt::Key_Home);
    for (int index = 0; index < 14; ++index) {
        sendKey(canvas, Qt::Key_Right);
    }
    const QPoint clickPosition = inputMethodCursorRect(canvas).center();

    sendMouseEvent(canvas, QEvent::MouseButtonPress, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, clickPosition,
                   Qt::LeftButton, Qt::NoButton);
    sendMouseEvent(canvas, QEvent::MouseButtonDblClick, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    check(canvas.selectedText() == QStringLiteral("second"),
          "double-click did not select the word under the pointer");

    sendMouseEvent(canvas, QEvent::MouseButtonRelease, clickPosition,
                   Qt::LeftButton, Qt::NoButton);
    sendMouseEvent(canvas, QEvent::MouseButtonPress, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    check(canvas.selectedText() == QStringLiteral("second line"),
          "triple-click did not select only the clicked visual line");
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, clickPosition,
                   Qt::LeftButton, Qt::NoButton);
    canvas.hide();
}

void testDoubleClickSelectsAlphanumericWord(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 500);
    canvas.setDocument(documentWithText(QStringLiteral("invoice A320 total")));
    canvas.show();
    QApplication::processEvents();

    sendKey(canvas, Qt::Key_Home);
    for (int index = 0; index < 10; ++index) {
        sendKey(canvas, Qt::Key_Right);
    }
    const QPoint clickPosition = inputMethodCursorRect(canvas).center();
    sendMouseEvent(canvas, QEvent::MouseButtonPress, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, clickPosition,
                   Qt::LeftButton, Qt::NoButton);
    sendMouseEvent(canvas, QEvent::MouseButtonDblClick, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    check(canvas.selectedText() == QStringLiteral("A320"),
          "double-click did not select an alphanumeric word as one unit");
    canvas.hide();
}

void testTabEditingSemantics(docxstudio::app::SpellChecker& spelling) {
    QWidget window;
    DocumentCanvas body(spelling, &window);
    QWidget ribbonTarget(&window);
    body.setGeometry(0, 0, 600, 300);
    ribbonTarget.setGeometry(0, 320, 100, 40);
    ribbonTarget.setFocusPolicy(Qt::StrongFocus);
    window.resize(640, 380);
    window.show();
    body.setFocus();
    QApplication::processEvents();
    check(body.hasFocus(), "canvas could not receive focus for the Tab test");

    body.insertText(QStringLiteral("left"));
    sendKey(body, Qt::Key_Tab);
    body.insertText(QStringLiteral("right"));
    check(body.hasFocus(), "Tab moved focus from the document to the ribbon");
    check(onlyText(body) == QStringLiteral("left\tright"),
          "Tab in ordinary text did not insert an editor tab");

    DocumentCanvas list(spelling);
    list.toggleBullets();
    list.insertText(QStringLiteral("item"));
    sendKey(list, Qt::Key_Tab);
    check(onlyText(list) == QStringLiteral("    \u25e6\titem"),
          "Tab did not indent the current list item");
    sendKey(list, Qt::Key_Tab, Qt::ShiftModifier);
    check(onlyText(list) == QStringLiteral("\u2022\titem"),
          "Shift+Tab did not outdent the current list item");
    sendKey(list, Qt::Key_Tab);
    check(onlyText(list) == QStringLiteral("    \u25e6\titem"),
          "Tab did not re-indent the current list item");
    sendKey(list, Qt::Key_Backtab, Qt::ShiftModifier);
    check(onlyText(list) == QStringLiteral("\u2022\titem"),
          "Backtab did not preserve Shift+Tab list outdent semantics");
    window.hide();
}

void testGraphemeBackspace(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas combining(spelling);
    combining.insertText(QStringLiteral("Ae\u0301"));
    sendKey(combining, Qt::Key_Backspace);
    check(onlyText(combining) == QStringLiteral("A"),
          "Backspace split a base character from its combining mark");

    DocumentCanvas emoji(spelling);
    emoji.insertText(QString::fromUtf8("X\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb"));
    sendKey(emoji, Qt::Key_Backspace);
    check(onlyText(emoji) == QStringLiteral("X"),
          "Backspace split an emoji ZWJ grapheme cluster");
}

void testClipboard(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    QApplication::clipboard()->clear();
    canvas.insertText(QStringLiteral("clipboard text"));
    sendKey(canvas, Qt::Key_A, Qt::ControlModifier);
    sendKey(canvas, Qt::Key_C, Qt::ControlModifier);
    check(QApplication::clipboard()->text() == QStringLiteral("clipboard text"),
          "copy shortcut did not populate the clipboard");

    sendKey(canvas, Qt::Key_X, Qt::ControlModifier);
    check(onlyText(canvas).isEmpty(),
          "cut shortcut did not remove the selected text");
    sendKey(canvas, Qt::Key_V, Qt::ControlModifier);
    check(onlyText(canvas) == QStringLiteral("clipboard text"),
          "paste shortcut did not restore clipboard text");
}

void testInputMethodCommit(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    commitInputMethodText(canvas, QString::fromUtf8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));
    check(onlyText(canvas) == QString::fromUtf8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"),
          "IME commit text did not reach the semantic model");
}

void testFindAndReplace(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas wrapped(spelling);
    wrapped.insertText(QStringLiteral("one two one"));
    check(wrapped.findNext(QStringLiteral("one")),
          "find-next did not wrap within its starting paragraph");
    check(wrapped.selectedText() == QStringLiteral("one"),
          "wrapped find-next selected the wrong text");
    const auto wrapSnapshot = wrapped.snapshot();
    const auto wrapSelection = wrapped.selection();
    check(wrapSelection.anchor.paragraph_id ==
              wrapSnapshot.document.paragraphs().front().id() &&
              wrapSelection.anchor.utf16_offset == 0 &&
              wrapSelection.focus.utf16_offset == 3,
          "wrapped find-next selected the wrong semantic range");

    DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("Alpha beta ALPHA beta"));
    sendKey(canvas, Qt::Key_Home);
    check(canvas.findNext(QStringLiteral("beta")),
          "find-next did not find an existing term");
    check(canvas.selectedText() == QStringLiteral("beta"),
          "find-next selected the wrong range");

    const int replacements =
        canvas.replaceAll(QStringLiteral("alpha"), QStringLiteral("omega"));
    check(replacements == 2,
          "case-insensitive replace-all returned the wrong replacement count");
    check(onlyText(canvas) == QStringLiteral("omega beta omega beta"),
          "replace-all produced incorrect semantic text");

    DocumentCanvas shorter(spelling);
    shorter.insertText(QStringLiteral("alpha"));
    check(shorter.replaceAll(QStringLiteral("alpha"), QStringLiteral("x")) == 1,
          "shortening replace-all did not replace its match");
    shorter.insertText(QStringLiteral("!"));
    check(onlyText(shorter) == QStringLiteral("x!"),
          "shortening replace-all left an invalid or shifted caret");
    shorter.undo();
    shorter.undo();
    check(onlyText(shorter) == QStringLiteral("alpha"),
          "undo did not restore text before shortening replace-all");
    shorter.redo();
    shorter.insertText(QStringLiteral("?"));
    check(onlyText(shorter) == QStringLiteral("x?"),
          "redo did not restore the transformed replace-all caret");
}

void testReverseSelectionFormatting(docxstudio::app::SpellChecker& spelling) {
    const QColor blue(QStringLiteral("#336699"));
    DocumentCanvas canvas(spelling);
    canvas.setDocument(documentWithColor(
        QStringLiteral("Atarget"), 1, 7,
        static_cast<std::uint32_t>(blue.rgba())));
    sendKey(canvas, Qt::Key_End, Qt::ControlModifier);
    for (int index = 0; index < 6; ++index) {
        sendKey(canvas, Qt::Key_Left, Qt::ShiftModifier);
    }
    check(canvas.selectedText() == QStringLiteral("target"),
          "reverse selection setup selected the wrong text");
    check(canvas.currentTextColor().rgba() == blue.rgba(),
          "reverse selection exposed the format outside its start boundary");
    canvas.insertText(QStringLiteral("word"));
    check(onlyText(canvas) == QStringLiteral("Aword"),
          "reverse selection replacement produced incorrect text");
    check(formatAt(canvas, 2).foreground_argb ==
              static_cast<std::uint32_t>(blue.rgba()),
          "reverse selection replacement lost the selected run color");
}

void testDirtyAndLayoutHistory(docxstudio::app::SpellChecker& spelling) {
    const QColor blue(QStringLiteral("#336699"));
    DocumentCanvas noOp(spelling);
    noOp.setDocument(documentWithColor(
        QStringLiteral("colored"), 0, 7,
        static_cast<std::uint32_t>(blue.rgba())));
    noOp.selectAll();
    noOp.setForeground(blue);
    check(!noOp.isModified() && !noOp.hasNonTextChanges(),
          "setting an already-present format dirtied an imported document");
    sendKey(noOp, Qt::Key_Home);
    noOp.insertText(QStringLiteral("X"));
    check(noOp.isModified() && !noOp.hasNonTextChanges(),
          "safe inherited-color typing was poisoned by a no-op format");

    DocumentCanvas undoneFormat(spelling);
    undoneFormat.setDocument(documentWithText(QStringLiteral("plain")));
    undoneFormat.selectAll();
    undoneFormat.toggleBold();
    check(undoneFormat.hasNonTextChanges(),
          "real format change was not classified as non-text");
    undoneFormat.undo();
    check(!undoneFormat.isModified() && !undoneFormat.hasNonTextChanges(),
          "undo did not restore the clean formatting baseline");
    sendKey(undoneFormat, Qt::Key_Home);
    undoneFormat.insertText(QStringLiteral("X"));
    check(!undoneFormat.hasNonTextChanges(),
          "undone formatting change poisoned a later safe text edit");

    DocumentCanvas layout(spelling);
    layout.insertText(QStringLiteral("hello"));
    layout.setMarginsPoints(36, 36, 36, 36);
    check(layout.marginTopPoints() == 36 && layout.hasPageLayoutChanges(),
          "margin change was not recorded as page-layout work");
    layout.undo();
    check(onlyText(layout) == QStringLiteral("hello") &&
              layout.marginTopPoints() == 72,
          "Undo skipped the latest margin change and removed earlier text");
    layout.undo();
    check(onlyText(layout).isEmpty(),
          "second Undo did not remove the earlier text transaction");
    layout.redo();
    layout.redo();
    check(onlyText(layout) == QStringLiteral("hello") &&
              layout.marginTopPoints() == 36,
          "Redo did not restore text and margin transactions in order");

    DocumentCanvas noOpLayout(spelling);
    noOpLayout.setMarginsPoints(72, 72, 72, 72);
    check(!noOpLayout.isModified() && !noOpLayout.hasPageLayoutChanges(),
          "no-op margin preset dirtied a new document");
}

void testPreviewCursorAndFormatting(docxstudio::app::SpellChecker& spelling) {
    const QColor blue(QStringLiteral("#336699"));
    DocumentCanvas styled(spelling);
    styled.setDocument(documentWithText(QStringLiteral("plain")));
    styled.selectAll();
    const auto styledBase = styled.snapshot();
    const auto styledId = styledBase.document.paragraphs().front().id();
    docxstudio::core::CharacterFormatDelta delta;
    delta.bold = docxstudio::core::PropertyDelta<bool>::set(true);
    delta.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(
            static_cast<std::uint32_t>(blue.rgba()));
    QString summary;
    QString error;
    int formatSignals = 0;
    QObject::connect(&styled, &DocumentCanvas::cursorFormatChanged,
                     [&formatSignals](const QString&, double, const QColor&) {
        ++formatSignals;
    });
    check(styled.createOperationsPreview(
              styledBase.revision,
              {docxstudio::core::SetCharacterFormat{
                  {{styledId, 0}, {styledId, 5}}, delta}},
              QStringLiteral("Style text"), summary, error),
          "could not create formatting preview");
    check(styled.acceptPreview(error), "could not accept formatting preview");
    check(formatSignals > 0 && styled.currentTextColor().rgba() == blue.rgba(),
          "accepted preview did not refresh active format state");
    styled.insertText(QStringLiteral("R"));
    check(onlyText(styled) == QStringLiteral("R"),
          "typing did not replace the selection retained after preview");
    check(formatAt(styled, 1).bold == true &&
              formatAt(styled, 1).foreground_argb ==
                  static_cast<std::uint32_t>(blue.rgba()),
          "typing after accepted preview used stale formatting");

    DocumentCanvas deleted(spelling);
    deleted.setDocument(documentWithText(QStringLiteral("abcdef")));
    sendKey(deleted, Qt::Key_Home);
    for (int index = 0; index < 3; ++index) sendKey(deleted, Qt::Key_Right);
    const auto deleteBase = deleted.snapshot();
    const auto deleteId = deleteBase.document.paragraphs().front().id();
    check(deleted.createOperationsPreview(
              deleteBase.revision,
              {docxstudio::core::DeleteRange{
                  {{deleteId, 0}, {deleteId, 6}}}},
              QStringLiteral("Delete text"), summary, error),
          "could not create deletion preview");
    check(deleted.acceptPreview(error), "could not accept deletion preview");
    deleted.insertText(QStringLiteral("X"));
    check(onlyText(deleted) == QStringLiteral("X"),
          "accepted deletion preview left an invalid caret");
}

void testPreviewLocksLiveEditing(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("live text"));
    canvas.markSaved();
    canvas.selectAll();
    QString summary;
    QString error;
    check(canvas.createReplacementPreview(
              QStringLiteral("preview text"), summary, error),
          "could not create edit-lock preview");
    const auto liveBefore = canvas.snapshot();
    const auto selectionBefore = canvas.selection();
    const double marginBefore = canvas.marginTopPoints();
    int rejected = 0;
    QObject::connect(&canvas, &DocumentCanvas::operationFailed,
                     [&rejected](const QString&) { ++rejected; });

    canvas.insertText(QStringLiteral("hidden"));
    canvas.toggleBold();
    canvas.setMarginsPoints(36, 36, 36, 36);
    canvas.undo();
    sendKey(canvas, Qt::Key_Left);

    check(canvas.snapshot().document == liveBefore.document &&
              canvas.snapshot().revision == liveBefore.revision,
          "editing during a preview changed the hidden live document");
    check(canvas.selection() == selectionBefore,
          "navigation during a preview changed the live selection");
    check(canvas.marginTopPoints() == marginBefore &&
              !canvas.hasPageLayoutChanges(),
          "page layout changed behind an active preview");
    check(canvas.hasPreview(),
          "a rejected live edit unexpectedly discarded the preview");
    check(rejected >= 4,
          "preview-locked mutations did not report why they were rejected");
    check(!canvas.exportPdf(QStringLiteral("/tmp/should-not-exist.pdf"), error) &&
              error.contains(QStringLiteral("preview"), Qt::CaseInsensitive),
          "PDF export did not reject an unaccepted preview");
    check(canvas.acceptPreview(error),
          "preview could not be accepted after rejected live edits");
    check(onlyText(canvas) == QStringLiteral("preview text"),
          "accepted preview was corrupted by rejected live edits");
}

void testNoOpPreviewsAreRejected(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas emptyInsertion(spelling);
    emptyInsertion.toggleBold();
    const auto emptyBase = emptyInsertion.snapshot();
    const auto emptyParagraph =
        emptyBase.document.paragraphs().front().id();
    QString summary;
    QString error;
    check(!emptyInsertion.createOperationsPreview(
              emptyBase.revision,
              {docxstudio::core::InsertText{
                  {emptyParagraph, 0}, u"", std::nullopt}},
              QStringLiteral("No-op insertion"), summary, error),
          "an empty insertion unexpectedly created a preview");
    check(error.contains(QStringLiteral("does not change"),
                         Qt::CaseInsensitive) &&
              !emptyInsertion.hasPreview(),
          "a rejected no-op preview did not return a clear error");
    emptyInsertion.insertText(QStringLiteral("X"));
    check(formatAt(emptyInsertion, 1).bold.value_or(false),
          "rejecting a no-op preview cleared the active typing format");

    DocumentCanvas identicalReplacement(spelling);
    identicalReplacement.setDocument(documentWithText(QStringLiteral("same")));
    identicalReplacement.selectAll();
    error.clear();
    check(!identicalReplacement.createReplacementPreview(
              QStringLiteral("same"), summary, error),
          "an identical replacement unexpectedly created a preview");
    check(error.contains(QStringLiteral("does not change"),
                         Qt::CaseInsensitive) &&
              !identicalReplacement.hasPreview(),
          "identical replacement did not report a no-change preview");
    check(identicalReplacement.selectedText() == QStringLiteral("same") &&
              !identicalReplacement.isModified(),
          "rejecting an identical replacement changed selection or dirty state");
}

void testSelectionCollapse(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas collapseLeft(spelling);
    collapseLeft.insertText(QStringLiteral("abcdef"));
    const auto leftParagraph = collapseLeft.snapshot().document.paragraphs().front().id();
    sendKey(collapseLeft, Qt::Key_Home);
    sendKey(collapseLeft, Qt::Key_Right);
    sendKey(collapseLeft, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(collapseLeft, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(collapseLeft, Qt::Key_Right, Qt::ShiftModifier);
    check(collapseLeft.selectedText() == QStringLiteral("bcd"),
          "selection setup for left-collapse was incorrect");
    sendKey(collapseLeft, Qt::Key_Left);
    checkCollapsedCaret(collapseLeft, leftParagraph, 1,
                        "Left did not collapse the selection to its start");

    DocumentCanvas collapseRight(spelling);
    collapseRight.insertText(QStringLiteral("abcdef"));
    const auto rightParagraph = collapseRight.snapshot().document.paragraphs().front().id();
    sendKey(collapseRight, Qt::Key_Home);
    sendKey(collapseRight, Qt::Key_Right);
    sendKey(collapseRight, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(collapseRight, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(collapseRight, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(collapseRight, Qt::Key_Right);
    checkCollapsedCaret(collapseRight, rightParagraph, 4,
                        "Right did not collapse the selection to its end");
}

void testWordAndDocumentNavigation(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas words(spelling);
    words.insertText(QStringLiteral("one two three"));
    const auto wordParagraph = words.snapshot().document.paragraphs().front().id();
    sendKey(words, Qt::Key_Home);
    sendKey(words, Qt::Key_Right, Qt::ControlModifier);
    checkCollapsedCaret(words, wordParagraph, 4,
                        "Ctrl+Right did not move to the next word start");
    sendKey(words, Qt::Key_Right, Qt::ControlModifier);
    checkCollapsedCaret(words, wordParagraph, 8,
                        "second Ctrl+Right did not move to the next word start");
    sendKey(words, Qt::Key_Left, Qt::ControlModifier);
    checkCollapsedCaret(words, wordParagraph, 4,
                        "Ctrl+Left did not move to the previous word start");

    DocumentCanvas deleteForward(spelling);
    deleteForward.insertText(QStringLiteral("one two three"));
    sendKey(deleteForward, Qt::Key_Home);
    sendKey(deleteForward, Qt::Key_Delete, Qt::ControlModifier);
    check(onlyText(deleteForward) == QStringLiteral("two three"),
          "Ctrl+Delete did not delete through the next word boundary");

    DocumentCanvas deleteBackward(spelling);
    deleteBackward.insertText(QStringLiteral("one two three"));
    sendKey(deleteBackward, Qt::Key_Backspace, Qt::ControlModifier);
    check(onlyText(deleteBackward) == QStringLiteral("one two "),
          "Ctrl+Backspace did not delete to the previous word boundary");

    DocumentCanvas documentEdges(spelling);
    documentEdges.insertText(QStringLiteral("first\nmiddle\nlast"));
    const auto edgeSnapshot = documentEdges.snapshot();
    const auto firstId = edgeSnapshot.document.paragraphs().front().id();
    const auto lastId = edgeSnapshot.document.paragraphs().back().id();
    sendKey(documentEdges, Qt::Key_Home, Qt::ControlModifier);
    checkCollapsedCaret(documentEdges, firstId, 0,
                        "Ctrl+Home did not move to the document start");
    sendKey(documentEdges, Qt::Key_End, Qt::ControlModifier);
    checkCollapsedCaret(documentEdges, lastId, 4,
                        "Ctrl+End did not move to the document end");
}

void testWrappedLineAffinity(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 500);
    canvas.show();
    const QString words = QStringLiteral(
        "one two three four five six seven eight nine ten eleven twelve "
        "thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty ");
    canvas.insertText(words.repeated(6));
    QApplication::processEvents();

    sendKey(canvas, Qt::Key_Home);
    const auto paragraphId = canvas.snapshot().document.paragraphs().front().id();
    const QRect homeRect = inputMethodCursorRect(canvas);
    sendKey(canvas, Qt::Key_Down);
    const auto firstDown = canvas.selection().focus.utf16_offset;
    check(firstDown > 0, "Down did not reach the second wrapped line");
    check(std::abs(inputMethodCursorRect(canvas).x() - homeRect.x()) <= 1,
          "Down did not preserve the caret's visual column");
    sendKey(canvas, Qt::Key_Up);
    checkCollapsedCaret(canvas, paragraphId, 0,
                        "Up did not round-trip from a shared wrapped-line boundary");

    sendKey(canvas, Qt::Key_Down);
    check(canvas.selection().focus.utf16_offset == firstDown,
          "repeated Down reached a different second-line position");
    sendKey(canvas, Qt::Key_Down);
    const auto secondDown = canvas.selection().focus.utf16_offset;
    check(secondDown > firstDown, "second Down did not reach the third wrapped line");
    check(std::abs(inputMethodCursorRect(canvas).x() - homeRect.x()) <= 1,
          "repeated Down lost the preferred visual column");
    sendKey(canvas, Qt::Key_Up);
    checkCollapsedCaret(canvas, paragraphId, firstDown,
                        "Up did not restore the preceding wrapped-line position");
    canvas.hide();
}

void testTypingUndoGroups(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas grouped(spelling);
    sendTextKey(grouped, Qt::Key_A, QStringLiteral("a"));
    sendTextKey(grouped, Qt::Key_B, QStringLiteral("b"));
    sendTextKey(grouped, Qt::Key_C, QStringLiteral("c"));
    check(onlyText(grouped) == QStringLiteral("abc"),
          "printable typing did not reach the document");
    grouped.undo();
    check(onlyText(grouped).isEmpty(),
          "one Undo did not remove a consecutive typing group");
    grouped.redo();
    check(onlyText(grouped) == QStringLiteral("abc"),
          "one Redo did not restore a consecutive typing group");

    DocumentCanvas navigationBoundary(spelling);
    sendTextKey(navigationBoundary, Qt::Key_A, QStringLiteral("a"));
    sendTextKey(navigationBoundary, Qt::Key_B, QStringLiteral("b"));
    sendTextKey(navigationBoundary, Qt::Key_C, QStringLiteral("c"));
    sendKey(navigationBoundary, Qt::Key_Left);
    sendTextKey(navigationBoundary, Qt::Key_X, QStringLiteral("X"));
    navigationBoundary.undo();
    check(onlyText(navigationBoundary) == QStringLiteral("abc"),
          "typing after navigation merged into the preceding typing group");
    navigationBoundary.undo();
    check(onlyText(navigationBoundary).isEmpty(),
          "the pre-navigation typing group was not one transaction");

    DocumentCanvas timeoutBoundary(spelling);
    sendTextKey(timeoutBoundary, Qt::Key_A, QStringLiteral("a"));
    QEventLoop wait;
    QTimer::singleShot(1100, &wait, &QEventLoop::quit);
    wait.exec();
    sendTextKey(timeoutBoundary, Qt::Key_B, QStringLiteral("b"));
    timeoutBoundary.undo();
    check(onlyText(timeoutBoundary) == QStringLiteral("a"),
          "typing after one second merged into the expired typing group");

    DocumentCanvas savedBoundary(spelling);
    sendTextKey(savedBoundary, Qt::Key_A, QStringLiteral("a"));
    sendTextKey(savedBoundary, Qt::Key_B, QStringLiteral("b"));
    savedBoundary.markSaved();
    sendTextKey(savedBoundary, Qt::Key_C, QStringLiteral("c"));
    savedBoundary.undo();
    check(onlyText(savedBoundary) == QStringLiteral("ab") &&
              !savedBoundary.isModified(),
          "Undo after saving did not restore the clean saved typing boundary");
    savedBoundary.redo();
    check(onlyText(savedBoundary) == QStringLiteral("abc") &&
              savedBoundary.isModified(),
          "Redo after saving did not restore the dirty post-save typing group");

    DocumentCanvas previewBoundary(spelling);
    sendTextKey(previewBoundary, Qt::Key_A, QStringLiteral("a"));
    sendTextKey(previewBoundary, Qt::Key_B, QStringLiteral("b"));
    QString summary;
    QString error;
    check(previewBoundary.createReplacementPreview(
              QStringLiteral("P"), summary, error),
          "could not create a preview after a typing group");
    check(previewBoundary.acceptPreview(error),
          "could not accept a preview after a typing group");
    sendTextKey(previewBoundary, Qt::Key_C, QStringLiteral("c"));
    previewBoundary.undo();
    check(onlyText(previewBoundary) == QStringLiteral("abP"),
          "typing after an accepted preview merged into the preview transaction");
    previewBoundary.undo();
    check(onlyText(previewBoundary) == QStringLiteral("ab"),
          "accepted preview was not a separate undo transaction");
}

void testAltGrPrintableText(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    sendTextKey(canvas, Qt::Key_Q, QStringLiteral("@"),
                Qt::ControlModifier | Qt::AltModifier);
    check(onlyText(canvas) == QStringLiteral("@"),
          "Ctrl+Alt/AltGr printable text was dropped");

    sendTextKey(canvas, Qt::Key_T, QStringLiteral("t"),
                Qt::ControlModifier | Qt::AltModifier);
    check(onlyText(canvas) == QStringLiteral("@"),
          "an ordinary Ctrl+Alt shortcut was inserted as text");
}

void testUndoRedoCaret(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas undoCaret(spelling);
    undoCaret.insertText(QStringLiteral("abcd"));
    sendKey(undoCaret, Qt::Key_Home);
    sendKey(undoCaret, Qt::Key_Right);
    sendKey(undoCaret, Qt::Key_Right);
    undoCaret.insertText(QStringLiteral("X"));
    undoCaret.undo();
    undoCaret.insertText(QStringLiteral("U"));
    check(onlyText(undoCaret) == QStringLiteral("abUcd"),
          "undo did not restore the caret position before the undone edit");

    DocumentCanvas redoCaret(spelling);
    redoCaret.insertText(QStringLiteral("abcd"));
    sendKey(redoCaret, Qt::Key_Home);
    sendKey(redoCaret, Qt::Key_Right);
    sendKey(redoCaret, Qt::Key_Right);
    redoCaret.insertText(QStringLiteral("X"));
    redoCaret.undo();
    redoCaret.redo();
    redoCaret.insertText(QStringLiteral("R"));
    check(onlyText(redoCaret) == QStringLiteral("abXRcd"),
          "redo did not restore the caret position after the redone edit");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    docxstudio::app::SpellChecker spelling;

    testDefaultAndPlainTyping(spelling);
    testEditorDefaultsAffectLayout(spelling);
    testDefaultParagraphFlow(spelling);
    testDefaultTextRendersBlack(spelling);
    testSpellcheckCommitsCompletedWords(spelling);
    testColorTyping(spelling);
    testHighlightRemoval(spelling);
    testColorAdjustmentUndoGrouping(spelling);
    testImportedColorInheritance(spelling);
    testCollapsedBoldToggle(spelling);
    testBaselineToggle(spelling);
    testFontSizeCommit(spelling);
    testBreakKeys(spelling);
    testSemanticListProperties(spelling);
    testAdversarialListEditing(spelling);
    testMouseClickSelection(spelling);
    testDoubleClickSelectsAlphanumericWord(spelling);
    testTabEditingSemantics(spelling);
    testGraphemeBackspace(spelling);
    testClipboard(spelling);
    testInputMethodCommit(spelling);
    testFindAndReplace(spelling);
    testReverseSelectionFormatting(spelling);
    testDirtyAndLayoutHistory(spelling);
    testPreviewCursorAndFormatting(spelling);
    testPreviewLocksLiveEditing(spelling);
    testNoOpPreviewsAreRejected(spelling);
    testSelectionCollapse(spelling);
    testWordAndDocumentNavigation(spelling);
    testWrappedLineAffinity(spelling);
    testTypingUndoGroups(spelling);
    testAltGrPrintableText(spelling);
    testUndoRedoCaret(spelling);

    std::cout << "editor interaction tests passed\n";
    return 0;
}

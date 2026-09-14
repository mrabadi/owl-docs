#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/SpellChecker.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>
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

void sendWheel(DocumentCanvas& canvas, int verticalDelta,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier,
               int pixelDelta = 0) {
    const QPoint position = canvas.viewport()->rect().center();
    const QPoint globalPosition = canvas.viewport()->mapToGlobal(position);
    QWheelEvent event(
        QPointF(position), QPointF(globalPosition), QPoint(0, pixelDelta),
        QPoint(0, verticalDelta), Qt::NoButton, modifiers,
        Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas.viewport(), &event);
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

    DocumentCanvas transientTyping(spelling);
    transientTyping.insertText(QStringLiteral("x"));
    transientTyping.selectAll();
    transientTyping.setHighlight(yellow);
    sendKey(transientTyping, Qt::Key_End);
    const auto beforeTransientClear = transientTyping.snapshot();
    transientTyping.clearHighlight();
    check(transientTyping.snapshot().revision ==
              beforeTransientClear.revision,
          "clearing a transient caret format created an invisible document edit");
    transientTyping.insertText(QStringLiteral("p"));
    check(formatAt(transientTyping, 1).highlight_argb ==
              static_cast<std::uint32_t>(yellow.rgba()) &&
              !formatAt(transientTyping, 2).highlight_argb.has_value(),
          "typing after transient No Highlight inherited the adjacent highlight");

    DocumentCanvas transientEnter(spelling);
    transientEnter.insertText(QStringLiteral("x"));
    transientEnter.selectAll();
    transientEnter.setHighlight(yellow);
    sendKey(transientEnter, Qt::Key_End);
    transientEnter.clearHighlight();
    sendKey(transientEnter, Qt::Key_Return);
    auto entered = transientEnter.snapshot();
    check(entered.document.paragraphs().size() == 2 &&
              entered.document.paragraphs()[1].text().empty() &&
              entered.document.paragraphs()[1]
                  .paragraphMarkCharacterFormat().empty(),
          "Enter after transient No Highlight reintroduced the adjacent highlight");
    transientEnter.insertText(QStringLiteral("p"));
    entered = transientEnter.snapshot();
    check(!entered.document.paragraphs()[1]
               .characterFormatAt(1).highlight_argb.has_value(),
          "typing on a new line after transient No Highlight was highlighted");
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

    DocumentCanvas nonempty(spelling);
    nonempty.insertText(QStringLiteral("plain"));
    const auto beforeTransientToggle = nonempty.snapshot();
    nonempty.toggleBold();
    const auto afterTransientToggle = nonempty.snapshot();
    check(afterTransientToggle.revision == beforeTransientToggle.revision &&
              afterTransientToggle.document == beforeTransientToggle.document &&
              afterTransientToggle.document.paragraphs().front()
                  .paragraphMarkCharacterFormat().empty(),
          "collapsed formatting in non-empty text created an invisible document edit");
    nonempty.insertText(QStringLiteral("B"));
    check(formatAt(nonempty, 6).bold == true,
          "transient non-empty caret formatting did not affect later typing");
}

void testEmptyParagraphTypingFormatSurvivesNavigation(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 520);
    canvas.show();
    canvas.setFocus();
    QApplication::processEvents();

    QString ribbonFamily;
    double ribbonPointSize = 0.0;
    QColor ribbonTextColor;
    bool ribbonBold = false;
    bool ribbonItalic = false;
    bool ribbonUnderline = false;
    QObject::connect(
        &canvas, &DocumentCanvas::cursorFormatChanged,
        [&ribbonFamily, &ribbonPointSize, &ribbonTextColor](
            const QString& family, double pointSize, const QColor& color) {
            ribbonFamily = family;
            ribbonPointSize = pointSize;
            ribbonTextColor = color;
        });
    QObject::connect(
        &canvas, &DocumentCanvas::cursorStyleChanged,
        [&ribbonBold, &ribbonItalic, &ribbonUnderline](
            bool bold, bool italic, bool underline, bool, bool, bool) {
            ribbonBold = bold;
            ribbonItalic = italic;
            ribbonUnderline = underline;
        });

    canvas.insertText(QStringLiteral("Anchor"));
    QApplication::processEvents();
    const QPoint firstParagraphPoint = inputMethodCursorRect(canvas).center();
    sendKey(canvas, Qt::Key_Return);
    auto snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().size() == 2 &&
              snapshot.document.paragraphs()[1].text().empty(),
          "Enter did not create the empty paragraph format fixture");
    const auto firstParagraphId = snapshot.document.paragraphs()[0].id();
    const auto emptyParagraphId = snapshot.document.paragraphs()[1].id();

    const QString chosenFamily = QStringLiteral("DejaVu Serif");
    constexpr double chosenPointSize = 19.0;
    const QColor chosenColor(QStringLiteral("#3157a4"));
    const QColor chosenHighlight(QStringLiteral("#f6d32d"));
    canvas.setFontFamily(chosenFamily);
    canvas.setFontPointSize(chosenPointSize);
    canvas.setForeground(chosenColor);
    canvas.setHighlight(chosenHighlight);
    canvas.toggleBold();
    canvas.toggleItalic();
    canvas.toggleUnderline();
    QApplication::processEvents();

    snapshot = canvas.snapshot();
    const auto& emptyMark = snapshot.document.paragraphs()[1]
                                .paragraphMarkCharacterFormat();
    check(emptyMark.font_family == chosenFamily.toStdString() &&
              emptyMark.font_size_half_points == 38 &&
              emptyMark.foreground_argb ==
                  static_cast<std::uint32_t>(chosenColor.rgba()) &&
              emptyMark.highlight_argb ==
                  static_cast<std::uint32_t>(chosenHighlight.rgba()) &&
              emptyMark.bold == true && emptyMark.italic == true &&
              emptyMark.underline ==
                  docxstudio::core::UnderlineStyle::single,
          "formatting an empty paragraph was not stored in its paragraph mark");
    const QPoint emptyParagraphPoint = inputMethodCursorRect(canvas).center();

    // Exercise the reported failure path with real hit-tested mouse clicks:
    // leave the empty paragraph, then return to it.
    sendMouseEvent(canvas, QEvent::MouseButtonPress, firstParagraphPoint,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, firstParagraphPoint,
                   Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    check(canvas.selection().focus.paragraph_id == firstParagraphId,
          "the away click did not leave the formatted empty paragraph");

    sendMouseEvent(canvas, QEvent::MouseButtonPress, emptyParagraphPoint,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, emptyParagraphPoint,
                   Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    check(canvas.selection().focus.paragraph_id == emptyParagraphId &&
              canvas.selection().focus.utf16_offset == 0,
          "the return click did not restore the empty paragraph caret");

    canvas.refreshCursorFormat();
    check(canvas.currentFontFamily() == chosenFamily &&
              canvas.currentFontPointSize() == chosenPointSize &&
              canvas.currentTextColor().rgba() == chosenColor.rgba() &&
              canvas.currentHighlightColor().rgba() ==
                  chosenHighlight.rgba(),
          "current typing format was lost after returning to the empty paragraph");
    check(ribbonFamily == chosenFamily &&
              ribbonPointSize == chosenPointSize &&
              ribbonTextColor.rgba() == chosenColor.rgba() && ribbonBold &&
              ribbonItalic && ribbonUnderline,
          "cursor format signals did not restore the ribbon state for the empty paragraph");

    canvas.insertText(QStringLiteral("Styled"));
    snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs()[1].text() == u"Styled",
          "typing after returning to the empty paragraph inserted incorrectly");
    const auto insertedFormat =
        snapshot.document.paragraphs()[1].characterFormatAt(6);
    check(insertedFormat.font_family == chosenFamily.toStdString() &&
              insertedFormat.font_size_half_points == 38 &&
              insertedFormat.foreground_argb ==
                  static_cast<std::uint32_t>(chosenColor.rgba()) &&
              insertedFormat.highlight_argb ==
                  static_cast<std::uint32_t>(chosenHighlight.rgba()) &&
              insertedFormat.bold == true && insertedFormat.italic == true &&
              insertedFormat.underline ==
                  docxstudio::core::UnderlineStyle::single,
          "text inserted after returning did not consume the paragraph-mark format");

    sendKey(canvas, Qt::Key_Return);
    const auto afterEnter = canvas.snapshot();
    check(afterEnter.document.paragraphs().size() == 3 &&
              afterEnter.document.paragraphs()[2].text().empty(),
          "Enter after styled text did not create a new empty paragraph");
    const auto inheritedMark = afterEnter.document.paragraphs()[2]
                                   .paragraphMarkCharacterFormat();
    check(inheritedMark.font_family == chosenFamily.toStdString() &&
              inheritedMark.font_size_half_points == 38 &&
              inheritedMark.foreground_argb ==
                  static_cast<std::uint32_t>(chosenColor.rgba()) &&
              inheritedMark.highlight_argb ==
                  static_cast<std::uint32_t>(chosenHighlight.rgba()) &&
              inheritedMark.bold == true && inheritedMark.italic == true &&
              inheritedMark.underline ==
                  docxstudio::core::UnderlineStyle::single,
          "Enter-created paragraph did not inherit the active typing format");

    canvas.undo();
    check(canvas.snapshot().document.paragraphs().size() == 2,
          "Undo did not remove the styled Enter-created paragraph");
    canvas.redo();
    const auto afterRedo = canvas.snapshot();
    check(afterRedo.document.paragraphs().size() == 3 &&
              afterRedo.document.paragraphs()[2]
                      .paragraphMarkCharacterFormat() == inheritedMark,
          "Redo did not restore the inherited empty-paragraph format");
    canvas.insertText(QStringLiteral("Next"));
    const auto finalSnapshot = canvas.snapshot();
    check(finalSnapshot.document.paragraphs()[2].text() == u"Next" &&
              finalSnapshot.document.paragraphs()[2]
                      .characterFormatAt(4) == inheritedMark,
          "typing after redo did not use the restored inherited format");
    canvas.hide();

    // Imported and legacy content can have styled characters but a sparse
    // paragraph mark. Deleting all text must promote the visible style before
    // the user navigates away, otherwise returning to the empty line resets
    // the ribbon and subsequent typing.
    auto deletionDocument = documentWithParagraphs(
        {QStringLiteral("Styled"), QStringLiteral("Anchor")});
    const auto deletionParagraphId =
        deletionDocument.paragraphs()[0].id();
    const auto deletionAnchorId = deletionDocument.paragraphs()[1].id();
    docxstudio::core::CharacterFormatDelta importedStyle;
    importedStyle.font_family =
        docxstudio::core::PropertyDelta<std::string>::set(
            chosenFamily.toStdString());
    importedStyle.font_size_half_points =
        docxstudio::core::PropertyDelta<std::int32_t>::set(38);
    importedStyle.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(
            static_cast<std::uint32_t>(chosenColor.rgba()));
    importedStyle.highlight_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(
            static_cast<std::uint32_t>(chosenHighlight.rgba()));
    importedStyle.bold =
        docxstudio::core::PropertyDelta<bool>::set(true);
    importedStyle.italic =
        docxstudio::core::PropertyDelta<bool>::set(true);
    importedStyle.underline = docxstudio::core::PropertyDelta<
        docxstudio::core::UnderlineStyle>::set(
            docxstudio::core::UnderlineStyle::single);
    check(static_cast<bool>(deletionDocument.applyCharacterFormat(
              {{deletionParagraphId, 0}, {deletionParagraphId, 6}},
              importedStyle)),
          "could not create styled deletion-to-empty fixture");
    check(deletionDocument.paragraphs()[0]
              .paragraphMarkCharacterFormat().empty(),
          "deletion-to-empty fixture unexpectedly began with a styled mark");

    DocumentCanvas deletionCanvas(spelling);
    deletionCanvas.resize(900, 520);
    deletionCanvas.setDocument(std::move(deletionDocument));
    deletionCanvas.show();
    deletionCanvas.setFocus();
    QApplication::processEvents();
    const QPoint deletionParagraphPoint =
        inputMethodCursorRect(deletionCanvas).center();
    sendKey(deletionCanvas, Qt::Key_Down);
    check(deletionCanvas.selection().focus.paragraph_id == deletionAnchorId,
          "could not navigate to the deletion fixture's anchor paragraph");
    const QPoint deletionAnchorPoint =
        inputMethodCursorRect(deletionCanvas).center();
    sendMouseEvent(deletionCanvas, QEvent::MouseButtonPress,
                   deletionParagraphPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(deletionCanvas, QEvent::MouseButtonRelease,
                   deletionParagraphPoint, Qt::LeftButton, Qt::NoButton);
    sendKey(deletionCanvas, Qt::Key_End);
    sendKey(deletionCanvas, Qt::Key_Home, Qt::ShiftModifier);
    sendKey(deletionCanvas, Qt::Key_Delete);

    auto deletedSnapshot = deletionCanvas.snapshot();
    check(deletedSnapshot.document.paragraphs()[0].text().empty() &&
              deletedSnapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .font_family == chosenFamily.toStdString() &&
              deletedSnapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .foreground_argb ==
                  static_cast<std::uint32_t>(chosenColor.rgba()),
          "deleting all styled text did not promote its insertion format");
    check(deletionCanvas.hasNonTextChanges(),
          "paragraph-mark promotion was not classified for safe DOCX saving");
    const QPoint emptiedParagraphPoint =
        inputMethodCursorRect(deletionCanvas).center();
    sendMouseEvent(deletionCanvas, QEvent::MouseButtonPress,
                   deletionAnchorPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(deletionCanvas, QEvent::MouseButtonRelease,
                   deletionAnchorPoint, Qt::LeftButton, Qt::NoButton);
    sendMouseEvent(deletionCanvas, QEvent::MouseButtonPress,
                   emptiedParagraphPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(deletionCanvas, QEvent::MouseButtonRelease,
                   emptiedParagraphPoint, Qt::LeftButton, Qt::NoButton);
    deletionCanvas.refreshCursorFormat();
    check(deletionCanvas.currentFontFamily() == chosenFamily &&
              deletionCanvas.currentFontPointSize() == chosenPointSize &&
              deletionCanvas.currentTextColor().rgba() ==
                  chosenColor.rgba() &&
              deletionCanvas.currentHighlightColor().rgba() ==
                  chosenHighlight.rgba(),
          "returning to a deletion-created empty line reset its format");
    deletionCanvas.insertText(QStringLiteral("Again"));
    deletedSnapshot = deletionCanvas.snapshot();
    check(deletedSnapshot.document.paragraphs()[0].text() == u"Again" &&
              deletedSnapshot.document.paragraphs()[0]
                      .characterFormatAt(5) ==
                  deletedSnapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat(),
          "typing after returning to a deletion-created empty line lost its format");
    deletionCanvas.hide();

    const auto verifyTransientSingleCharacterDeletion =
        [&](Qt::Key deletionKey) {
            auto transientDocument = documentWithParagraphs(
                {QStringLiteral("x"), QStringLiteral("Anchor")});
            const auto transientParagraphId =
                transientDocument.paragraphs()[0].id();
            const auto transientAnchorId =
                transientDocument.paragraphs()[1].id();
            DocumentCanvas transientCanvas(spelling);
            transientCanvas.resize(900, 520);
            transientCanvas.setDocument(std::move(transientDocument));
            transientCanvas.show();
            transientCanvas.setFocus();
            QApplication::processEvents();
            const QPoint firstPoint =
                inputMethodCursorRect(transientCanvas).center();
            sendKey(transientCanvas, Qt::Key_Down);
            check(transientCanvas.selection().focus.paragraph_id ==
                      transientAnchorId,
                  "could not reach transient-deletion anchor paragraph");
            const QPoint anchorPoint =
                inputMethodCursorRect(transientCanvas).center();
            sendMouseEvent(transientCanvas, QEvent::MouseButtonPress,
                           firstPoint, Qt::LeftButton, Qt::LeftButton);
            sendMouseEvent(transientCanvas, QEvent::MouseButtonRelease,
                           firstPoint, Qt::LeftButton, Qt::NoButton);
            if (deletionKey == Qt::Key_Backspace) {
                sendKey(transientCanvas, Qt::Key_End);
            }

            transientCanvas.setFontFamily(chosenFamily);
            transientCanvas.setFontPointSize(chosenPointSize);
            transientCanvas.setForeground(chosenColor);
            transientCanvas.setHighlight(chosenHighlight);
            transientCanvas.toggleBold();
            transientCanvas.toggleItalic();
            transientCanvas.toggleUnderline();
            sendKey(transientCanvas, deletionKey);

            auto transientSnapshot = transientCanvas.snapshot();
            const auto transientMark =
                transientSnapshot.document.paragraphs()[0]
                    .paragraphMarkCharacterFormat();
            check(transientSnapshot.document.paragraphs()[0].id() ==
                      transientParagraphId &&
                      transientSnapshot.document.paragraphs()[0].text().empty() &&
                      transientMark.font_family == chosenFamily.toStdString() &&
                      transientMark.font_size_half_points == 38 &&
                      transientMark.foreground_argb ==
                          static_cast<std::uint32_t>(chosenColor.rgba()) &&
                      transientMark.highlight_argb ==
                          static_cast<std::uint32_t>(chosenHighlight.rgba()) &&
                      transientMark.bold == true && transientMark.italic == true &&
                      transientMark.underline ==
                          docxstudio::core::UnderlineStyle::single,
                  "single-character deletion lost the transient caret format");

            sendMouseEvent(transientCanvas, QEvent::MouseButtonPress,
                           anchorPoint, Qt::LeftButton, Qt::LeftButton);
            sendMouseEvent(transientCanvas, QEvent::MouseButtonRelease,
                           anchorPoint, Qt::LeftButton, Qt::NoButton);
            sendMouseEvent(transientCanvas, QEvent::MouseButtonPress,
                           firstPoint, Qt::LeftButton, Qt::LeftButton);
            sendMouseEvent(transientCanvas, QEvent::MouseButtonRelease,
                           firstPoint, Qt::LeftButton, Qt::NoButton);
            transientCanvas.refreshCursorFormat();
            check(transientCanvas.currentFontFamily() == chosenFamily &&
                      transientCanvas.currentFontPointSize() ==
                          chosenPointSize &&
                      transientCanvas.currentTextColor().rgba() ==
                          chosenColor.rgba() &&
                      transientCanvas.currentHighlightColor().rgba() ==
                          chosenHighlight.rgba(),
                  "returning after single-character deletion reset the transient format");
            transientCanvas.insertText(QStringLiteral("T"));
            transientSnapshot = transientCanvas.snapshot();
            check(transientSnapshot.document.paragraphs()[0].text() == u"T" &&
                      transientSnapshot.document.paragraphs()[0]
                              .characterFormatAt(1) == transientMark,
                  "typing after single-character deletion did not use its promoted format");
            transientCanvas.hide();
        };
    verifyTransientSingleCharacterDeletion(Qt::Key_Backspace);
    verifyTransientSingleCharacterDeletion(Qt::Key_Delete);
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

void testZoomIsViewOnlyAndWheelDriven(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 500);
    canvas.setDocument(documentWithText(QStringLiteral("zoom me")));
    canvas.markSaved();
    canvas.show();
    canvas.setFocus();
    QApplication::processEvents();

    const auto original = canvas.snapshot();
    const auto originalSelection = canvas.selection();
    const auto originalLayoutGeneration = canvas.layoutGeneration();
    int signalCount = 0;
    int lastSignaledZoom = -1;
    QObject::connect(&canvas, &DocumentCanvas::zoomChanged, &canvas,
                     [&](int percent) {
                         ++signalCount;
                         lastSignaledZoom = percent;
                     });

    canvas.setZoomPercent(100);
    check(signalCount == 0,
          "setting the current zoom emitted a redundant zoom signal");
    canvas.setZoomPercent(999);
    check(canvas.zoomPercent() == DocumentCanvas::kMaximumZoomPercent &&
              lastSignaledZoom == DocumentCanvas::kMaximumZoomPercent,
          "zoom did not clamp and report its 400 percent maximum");
    canvas.setZoomPercent(-999);
    check(canvas.zoomPercent() == DocumentCanvas::kMinimumZoomPercent &&
              lastSignaledZoom == DocumentCanvas::kMinimumZoomPercent,
          "zoom did not clamp and report its 25 percent minimum");
    check(signalCount == 2,
          "zoom bounds emitted an unexpected number of change signals");

    auto afterZoom = canvas.snapshot();
    check(afterZoom.revision == original.revision &&
              paragraphTexts(afterZoom) == paragraphTexts(original) &&
              canvas.selection() == originalSelection &&
              !canvas.isModified(),
          "changing zoom mutated or dirtied the document");
    check(canvas.layoutGeneration() == originalLayoutGeneration,
          "changing zoom unnecessarily repaginated the document");

    canvas.setZoomPercent(100);
    sendWheel(canvas, 120, Qt::ControlModifier);
    check(canvas.zoomPercent() == 110,
          "Ctrl+wheel-up did not zoom in by one step");
    sendWheel(canvas, -120, Qt::ControlModifier);
    check(canvas.zoomPercent() == 100,
          "Ctrl+wheel-down did not zoom out by one step");
    sendWheel(canvas, 60, Qt::ControlModifier);
    check(canvas.zoomPercent() == 100,
          "a partial high-resolution wheel delta zoomed too early");
    sendWheel(canvas, 60, Qt::ControlModifier);
    check(canvas.zoomPercent() == 110,
          "high-resolution wheel deltas did not accumulate to one step");
    sendWheel(canvas, -240, Qt::ControlModifier);
    check(canvas.zoomPercent() == 90,
          "a multi-notch wheel delta did not apply every zoom step");
    sendWheel(canvas, 0, Qt::ControlModifier, 20);
    check(canvas.zoomPercent() == 90,
          "a partial touchpad pixel delta zoomed too early");
    sendWheel(canvas, 0, Qt::ControlModifier, 20);
    check(canvas.zoomPercent() == 100,
          "touchpad pixel deltas did not accumulate smoothly");
    sendWheel(canvas, 120);
    check(canvas.zoomPercent() == 100,
          "plain wheel scrolling unexpectedly changed zoom");

    // Keyboard formatting shortcuts deliberately remain available. Zoom is
    // mouse-wheel-only here, so Ctrl+=/Ctrl++ and Ctrl+- cannot steal the
    // existing subscript/superscript command bindings owned by MainWindow.
    sendKey(canvas, Qt::Key_Equal, Qt::ControlModifier);
    sendKey(canvas, Qt::Key_Plus,
            Qt::ControlModifier | Qt::ShiftModifier);
    sendKey(canvas, Qt::Key_Minus, Qt::ControlModifier);
    check(canvas.zoomPercent() == 100,
          "a formatting-compatible keyboard sequence changed zoom");

    for (int index = 0; index < 60; ++index) {
        sendWheel(canvas, 120, Qt::ControlModifier);
    }
    check(canvas.zoomPercent() == DocumentCanvas::kMaximumZoomPercent,
          "repeated Ctrl+wheel-up did not stop at 400 percent");
    const int signalsAtMaximum = signalCount;
    sendWheel(canvas, 120, Qt::ControlModifier);
    check(signalCount == signalsAtMaximum,
          "wheel input past 400 percent emitted a false zoom change");
    for (int index = 0; index < 60; ++index) {
        sendWheel(canvas, -120, Qt::ControlModifier);
    }
    check(canvas.zoomPercent() == DocumentCanvas::kMinimumZoomPercent,
          "repeated Ctrl+wheel-down did not stop at 25 percent");

    canvas.setZoomPercent(100);
    QString summary;
    QString error;
    check(canvas.createReplacementPreview(QStringLiteral("preview"), summary,
                                          error),
          "could not create the zoom preview fixture");
    const auto liveBeforePreviewZoom = canvas.snapshot();
    sendWheel(canvas, 120, Qt::ControlModifier);
    check(canvas.zoomPercent() == 110 && canvas.hasPreview(),
          "Ctrl+wheel zoom was blocked by an active Codex preview");
    const auto liveAfterPreviewZoom = canvas.snapshot();
    check(liveAfterPreviewZoom.revision == liveBeforePreviewZoom.revision &&
              paragraphTexts(liveAfterPreviewZoom) ==
                  paragraphTexts(liveBeforePreviewZoom) &&
              !canvas.isModified(),
          "zooming an active preview mutated the live document");
    canvas.discardPreview();
    canvas.hide();
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

    // A visually empty list item still contains its marker text. Formatting
    // chosen at that caret is therefore transient until the marker is removed;
    // every way of leaving the item must promote that active format into the
    // resulting empty paragraph mark so navigation cannot reset it.
    const auto verifyEmptyListExitFormat =
        [&spelling](int exitKind, bool clearBold) {
            DocumentCanvas formatExit(spelling);
            formatExit.insertText(QStringLiteral("Anchor"));
            sendKey(formatExit, Qt::Key_Return);

            const QString family = QStringLiteral("DejaVu Serif");
            const QColor foreground(QStringLiteral("#3157a4"));
            const QColor highlight(QStringLiteral("#f6d32d"));
            if (clearBold) {
                formatExit.toggleBold();
                formatExit.toggleBullets();
                formatExit.toggleBold();
            } else {
                formatExit.toggleBullets();
                formatExit.setFontFamily(family);
                formatExit.setFontPointSize(19.0);
                formatExit.setForeground(foreground);
                formatExit.setHighlight(highlight);
                formatExit.toggleBold();
                formatExit.toggleItalic();
                formatExit.toggleUnderline();
            }

            if (exitKind == 0) {
                sendKey(formatExit, Qt::Key_Return);
            } else if (exitKind == 1) {
                sendKey(formatExit, Qt::Key_Backspace);
            } else {
                formatExit.toggleBullets();
            }

            auto snapshot = formatExit.snapshot();
            check(snapshot.document.paragraphs().size() == 2 &&
                      snapshot.document.paragraphs()[1].text().empty() &&
                      !snapshot.document.paragraphs()[1].format().list_id,
                  "leaving a formatted empty list item did not produce an ordinary empty paragraph");
            const auto emptyParagraphId =
                snapshot.document.paragraphs()[1].id();
            const auto mark = snapshot.document.paragraphs()[1]
                                  .paragraphMarkCharacterFormat();
            if (clearBold) {
                check(mark.bold == false,
                      "leaving an empty list item lost an explicit formatting clear");
            } else {
                check(mark.font_family == family.toStdString() &&
                          mark.font_size_half_points == 38 &&
                          mark.foreground_argb ==
                              static_cast<std::uint32_t>(foreground.rgba()) &&
                          mark.highlight_argb ==
                              static_cast<std::uint32_t>(highlight.rgba()) &&
                          mark.bold == true && mark.italic == true &&
                          mark.underline ==
                              docxstudio::core::UnderlineStyle::single,
                      "leaving an empty list item lost the active caret format");
            }

            sendKey(formatExit, Qt::Key_Up);
            sendKey(formatExit, Qt::Key_Down);
            check(formatExit.selection().focus.paragraph_id == emptyParagraphId,
                  "could not return to the empty paragraph after leaving a list");
            formatExit.insertText(QStringLiteral("X"));
            snapshot = formatExit.snapshot();
            const auto inserted = snapshot.document.paragraphs()[1]
                                      .characterFormatAt(1);
            if (clearBold) {
                check(inserted.bold == false,
                      "typing after returning to an exited list item lost its explicit formatting clear");
            } else {
                check(inserted == mark,
                      "typing after returning to an exited list item did not use its stored format");
            }
        };
    for (int exitKind = 0; exitKind < 3; ++exitKind) {
        verifyEmptyListExitFormat(exitKind, false);
        verifyEmptyListExitFormat(exitKind, true);
    }

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

void testPasteTextOnly(docxstudio::app::SpellChecker& spelling) {
    const QColor requestedColor(QStringLiteral("#365f91"));
    DocumentCanvas canvas(spelling);
    canvas.setDocument(documentWithColor(
        QStringLiteral("replace"), 0, 7,
        static_cast<std::uint32_t>(requestedColor.rgba())));
    canvas.selectAll();

    auto* richClipboard = new QMimeData;
    richClipboard->setText(QStringLiteral("plain\ntext"));
    richClipboard->setHtml(QStringLiteral(
        "<table><tr><td><b>rich object</b></td></tr></table>"));
    richClipboard->setData(
        QStringLiteral("application/x-owl-docs-inline-image-v2"),
        QByteArrayLiteral("not a valid Owl Docs picture"));
    QApplication::clipboard()->setMimeData(richClipboard);

    const auto beforePaste = canvas.snapshot();
    int failures = 0;
    QObject::connect(&canvas, &DocumentCanvas::operationFailed,
                     &canvas, [&failures](const QString&) { ++failures; });
    sendKey(canvas, Qt::Key_V,
            Qt::ControlModifier | Qt::ShiftModifier);
    const auto pasted = canvas.snapshot();
    check(pasted.revision.value() == beforePaste.revision.value() + 1,
          "Paste as Text Only was not one document transaction");
    check(paragraphTexts(pasted) ==
              QStringList{QStringLiteral("plain"), QStringLiteral("text")},
          "Ctrl+Shift+V did not insert only the clipboard plain text");
    check(pasted.document.tables().empty() &&
              pasted.document.paragraphs().front().images().empty() &&
              failures == 0,
          "Paste as Text Only interpreted rich/native clipboard content");
    check(pasted.document.paragraphs().front()
                  .characterFormatAt(1).foreground_argb ==
              static_cast<std::uint32_t>(requestedColor.rgba()) &&
              pasted.document.paragraphs().back()
                  .characterFormatAt(1).foreground_argb ==
              static_cast<std::uint32_t>(requestedColor.rgba()),
          "plain-text paste did not use the current insertion formatting");
    canvas.undo();
    check(paragraphTexts(canvas.snapshot()) ==
              QStringList{QStringLiteral("replace")},
          "one Undo did not restore a multi-line plain-text paste");

    auto* nonTextClipboard = new QMimeData;
    nonTextClipboard->setHtml(QStringLiteral("<b>HTML only</b>"));
    nonTextClipboard->setData(QStringLiteral("image/png"),
                              QByteArrayLiteral("not a PNG"));
    QApplication::clipboard()->setMimeData(nonTextClipboard);
    const auto beforeRejectedPaste = canvas.snapshot();
    canvas.pasteTextOnly();
    check(canvas.snapshot().revision == beforeRejectedPaste.revision &&
              paragraphTexts(canvas.snapshot()) ==
                  paragraphTexts(beforeRejectedPaste),
          "a clipboard without text/plain mutated the document");

    DocumentCanvas contextCanvas(spelling);
    contextCanvas.resize(800, 500);
    contextCanvas.show();
    contextCanvas.setFocus();
    QApplication::processEvents();
    QApplication::clipboard()->setText(QStringLiteral("from context menu"));
    bool foundContextAction = false;
    QTimer::singleShot(0, &contextCanvas, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu != nullptr,
              "document right-click did not open a context menu");
        auto* action = menu->findChild<QAction*>(
            QStringLiteral("context.pasteTextOnly"));
        check(action && action->isEnabled() &&
                  action->text() == QStringLiteral("Paste as Text Only"),
              "document context menu has no enabled plain-text paste action");
        foundContextAction = true;
        action->trigger();
        menu->close();
    });
    const QPoint contextPoint = inputMethodCursorRect(contextCanvas).center();
    QContextMenuEvent contextEvent(
        QContextMenuEvent::Mouse, contextPoint,
        contextCanvas.viewport()->mapToGlobal(contextPoint));
    QApplication::sendEvent(contextCanvas.viewport(), &contextEvent);
    check(foundContextAction &&
              onlyText(contextCanvas) == QStringLiteral("from context menu"),
          "Paste as Text Only context action did not insert clipboard text");
    contextCanvas.hide();
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

void testSemanticSearchAcrossBodyAndTables(
    docxstudio::app::SpellChecker& spelling) {
    auto first = docxstudio::core::Paragraph::create(
        u"needle first Cat concatenate cat CAT");
    auto second = docxstudio::core::Paragraph::create(u"needle final");
    check(first && second, "could not create search fixture paragraphs");
    const auto firstId = first.value().id();
    const auto secondId = second.value().id();
    auto created = docxstudio::core::Document::create(
        {first.value(), second.value()});
    check(static_cast<bool>(created),
          "could not create search fixture document");
    auto tableResult = docxstudio::core::Table::create(2, 2, false);
    check(static_cast<bool>(tableResult),
          "could not create search fixture table");
    const auto tableId = tableResult.value().id();
    const auto cell00Id = tableResult.value().cell(0, 0)->id;
    const auto cell10Id = tableResult.value().cell(1, 0)->id;
    const auto cell11Id = tableResult.value().cell(1, 1)->id;
    check(static_cast<bool>(created.value().insertTable(
              secondId, tableResult.value())),
          "could not insert search fixture table");
    check(static_cast<bool>(created.value().setTableCellText(
              tableId, 0, 0, u"red needle green")),
          "could not populate first searchable table cell");
    check(static_cast<bool>(created.value().setTableCellText(
              tableId, 0, 1, u"no match")),
          "could not populate nonmatching table cell");
    check(static_cast<bool>(created.value().setTableCellText(
              tableId, 1, 0, u"needle lower")),
          "could not populate second searchable table cell");
    check(static_cast<bool>(created.value().setTableCellText(
              tableId, 1, 1, u"last needle")),
          "could not populate third searchable table cell");

    const auto applyCellColor = [&](std::size_t start, std::size_t end,
                                    std::uint32_t argb) {
        docxstudio::core::CharacterFormatDelta delta;
        delta.foreground_argb =
            docxstudio::core::PropertyDelta<std::uint32_t>::set(argb);
        check(static_cast<bool>(
                  created.value().applyTableCellCharacterFormat(
                      tableId, 0, 0, start, end, delta)),
              "could not format search fixture table cell");
    };
    constexpr std::uint32_t red = 0xffa02020U;
    constexpr std::uint32_t blue = 0xff2050a0U;
    constexpr std::uint32_t green = 0xff208040U;
    applyCellColor(0, 4, red);
    applyCellColor(4, 10, blue);
    applyCellColor(10, 16, green);

    DocumentCanvas canvas(spelling);
    canvas.setDocument(std::move(created.value()));

    const auto hits = canvas.searchHits(QStringLiteral("needle"));
    check(hits.size() == 5,
          "semantic search returned the wrong body/table hit count");
    const auto matches = canvas.searchMatches(QStringLiteral("needle"));
    check(matches.size() == hits.size() && matches[1].hit == hits[1] &&
              matches[1].containerText == QStringLiteral("red needle green") &&
              matches[0].containerOrdinal == 1 &&
              matches[0].searchUnitOrdinal == 0 &&
              matches[1].containerOrdinal == 1 &&
              matches[1].searchUnitOrdinal == 1 &&
              matches[2].searchUnitOrdinal == 3 &&
              matches[4].containerOrdinal == 2 &&
              matches[4].searchUnitOrdinal == 5,
          "batched search did not return hit text from one result snapshot");
    const auto* firstParagraph =
        std::get_if<docxstudio::app::BodyParagraphSearchHit>(
            &hits[0].target);
    const auto* firstCell =
        std::get_if<docxstudio::app::TableCellSearchHit>(
            &hits[1].target);
    const auto* secondCell =
        std::get_if<docxstudio::app::TableCellSearchHit>(
            &hits[2].target);
    const auto* thirdCell =
        std::get_if<docxstudio::app::TableCellSearchHit>(
            &hits[3].target);
    const auto* lastParagraph =
        std::get_if<docxstudio::app::BodyParagraphSearchHit>(
            &hits[4].target);
    check(firstParagraph && firstParagraph->paragraphId == firstId &&
              firstCell && firstCell->tableId == tableId &&
              firstCell->cellId == cell00Id && firstCell->row == 0 &&
              firstCell->column == 0 &&
              secondCell && secondCell->cellId == cell10Id &&
              secondCell->row == 1 && secondCell->column == 0 &&
              thirdCell && thirdCell->cellId == cell11Id &&
              thirdCell->row == 1 && thirdCell->column == 1 &&
              lastParagraph && lastParagraph->paragraphId == secondId,
          "semantic search did not follow body-block and row-major cell order");
    check(canvas.searchHitContainerText(hits[1]) ==
              std::optional<QString>{QStringLiteral("red needle green")},
          "search hit presentation did not resolve its table-cell text");

    using docxstudio::app::DocumentSearchOptions;
    check(canvas.searchHits(QStringLiteral("Cat"),
                            DocumentSearchOptions{false, true}).size() == 3,
          "case-insensitive whole-word search returned the wrong hits");
    check(canvas.searchHits(QStringLiteral("Cat"),
                            DocumentSearchOptions{true, true}).size() == 1,
          "case-sensitive whole-word search returned the wrong hits");
    check(canvas.searchHits(QStringLiteral("Cat"),
                            DocumentSearchOptions{false, false}).size() == 4,
          "non-whole-word search omitted a substring hit");

    check(canvas.activateSearchHit(hits[1]) &&
              canvas.currentSearchHit() ==
                  std::optional<docxstudio::app::DocumentSearchHit>{hits[1]} &&
              canvas.selectedText() == QStringLiteral("needle"),
          "activating a table-cell hit did not select that exact match");
    const auto next = canvas.findNextHit(QStringLiteral("needle"));
    check(next && *next == hits[2],
          "find-next did not advance from one table cell to the next");
    const auto previous = canvas.findPreviousHit(QStringLiteral("needle"));
    check(previous && *previous == hits[1],
          "find-previous did not return to the preceding table cell");
    const auto precedingBody = canvas.findPreviousHit(
        QStringLiteral("needle"));
    check(precedingBody && *precedingBody == hits.front(),
          "find-previous did not continue in document order");
    const auto wrappedPrevious = canvas.findPreviousHit(
        QStringLiteral("needle"));
    check(wrappedPrevious && *wrappedPrevious == hits.back(),
          "find-previous did not wrap from the first hit to the last");
    check(canvas.activateSearchHit(hits.back()),
          "could not activate the final body search hit");
    const auto wrapped = canvas.findNextHit(QStringLiteral("needle"));
    check(wrapped && *wrapped == hits.front(),
          "find-next did not wrap from the final hit to the first");

    check(canvas.activateSearchHit(hits[1]) &&
              canvas.replaceCurrent(QStringLiteral("needle"),
                                    QStringLiteral("term")),
          "replace-current could not replace a semantic table-cell hit");
    check(canvas.snapshot().document.findTable(tableId)->cell(0, 0)->text ==
              u"red term green",
          "replace-current produced the wrong table-cell text");
    canvas.undo();
    const auto baseline = canvas.snapshot();
    check(baseline.document.findTable(tableId)->cell(0, 0)->text ==
              u"red needle green",
          "one undo did not restore a table-cell replacement");

    check(canvas.replaceAllMatches(QStringLiteral("needle"),
                                   QStringLiteral("OWL")) == 5,
          "replace-all returned the wrong cross-container count");
    const auto replaced = canvas.snapshot();
    check(replaced.document.findParagraph(firstId)->text() ==
              u"OWL first Cat concatenate cat CAT" &&
              replaced.document.findParagraph(secondId)->text() ==
              u"OWL final" &&
              replaced.document.findTable(tableId)->cell(0, 0)->text ==
              u"red OWL green" &&
              replaced.document.findTable(tableId)->cell(1, 0)->text ==
              u"OWL lower" &&
              replaced.document.findTable(tableId)->cell(1, 1)->text ==
              u"last OWL",
          "replace-all produced incorrect body/table text");
    const auto* formattedCell =
        replaced.document.findTable(tableId)->cell(0, 0);
    check(formattedCell->characterFormatAt(1).foreground_argb == red &&
              formattedCell->characterFormatAt(5).foreground_argb == blue &&
              formattedCell->characterFormatAt(9).foreground_argb == green,
          "table-cell replacement damaged match or surrounding run formatting");
    canvas.undo();
    check(canvas.snapshot().document == baseline.document,
          "one undo did not restore the complete cross-container replace-all");
    canvas.undo();
    check(canvas.snapshot().document == baseline.document,
          "replace-all created more than one undo transaction");

    DocumentCanvas staleCanvas(spelling);
    staleCanvas.setDocument(documentWithText(QStringLiteral("needle")));
    const auto staleHit = staleCanvas.searchHits(
        QStringLiteral("needle")).front();
    staleCanvas.insertText(QStringLiteral("x"));
    const auto afterMutation = staleCanvas.snapshot();
    check(!staleCanvas.searchHitContainerText(staleHit) &&
              !staleCanvas.activateSearchHit(staleHit) &&
              !staleCanvas.replaceSearchHit(staleHit,
                                            QStringLiteral("stale")) &&
              staleCanvas.snapshot().document == afterMutation.document,
          "a stale search hit was resolved, activated, or replaced");
}

void testSearchNavigationDuringPreview(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.setDocument(documentWithText(QStringLiteral("live needle")));
    canvas.insertText(QStringLiteral("x"));
    const auto live = canvas.snapshot();
    const auto liveParagraph = live.document.paragraphs().front().id();
    const auto previewParagraph = docxstudio::core::NodeId::generate();
    QString summary;
    QString error;
    check(canvas.createOperationsPreview(
              live.revision,
              {docxstudio::core::SplitParagraph{
                   {liveParagraph, 5}, previewParagraph},
               docxstudio::core::InsertText{
                   {previewParagraph, 0}, u"preview needle", std::nullopt}},
              QStringLiteral("Preview-only search paragraph"), summary,
              error),
          "could not create preview-only search fixture");

    const auto previewHits = canvas.searchHits(QStringLiteral("preview"));
    check(previewHits.size() == 1 && previewHits.front().previewId &&
              previewHits.front().revision == live.revision &&
              std::get_if<docxstudio::app::BodyParagraphSearchHit>(
                  &previewHits.front().target)->paragraphId ==
                  previewParagraph &&
              canvas.searchHitContainerText(previewHits.front()) ==
                  std::optional<QString>{
                      QStringLiteral("preview needle needle")},
          "search did not read the displayed preview branch");
    check(canvas.activateSearchHit(previewHits.front()) &&
              canvas.currentSearchHit() ==
                  std::optional<docxstudio::app::DocumentSearchHit>{
                      previewHits.front()},
          "preview search hit could not be activated read-only");
    const auto next = canvas.findNextHit(QStringLiteral("needle"));
    check(next.has_value(),
          "find-next was incorrectly disabled during a preview");

    int failures = 0;
    QObject::connect(&canvas, &DocumentCanvas::operationFailed,
                     [&failures](const QString&) { ++failures; });
    check(!canvas.replaceSearchHit(previewHits.front(),
                                  QStringLiteral("blocked")) &&
              canvas.replaceAllMatches(QStringLiteral("needle"),
                                       QStringLiteral("blocked")) == 0 &&
              failures == 2 &&
              canvas.snapshot().document == live.document,
          "preview lock did not reject search writes atomically");

    canvas.discardPreview();
    check(!canvas.hasPreview() &&
              canvas.selection().anchor == canvas.selection().focus &&
              canvas.selection().focus.paragraph_id == liveParagraph &&
              canvas.selection().focus.utf16_offset == 0 &&
              !canvas.searchHitContainerText(previewHits.front()) &&
              !canvas.activateSearchHit(previewHits.front()),
          "discarding a preview did not restore a safe live selection");
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

void testBoundedCanvasHistoryStaysSynchronized(
    docxstudio::app::SpellChecker& spelling) {
    docxstudio::core::DocumentSessionLimits limits;
    limits.maximum_history_entries = 3;
    DocumentCanvas canvas(spelling, limits);

    // This local-only layout entry predates the document transactions that
    // the core will evict. It must be dropped with that unreachable prefix,
    // otherwise its saved cursor can later be restored against the wrong
    // document baseline.
    canvas.setMarginsPoints(64.0, 65.0, 66.0, 67.0);
    for (const QChar character : QStringLiteral("abcde")) {
        canvas.insertText(QString(character));
    }
    check(onlyText(canvas) == QStringLiteral("abcde"),
          "bounded-history fixture did not apply its edits");

    canvas.undo();
    canvas.undo();
    canvas.undo();
    check(onlyText(canvas) == QStringLiteral("ab"),
          "canvas undo history did not retain the same suffix as the core");
    canvas.undo();
    check(onlyText(canvas) == QStringLiteral("ab") &&
              canvas.marginTopPoints() == 64.0 &&
              canvas.marginLeftPoints() == 67.0,
          "canvas crossed the core history baseline through a stale cursor entry");

    canvas.redo();
    canvas.redo();
    canvas.redo();
    check(onlyText(canvas) == QStringLiteral("abcde"),
          "canvas redo history diverged after bounded undo eviction");
    canvas.redo();
    check(onlyText(canvas) == QStringLiteral("abcde"),
          "canvas retained a stale redo entry after the core was exhausted");

    canvas.insertText(QStringLiteral("Z"));
    canvas.undo();
    check(onlyText(canvas) == QStringLiteral("abcde"),
          "a stale bounded-history entry permanently blocked later undo");
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
    testEmptyParagraphTypingFormatSurvivesNavigation(spelling);
    testBaselineToggle(spelling);
    testZoomIsViewOnlyAndWheelDriven(spelling);
    testFontSizeCommit(spelling);
    testBreakKeys(spelling);
    testSemanticListProperties(spelling);
    testAdversarialListEditing(spelling);
    testMouseClickSelection(spelling);
    testDoubleClickSelectsAlphanumericWord(spelling);
    testTabEditingSemantics(spelling);
    testGraphemeBackspace(spelling);
    testClipboard(spelling);
    testPasteTextOnly(spelling);
    testInputMethodCommit(spelling);
    testFindAndReplace(spelling);
    testSemanticSearchAcrossBodyAndTables(spelling);
    testSearchNavigationDuringPreview(spelling);
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
    testBoundedCanvasHistoryStaysSynchronized(spelling);

    std::cout << "editor interaction tests passed\n";
    return 0;
}

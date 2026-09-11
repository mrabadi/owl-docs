#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ChatDock.h"
#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/SpellChecker.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QKeyEvent>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolButton>

#include <cstdlib>
#include <cstdint>
#include <iostream>
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
    const QString tierDetails = QStringLiteral(
        "Standard: Normal speed\nFast: May use additional credits");
    chat.setServiceTiers({QStringLiteral("fast"), QStringLiteral("default")},
                         QStringLiteral("default"), tierDetails);
    check(chat.selectedServiceTier() == QStringLiteral("default"),
          "server-advertised default service tier was not selected");
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

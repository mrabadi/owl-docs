#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/editor_tools.hpp"

#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QStringList>
#include <QTextBoundaryFinder>

#include <cstdint>
#include <cstdlib>
#include <iostream>
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

QString paragraphText(const docxstudio::app::DocumentCanvas& canvas) {
    const auto snapshot = canvas.snapshot();
    const auto& text = snapshot.document.paragraphs().front().text();
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

QStringList paragraphTexts(const docxstudio::app::DocumentCanvas& canvas) {
    QStringList result;
    const auto snapshot = canvas.snapshot();
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(QString::fromUtf16(
            paragraph.text().data(),
            static_cast<qsizetype>(paragraph.text().size())));
    }
    return result;
}

docxstudio::core::Document formattedBoundaryDocument() {
    auto paragraph = docxstudio::core::Paragraph::create(u"Ablue");
    check(static_cast<bool>(paragraph), "could not create formatted bridge paragraph");
    const auto id = paragraph.value().id();
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{std::move(paragraph.value())});
    check(static_cast<bool>(document), "could not create formatted bridge document");
    docxstudio::core::CharacterFormatDelta delta;
    delta.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xff336699U);
    delta.highlight_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xffffcc00U);
    delta.baseline = docxstudio::core::PropertyDelta<
        docxstudio::core::BaselinePosition>::set(
            docxstudio::core::BaselinePosition::superscript);
    const auto result = document.value().applyCharacterFormat(
        {{id, 1}, {id, 5}}, delta);
    check(static_cast<bool>(result), "could not format bridge test range");
    return std::move(document.value());
}

docxstudio::core::Document shiftedFormattingDocument() {
    auto paragraph = docxstudio::core::Paragraph::create(u"AABBBB");
    check(static_cast<bool>(paragraph), "could not create shifted-format paragraph");
    const auto id = paragraph.value().id();
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{std::move(paragraph.value())});
    check(static_cast<bool>(document), "could not create shifted-format document");
    docxstudio::core::CharacterFormatDelta delta;
    delta.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xff336699U);
    const auto result = document.value().applyCharacterFormat(
        {{id, 2}, {id, 6}}, delta);
    check(static_cast<bool>(result), "could not format shifted test range");
    return std::move(document.value());
}

std::vector<std::uint8_t> bridgePng() {
    QImage image(7, 5, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#336699")));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "could not encode bridge image fixture");
    return {
        reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
            encoded.size()};
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("Helo world"));
    canvas.markSaved();

    const QString documentId = QStringLiteral("document:test");
    const auto before = canvas.snapshot();
    const std::string blockId = before.document.paragraphs().front().id().toString();
    QString summary;
    QString error;

    const auto read = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", before.revision.value()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    check(error.isEmpty(), "bounded document read failed");
    check(read.at("revision") == before.revision.value(), "read returned wrong revision");
    check(read.at("blocks").size() == 1, "read returned wrong block count");
    check(read.at("blocks").at(0).at("text") == "Helo world", "read returned wrong text");
    check(read.at("blocks").at(0).at("id") == blockId, "read returned unstable block id");

    docxstudio::app::DocumentCanvas emptyFormatCanvas(spelling);
    emptyFormatCanvas.setFontFamily(QStringLiteral("DejaVu Serif"));
    emptyFormatCanvas.setFontPointSize(19.0);
    emptyFormatCanvas.setForeground(QColor(QStringLiteral("#3157a4")));
    emptyFormatCanvas.toggleBold();
    const auto emptyFormatSnapshot = emptyFormatCanvas.snapshot();
    error.clear();
    const auto emptyFormatRead = docxstudio::app::invokeEditorTool(
        emptyFormatCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", emptyFormatSnapshot.revision.value()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    const auto& paragraphMarkStyle = emptyFormatRead.at("blocks")
                                         .at(0)
                                         .at("attributes")
                                         .at("paragraphMarkStyle");
    check(error.isEmpty() &&
              emptyFormatRead.at("blocks").at(0).at("text") == "" &&
              paragraphMarkStyle.at("fontFamily") == "DejaVu Serif" &&
              paragraphMarkStyle.at("fontSizePoints") == 19.0 &&
              paragraphMarkStyle.at("foregroundColor") == "#3157A4" &&
              paragraphMarkStyle.at("bold") == true,
          "editor read omitted durable empty-paragraph insertion formatting");

    const QString readTool = QString::fromLatin1(
        docxstudio::codex::kEditorReadTool.data(),
        static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size()));
    const auto expectRejectedRead =
        [&](docxstudio::codex::Json arguments, const QString& expectedError,
            const char* failureMessage) {
            arguments["documentId"] = documentId.toStdString();
            const auto rejected = docxstudio::app::invokeEditorTool(
                canvas, documentId, readTool, arguments, summary, error);
            check(rejected.empty() && error.contains(expectedError),
                  failureMessage);
        };
    expectRejectedRead(
        {{"scope", "document"}, {"expectedRevision", "zero"}},
        QStringLiteral("expectedRevision"),
        "read silently ignored a malformed expected revision");
    expectRejectedRead(
        {{"scope", 7}}, QStringLiteral("scope"),
        "read accepted a non-string scope");
    expectRejectedRead(
        {{"scope", "document"}, {"includeFormatting", "yes"}},
        QStringLiteral("includeFormatting"),
        "read accepted a non-boolean formatting flag");
    expectRejectedRead(
        {{"scope", "document"}, {"maxCharacters", 0}},
        QStringLiteral("maxCharacters"),
        "read silently replaced a zero character limit");
    expectRejectedRead(
        {{"scope", "document"}, {"maxCharacters", 200001}},
        QStringLiteral("maxCharacters"),
        "read silently clamped an oversized character limit");
    expectRejectedRead(
        {{"scope", "blocks"}, {"blockIds", "not-an-array"}},
        QStringLiteral("blockIds"),
        "read silently ignored a malformed blockIds value");
    expectRejectedRead(
        {{"scope", "blocks"}, {"blockIds", {42}}},
        QStringLiteral("blockIds"),
        "read silently ignored a non-string block ID");
    expectRejectedRead(
        {{"scope", "blocks"}, {"blockIds", {"not-a-node-id"}}},
        QStringLiteral("invalid stable ID"),
        "read silently ignored an invalid stable block ID");

    docxstudio::app::DocumentCanvas imageCanvas(spelling);
    imageCanvas.insertText(QStringLiteral("A"));
    const auto png = bridgePng();
    check(imageCanvas.insertInlineImage(png, QStringLiteral("chart.png")),
          "could not insert bridge metadata image");
    const docxstudio::core::ImageLayout bridgeLayout{
        docxstudio::core::ImagePlacement::square,
        100, 200, 300, 400, false};
    check(imageCanvas.setSelectedImageLayout(bridgeLayout),
          "could not set bridge metadata image layout");
    const auto imageSnapshot = imageCanvas.snapshot();
    const auto& imageAtom =
        imageSnapshot.document.paragraphs().front().images().front();
    error.clear();
    const auto imageRead = docxstudio::app::invokeEditorTool(
        imageCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", imageSnapshot.revision.value()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    const auto& imageBlock = imageRead.at("blocks").at(0);
    const auto& images = imageBlock.at("attributes").at("images");
    check(error.isEmpty() && imageBlock.at("text") == "A\xef\xbf\xbc" &&
              images.size() == 1 &&
              images.at(0).at("id") == imageAtom.id.toString() &&
              images.at(0).at("utf16Offset") == 1 &&
              images.at(0).at("accessibleName") == "chart.png" &&
              images.at(0).at("mimeType") == "image/png" &&
              images.at(0).at("widthEmu") == imageAtom.width_emu &&
              images.at(0).at("heightEmu") == imageAtom.height_emu &&
              images.at(0).at("layout").at("placement") == "square" &&
              images.at(0).at("layout").at("distanceTopEmu") == 100 &&
              images.at(0).at("layout").at("distanceRightEmu") == 200 &&
              images.at(0).at("layout").at("distanceBottomEmu") == 300 &&
              images.at(0).at("layout").at("distanceLeftEmu") == 400 &&
              images.at(0).at("layout").at("moveWithText") == false &&
              images.at(0).at("encodedBytes") == png.size() &&
              !images.at(0).contains("bytes") &&
              !images.at(0).contains("path"),
          "bounded editor read omitted image metadata or exposed payload/path data");

    auto searchFirst = docxstudio::core::Paragraph::create(u"Alpha \U0001f600 alpha");
    auto searchLast = docxstudio::core::Paragraph::create(u"tail ALPHA");
    check(static_cast<bool>(searchFirst) && static_cast<bool>(searchLast),
          "could not create search paragraphs");
    const auto firstSearchId = searchFirst.value().id();
    const auto lastSearchId = searchLast.value().id();
    auto searchDocument = docxstudio::core::Document::create(
        {std::move(searchFirst.value()), std::move(searchLast.value())});
    check(static_cast<bool>(searchDocument), "could not create search document");
    auto searchTable = docxstudio::core::Table::create(2, 2, true);
    check(static_cast<bool>(searchTable), "could not create search table");
    const auto searchTableId = searchTable.value().id();
    const auto searchCell00Id = searchTable.value().cell(0, 0)->id;
    const auto searchCell10Id = searchTable.value().cell(1, 0)->id;
    check(static_cast<bool>(searchDocument.value().insertTable(
              lastSearchId, std::move(searchTable.value()))),
          "could not insert search table");
    check(static_cast<bool>(searchDocument.value().setTableCellText(
              searchTableId, 0, 0, u"alpha table")),
          "could not set first search table cell");
    check(static_cast<bool>(searchDocument.value().setTableCellText(
              searchTableId, 0, 1, u"xalpha")),
          "could not set embedded-word search table cell");
    check(static_cast<bool>(searchDocument.value().setTableCellText(
              searchTableId, 1, 0, u"ALPHA")),
          "could not set final search table cell");

    docxstudio::app::DocumentCanvas searchCanvas(spelling);
    searchCanvas.setDocument(std::move(searchDocument.value()));
    searchCanvas.markSaved();
    const auto searchBefore = searchCanvas.snapshot();
    const auto search = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", searchBefore.revision.value()},
         {"query", "alpha"},
         {"caseSensitive", false},
         {"wholeWord", true},
         {"maxResults", 500}},
        summary, error);
    check(error.isEmpty(), "editor search failed");
    check(search.at("revision") == searchBefore.revision.value(),
          "search returned wrong revision");
    check(search.at("results").size() == 5,
          "whole-word search returned wrong result count");
    check(search.at("truncated") == false,
          "complete search was marked truncated");
    const auto& searchResults = search.at("results");
    check(searchResults.at(0).at("blockId") == firstSearchId.toString() &&
              searchResults.at(0).at("blockType") == "paragraph" &&
              searchResults.at(0).at("start") == 0 &&
              searchResults.at(0).at("end") == 5,
          "first paragraph search result is incorrect");
    check(searchResults.at(1).at("blockId") == firstSearchId.toString() &&
              searchResults.at(1).at("start") == 9 &&
              searchResults.at(1).at("end") == 14,
          "search offsets are not UTF-16 offsets");
    check(searchResults.at(2).at("blockId") == searchCell00Id.toString() &&
              searchResults.at(2).at("tableId") == searchTableId.toString() &&
              searchResults.at(2).at("cellId") == searchCell00Id.toString() &&
              searchResults.at(2).at("row") == 0 &&
              searchResults.at(2).at("column") == 0,
          "table-cell search result omitted stable coordinates or identifiers");
    check(searchResults.at(3).at("cellId") == searchCell10Id.toString() &&
              searchResults.at(3).at("row") == 1 &&
              searchResults.at(3).at("column") == 0,
          "table-cell search order is not deterministic row-major order");
    check(searchResults.at(4).at("blockId") == lastSearchId.toString(),
          "search did not resume body order after the table");

    const auto tableCellRead = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", searchBefore.revision.value()},
         {"scope", "blocks"},
         {"blockIds", {firstSearchId.toString(), lastSearchId.toString()}},
         {"tableCellTargets",
          {{{"tableId", searchResults.at(2).at("tableId")},
            {"cellId", searchResults.at(2).at("cellId")}},
           {{"tableId", searchResults.at(3).at("tableId")},
            {"cellId", searchResults.at(3).at("cellId")}}}}},
        summary, error);
    check(error.isEmpty() && tableCellRead.at("blocks").size() == 4,
          "typed table-cell read failed");
    const auto& readBlocks = tableCellRead.at("blocks");
    check(readBlocks.at(0).at("id") == firstSearchId.toString() &&
              readBlocks.at(1).at("id") == searchCell00Id.toString() &&
              readBlocks.at(1).at("type") == "tableCell" &&
              readBlocks.at(1).at("text") == "alpha table" &&
              readBlocks.at(1).at("attributes").at("tableId") ==
                  searchTableId.toString() &&
              readBlocks.at(1).at("attributes").at("row") == 0 &&
              readBlocks.at(1).at("attributes").at("column") == 0 &&
              readBlocks.at(2).at("id") == searchCell10Id.toString() &&
              readBlocks.at(3).at("id") == lastSearchId.toString(),
          "table-cell read did not preserve body/row-major order or identity");
    const auto searchAfter = searchCanvas.snapshot();
    check(searchAfter.revision == searchBefore.revision &&
              searchAfter.document == searchBefore.document &&
              !searchCanvas.isModified(),
          "read-only search mutated the document");

    const auto boundedSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "alpha"},
         {"wholeWord", true},
         {"maxResults", 3}},
        summary, error);
    check(error.isEmpty() && boundedSearch.at("results").size() == 3 &&
              boundedSearch.at("truncated") == true,
          "bounded search did not report truncation");

    const auto caseSensitiveSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "alpha"},
         {"caseSensitive", true},
         {"wholeWord", true}},
        summary, error);
    check(error.isEmpty() && caseSensitiveSearch.at("results").size() == 2,
          "case-sensitive whole-word search returned wrong results");

    const auto wrongDocumentSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", "document:other"}, {"query", "alpha"}},
        summary, error);
    check(wrongDocumentSearch.empty() && error.contains(QStringLiteral("documentId")),
          "search accepted a document capability mismatch");

    const auto staleSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", searchBefore.revision.value() + 1},
         {"query", "alpha"}},
        summary, error);
    check(staleSearch.empty() && error.contains(QStringLiteral("REVISION_CONFLICT")),
          "stale search did not report a revision conflict");

    const auto emptySearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", ""}},
        summary, error);
    check(emptySearch.empty() && error.contains(QStringLiteral("nonempty")),
          "search accepted an empty query");

    const auto invalidUtf8Search = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", std::string("\xc3\x28", 2)}},
        summary, error);
    check(invalidUtf8Search.empty() &&
              error.contains(QStringLiteral("valid UTF-8")),
          "search silently replaced malformed UTF-8");

    const auto oversizedSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "alpha"},
         {"maxResults", 501}},
        summary, error);
    check(oversizedSearch.empty() && error.contains(QStringLiteral("500")),
          "search accepted an unbounded result count");

    const QString surrogateText =
        QString::fromUtf8("\xF0\x9F\x98\x80") + QString(79, QLatin1Char('x')) +
        QStringLiteral("needle") + QString(79, QLatin1Char('y')) +
        QString::fromUtf8("\xF0\x9F\x98\x80");
    auto surrogateParagraph = docxstudio::core::Paragraph::create(
        surrogateText.toStdU16String());
    check(static_cast<bool>(surrogateParagraph),
          "could not create surrogate-context paragraph");
    auto surrogateDocument = docxstudio::core::Document::create(
        {std::move(surrogateParagraph.value())});
    check(static_cast<bool>(surrogateDocument),
          "could not create surrogate-context document");
    docxstudio::app::DocumentCanvas surrogateCanvas(spelling);
    surrogateCanvas.setDocument(std::move(surrogateDocument.value()));
    const auto surrogateSearch = docxstudio::app::invokeEditorTool(
        surrogateCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", "needle"}},
        summary, error);
    check(error.isEmpty() && surrogateSearch.at("results").size() == 1,
          "surrogate-context search failed");
    const auto& surrogateMatch = surrogateSearch.at("results").at(0);
    const QString surrogateContext = QString::fromStdString(
        surrogateMatch.at("context").get<std::string>());
    check(surrogateMatch.at("start") == 81 && surrogateMatch.at("end") == 87 &&
              surrogateMatch.at("contextStart") == 2 &&
              surrogateMatch.at("contextEnd") == 166,
          "search returned incorrect UTF-16 context offsets");
    check(surrogateContext.size() == 164 &&
              surrogateContext.front() == QLatin1Char('x') &&
              surrogateContext.back() == QLatin1Char('y'),
          "search context split a UTF-16 surrogate pair");

    const QString astralQuery =
        QString::fromUtf8("\xF0\x9F\x98\x80").repeated(600);
    auto astralParagraph = docxstudio::core::Paragraph::create(
        astralQuery.toStdU16String());
    check(static_cast<bool>(astralParagraph),
          "could not create astral-query paragraph");
    auto astralDocument = docxstudio::core::Document::create(
        {std::move(astralParagraph.value())});
    check(static_cast<bool>(astralDocument),
          "could not create astral-query document");
    docxstudio::app::DocumentCanvas astralCanvas(spelling);
    astralCanvas.setDocument(std::move(astralDocument.value()));
    const auto astralSearch = docxstudio::app::invokeEditorTool(
        astralCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", astralQuery.toStdString()}},
        summary, error);
    check(error.isEmpty() && astralSearch.at("results").size() == 1 &&
              astralSearch.at("results").at(0).at("end") == 1200,
          "schema-valid astral query was not counted as Unicode code points");
    const QString tooManyAstral =
        QString::fromUtf8("\xF0\x9F\x98\x80").repeated(1025);
    const auto excessiveCodePointSearch =
        docxstudio::app::invokeEditorTool(
            astralCanvas, documentId,
            QString::fromLatin1(
                docxstudio::codex::kEditorSearchTool.data(),
                static_cast<qsizetype>(
                    docxstudio::codex::kEditorSearchTool.size())),
            {{"documentId", documentId.toStdString()},
             {"query", tooManyAstral.toStdString()}},
            summary, error);
    check(excessiveCodePointSearch.empty() &&
              error.contains(QStringLiteral("1024")),
          "search accepted more Unicode code points than its schema limit");

    const char32_t familyCodePoints[] = {
        0x1f468, 0x200d, 0x1f469, 0x200d, 0x1f467, 0x200d, 0x1f466};
    const QString family = QString::fromUcs4(familyCodePoints, 7);
    const QString graphemeText =
        QStringLiteral("A") + QString(90, QChar(0x0301)) +
        QStringLiteral("needle") + family.repeated(8);
    auto graphemeParagraph = docxstudio::core::Paragraph::create(
        graphemeText.toStdU16String());
    check(static_cast<bool>(graphemeParagraph),
          "could not create grapheme-context paragraph");
    auto graphemeDocument = docxstudio::core::Document::create(
        {std::move(graphemeParagraph.value())});
    check(static_cast<bool>(graphemeDocument),
          "could not create grapheme-context document");
    docxstudio::app::DocumentCanvas graphemeCanvas(spelling);
    graphemeCanvas.setDocument(std::move(graphemeDocument.value()));
    const auto graphemeSearch = docxstudio::app::invokeEditorTool(
        graphemeCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", "needle"}},
        summary, error);
    check(error.isEmpty() && graphemeSearch.at("results").size() == 1,
          "grapheme-context search failed");
    const auto& graphemeMatch = graphemeSearch.at("results").at(0);
    const qsizetype graphemeContextStart =
        static_cast<qsizetype>(graphemeMatch.at("contextStart").get<std::size_t>());
    const qsizetype graphemeContextEnd =
        static_cast<qsizetype>(graphemeMatch.at("contextEnd").get<std::size_t>());
    QTextBoundaryFinder originalGraphemes(QTextBoundaryFinder::Grapheme,
                                          graphemeText);
    originalGraphemes.setPosition(graphemeContextStart);
    check(originalGraphemes.isAtBoundary() && graphemeContextStart == 91,
          "search context began inside a combining sequence");
    originalGraphemes.setPosition(graphemeContextEnd);
    check(originalGraphemes.isAtBoundary() &&
              graphemeContextEnd >= 97 &&
              graphemeContextEnd < graphemeText.size(),
          "search context ended inside an emoji ZWJ sequence");
    const QString graphemeContext = QString::fromStdString(
        graphemeMatch.at("context").get<std::string>());
    check(graphemeContext.startsWith(QStringLiteral("needle")) &&
              !graphemeContext.endsWith(QChar(0x200d)),
          "grapheme-safe context omitted the match or retained a dangling joiner");

    const auto partialGraphemeSearch = docxstudio::app::invokeEditorTool(
        graphemeCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", "A"}},
        summary, error);
    check(error.isEmpty() && partialGraphemeSearch.at("results").empty(),
          "search returned an edit range that splits a combining sequence");

    const QString adversarialWord(30000, QLatin1Char('a'));
    auto adversarialParagraph = docxstudio::core::Paragraph::create(
        adversarialWord.toStdU16String());
    check(static_cast<bool>(adversarialParagraph),
          "could not create adversarial search paragraph");
    auto adversarialDocument = docxstudio::core::Document::create(
        {std::move(adversarialParagraph.value())});
    check(static_cast<bool>(adversarialDocument),
          "could not create adversarial search document");
    docxstudio::app::DocumentCanvas adversarialCanvas(spelling);
    adversarialCanvas.setDocument(std::move(adversarialDocument.value()));
    QElapsedTimer searchTimer;
    searchTimer.start();
    const auto adversarialSearch = docxstudio::app::invokeEditorTool(
        adversarialCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "a"},
         {"wholeWord", true}},
        summary, error);
    check(error.isEmpty() && adversarialSearch.at("results").empty() &&
              !adversarialSearch.at("truncated").get<bool>(),
          "adversarial whole-word search returned an incorrect result");
    check(searchTimer.elapsed() < 3000,
          "whole-word search rebuilt boundary tables per candidate match");

    docxstudio::app::DocumentCanvas formattingCanvas(spelling);
    formattingCanvas.setDocument(formattedBoundaryDocument());
    const auto formattingBase = formattingCanvas.snapshot();
    const std::string formattingBlockId =
        formattingBase.document.paragraphs().front().id().toString();
    const auto formattingRead = docxstudio::app::invokeEditorTool(
        formattingCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    check(error.isEmpty(), "formatted Codex read failed");
    const auto& style = formattingRead.at("blocks").at(0)
                            .at("attributes").at("characterRuns").at(0)
                            .at("style");
    check(style.at("foregroundColor") == "#336699" &&
              style.at("highlightColor") == "#FFCC00" &&
              style.at("verticalAlign") == "superscript",
          "Codex read omitted color, highlight, or vertical alignment");

    const auto colorReplacement = docxstudio::app::invokeEditorTool(
        formattingCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", formattingBase.revision.value()},
         {"summary", "Replace the colored word"},
         {"operations",
          {{{"kind", "replace_text"},
            {"target", {{"blockId", formattingBlockId}, {"start", 1}, {"end", 5}}},
            {"text", "navy"}}}}},
        summary, error);
    check(error.isEmpty() && !colorReplacement.empty(),
          "Codex could not preview a colored boundary replacement");
    check(formattingCanvas.acceptPreview(error),
          "Codex colored boundary preview could not be accepted");
    check(paragraphText(formattingCanvas) == QStringLiteral("Anavy"),
          "Codex colored boundary replacement produced wrong text");
    const auto replacedFormat = formattingCanvas.snapshot().document.paragraphs()
                                    .front().characterFormatAt(2);
    check(replacedFormat.foreground_argb == 0xff336699U &&
              replacedFormat.highlight_argb == 0xffffcc00U &&
              replacedFormat.baseline ==
                  docxstudio::core::BaselinePosition::superscript,
          "Codex replace_text inherited the preceding black run");
    check(!formattingCanvas.hasNonTextChanges(),
          "format-preserving Codex replacement was classified as a format edit");

    docxstudio::app::DocumentCanvas sequentialCanvas(spelling);
    sequentialCanvas.setDocument(shiftedFormattingDocument());
    const auto sequentialBase = sequentialCanvas.snapshot();
    const std::string sequentialBlockId =
        sequentialBase.document.paragraphs().front().id().toString();
    const auto sequentialPreview = docxstudio::app::invokeEditorTool(
        sequentialCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", sequentialBase.revision.value()},
         {"summary", "Apply sequential replacements"},
         {"operations",
          {{{"kind", "replace_text"},
            {"target", {{"blockId", sequentialBlockId}, {"start", 0}, {"end", 2}}},
            {"text", "AAAA"}},
           {{"kind", "replace_text"},
            {"target", {{"blockId", sequentialBlockId}, {"start", 4}, {"end", 8}}},
            {"text", "X"}}}}},
        summary, error);
    check(error.isEmpty() && !sequentialPreview.empty(),
          "sequential shifted-offset preview was rejected");
    check(sequentialCanvas.acceptPreview(error),
          "sequential shifted-offset preview could not be accepted");
    check(paragraphText(sequentialCanvas) == QStringLiteral("AAAAX"),
          "sequential shifted-offset replacement produced wrong text");
    check(sequentialCanvas.snapshot().document.paragraphs().front()
              .characterFormatAt(5).foreground_argb == 0xff336699U,
          "sequential replacement resolved formatting against the stale base");
    check(!sequentialCanvas.hasNonTextChanges(),
          "format-preserving sequential replacements were classified as format edits");

    docxstudio::app::DocumentCanvas pageBreakCanvas(spelling);
    pageBreakCanvas.insertText(QStringLiteral("abcdef"));
    pageBreakCanvas.markSaved();
    const auto pageBreakBase = pageBreakCanvas.snapshot();
    const std::string pageBreakBlockId =
        pageBreakBase.document.paragraphs().front().id().toString();
    const auto pageBreakPreview = docxstudio::app::invokeEditorTool(
        pageBreakCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", pageBreakBase.revision.value()},
         {"summary", "Insert a page break in the paragraph"},
         {"operations",
          {{{"kind", "insert_page_break"},
            {"target", {{"blockId", pageBreakBlockId}, {"start", 3}}}}}}},
        summary, error);
    check(error.isEmpty() && !pageBreakPreview.empty(),
          "mid-paragraph page-break preview was rejected");
    check(pageBreakCanvas.acceptPreview(error),
          "mid-paragraph page-break preview could not be accepted");
    check(paragraphTexts(pageBreakCanvas) ==
              QStringList{QStringLiteral("abc"), QStringLiteral("def")},
          "Codex page break did not split at the requested UTF-16 offset");
    check(pageBreakCanvas.snapshot().document.paragraphs().back()
              .format().page_break_before == true,
          "Codex page break did not format the split-off paragraph");
    pageBreakCanvas.undo();
    check(paragraphTexts(pageBreakCanvas) == QStringList{QStringLiteral("abcdef")},
          "Codex page break was not one undo transaction");

    const auto unsupportedScope = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "viewport"}},
        summary, error);
    check(unsupportedScope.empty(),
          "unsupported viewport scope fell through to a document read");
    check(error.contains(QStringLiteral("Unsupported editor read scope")),
          "unsupported viewport scope did not return a clear error");

    docxstudio::app::DocumentCanvas unicodeCanvas(spelling);
    unicodeCanvas.insertText(QString::fromUtf8("A😀BC"));
    unicodeCanvas.selectAll();
    unicodeCanvas.toggleBold();
    const auto truncatedRead = docxstudio::app::invokeEditorTool(
        unicodeCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "selection"},
         {"includeFormatting", true},
         {"maxCharacters", 2}},
        summary, error);
    check(error.isEmpty(), "UTF-16 boundary read failed");
    check(truncatedRead.at("truncated") == true,
          "bounded UTF-16 read was not marked truncated");
    const auto& truncatedBlock = truncatedRead.at("blocks").at(0);
    check(truncatedBlock.at("text") == "A",
          "bounded read split a UTF-16 surrogate pair");
    check(truncatedBlock.at("attributes").at("characterRuns").at(0).at("end") == 1,
          "formatting range was not clipped to returned text");
    check(truncatedBlock.at("attributes").at("selectionStart") == 0 &&
              truncatedBlock.at("attributes").at("selectionEnd") == 1,
          "selection range was not clipped to returned text");

    const auto preview = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", before.revision.value()},
         {"summary", "Correct spelling and emphasize the greeting"},
         {"operations",
          {{{"kind", "replace_text"},
            {"target", {{"blockId", blockId}, {"start", 0}, {"end", 4}}},
            {"text", "Hello"}},
           {{"kind", "set_text_style"},
            {"target", {{"blockId", blockId}, {"start", 0}, {"end", 5}}},
            {"style", {{"bold", true}}}}}}},
        summary, error);
    check(error.isEmpty(), "semantic preview creation failed");
    check(preview.at("committed") == false, "tool committed a preview");
    check(!preview.at("previewId").get<std::string>().empty(), "preview has no id");
    check(canvas.hasPreview(), "canvas did not retain the preview branch");
    check(paragraphText(canvas) == QStringLiteral("Helo world"),
          "preview changed the authoritative document");
    check(!canvas.isModified(), "preview marked the live document modified");
    check(!canvas.hasNonTextChanges(),
          "preview marked the live document as having formatting changes");

    check(canvas.acceptPreview(error), "preview could not be accepted");
    check(paragraphText(canvas) == QStringLiteral("Hello world"),
          "accepted semantic preview has wrong text");
    check(canvas.snapshot().document.paragraphs().front()
              .characterFormatAt(1).bold.value_or(false),
          "accepted semantic preview lost formatting");
    check(canvas.hasNonTextChanges(),
          "accepted formatting preview was not recorded as a non-text change");
    canvas.undo();
    check(paragraphText(canvas) == QStringLiteral("Helo world"),
          "semantic preview was not one undo transaction");
    canvas.markSaved();

    const auto equationBase = canvas.snapshot();
    const auto equation = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", equationBase.revision.value()},
         {"summary", "Insert a fraction"},
         {"operations",
          {{{"kind", "insert_equation"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"latex", "\\frac{ a }{b}"},
            {"display", true}}}}},
        summary, error);
    check(error.isEmpty(), "safe equation preview was rejected");
    check(!equation.empty(), "safe equation preview was not created");
    check(equation.at("warnings").empty(),
          "semantic equation preview returned a stale marker warning");
    check(canvas.hasPreview(), "semantic equation preview was not retained");
    check(paragraphText(canvas) == QStringLiteral("Helo world"),
          "semantic equation preview changed the authoritative document");
    check(canvas.acceptPreview(error), "safe equation preview could not be accepted");
    const auto equationSnapshot = canvas.snapshot();
    const auto& equationParagraph = equationSnapshot.document.paragraphs().front();
    check(equationParagraph.text().front() ==
              docxstudio::core::kInlineObjectReplacementCharacter,
          "accepted equation did not occupy one semantic inline-object position");
    check(equationParagraph.equations().size() == 1,
          "accepted equation has no semantic atom");
    const auto& equationAtom = equationParagraph.equations().front();
    check(equationAtom.utf16_offset == 0 &&
              equationAtom.canonical_latex == "\\frac{a}{b}" &&
              equationAtom.display,
          "equation source was not canonicalized or its display flag was lost");
    check(canvas.hasNonTextChanges(),
          "semantic equation preview was not classified as a non-text change");

    const auto equationRead = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "document"},
         {"includeFormatting", false}},
        summary, error);
    check(error.isEmpty(), "semantic equation read failed");
    const auto& equationBlock = equationRead.at("blocks").at(0);
    const QString equationText =
        QString::fromStdString(equationBlock.at("text").get<std::string>());
    check(equationText.size() == QStringLiteral("Helo world").size() + 1 &&
              equationText.front().unicode() ==
                  docxstudio::core::kInlineObjectReplacementCharacter,
          "equation read did not retain offset-safe U+FFFC text");
    const auto& equationMetadata =
        equationBlock.at("attributes").at("equations");
    check(equationMetadata.size() == 1,
          "equation read omitted semantic metadata");
    check(equationMetadata.at(0).at("id") == equationAtom.id.toString() &&
              equationMetadata.at(0).at("utf16Offset") == 0 &&
              equationMetadata.at(0).at("canonicalLatex") ==
                  "\\frac{a}{b}" &&
              equationMetadata.at(0).at("display") == true,
          "equation read returned incorrect semantic metadata");
    canvas.undo();
    check(paragraphText(canvas) == QStringLiteral("Helo world") &&
              canvas.snapshot().document.paragraphs().front().equations().empty(),
          "semantic equation insertion was not one undo transaction");

    const auto oversized = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", canvas.snapshot().revision.value()},
         {"summary", "Oversized equation"},
         {"operations",
          {{{"kind", "insert_equation"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"latex", std::string(8U * 1024U + 1U, 'x')}}}}},
        summary, error);
    check(oversized.empty(), "oversized LaTeX equation unexpectedly succeeded");
    check(error.contains(QStringLiteral("exceeds"), Qt::CaseInsensitive),
          "oversized LaTeX equation did not return a bounded-input error");

    const auto forbidden = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", canvas.snapshot().revision.value()},
         {"summary", "Unsafe equation"},
         {"operations",
          {{{"kind", "insert_equation"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"latex", "\\input{/etc/passwd}"}}}}},
        summary, error);
    check(forbidden.empty(), "forbidden LaTeX command unexpectedly succeeded");
    check(error.contains(QStringLiteral("Forbidden"), Qt::CaseInsensitive),
          "forbidden LaTeX command did not return a useful error");

    const auto stale = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", before.revision.value()},
         {"summary", "Stale edit"},
         {"operations",
          {{{"kind", "insert_text"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"text", "X"}}}}},
        summary, error);
    check(stale.empty(), "stale preview unexpectedly succeeded");
    check(error.contains(QStringLiteral("REVISION_CONFLICT")),
          "stale preview did not report a revision conflict");

    std::cout << "editor tool bridge tests passed\n";
    return 0;
}

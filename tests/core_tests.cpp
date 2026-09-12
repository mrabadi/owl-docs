#include "docxstudio/core/document_session.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace docxstudio::core;

namespace {

int failures = 0;

#define CHECK(expression)                                                                    \
    do {                                                                                     \
        if (!(expression)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression     \
                      << '\n';                                                               \
            ++failures;                                                                      \
        }                                                                                    \
    } while (false)

Document documentWithText(const std::u16string& text) {
    auto paragraph = Paragraph::create(text);
    CHECK(paragraph.hasValue());
    auto document = Document::create({paragraph.value()});
    CHECK(document.hasValue());
    return document.value();
}

std::vector<std::uint8_t> tinyPngBytes() {
    // A real 2 x 2 RGB PNG. The core validates its complete container but
    // intentionally leaves pixel decoding to the raster/UI layer.
    return {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
        0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x02U,
        0x08U, 0x02U, 0x00U, 0x00U, 0x00U, 0xfdU, 0xd4U, 0x9aU,
        0x73U, 0x00U, 0x00U, 0x00U, 0x16U, 0x49U, 0x44U, 0x41U,
        0x54U, 0x78U, 0x9cU, 0x63U, 0x7cU, 0x16U, 0xa2U, 0xc0U,
        0xc0U, 0xc0U, 0xc0U, 0xc4U, 0xc0U, 0xc0U, 0xc0U, 0xc0U,
        0xc0U, 0x00U, 0x00U, 0x11U, 0x28U, 0x01U, 0x5eU, 0xb8U,
        0xf9U, 0xb4U, 0x71U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U,
        0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
    };
}

std::vector<std::uint8_t> tinyJpegBytes() {
    // A real 1 x 1 grayscale baseline JPEG.
    return {
        0xffU, 0xd8U, 0xffU, 0xe0U, 0x00U, 0x10U, 0x4aU, 0x46U,
        0x49U, 0x46U, 0x00U, 0x01U, 0x01U, 0x00U, 0x00U, 0x01U,
        0x00U, 0x01U, 0x00U, 0x00U, 0xffU, 0xdbU, 0x00U, 0x43U,
        0x00U, 0x06U, 0x04U, 0x05U, 0x06U, 0x05U, 0x04U, 0x06U,
        0x06U, 0x05U, 0x06U, 0x07U, 0x07U, 0x06U, 0x08U, 0x0aU,
        0x10U, 0x0aU, 0x0aU, 0x09U, 0x09U, 0x0aU, 0x14U, 0x0eU,
        0x0fU, 0x0cU, 0x10U, 0x17U, 0x14U, 0x18U, 0x18U, 0x17U,
        0x14U, 0x16U, 0x16U, 0x1aU, 0x1dU, 0x25U, 0x1fU, 0x1aU,
        0x1bU, 0x23U, 0x1cU, 0x16U, 0x16U, 0x20U, 0x2cU, 0x20U,
        0x23U, 0x26U, 0x27U, 0x29U, 0x2aU, 0x29U, 0x19U, 0x1fU,
        0x2dU, 0x30U, 0x2dU, 0x28U, 0x30U, 0x25U, 0x28U, 0x29U,
        0x28U, 0xffU, 0xc0U, 0x00U, 0x0bU, 0x08U, 0x00U, 0x01U,
        0x00U, 0x01U, 0x01U, 0x01U, 0x11U, 0x00U, 0xffU, 0xc4U,
        0x00U, 0x14U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0xffU, 0xc4U, 0x00U, 0x14U,
        0x10U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0xffU, 0xdaU, 0x00U, 0x08U, 0x01U, 0x01U,
        0x00U, 0x00U, 0x3fU, 0x00U, 0x3fU, 0xffU, 0xd9U,
    };
}

EncodedImagePayload maximumSizedJpegPayload() {
    const auto tiny = tinyJpegBytes();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMaximumEncodedImageBytes);
    bytes.insert(bytes.end(), tiny.begin(), tiny.begin() + 2);
    const std::size_t suffix_size = tiny.size() - 2U;
    while (bytes.size() + suffix_size < kMaximumEncodedImageBytes) {
        const std::size_t remaining =
            kMaximumEncodedImageBytes - bytes.size() - suffix_size;
        CHECK(remaining >= 4U);
        std::size_t segment_size = std::min<std::size_t>(remaining, 65537U);
        const std::size_t tail = remaining - segment_size;
        if (tail > 0U && tail < 4U) {
            segment_size -= 4U - tail;
        }
        const std::size_t payload_size = segment_size - 4U;
        const auto declared_size =
            static_cast<std::uint16_t>(payload_size + 2U);
        bytes.push_back(0xffU);
        bytes.push_back(0xe1U);
        bytes.push_back(static_cast<std::uint8_t>(declared_size >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(declared_size));
        bytes.insert(bytes.end(), payload_size, 0U);
    }
    bytes.insert(bytes.end(), tiny.begin() + 2, tiny.end());
    CHECK(bytes.size() == kMaximumEncodedImageBytes);
    return EncodedImagePayload(std::move(bytes));
}

void testNodeIdsAndRevision() {
    const auto first = NodeId::generate();
    const auto second = NodeId::generate();
    CHECK(first.isValid());
    CHECK(second.isValid());
    CHECK(first != second);

    const auto encoded = first.toString();
    CHECK(encoded.size() == 36);
    const auto decoded = NodeId::parse(encoded);
    CHECK(decoded.has_value());
    CHECK(*decoded == first);
    CHECK(!NodeId::parse("not-an-identifier").has_value());
    CHECK(!NodeId::parse("00000000-0000-0000-0000-000000000000").has_value());

    Revision revision{41};
    CHECK(revision.next().has_value());
    CHECK(revision.next()->value() == 42);
}

void testUtf16BoundariesAndAtomicBatch() {
    DocumentSession session(documentWithText(u"A\U0001F600B"));
    const auto initial = session.snapshot();
    const auto paragraph_id = initial.document.paragraphs().front().id();
    CHECK(initial.document.paragraphs().front().text().size() == 4);

    // Offset two sits between the high and low surrogate for the emoji.
    std::vector<Operation> invalid{
        InsertText{Position{paragraph_id, 4}, u"!", std::nullopt},
        InsertText{Position{paragraph_id, 2}, u"x", std::nullopt},
    };
    const auto rejected = session.applyBatch(initial.revision, invalid);
    CHECK(!rejected);
    CHECK(rejected.error().code == ErrorCode::invalid_position);
    const auto after_rejection = session.snapshot();
    CHECK(after_rejection.revision == initial.revision);
    CHECK(after_rejection.document == initial.document);

    std::vector<Operation> valid{InsertText{Position{paragraph_id, 3}, u"x", std::nullopt}};
    const auto accepted = session.applyBatch(initial.revision, valid);
    CHECK(accepted);
    CHECK(accepted.value().revision == Revision{1});
    CHECK(session.snapshot().document.paragraphs().front().text() == u"A\U0001F600xB");

    const auto stale = session.applyBatch(initial.revision, valid);
    CHECK(!stale);
    CHECK(stale.error().code == ErrorCode::revision_conflict);
}

void testSparseFormatting() {
    DocumentSession session(documentWithText(u"abcd"));
    auto snapshot = session.snapshot();
    const auto paragraph_id = snapshot.document.paragraphs().front().id();

    CharacterFormatDelta make_bold;
    make_bold.bold = PropertyDelta<bool>::set(true);
    make_bold.font_size_half_points = PropertyDelta<std::int32_t>::set(24);
    std::vector<Operation> formatting{
        SetCharacterFormat{Range{{paragraph_id, 1}, {paragraph_id, 3}}, make_bold},
    };
    auto result = session.applyBatch(snapshot.revision, formatting);
    CHECK(result);

    snapshot = session.snapshot();
    const auto& paragraph = snapshot.document.paragraphs().front();
    CHECK(paragraph.characterFormats().size() == 1);
    CHECK(paragraph.characterFormats().front().start == 1);
    CHECK(paragraph.characterFormats().front().end == 3);
    CHECK(paragraph.characterFormatAt(2).bold == true);
    CHECK(paragraph.characterFormatAt(2).font_size_half_points == 24);

    // Text inserted at a run boundary inherits the direct format on its left.
    std::vector<Operation> insertion{
        InsertText{Position{paragraph_id, 3}, u"X", std::nullopt},
    };
    result = session.applyBatch(snapshot.revision, insertion);
    CHECK(result);
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().characterFormatAt(4).bold == true);

    CharacterFormatDelta clear_bold;
    clear_bold.bold = PropertyDelta<bool>::clear();
    std::vector<Operation> clearing{
        SetCharacterFormat{Range{{paragraph_id, 1}, {paragraph_id, 4}}, clear_bold},
    };
    result = session.applyBatch(snapshot.revision, clearing);
    CHECK(result);
    snapshot = session.snapshot();
    const auto cleared = snapshot.document.paragraphs().front().characterFormatAt(2);
    CHECK(!cleared.bold.has_value());
    CHECK(cleared.font_size_half_points == 24);

    ParagraphFormatDelta paragraph_delta;
    paragraph_delta.alignment = PropertyDelta<ParagraphAlignment>::set(
        ParagraphAlignment::justified);
    paragraph_delta.space_after_emu = PropertyDelta<std::int64_t>::set(152400);
    std::vector<Operation> paragraph_format{
        SetParagraphFormat{{paragraph_id}, paragraph_delta},
    };
    result = session.applyBatch(snapshot.revision, paragraph_format);
    CHECK(result);
    const auto paragraph_snapshot = session.snapshot();
    CHECK(paragraph_snapshot.document.paragraphs().front().format().alignment ==
          ParagraphAlignment::justified);
    CHECK(paragraph_snapshot.document.paragraphs().front().format().space_after_emu == 152400);
}

void testSemanticListFormatting() {
    const ListLayout defaults;
    for (std::size_t level = 0; level < kListLevelCount; ++level) {
        CHECK(defaults.levels[level].bullet_indent_spaces ==
              static_cast<std::int32_t>(level * 4U));
        CHECK(defaults.levels[level].text_indent_spaces == 2);
    }

    ParagraphFormat incomplete;
    incomplete.list_level = std::uint8_t{0};
    CHECK(!incomplete.validate());

    DocumentSession session(documentWithText(u"\u2022\tOne"));
    auto snapshot = session.snapshot();
    const auto paragraphId = snapshot.document.paragraphs().front().id();
    const auto listId = NodeId::generate();
    ParagraphFormatDelta createList;
    createList.list_id = PropertyDelta<NodeId>::set(listId);
    createList.list_level = PropertyDelta<std::uint8_t>::set(std::uint8_t{2});
    createList.list_layout = PropertyDelta<ListLayout>::set(defaults);
    CHECK(session.applyBatch(snapshot.revision,
                             std::vector<Operation>{SetParagraphFormat{
                                 {paragraphId}, createList}}));
    snapshot = session.snapshot();
    const auto& created = snapshot.document.paragraphs().front().format();
    CHECK(created.list_id == listId);
    CHECK(created.list_level == std::uint8_t{2});
    CHECK(created.list_layout == defaults);

    auto customized = defaults;
    customized.levels[2].bullet_indent_spaces = 11;
    customized.levels[2].text_indent_spaces = 3;
    ParagraphFormatDelta updateLayout;
    updateLayout.list_layout = PropertyDelta<ListLayout>::set(customized);
    CHECK(session.applyBatch(snapshot.revision,
                             std::vector<Operation>{SetParagraphFormat{
                                 {paragraphId}, updateLayout}}));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().format().list_layout == customized);

    auto invalidLayout = customized;
    invalidLayout.levels[0].bullet_indent_spaces = kMaximumListIndentSpaces + 1;
    ParagraphFormatDelta invalidLayoutDelta;
    invalidLayoutDelta.list_layout = PropertyDelta<ListLayout>::set(invalidLayout);
    const auto beforeInvalid = snapshot;
    CHECK(!session.applyBatch(snapshot.revision,
                              std::vector<Operation>{SetParagraphFormat{
                                  {paragraphId}, invalidLayoutDelta}}));
    CHECK(session.snapshot().revision == beforeInvalid.revision);
    CHECK(session.snapshot().document == beforeInvalid.document);

    ParagraphFormatDelta invalidLevel;
    invalidLevel.list_level = PropertyDelta<std::uint8_t>::set(
        static_cast<std::uint8_t>(kListLevelCount));
    CHECK(!session.applyBatch(snapshot.revision,
                              std::vector<Operation>{SetParagraphFormat{
                                  {paragraphId}, invalidLevel}}));

    ParagraphFormatDelta partialClear;
    partialClear.list_id = PropertyDelta<NodeId>::clear();
    CHECK(!session.applyBatch(snapshot.revision,
                              std::vector<Operation>{SetParagraphFormat{
                                  {paragraphId}, partialClear}}));

    const auto rightId = NodeId::generate();
    CHECK(session.applyBatch(snapshot.revision,
                             std::vector<Operation>{SplitParagraph{
                                 {paragraphId, 2}, rightId}}));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().size() == 2);
    CHECK(snapshot.document.paragraphs()[1].format() ==
          snapshot.document.paragraphs()[0].format());
    CHECK(session.undo(snapshot.revision));
    CHECK(session.snapshot().document.paragraphs().size() == 1);
    CHECK(session.snapshot().document.paragraphs().front().format().list_layout == customized);
}

void testParagraphStructureAndHistory() {
    DocumentSession session(documentWithText(u"abcd"));
    auto snapshot = session.snapshot();
    const auto first_id = snapshot.document.paragraphs().front().id();
    const auto second_id = NodeId::generate();

    std::vector<Operation> split{SplitParagraph{Position{first_id, 2}, second_id}};
    auto result = session.applyBatch(snapshot.revision, split);
    CHECK(result);
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().size() == 2);
    CHECK(snapshot.document.bodyBlocks().size() == 2);
    CHECK(snapshot.document.bodyBlocks()[0].kind == BodyBlockKind::paragraph);
    CHECK(snapshot.document.bodyBlocks()[0].id == first_id);
    CHECK(snapshot.document.bodyBlocks()[1].id == second_id);
    CHECK(snapshot.document.paragraphs()[0].text() == u"ab");
    CHECK(snapshot.document.paragraphs()[1].id() == second_id);
    CHECK(snapshot.document.paragraphs()[1].text() == u"cd");

    // A cross-paragraph deletion also removes the paragraph boundary.
    std::vector<Operation> deletion{
        DeleteRange{Range{{first_id, 1}, {second_id, 1}}},
    };
    result = session.applyBatch(snapshot.revision, deletion);
    CHECK(result);
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().size() == 1);
    CHECK(snapshot.document.bodyBlocks().size() == 1);
    CHECK(snapshot.document.bodyBlocks().front().id == first_id);
    CHECK(snapshot.document.paragraphs().front().id() == first_id);
    CHECK(snapshot.document.paragraphs().front().text() == u"ad");

    auto undo = session.undo(snapshot.revision);
    CHECK(undo);
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().size() == 2);
    CHECK(snapshot.document.paragraphs()[0].text() == u"ab");
    CHECK(snapshot.document.paragraphs()[1].text() == u"cd");

    undo = session.undo(snapshot.revision);
    CHECK(undo);
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().size() == 1);
    CHECK(snapshot.document.paragraphs().front().text() == u"abcd");

    auto redo = session.redo(snapshot.revision);
    CHECK(redo);
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().size() == 2);
    CHECK(snapshot.document.paragraphs()[1].id() == second_id);
}

void testUndoCoalescing() {
    DocumentSession session(documentWithText(u""));
    auto snapshot = session.snapshot();
    const auto paragraph_id = snapshot.document.paragraphs().front().id();

    std::vector<Operation> first{
        InsertText{Position{paragraph_id, 0}, u"a", std::nullopt}};
    CHECK(session.applyBatch(snapshot.revision, first));
    snapshot = session.snapshot();
    std::vector<Operation> second{
        InsertText{Position{paragraph_id, 1}, u"b", std::nullopt}};
    CHECK(session.applyBatch(snapshot.revision, second,
                             UndoGrouping::coalesce_with_previous));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().text() == u"ab");

    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().text().empty());
    CHECK(session.redo(snapshot.revision));
    CHECK(session.snapshot().document.paragraphs().front().text() == u"ab");
}

void testPreviewIsolationAndAcceptance() {
    DocumentSession session(documentWithText(u"base"));
    auto live = session.snapshot();
    const auto paragraph_id = live.document.paragraphs().front().id();

    auto preview_result = session.createPreview(live.revision);
    CHECK(preview_result);
    const auto preview_id = preview_result.value().id;
    std::vector<Operation> preview_edit{
        InsertText{Position{paragraph_id, 4}, u" draft", std::nullopt},
    };
    auto preview_batch = session.applyPreviewBatch(preview_id, Revision{}, preview_edit);
    CHECK(preview_batch);
    CHECK(preview_batch.value().revision == Revision{1});
    CHECK(session.snapshot().document.paragraphs().front().text() == u"base");
    CHECK(session.previewSnapshot(preview_id).value().document.paragraphs().front().text() ==
          u"base draft");

    auto preview_undo = session.undoPreview(preview_id, Revision{1});
    CHECK(preview_undo);
    CHECK(session.previewSnapshot(preview_id).value().document.paragraphs().front().text() ==
          u"base");
    auto preview_redo = session.redoPreview(preview_id, Revision{2});
    CHECK(preview_redo);
    CHECK(session.previewSnapshot(preview_id).value().document.paragraphs().front().text() ==
          u"base draft");

    auto accepted = session.acceptPreview(preview_id, live.revision, Revision{3});
    CHECK(accepted);
    live = session.snapshot();
    CHECK(live.revision == Revision{1});
    CHECK(live.document.paragraphs().front().text() == u"base draft");
    CHECK(session.previewCount() == 0);

    auto undo = session.undo(live.revision);
    CHECK(undo);
    CHECK(session.snapshot().document.paragraphs().front().text() == u"base");
}

void testStaleAndIndependentPreviews() {
    DocumentSession session(documentWithText(u"x"));
    auto live = session.snapshot();
    const auto paragraph_id = live.document.paragraphs().front().id();
    auto first = session.createPreview(live.revision);
    auto second = session.createPreview(live.revision);
    CHECK(first && second);

    std::vector<Operation> first_edit{
        InsertText{Position{paragraph_id, 1}, u"1", std::nullopt},
    };
    std::vector<Operation> second_edit{
        InsertText{Position{paragraph_id, 1}, u"2", std::nullopt},
    };
    CHECK(session.applyPreviewBatch(first.value().id, Revision{}, first_edit));
    CHECK(session.applyPreviewBatch(second.value().id, Revision{}, second_edit));
    CHECK(session.previewSnapshot(first.value().id).value().document.paragraphs().front().text() ==
          u"x1");
    CHECK(session.previewSnapshot(second.value().id).value().document.paragraphs().front().text() ==
          u"x2");

    std::vector<Operation> live_edit{
        InsertText{Position{paragraph_id, 1}, u"L", std::nullopt},
    };
    CHECK(session.applyBatch(live.revision, live_edit));
    live = session.snapshot();
    auto stale_accept = session.acceptPreview(first.value().id, live.revision, Revision{1});
    CHECK(!stale_accept);
    CHECK(stale_accept.error().code == ErrorCode::preview_conflict);
    CHECK(session.discardPreview(first.value().id));
    CHECK(session.discardPreview(second.value().id));
    CHECK(session.previewCount() == 0);
}

void testSemanticImageAtomEditing() {
    CHECK(kMaximumInlineImagesPerDocument == 512);
    CHECK(kMaximumEncodedImageBytes == 16U * 1024U * 1024U);
    CHECK(kMaximumDocumentEncodedImageBytes == 32U * 1024U * 1024U);
    CHECK(kMaximumImageAccessibleNameBytes == 4U * 1024U);
    CHECK(kMaximumInlineImageDimensionEmu == 254000000);
    CHECK(imageContentType(ImageFormat::png) == "image/png");
    CHECK(imageContentType(ImageFormat::jpeg) == "image/jpeg");
    CHECK(imageContentType(static_cast<ImageFormat>(255)).empty());

    const EncodedImagePayload payload(tinyPngBytes());
    const EncodedImagePayload jpeg_payload(tinyJpegBytes());
    const EncodedImagePayload shared = payload;
    const EncodedImagePayload equal_value(tinyPngBytes());
    const EncodedImagePayload different(tinyJpegBytes());
    CHECK(payload == shared);
    CHECK(payload == equal_value);
    CHECK(payload != different);
    CHECK(payload.bytes().data() == shared.bytes().data());
    CHECK(payload.bytes().data() != equal_value.bytes().data());

    auto surrogate_document = documentWithText(u"A\U0001F600B");
    const auto surrogate_paragraph =
        surrogate_document.paragraphs().front().id();
    const auto surrogate_original = surrogate_document;
    const auto invalid_image_id = NodeId::generate();
    CHECK(!surrogate_document.insertImage(
        {surrogate_paragraph, 2}, payload, ImageFormat::png, "diagram",
        914400, 914400, invalid_image_id));
    CHECK(surrogate_document == surrogate_original);

    auto invalid_document = documentWithText(u"ab");
    const auto invalid_paragraph = invalid_document.paragraphs().front().id();
    const auto invalid_original = invalid_document;
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, EncodedImagePayload{}, ImageFormat::png, {},
        914400, 914400, invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, static_cast<ImageFormat>(255), {},
        914400, 914400, invalid_image_id));
    auto malformed_png = tinyPngBytes();
    malformed_png.pop_back();
    const auto cheap_duplicate_rejection = invalid_document.insertImage(
        {invalid_paragraph, 1}, EncodedImagePayload(malformed_png),
        ImageFormat::png, {}, 914400, 914400, invalid_paragraph);
    CHECK(!cheap_duplicate_rejection);
    CHECK(cheap_duplicate_rejection.error().code ==
          ErrorCode::duplicate_node_id);
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, EncodedImagePayload(std::move(malformed_png)),
        ImageFormat::png, {}, 914400, 914400, invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::jpeg, {}, 914400,
        914400, invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png,
        std::string(kMaximumImageAccessibleNameBytes + 1U, 'a'), 914400,
        914400, invalid_image_id));
    const std::string invalid_utf8{"\xc0\xaf", 2};
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png, invalid_utf8,
        914400, 914400, invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png, {}, 0, 914400,
        invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png, {}, 914400, -1,
        invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png, {},
        kMaximumInlineImageDimensionEmu + 1, 914400, invalid_image_id));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png, {}, 914400,
        914400, NodeId{}));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, payload, ImageFormat::png, {}, 914400,
        914400, invalid_paragraph));
    const EncodedImagePayload oversized_payload(
        std::vector<std::uint8_t>(kMaximumEncodedImageBytes + 1U, 0x5a));
    CHECK(!invalid_document.insertImage(
        {invalid_paragraph, 1}, oversized_payload, ImageFormat::png, {},
        914400, 914400, invalid_image_id));
    CHECK(invalid_document == invalid_original);

    auto document = documentWithText(u"ab");
    const auto paragraph_id = document.paragraphs().front().id();
    const auto image_id = NodeId::generate();
    CharacterFormat character_format;
    character_format.italic = true;
    CHECK(document.insertImage(
        {paragraph_id, 1}, payload, ImageFormat::png, "Revenue diagram",
        914400, 457200, image_id, character_format));
    CHECK(document.paragraphs().front().text() == u"a\ufffcb");
    CHECK(document.paragraphs().front().images().size() == 1);
    const auto* image = document.paragraphs().front().imageAt(1);
    CHECK(image != nullptr);
    if (image) {
        CHECK(image->id == image_id);
        CHECK(image->utf16_offset == 1);
        CHECK(image->format == ImageFormat::png);
        CHECK(imageContentType(image->format) == "image/png");
        CHECK(image->accessible_name == "Revenue diagram");
        CHECK(image->width_emu == 914400);
        CHECK(image->height_emu == 457200);
        CHECK(image->encoded_payload == payload);
        CHECK(image->encoded_payload.bytes().data() == payload.bytes().data());
    }
    CHECK(document.findImage(image_id) == image);
    CHECK(document.paragraphs().front().characterFormatAt(2).italic == true);

    const auto* encoded_address =
        document.findImage(image_id)->encoded_payload.bytes().data();
    CHECK(document.insertText({paragraph_id, 0}, u"X"));
    CHECK(document.paragraphs().front().imageAt(2)->id == image_id);
    CHECK(document.findImage(image_id)->encoded_payload.bytes().data() ==
          encoded_address);
    CHECK(document.insertText({paragraph_id, 2}, u"Y"));
    CHECK(document.paragraphs().front().imageAt(3)->id == image_id);
    CHECK(document.insertText({paragraph_id, 4}, u"Z"));
    CHECK(document.paragraphs().front().imageAt(3)->id == image_id);
    CHECK(document.deleteRange({{paragraph_id, 3}, {paragraph_id, 3}}));
    CHECK(document.findImage(image_id) != nullptr);
    CHECK(document.deleteRange({{paragraph_id, 3}, {paragraph_id, 4}}));
    CHECK(document.findImage(image_id) == nullptr);

    auto ordered = documentWithText(u"Q");
    const auto ordered_paragraph = ordered.paragraphs().front().id();
    const auto first_image = NodeId::generate();
    const auto second_image = NodeId::generate();
    const auto third_image = NodeId::generate();
    const auto equation_id = NodeId::generate();
    CHECK(ordered.insertImage({ordered_paragraph, 1}, payload,
                              ImageFormat::png, {}, 1000, 2000,
                              first_image));
    CHECK(ordered.insertImage({ordered_paragraph, 1}, jpeg_payload,
                              ImageFormat::jpeg, {}, 2000, 3000,
                              second_image));
    CHECK(ordered.insertEquation({ordered_paragraph, 2}, "x", false,
                                 equation_id));
    CHECK(ordered.insertImage({ordered_paragraph, 2}, payload,
                              ImageFormat::png, {}, 3000, 4000,
                              third_image));
    CHECK(ordered.paragraphs().front().text() ==
          u"Q\ufffc\ufffc\ufffc\ufffc");
    CHECK(ordered.paragraphs().front().imageAt(1)->id == second_image);
    CHECK(ordered.paragraphs().front().imageAt(2)->id == third_image);
    CHECK(ordered.paragraphs().front().equationAt(3)->id == equation_id);
    CHECK(ordered.paragraphs().front().imageAt(4)->id == first_image);
    CHECK(ordered.deleteRange(
        {{ordered_paragraph, 2}, {ordered_paragraph, 3}}));
    CHECK(ordered.findImage(third_image) == nullptr);
    CHECK(ordered.paragraphs().front().equationAt(2)->id == equation_id);
    CHECK(ordered.paragraphs().front().imageAt(3)->id == first_image);
    CHECK(ordered.replaceRange(
        {{ordered_paragraph, 1}, {ordered_paragraph, 3}}, u"z"));
    CHECK(ordered.paragraphs().front().text() == u"Qz\ufffc");
    CHECK(ordered.findImage(second_image) == nullptr);
    CHECK(ordered.findEquation(equation_id) == nullptr);
    CHECK(ordered.paragraphs().front().imageAt(2)->id == first_image);

    auto split = documentWithText(u"ab");
    const auto split_paragraph = split.paragraphs().front().id();
    const auto split_image = NodeId::generate();
    CHECK(split.insertImage({split_paragraph, 1}, payload, ImageFormat::png,
                            {}, 1000, 1000, split_image));
    const auto right_paragraph = NodeId::generate();
    CHECK(split.splitParagraph({split_paragraph, 1}, right_paragraph));
    CHECK(split.paragraphs()[0].text() == u"a");
    CHECK(split.paragraphs()[0].images().empty());
    CHECK(split.paragraphs()[1].text() == u"\ufffcb");
    CHECK(split.paragraphs()[1].imageAt(0)->id == split_image);
    CHECK(split.mergeWithNext(split_paragraph));
    CHECK(split.paragraphs().front().imageAt(1)->id == split_image);
    const auto after_image_paragraph = NodeId::generate();
    CHECK(split.splitParagraph({split_paragraph, 2},
                               after_image_paragraph));
    CHECK(split.paragraphs()[0].imageAt(1)->id == split_image);
    CHECK(split.paragraphs()[1].images().empty());
    CHECK(split.mergeWithNext(split_paragraph));

    auto first = Paragraph::create(u"AB");
    auto middle = Paragraph::create(u"M");
    auto last = Paragraph::create(u"CD");
    CHECK(first && middle && last);
    auto multiline_result =
        Document::create({first.value(), middle.value(), last.value()});
    CHECK(multiline_result);
    auto multiline = multiline_result.value();
    const auto first_paragraph = multiline.paragraphs()[0].id();
    const auto middle_paragraph = multiline.paragraphs()[1].id();
    const auto last_paragraph = multiline.paragraphs()[2].id();
    const auto retained_first_image = NodeId::generate();
    const auto removed_middle_image = NodeId::generate();
    const auto retained_last_image = NodeId::generate();
    const auto removed_first_equation = NodeId::generate();
    const auto removed_last_equation = NodeId::generate();
    CHECK(multiline.insertImage({first_paragraph, 1}, payload,
                                ImageFormat::png, {}, 1000, 1000,
                                retained_first_image));
    CHECK(multiline.insertEquation({first_paragraph, 2}, "a", false,
                                   removed_first_equation));
    CHECK(multiline.insertImage({middle_paragraph, 0}, payload,
                                ImageFormat::png, {}, 1000, 1000,
                                removed_middle_image));
    CHECK(multiline.insertEquation({last_paragraph, 0}, "b", false,
                                   removed_last_equation));
    CHECK(multiline.insertImage({last_paragraph, 2}, jpeg_payload,
                                ImageFormat::jpeg, {}, 1000, 1000,
                                retained_last_image));
    CHECK(multiline.deleteRange(
        {{first_paragraph, 2}, {last_paragraph, 2}}));
    CHECK(multiline.paragraphs().size() == 1);
    CHECK(multiline.paragraphs().front().text() ==
          u"A\ufffc\ufffcD");
    CHECK(multiline.paragraphs().front().imageAt(1)->id ==
          retained_first_image);
    CHECK(multiline.paragraphs().front().imageAt(2)->id ==
          retained_last_image);
    CHECK(multiline.findImage(removed_middle_image) == nullptr);
    CHECK(multiline.findEquation(removed_first_equation) == nullptr);
    CHECK(multiline.findEquation(removed_last_equation) == nullptr);

    auto invariant = documentWithText(u"xy");
    const auto invariant_paragraph = invariant.paragraphs().front().id();
    const auto invariant_image = NodeId::generate();
    const auto invariant_equation = NodeId::generate();
    CHECK(invariant.insertImage({invariant_paragraph, 1}, payload,
                                ImageFormat::png, {}, 1000, 1000,
                                invariant_image));
    CHECK(invariant.insertEquation({invariant_paragraph, 2}, "x", false,
                                   invariant_equation));
    CHECK(Document::create({invariant.paragraphs().front()}));
    CHECK(!invariant.insertEquation({invariant_paragraph, 0}, "y", false,
                                    invariant_image));
    CHECK(!invariant.insertImage({invariant_paragraph, 0}, payload,
                                 ImageFormat::png, {}, 1000, 1000,
                                 invariant_equation));
    CHECK(!invariant.splitParagraph({invariant_paragraph, 0},
                                    invariant_image));
    auto colliding_table = Table::create(1, 1, false, invariant_image);
    CHECK(colliding_table);
    CHECK(!invariant.insertTable(std::nullopt, colliding_table.value()));

    auto missing_metadata = invariant.paragraphs().front();
    auto& missing_images = const_cast<std::vector<ImageAtom>&>(
        missing_metadata.images());
    missing_images.clear();
    CHECK(!Document::create({missing_metadata}));

    auto missing_placeholder = invariant.paragraphs().front();
    auto& changed_text = const_cast<std::u16string&>(
        missing_placeholder.text());
    changed_text[1] = u'z';
    CHECK(!Document::create({missing_placeholder}));

    auto duplicate_id = invariant.paragraphs().front();
    auto& duplicate_id_images = const_cast<std::vector<ImageAtom>&>(
        duplicate_id.images());
    duplicate_id_images.front().id = invariant_equation;
    CHECK(!Document::create({duplicate_id}));

    auto colliding_metadata = invariant.paragraphs().front();
    auto& colliding_images = const_cast<std::vector<ImageAtom>&>(
        colliding_metadata.images());
    colliding_images.front().utf16_offset = 2;
    CHECK(!Document::create({colliding_metadata}));

    auto duplicate_metadata = invariant.paragraphs().front();
    auto& duplicate_images = const_cast<std::vector<ImageAtom>&>(
        duplicate_metadata.images());
    auto duplicate_image = duplicate_images.front();
    duplicate_image.id = NodeId::generate();
    duplicate_images.push_back(std::move(duplicate_image));
    CHECK(!Document::create({duplicate_metadata}));

    auto unordered_atoms = documentWithText(u"x");
    const auto unordered_paragraph =
        unordered_atoms.paragraphs().front().id();
    CHECK(unordered_atoms.insertImage(
        {unordered_paragraph, 0}, payload, ImageFormat::png, {}, 1000, 1000,
        NodeId::generate()));
    CHECK(unordered_atoms.insertImage(
        {unordered_paragraph, 1}, jpeg_payload, ImageFormat::jpeg, {}, 1000,
        1000, NodeId::generate()));
    auto unordered_images = unordered_atoms.paragraphs().front();
    auto& image_metadata = const_cast<std::vector<ImageAtom>&>(
        unordered_images.images());
    std::swap(image_metadata[0], image_metadata[1]);
    CHECK(!Document::create({unordered_images}));

    auto out_of_bounds_image = unordered_atoms.paragraphs().front();
    auto& out_of_bounds_metadata = const_cast<std::vector<ImageAtom>&>(
        out_of_bounds_image.images());
    out_of_bounds_metadata.front().utf16_offset =
        out_of_bounds_image.text().size();
    CHECK(!Document::create({out_of_bounds_image}));

    auto unordered_equations = documentWithText(u"x");
    const auto equation_paragraph =
        unordered_equations.paragraphs().front().id();
    CHECK(unordered_equations.insertEquation(
        {equation_paragraph, 0}, "a", false, NodeId::generate()));
    CHECK(unordered_equations.insertEquation(
        {equation_paragraph, 1}, "b", false, NodeId::generate()));
    auto unordered_equation_paragraph =
        unordered_equations.paragraphs().front();
    auto& equation_metadata = const_cast<std::vector<EquationAtom>&>(
        unordered_equation_paragraph.equations());
    std::swap(equation_metadata[0], equation_metadata[1]);
    CHECK(!Document::create({unordered_equation_paragraph}));
}

void testImageResourceLimitsAndSessionHistory() {
    const EncodedImagePayload maximum_payload = maximumSizedJpegPayload();
    auto byte_limited = documentWithText({});
    const auto byte_limited_paragraph =
        byte_limited.paragraphs().front().id();
    CHECK(byte_limited.insertImage(
        {byte_limited_paragraph, 0}, maximum_payload, ImageFormat::jpeg, {},
        1000, 1000, NodeId::generate()));
    CHECK(byte_limited.insertImage(
        {byte_limited_paragraph, 1}, maximum_payload, ImageFormat::jpeg, {},
        1000, 1000, NodeId::generate()));
    const auto byte_limited_before = byte_limited;
    CHECK(!byte_limited.insertImage(
        {byte_limited_paragraph, 2},
        EncodedImagePayload(tinyPngBytes()),
        ImageFormat::png, {}, 1000, 1000, NodeId::generate()));
    CHECK(byte_limited == byte_limited_before);
    CHECK(Document::create({byte_limited.paragraphs().front()}));
    auto over_byte_limit = byte_limited.paragraphs().front();
    auto& over_byte_limit_images = const_cast<std::vector<ImageAtom>&>(
        over_byte_limit.images());
    auto extra_large_image = over_byte_limit_images.front();
    extra_large_image.id = NodeId::generate();
    extra_large_image.utf16_offset = 2;
    over_byte_limit_images.push_back(std::move(extra_large_image));
    auto& over_byte_limit_text = const_cast<std::u16string&>(
        over_byte_limit.text());
    over_byte_limit_text.push_back(kInlineObjectReplacementCharacter);
    CHECK(!Document::create({over_byte_limit}));

    const EncodedImagePayload tiny_payload(tinyPngBytes());
    auto count_limited = documentWithText({});
    const auto count_limited_paragraph =
        count_limited.paragraphs().front().id();
    for (std::size_t index = 0; index < kMaximumInlineImagesPerDocument;
         ++index) {
        CHECK(count_limited.insertImage(
            {count_limited_paragraph, index}, tiny_payload, ImageFormat::png,
            {}, 1000, 1000, NodeId::generate()));
    }
    CHECK(count_limited.paragraphs().front().images().size() ==
          kMaximumInlineImagesPerDocument);
    CHECK(!count_limited.insertImage(
        {count_limited_paragraph, kMaximumInlineImagesPerDocument},
        tiny_payload, ImageFormat::png, {}, 1000, 1000,
        NodeId::generate()));
    auto malformed_at_count_limit = tinyPngBytes();
    malformed_at_count_limit.pop_back();
    const auto cheap_count_rejection = count_limited.insertImage(
        {count_limited_paragraph, kMaximumInlineImagesPerDocument},
        EncodedImagePayload(std::move(malformed_at_count_limit)),
        ImageFormat::png, {}, 1000, 1000, NodeId::generate());
    CHECK(!cheap_count_rejection);
    CHECK(cheap_count_rejection.error().message.find("count limit") !=
          std::string::npos);
    auto over_count_limit = count_limited.paragraphs().front();
    auto& over_count_limit_images = const_cast<std::vector<ImageAtom>&>(
        over_count_limit.images());
    auto extra_counted_image = over_count_limit_images.front();
    extra_counted_image.id = NodeId::generate();
    extra_counted_image.utf16_offset = kMaximumInlineImagesPerDocument;
    over_count_limit_images.push_back(std::move(extra_counted_image));
    auto& over_count_limit_text = const_cast<std::u16string&>(
        over_count_limit.text());
    over_count_limit_text.push_back(kInlineObjectReplacementCharacter);
    CHECK(!Document::create({over_count_limit}));

    DocumentSession session(documentWithText(u"x"));
    auto snapshot = session.snapshot();
    const auto paragraph_id = snapshot.document.paragraphs().front().id();
    const auto image_id = NodeId::generate();
    const EncodedImagePayload payload(tinyPngBytes());
    const auto* payload_address = payload.bytes().data();
    const std::vector<Operation> insertion{
        InsertImage{{paragraph_id, 1}, payload, ImageFormat::png,
                    "Chart", 914400, 457200, image_id, std::nullopt},
    };
    CHECK(session.applyBatch(snapshot.revision, insertion));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().text() == u"x\ufffc");
    CHECK(snapshot.document.findImage(image_id)->encoded_payload.bytes().data() ==
          payload_address);

    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id) == nullptr);
    CHECK(snapshot.document.paragraphs().front().text() == u"x");
    CHECK(session.redo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id)->encoded_payload.bytes().data() ==
          payload_address);

    const std::vector<Operation> resize{
        ResizeImage{image_id, 1828800, 914400},
    };
    CHECK(session.applyBatch(snapshot.revision, resize));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id)->width_emu == 1828800);
    CHECK(snapshot.document.findImage(image_id)->height_emu == 914400);
    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id)->width_emu == 914400);
    CHECK(session.redo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id)->width_emu == 1828800);

    const std::vector<Operation> deletion{
        DeleteRange{{{paragraph_id, 1}, {paragraph_id, 2}}},
    };
    CHECK(session.applyBatch(snapshot.revision, deletion));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id) == nullptr);
    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id) != nullptr);
    CHECK(session.redo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id) == nullptr);
    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id) != nullptr);

    auto accepted_preview = session.createPreview(snapshot.revision);
    CHECK(accepted_preview);
    const std::vector<Operation> preview_resize{
        ResizeImage{image_id, 2743200, 1371600},
    };
    CHECK(session.applyPreviewBatch(accepted_preview.value().id, Revision{},
                                    preview_resize));
    CHECK(session.snapshot().document.findImage(image_id)->width_emu ==
          1828800);
    auto preview_snapshot =
        session.previewSnapshot(accepted_preview.value().id);
    CHECK(preview_snapshot);
    CHECK(preview_snapshot.value().document.findImage(image_id)->width_emu ==
          2743200);
    CHECK(session.acceptPreview(accepted_preview.value().id,
                                snapshot.revision, Revision{1}));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id)->width_emu == 2743200);
    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findImage(image_id)->width_emu == 1828800);

    auto stale_preview = session.createPreview(snapshot.revision);
    CHECK(stale_preview);
    CHECK(session.applyPreviewBatch(stale_preview.value().id, Revision{},
                                    preview_resize));
    CHECK(session.applyBatch(
        snapshot.revision,
        std::vector<Operation>{ResizeImage{image_id, 3657600, 1828800}}));
    snapshot = session.snapshot();
    const auto stale_accept = session.acceptPreview(
        stale_preview.value().id, snapshot.revision, Revision{1});
    CHECK(!stale_accept);
    CHECK(stale_accept.error().code == ErrorCode::preview_conflict);
    CHECK(snapshot.document.findImage(image_id)->width_emu == 3657600);
    CHECK(session.discardPreview(stale_preview.value().id));

    DocumentSession atomic(documentWithText(u"base"));
    const auto before = atomic.snapshot();
    const auto atomic_paragraph = before.document.paragraphs().front().id();
    const auto atomic_image = NodeId::generate();
    const std::vector<Operation> invalid_batch{
        InsertImage{{atomic_paragraph, 4}, tiny_payload, ImageFormat::png,
                    {}, 1000, 1000, atomic_image, std::nullopt},
        ResizeImage{atomic_image, 0, 1000},
    };
    CHECK(!atomic.applyBatch(before.revision, invalid_batch));
    CHECK(atomic.snapshot().revision == before.revision);
    CHECK(atomic.snapshot().document == before.document);
}

void testBoundedSessionHistoryAndPreviews() {
    const DocumentSessionLimits defaults;
    CHECK(defaults.maximum_history_entries == 256);
    CHECK(defaults.maximum_preview_branches == 4);
    CHECK(defaults.maximum_retained_history_image_bytes ==
          256U * 1024U * 1024U);

    DocumentSessionLimits entry_limits;
    entry_limits.maximum_history_entries = 3;
    entry_limits.maximum_preview_branches = 2;
    DocumentSession entry_limited(documentWithText({}), entry_limits);
    auto snapshot = entry_limited.snapshot();
    const auto paragraph_id = snapshot.document.paragraphs().front().id();
    for (std::size_t index = 0; index < 5; ++index) {
        const std::vector<Operation> edit{
            InsertText{{paragraph_id, index}, u"x", std::nullopt},
        };
        CHECK(entry_limited.applyBatch(snapshot.revision, edit));
        snapshot = entry_limited.snapshot();
    }
    for (std::size_t index = 0; index < 3; ++index) {
        CHECK(entry_limited.undo(snapshot.revision));
        snapshot = entry_limited.snapshot();
    }
    CHECK(snapshot.document.paragraphs().front().text() == u"xx");
    const auto evicted_undo = entry_limited.undo(snapshot.revision);
    CHECK(!evicted_undo);
    CHECK(evicted_undo.error().code == ErrorCode::history_empty);

    DocumentSession preview_limited(documentWithText({}), entry_limits);
    const auto preview_base = preview_limited.snapshot();
    const auto first_preview =
        preview_limited.createPreview(preview_base.revision);
    const auto second_preview =
        preview_limited.createPreview(preview_base.revision);
    CHECK(first_preview);
    CHECK(second_preview);
    const auto excess_preview =
        preview_limited.createPreview(preview_base.revision);
    CHECK(!excess_preview);
    CHECK(excess_preview.error().code == ErrorCode::invalid_operation);
    CHECK(preview_limited.discardPreview(first_preview.value().id));
    CHECK(preview_limited.discardPreview(second_preview.value().id));

    DocumentSessionLimits preview_history_limits;
    preview_history_limits.maximum_history_entries = 2;
    preview_history_limits.maximum_preview_branches = 1;
    DocumentSession preview_history(documentWithText({}),
                                    preview_history_limits);
    const auto preview_history_base = preview_history.snapshot();
    const auto bounded_preview =
        preview_history.createPreview(preview_history_base.revision);
    CHECK(bounded_preview);
    auto preview_revision = bounded_preview.value().revision;
    const auto preview_paragraph =
        preview_history_base.document.paragraphs().front().id();
    for (std::size_t index = 0; index < 4; ++index) {
        const std::vector<Operation> edit{
            InsertText{{preview_paragraph, index}, u"p", std::nullopt},
        };
        const auto applied = preview_history.applyPreviewBatch(
            bounded_preview.value().id, preview_revision, edit);
        CHECK(applied);
        preview_revision = applied.value().revision;
    }
    for (std::size_t index = 0; index < 2; ++index) {
        const auto undone = preview_history.undoPreview(
            bounded_preview.value().id, preview_revision);
        CHECK(undone);
        preview_revision = undone.value().revision;
    }
    const auto evicted_preview_undo = preview_history.undoPreview(
        bounded_preview.value().id, preview_revision);
    CHECK(!evicted_preview_undo);
    CHECK(evicted_preview_undo.error().code == ErrorCode::history_empty);

    const auto image_bytes = tinyPngBytes().size();
    DocumentSessionLimits shared_media_limits;
    shared_media_limits.maximum_history_entries = 16;
    shared_media_limits.maximum_preview_branches = 1;
    shared_media_limits.maximum_retained_history_image_bytes = image_bytes;
    DocumentSession shared_media(documentWithText({}), shared_media_limits);
    snapshot = shared_media.snapshot();
    const auto shared_paragraph = snapshot.document.paragraphs().front().id();
    const auto shared_image_id = NodeId::generate();
    const EncodedImagePayload shared_payload(tinyPngBytes());
    const auto apply_shared = [&](Operation operation) {
        const std::vector<Operation> operations{std::move(operation)};
        const auto applied = shared_media.applyBatch(snapshot.revision,
                                                     operations);
        CHECK(applied);
        snapshot = shared_media.snapshot();
    };
    apply_shared(InsertImage{{shared_paragraph, 0}, shared_payload,
                             ImageFormat::png, {}, 1000, 1000,
                             shared_image_id, std::nullopt});
    apply_shared(ResizeImage{shared_image_id, 2000, 2000});
    apply_shared(ResizeImage{shared_image_id, 3000, 3000});
    apply_shared(ResizeImage{shared_image_id, 4000, 4000});
    apply_shared(DeleteRange{{{shared_paragraph, 0},
                              {shared_paragraph, 1}}});
    for (std::size_t index = 0; index < 5; ++index) {
        CHECK(shared_media.undo(snapshot.revision));
        snapshot = shared_media.snapshot();
    }
    CHECK(snapshot.document.paragraphs().front().text().empty());
    CHECK(!shared_media.undo(snapshot.revision));

    DocumentSession distinct_media(documentWithText({}),
                                   shared_media_limits);
    snapshot = distinct_media.snapshot();
    const auto distinct_paragraph =
        snapshot.document.paragraphs().front().id();
    const auto insert_and_delete_distinct = [&]() {
        const auto image_id = NodeId::generate();
        const std::vector<Operation> insertion{
            InsertImage{{distinct_paragraph, 0},
                        EncodedImagePayload(tinyPngBytes()),
                        ImageFormat::png, {}, 1000, 1000, image_id,
                        std::nullopt},
        };
        CHECK(distinct_media.applyBatch(snapshot.revision, insertion));
        snapshot = distinct_media.snapshot();
        const std::vector<Operation> deletion{
            DeleteRange{{{distinct_paragraph, 0}, {distinct_paragraph, 1}}},
        };
        CHECK(distinct_media.applyBatch(snapshot.revision, deletion));
        snapshot = distinct_media.snapshot();
    };
    insert_and_delete_distinct();
    insert_and_delete_distinct();
    CHECK(distinct_media.undo(snapshot.revision));
    snapshot = distinct_media.snapshot();
    CHECK(distinct_media.undo(snapshot.revision));
    snapshot = distinct_media.snapshot();
    CHECK(snapshot.document.paragraphs().front().text().empty());
    const auto evicted_media_undo = distinct_media.undo(snapshot.revision);
    CHECK(!evicted_media_undo);
    CHECK(evicted_media_undo.error().code == ErrorCode::history_empty);

    DocumentSession preview_retention(documentWithText({}),
                                      shared_media_limits);
    snapshot = preview_retention.snapshot();
    const auto retention_paragraph =
        snapshot.document.paragraphs().front().id();
    const EncodedImagePayload retained_by_preview(tinyPngBytes());
    const EncodedImagePayload retained_by_live_history(tinyPngBytes());
    const auto preview_image_id = NodeId::generate();
    const auto live_image_id = NodeId::generate();
    const auto apply_retention_live = [&](Operation operation) {
        const std::vector<Operation> operations{std::move(operation)};
        const auto applied = preview_retention.applyBatch(snapshot.revision,
                                                          operations);
        CHECK(applied);
        snapshot = preview_retention.snapshot();
    };
    apply_retention_live(InsertImage{
        {retention_paragraph, 0}, retained_by_preview, ImageFormat::png, {},
        1000, 1000, preview_image_id, std::nullopt});
    apply_retention_live(DeleteRange{{{retention_paragraph, 0},
                                      {retention_paragraph, 1}}});
    const auto retaining_preview =
        preview_retention.createPreview(snapshot.revision);
    CHECK(retaining_preview);
    const std::vector<Operation> preview_insertion{
        InsertImage{{retention_paragraph, 0}, retained_by_preview,
                    ImageFormat::png, {}, 1000, 1000, preview_image_id,
                    std::nullopt},
    };
    CHECK(preview_retention.applyPreviewBatch(
        retaining_preview.value().id, retaining_preview.value().revision,
        preview_insertion));
    apply_retention_live(InsertImage{
        {retention_paragraph, 0}, retained_by_live_history,
        ImageFormat::png, {}, 1000, 1000, live_image_id, std::nullopt});
    apply_retention_live(DeleteRange{{{retention_paragraph, 0},
                                      {retention_paragraph, 1}}});
    CHECK(preview_retention.discardPreview(
        retaining_preview.value().id));
    CHECK(preview_retention.undo(snapshot.revision));
    snapshot = preview_retention.snapshot();
    CHECK(preview_retention.undo(snapshot.revision));
    snapshot = preview_retention.snapshot();
    const auto pruned_after_discard =
        preview_retention.undo(snapshot.revision);
    CHECK(!pruned_after_discard);
    CHECK(pruned_after_discard.error().code == ErrorCode::history_empty);
}

void testSemanticEquationAtomEditing() {
    CHECK(!Paragraph::create(u"orphan\ufffcplaceholder"));

    auto document = documentWithText(u"ab");
    const auto paragraph_id = document.paragraphs().front().id();
    const auto original = document;
    CHECK(!document.insertText({paragraph_id, 1}, u"\ufffc"));
    CHECK(document == original);
    CHECK(!document.replaceRange(
        {{paragraph_id, 0}, {paragraph_id, 1}}, u"\ufffc"));
    CHECK(document == original);

    const auto equation_id = NodeId::generate();
    CHECK(!document.insertEquation({paragraph_id, 1}, "", false,
                                   equation_id));
    const std::string invalid_utf8{"\xc0\xaf", 2};
    CHECK(!document.insertEquation({paragraph_id, 1}, invalid_utf8, false,
                                   equation_id));
    const std::string oversized_latex(64U * 1024U + 1U, 'x');
    CHECK(!document.insertEquation({paragraph_id, 1}, oversized_latex, false,
                                   equation_id));
    CHECK(!document.insertEquation({paragraph_id, 1}, "x", false, NodeId{}));
    CHECK(!document.insertEquation({paragraph_id, 1}, "x", false,
                                   paragraph_id));
    CHECK(document == original);

    CharacterFormat equation_format;
    equation_format.italic = true;
    CHECK(document.insertEquation({paragraph_id, 1}, "\\frac{a}{b}", false,
                                  equation_id, equation_format));
    CHECK(document.paragraphs().front().text() == u"a\ufffcb");
    CHECK(document.paragraphs().front().equations().size() == 1);
    const auto* equation = document.paragraphs().front().equationAt(1);
    CHECK(equation != nullptr);
    if (equation) {
        CHECK(equation->id == equation_id);
        CHECK(equation->utf16_offset == 1);
        CHECK(equation->canonical_latex == "\\frac{a}{b}");
        CHECK(!equation->display);
    }
    CHECK(document.paragraphs().front().characterFormatAt(2).italic == true);
    CHECK(document.paragraphs().front().equationAt(0) == nullptr);
    CHECK(document.findEquation(equation_id) != nullptr);
    CHECK(document.findEquation(equation_id)->canonical_latex == "\\frac{a}{b}");

    const auto after_equation = document;
    CHECK(!document.insertEquation({paragraph_id, 0}, "y", true,
                                   equation_id));
    CHECK(document == after_equation);

    CHECK(document.insertText({paragraph_id, 0}, u"X"));
    CHECK(document.paragraphs().front().text() == u"Xa\ufffcb");
    CHECK(document.paragraphs().front().equationAt(2) != nullptr);
    CHECK(document.insertText({paragraph_id, 2}, u"Y"));
    CHECK(document.paragraphs().front().text() == u"XaY\ufffcb");
    CHECK(document.paragraphs().front().equationAt(3) != nullptr);
    CHECK(document.insertText({paragraph_id, 4}, u"Z"));
    CHECK(document.paragraphs().front().text() == u"XaY\ufffcZb");
    CHECK(document.paragraphs().front().equationAt(3) != nullptr);

    CHECK(document.deleteRange({{paragraph_id, 0}, {paragraph_id, 2}}));
    CHECK(document.paragraphs().front().text() == u"Y\ufffcZb");
    CHECK(document.paragraphs().front().equationAt(1) != nullptr);
    CHECK(document.deleteRange({{paragraph_id, 1}, {paragraph_id, 2}}));
    CHECK(document.paragraphs().front().text() == u"YZb");
    CHECK(document.paragraphs().front().equations().empty());

    auto inherited = documentWithText(u"ab");
    const auto inherited_paragraph = inherited.paragraphs().front().id();
    CharacterFormatDelta bold;
    bold.bold = PropertyDelta<bool>::set(true);
    CHECK(inherited.applyCharacterFormat(
        {{inherited_paragraph, 0}, {inherited_paragraph, 1}}, bold));
    const auto inherited_equation_id = NodeId::generate();
    CHECK(inherited.insertEquation({inherited_paragraph, 1}, "x^{2}", true,
                                   inherited_equation_id));
    CHECK(inherited.paragraphs().front().characterFormatAt(2).bold == true);
    CHECK(inherited.paragraphs().front().equationAt(1)->display);

    const auto right_paragraph_id = NodeId::generate();
    CHECK(inherited.splitParagraph({inherited_paragraph, 1}, right_paragraph_id));
    CHECK(inherited.paragraphs().size() == 2);
    CHECK(inherited.paragraphs()[0].text() == u"a");
    CHECK(inherited.paragraphs()[0].equations().empty());
    CHECK(inherited.paragraphs()[1].text() == u"\ufffcb");
    CHECK(inherited.paragraphs()[1].equationAt(0) != nullptr);
    CHECK(inherited.paragraphs()[1].equationAt(0)->id == inherited_equation_id);
    CHECK(inherited.mergeWithNext(inherited_paragraph));
    CHECK(inherited.paragraphs().size() == 1);
    CHECK(inherited.paragraphs().front().text() == u"a\ufffcb");
    CHECK(inherited.paragraphs().front().equationAt(1) != nullptr);
    CHECK(inherited.paragraphs().front().equationAt(1)->id ==
          inherited_equation_id);

    const auto after_equation_split_id = NodeId::generate();
    CHECK(inherited.splitParagraph({inherited_paragraph, 2},
                                   after_equation_split_id));
    CHECK(inherited.paragraphs()[0].text() == u"a\ufffc");
    CHECK(inherited.paragraphs()[0].equationAt(1) != nullptr);
    CHECK(inherited.paragraphs()[1].text() == u"b");
    CHECK(inherited.paragraphs()[1].equations().empty());
    CHECK(inherited.mergeWithNext(inherited_paragraph));
    CHECK(inherited.paragraphs().front().equationAt(1) != nullptr);

    // All stable node IDs share one namespace, including equations.
    CHECK(!inherited.splitParagraph({inherited_paragraph, 0},
                                    inherited_equation_id));
    auto colliding_table = Table::create(1, 1, false, inherited_equation_id);
    CHECK(colliding_table);
    CHECK(!inherited.insertTable(std::nullopt, colliding_table.value()));

    auto table = Table::create(1, 1, false);
    CHECK(table);
    CHECK(inherited.insertTable(std::nullopt, table.value()));
    CHECK(!inherited.setTableCellText(table.value().id(), 0, 0, u"\ufffc"));
    CHECK(inherited.findTable(table.value().id())->cell(0, 0)->text.empty());

    // A valid atom-bearing paragraph remains valid when copied into a fresh
    // document, which also exercises Document::create's atom invariants.
    auto copied = Document::create({inherited.paragraphs().front()});
    CHECK(copied);
    CHECK(copied.value().paragraphs().front().equationAt(1) != nullptr);
}

void testEquationStructureAndSessionHistory() {
    auto first = Paragraph::create(u"AB");
    auto middle = Paragraph::create(u"M");
    auto last = Paragraph::create(u"CD");
    CHECK(first && middle && last);
    auto created = Document::create({first.value(), middle.value(), last.value()});
    CHECK(created);
    auto document = created.value();
    const auto first_id = document.paragraphs()[0].id();
    const auto middle_id = document.paragraphs()[1].id();
    const auto last_id = document.paragraphs()[2].id();
    const auto first_equation = NodeId::generate();
    const auto removed_equation = NodeId::generate();
    const auto last_equation = NodeId::generate();
    CHECK(document.insertEquation({first_id, 1}, "a", false, first_equation));
    CHECK(document.insertEquation({middle_id, 0}, "b", false,
                                  removed_equation));
    CHECK(document.insertEquation({last_id, 1}, "c", true, last_equation));

    CHECK(document.deleteRange({{first_id, 2}, {last_id, 1}}));
    CHECK(document.paragraphs().size() == 1);
    CHECK(document.paragraphs().front().text() == u"A\ufffc\ufffcD");
    CHECK(document.paragraphs().front().equations().size() == 2);
    CHECK(document.paragraphs().front().equationAt(1)->id == first_equation);
    CHECK(document.paragraphs().front().equationAt(2)->id == last_equation);
    CHECK(document.paragraphs().front().equationAt(2)->display);
    CHECK(document.insertEquation({first_id, 4}, "d", false,
                                  removed_equation));

    DocumentSession session(documentWithText(u"x"));
    auto snapshot = session.snapshot();
    const auto paragraph_id = snapshot.document.paragraphs().front().id();
    const auto equation_id = NodeId::generate();
    CharacterFormat format;
    format.foreground_argb = 0xff336699U;
    const std::vector<Operation> insertion{
        InsertEquation{{paragraph_id, 1}, "\\sqrt{x}", true,
                       equation_id, format},
    };
    CHECK(session.applyBatch(snapshot.revision, insertion));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().text() == u"x\ufffc");
    CHECK(snapshot.document.paragraphs().front().equationAt(1)->id ==
          equation_id);
    CHECK(snapshot.document.paragraphs().front().characterFormatAt(2)
              .foreground_argb == 0xff336699U);

    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().text() == u"x");
    CHECK(snapshot.document.paragraphs().front().equations().empty());
    CHECK(session.redo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().equationAt(1)->id ==
          equation_id);

    auto preview = session.createPreview(snapshot.revision);
    CHECK(preview);
    const auto preview_equation_id = NodeId::generate();
    const std::vector<Operation> preview_operations{
        InsertEquation{{paragraph_id, 0}, "\\sum_{i=1}^{n}", false,
                       preview_equation_id, std::nullopt},
    };
    CHECK(session.applyPreviewBatch(preview.value().id, Revision{},
                                    preview_operations));
    CHECK(session.snapshot().document.paragraphs().front().equationAt(0) ==
          nullptr);
    const auto preview_snapshot = session.previewSnapshot(preview.value().id);
    CHECK(preview_snapshot);
    CHECK(preview_snapshot.value().document.paragraphs().front()
              .equationAt(0)->id == preview_equation_id);
    CHECK(session.acceptPreview(preview.value().id, snapshot.revision,
                                Revision{1}));
    snapshot = session.snapshot();
    CHECK(snapshot.document.paragraphs().front().equations().size() == 2);
    CHECK(session.undo(snapshot.revision));
    CHECK(session.snapshot().document.paragraphs().front().equations().size() == 1);

    // A bad later operation rejects the complete candidate, including a
    // previously inserted equation in the same batch.
    DocumentSession atomic(documentWithText(u"base"));
    const auto before = atomic.snapshot();
    const auto atomic_paragraph = before.document.paragraphs().front().id();
    const auto duplicate = NodeId::generate();
    const std::vector<Operation> invalid_batch{
        InsertEquation{{atomic_paragraph, 4}, "x", false, duplicate,
                       std::nullopt},
        InsertEquation{{atomic_paragraph, 0}, "y", false, duplicate,
                       std::nullopt},
    };
    CHECK(!atomic.applyBatch(before.revision, invalid_batch));
    CHECK(atomic.snapshot().revision == before.revision);
    CHECK(atomic.snapshot().document == before.document);
}

void testSemanticTableOperationsAndHistory() {
    CHECK(!Table::create(0, 2, false));
    CHECK(!Table::create(Table::maximum_rows + 1, 1, false));

    auto first = Paragraph::create(u"Before");
    auto second = Paragraph::create(u"After");
    CHECK(first && second);
    auto document = Document::create({first.value(), second.value()});
    CHECK(document);
    DocumentSession session(document.value());

    auto table_result = Table::create(2, 3, true);
    CHECK(table_result);
    const auto table = table_result.value();
    const auto table_id = table.id();
    CHECK(table.rowCount() == 2);
    CHECK(table.columnCount() == 3);
    CHECK(table.hasHeaderRow());
    CHECK(table.cells().size() == 6);
    CHECK(table.cell(1, 2) != nullptr);
    CHECK(table.cell(2, 0) == nullptr);
    for (std::size_t left = 0; left < table.cells().size(); ++left) {
        CHECK(table.cells()[left].id.isValid());
        CHECK(table.cells()[left].id != table_id);
        for (std::size_t right = left + 1; right < table.cells().size(); ++right) {
            CHECK(table.cells()[left].id != table.cells()[right].id);
        }
    }

    auto snapshot = session.snapshot();
    const auto first_id = snapshot.document.paragraphs()[0].id();
    const auto second_id = snapshot.document.paragraphs()[1].id();
    std::vector<Operation> insert_and_edit{
        InsertTable{second_id, table},
        SetTableCellText{table_id, 0, 0, u"Heading"},
        SetTableCellText{table_id, 1, 2, u"Value"},
    };
    auto result = session.applyBatch(snapshot.revision, insert_and_edit);
    CHECK(result);
    snapshot = session.snapshot();
    CHECK(snapshot.document.tables().size() == 1);
    CHECK(snapshot.document.bodyBlocks().size() == 3);
    CHECK((snapshot.document.bodyBlocks()[0] ==
           BodyBlockRef{BodyBlockKind::paragraph, first_id}));
    CHECK((snapshot.document.bodyBlocks()[1] ==
           BodyBlockRef{BodyBlockKind::table, table_id}));
    CHECK((snapshot.document.bodyBlocks()[2] ==
           BodyBlockRef{BodyBlockKind::paragraph, second_id}));
    CHECK(snapshot.document.findTable(table_id)->cell(0, 0)->text == u"Heading");
    CHECK(snapshot.document.findTable(table_id)->cell(1, 2)->text == u"Value");

    std::vector<NodeId> appended_ids{
        NodeId::generate(), NodeId::generate(), NodeId::generate()};
    const std::vector<Operation> append_row{
        AppendTableRow{table_id, appended_ids},
    };
    result = session.applyBatch(snapshot.revision, append_row);
    CHECK(result);
    snapshot = session.snapshot();
    CHECK(snapshot.document.findTable(table_id)->rowCount() == 3);
    CHECK(snapshot.document.findTable(table_id)->cell(2, 0)->id ==
          appended_ids[0]);
    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findTable(table_id)->rowCount() == 2);
    CHECK(session.redo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findTable(table_id)->rowCount() == 3);

    const auto before_bad_row = snapshot;
    const std::vector<Operation> duplicate_cell_row{
        AppendTableRow{table_id, {appended_ids[0], NodeId::generate(),
                                  NodeId::generate()}},
    };
    CHECK(!session.applyBatch(snapshot.revision, duplicate_cell_row));
    CHECK(session.snapshot().revision == before_bad_row.revision);
    CHECK(session.snapshot().document == before_bad_row.document);

    // Text operations must not silently consume a table sitting between the
    // paragraphs at the two range endpoints.
    const std::vector<Operation> cross_table_delete{
        DeleteRange{{{first_id, 1}, {second_id, 1}}},
    };
    const auto rejected_delete = session.applyBatch(
        snapshot.revision, cross_table_delete);
    CHECK(!rejected_delete);
    CHECK(session.snapshot().revision == snapshot.revision);

    const std::vector<Operation> move{
        MoveTable{table_id, first_id},
    };
    result = session.applyBatch(snapshot.revision, move);
    CHECK(result);
    snapshot = session.snapshot();
    CHECK((snapshot.document.bodyBlocks().front() ==
           BodyBlockRef{BodyBlockKind::table, table_id}));

    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK((snapshot.document.bodyBlocks()[1] ==
           BodyBlockRef{BodyBlockKind::table, table_id}));
    CHECK(session.redo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK((snapshot.document.bodyBlocks().front() ==
           BodyBlockRef{BodyBlockKind::table, table_id}));

    const std::vector<Operation> erase{DeleteTable{table_id}};
    CHECK(session.applyBatch(snapshot.revision, erase));
    snapshot = session.snapshot();
    CHECK(snapshot.document.tables().empty());
    CHECK(snapshot.document.bodyBlocks().size() == 2);
    CHECK(session.undo(snapshot.revision));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findTable(table_id) != nullptr);
    CHECK(snapshot.document.findTable(table_id)->cell(1, 2)->text == u"Value");

    // The entire candidate is rejected when a later table operation fails.
    auto other_table = Table::create(1, 1, false);
    CHECK(other_table);
    const std::vector<Operation> invalid_atomic{
        InsertTable{std::nullopt, other_table.value()},
        SetTableCellText{other_table.value().id(), 9, 0, u"Out of range"},
    };
    const auto before_invalid = session.snapshot();
    const auto invalid_result = session.applyBatch(
        before_invalid.revision, invalid_atomic);
    CHECK(!invalid_result);
    CHECK(session.snapshot().revision == before_invalid.revision);
    CHECK(session.snapshot().document == before_invalid.document);

    auto preview = session.createPreview(before_invalid.revision);
    CHECK(preview);
    const std::vector<Operation> preview_edit{
        SetTableCellText{table_id, 0, 1, u"Preview"},
    };
    CHECK(session.applyPreviewBatch(preview.value().id, Revision{}, preview_edit));
    CHECK(session.snapshot().document.findTable(table_id)->cell(0, 1)->text.empty());
    CHECK(session.previewSnapshot(preview.value().id)
              .value().document.findTable(table_id)->cell(0, 1)->text == u"Preview");

    // A caller can split at the caret and insert before the generated right
    // paragraph in the same revision-checked transaction.
    DocumentSession at_caret(documentWithText(u"leftright"));
    const auto caret_base = at_caret.snapshot();
    const auto left_id = caret_base.document.paragraphs().front().id();
    const auto right_id = NodeId::generate();
    auto caret_table = Table::create(1, 2, false);
    CHECK(caret_table);
    const auto caret_table_id = caret_table.value().id();
    const std::vector<Operation> insert_at_caret{
        SplitParagraph{{left_id, 4}, right_id},
        InsertTable{right_id, caret_table.value()},
    };
    CHECK(at_caret.applyBatch(caret_base.revision, insert_at_caret));
    const auto caret_result = at_caret.snapshot();
    CHECK(caret_result.document.paragraphs()[0].text() == u"left");
    CHECK(caret_result.document.paragraphs()[1].text() == u"right");
    CHECK(caret_result.document.bodyBlocks().size() == 3);
    CHECK(caret_result.document.bodyBlocks()[0].id == left_id);
    CHECK(caret_result.document.bodyBlocks()[1].id == caret_table_id);
    CHECK(caret_result.document.bodyBlocks()[2].id == right_id);
    CHECK(at_caret.undo(caret_result.revision));
    CHECK(at_caret.snapshot().document.paragraphs().size() == 1);
    CHECK(at_caret.snapshot().document.paragraphs().front().text() == u"leftright");
    CHECK(at_caret.snapshot().document.tables().empty());
}

void testTableFormattingStructureAndStyles() {
    DocumentSession session(documentWithText(u"body"));
    const auto apply = [&session](
                           Revision revision,
                           std::initializer_list<Operation> operations) {
        return session.applyBatch(
            revision,
            std::span<const Operation>(operations.begin(), operations.size()));
    };
    auto table = Table::create(2, 2, true);
    CHECK(table);
    const auto table_id = table.value().id();
    auto snapshot = session.snapshot();
    CHECK(apply(snapshot.revision, {
        InsertTable{std::nullopt, table.value()},
        SetTableCellText{table_id, 0, 0, u"abcd"},
        SetTableCellText{table_id, 0, 1, u"right"},
        SetTableCellText{table_id, 1, 0, u"lower"},
    }));

    CharacterFormatDelta bold;
    bold.bold = PropertyDelta<bool>::set(true);
    CharacterFormat inserted;
    inserted.foreground_argb = 0xff008000U;
    ParagraphFormatDelta centered;
    centered.alignment =
        PropertyDelta<ParagraphAlignment>::set(ParagraphAlignment::center);
    snapshot = session.snapshot();
    CHECK(apply(snapshot.revision, {
        SetTableCellCharacterFormat{table_id, 0, 0, 1, 3, bold},
        SetTableCellParagraphFormat{table_id, 0, 0, centered},
        SetTableCellText{table_id, 0, 0, u"abXcd", inserted},
        SetTableStyle{table_id, TableStyle::banded_blue},
    }));
    snapshot = session.snapshot();
    const auto* formatted = snapshot.document.findTable(table_id);
    CHECK(formatted != nullptr);
    CHECK(formatted->cell(0, 0)->characterFormatAt(3).foreground_argb ==
          inserted.foreground_argb);
    CHECK(formatted->cell(0, 0)->characterFormatAt(2).bold.value_or(false));
    CHECK(formatted->cell(0, 0)->characterFormatAt(4).bold.value_or(false));
    CHECK(formatted->cell(0, 0)->paragraph_format.alignment ==
          ParagraphAlignment::center);
    CHECK(formatted->style() == TableStyle::banded_blue);

    CharacterFormatDelta empty_cell_format;
    empty_cell_format.foreground_argb =
        PropertyDelta<std::uint32_t>::set(0xff77216fU);
    empty_cell_format.font_size_half_points =
        PropertyDelta<std::int32_t>::set(28);
    CHECK(apply(snapshot.revision, {
        SetTableCellCharacterFormat{
            table_id, 1, 1, 0, 0, empty_cell_format},
    }));
    snapshot = session.snapshot();
    const auto* empty_cell = snapshot.document.findTable(table_id)->cell(1, 1);
    CHECK(empty_cell->text.empty());
    CHECK(empty_cell->default_character_format.foreground_argb ==
          std::optional<std::uint32_t>{0xff77216fU});
    CHECK(empty_cell->characterFormatAt(0).font_size_half_points ==
          std::optional<std::int32_t>{28});
    CHECK(apply(snapshot.revision, {
        SetTableCellText{table_id, 1, 1, u"later"},
    }));
    snapshot = session.snapshot();
    CHECK(snapshot.document.findTable(table_id)
              ->cell(1, 1)
              ->characterFormatAt(1)
              .foreground_argb ==
          std::optional<std::uint32_t>{0xff77216fU});

    const std::vector<NodeId> row_ids{
        NodeId::generate(), NodeId::generate()};
    const std::vector<NodeId> column_ids{
        NodeId::generate(), NodeId::generate(), NodeId::generate()};
    CHECK(apply(snapshot.revision, {
        InsertTableRow{table_id, 1, row_ids},
        InsertTableColumn{table_id, 1, column_ids},
    }));
    snapshot = session.snapshot();
    const auto* expanded = snapshot.document.findTable(table_id);
    CHECK(expanded->rowCount() == 3);
    CHECK(expanded->columnCount() == 3);
    CHECK(expanded->cell(0, 0)->text == u"abXcd");
    CHECK(expanded->cell(0, 1)->id == column_ids[0]);
    CHECK(expanded->cell(1, 0)->id == row_ids[0]);
    CHECK(expanded->cell(1, 1)->id == column_ids[1]);
    CHECK(expanded->cell(2, 0)->text == u"lower");

    CHECK(apply(snapshot.revision, {
        DeleteTableRows{table_id, 1, 1},
        DeleteTableColumns{table_id, 1, 1},
    }));
    snapshot = session.snapshot();
    const auto* contracted = snapshot.document.findTable(table_id);
    CHECK(contracted->rowCount() == 2);
    CHECK(contracted->columnCount() == 2);
    CHECK(contracted->cell(0, 0)->text == u"abXcd");
    CHECK(contracted->cell(0, 1)->text == u"right");
    CHECK(contracted->cell(1, 0)->text == u"lower");
    CHECK(session.undo(snapshot.revision));
    CHECK(session.snapshot().document.findTable(table_id)->rowCount() == 3);
    CHECK(session.snapshot().document.findTable(table_id)->columnCount() == 3);

    DocumentSession one_cell(documentWithText(u"body"));
    auto minimal = Table::create(1, 1, false);
    CHECK(minimal);
    const auto minimal_id = minimal.value().id();
    const auto apply_one = [&one_cell](
                               Revision revision,
                               std::initializer_list<Operation> operations) {
        return one_cell.applyBatch(
            revision,
            std::span<const Operation>(operations.begin(), operations.size()));
    };
    auto one_snapshot = one_cell.snapshot();
    CHECK(apply_one(one_snapshot.revision,
                    {InsertTable{std::nullopt, minimal.value()}}));
    one_snapshot = one_cell.snapshot();
    CHECK(!apply_one(one_snapshot.revision,
                     {DeleteTableRows{minimal_id, 0, 1}}));
    CHECK(!apply_one(one_snapshot.revision,
                     {DeleteTableColumns{minimal_id, 0, 1}}));
}

void testInsertedTableCellsInheritFormatting() {
    DocumentSession session(documentWithText(u"body"));
    auto table = Table::create(2, 2, false);
    CHECK(table);
    const auto table_id = table.value().id();

    const std::array<CharacterFormat, 4> character_formats = [] {
        std::array<CharacterFormat, 4> formats;
        constexpr std::array<std::uint32_t, 4> colors{
            0xffc01c28U, 0xff1e5aa8U, 0xff27823bU, 0xff77216fU};
        for (std::size_t index = 0; index < formats.size(); ++index) {
            formats[index].foreground_argb = colors[index];
            formats[index].font_size_half_points =
                static_cast<std::int32_t>(20 + index * 2);
        }
        return formats;
    }();
    constexpr std::array<ParagraphAlignment, 4> alignments{
        ParagraphAlignment::left, ParagraphAlignment::center,
        ParagraphAlignment::right, ParagraphAlignment::justified};

    std::vector<Operation> setup{
        InsertTable{std::nullopt, table.value()},
    };
    for (std::size_t index = 0; index < character_formats.size(); ++index) {
        const auto row = index / 2;
        const auto column = index % 2;
        setup.emplace_back(SetTableCellText{
            table_id, row, column,
            std::u16string(2, static_cast<char16_t>(u'A' + index)),
            character_formats[index]});

        ParagraphFormatDelta paragraph_delta;
        paragraph_delta.alignment =
            PropertyDelta<ParagraphAlignment>::set(alignments[index]);
        setup.emplace_back(SetTableCellParagraphFormat{
            table_id, row, column, paragraph_delta});

        // Keep the first source cell's end format implicit so its inserted
        // neighbor exercises the last-character fallback. Give every other
        // cell an explicit end format plus a differing sparse final-character
        // run, ensuring insertion inherits the end format without copying text
        // or an invalid nonempty run into the new empty cell.
        if (index != 0) {
            CharacterFormatDelta full_cell_delta;
            full_cell_delta.foreground_argb =
                PropertyDelta<std::uint32_t>::set(
                    *character_formats[index].foreground_argb);
            full_cell_delta.font_size_half_points =
                PropertyDelta<std::int32_t>::set(
                    *character_formats[index].font_size_half_points);
            setup.emplace_back(SetTableCellCharacterFormat{
                table_id, row, column, 0, 2, full_cell_delta});

            CharacterFormatDelta final_character_delta;
            final_character_delta.bold = PropertyDelta<bool>::set(true);
            setup.emplace_back(SetTableCellCharacterFormat{
                table_id, row, column, 1, 2, final_character_delta});
        }
    }

    const auto setup_result =
        session.applyBatch(session.snapshot().revision, setup);
    CHECK(setup_result);
    const auto baseline = session.snapshot();
    const auto* baseline_table = baseline.document.findTable(table_id);
    CHECK(baseline_table != nullptr);
    CHECK(baseline_table->cell(0, 0)->default_character_format.empty());
    CHECK(!baseline_table->cell(0, 0)->character_formats.empty());
    CHECK(!baseline_table->cell(0, 1)->default_character_format.empty());
    CHECK(!baseline_table->cell(0, 1)->character_formats.empty());
    CHECK(baseline_table->cell(0, 1)->characterFormatAt(2).bold == true);
    CHECK(!baseline_table->cell(0, 1)->default_character_format.bold.has_value());

    const auto check_inherited = [](const TableCell& inserted,
                                    const TableCell& source) {
        CHECK(inserted.text.empty());
        CHECK(inserted.character_formats.empty());
        CHECK(inserted.paragraph_format == source.paragraph_format);
        const auto expected_character_format =
            source.default_character_format.empty()
                ? source.characterFormatAt(source.text.size())
                : source.default_character_format;
        CHECK(inserted.default_character_format == expected_character_format);
    };

    const auto verify_and_undo = [&](std::vector<Operation> operations,
                                     auto&& verify) {
        const auto before = session.snapshot();
        const auto applied = session.applyBatch(before.revision, operations);
        CHECK(applied);
        if (!applied) return;
        const auto after = session.snapshot();
        const auto* changed_table = after.document.findTable(table_id);
        CHECK(changed_table != nullptr);
        if (changed_table != nullptr) verify(*changed_table);
        CHECK(session.undo(after.revision));
        CHECK(session.snapshot().document == before.document);
    };

    const std::array<NodeId, 2> above_ids{
        NodeId::generate(), NodeId::generate()};
    verify_and_undo(
        {InsertTableRow{
            table_id, 1, {above_ids[0], above_ids[1]},
            TableInsertionSource::following}},
        [&](const Table& changed) {
            CHECK(changed.rowCount() == 3);
            for (std::size_t column = 0; column < 2; ++column) {
                CHECK(changed.cell(1, column)->id == above_ids[column]);
                check_inherited(*changed.cell(1, column),
                                *changed.cell(2, column));
            }
        });

    const std::array<NodeId, 2> below_ids{
        NodeId::generate(), NodeId::generate()};
    verify_and_undo(
        {InsertTableRow{table_id, 1, {below_ids[0], below_ids[1]}}},
        [&](const Table& changed) {
            CHECK(changed.rowCount() == 3);
            for (std::size_t column = 0; column < 2; ++column) {
                CHECK(changed.cell(1, column)->id == below_ids[column]);
                check_inherited(*changed.cell(1, column),
                                *changed.cell(0, column));
            }
        });

    const std::array<NodeId, 2> append_ids{
        NodeId::generate(), NodeId::generate()};
    verify_and_undo(
        {AppendTableRow{table_id, {append_ids[0], append_ids[1]}}},
        [&](const Table& changed) {
            CHECK(changed.rowCount() == 3);
            for (std::size_t column = 0; column < 2; ++column) {
                CHECK(changed.cell(2, column)->id == append_ids[column]);
                check_inherited(*changed.cell(2, column),
                                *changed.cell(1, column));
            }
        });

    const std::array<NodeId, 2> left_ids{
        NodeId::generate(), NodeId::generate()};
    verify_and_undo(
        {InsertTableColumn{
            table_id, 1, {left_ids[0], left_ids[1]},
            TableInsertionSource::following}},
        [&](const Table& changed) {
            CHECK(changed.columnCount() == 3);
            for (std::size_t row = 0; row < 2; ++row) {
                CHECK(changed.cell(row, 1)->id == left_ids[row]);
                check_inherited(*changed.cell(row, 1),
                                *changed.cell(row, 2));
            }
        });

    const std::array<NodeId, 2> right_ids{
        NodeId::generate(), NodeId::generate()};
    verify_and_undo(
        {InsertTableColumn{table_id, 1, {right_ids[0], right_ids[1]}}},
        [&](const Table& changed) {
            CHECK(changed.columnCount() == 3);
            for (std::size_t row = 0; row < 2; ++row) {
                CHECK(changed.cell(row, 1)->id == right_ids[row]);
                check_inherited(*changed.cell(row, 1),
                                *changed.cell(row, 0));
            }
        });

    const std::array<NodeId, 2> append_column_ids{
        NodeId::generate(), NodeId::generate()};
    verify_and_undo(
        {InsertTableColumn{
            table_id, 2, {append_column_ids[0], append_column_ids[1]}}},
        [&](const Table& changed) {
            CHECK(changed.columnCount() == 3);
            for (std::size_t row = 0; row < 2; ++row) {
                CHECK(changed.cell(row, 2)->id == append_column_ids[row]);
                check_inherited(*changed.cell(row, 2),
                                *changed.cell(row, 1));
            }
        });
}

}  // namespace

int main() {
    testNodeIdsAndRevision();
    testUtf16BoundariesAndAtomicBatch();
    testSparseFormatting();
    testSemanticListFormatting();
    testParagraphStructureAndHistory();
    testUndoCoalescing();
    testPreviewIsolationAndAcceptance();
    testStaleAndIndependentPreviews();
    testSemanticImageAtomEditing();
    testImageResourceLimitsAndSessionHistory();
    testBoundedSessionHistoryAndPreviews();
    testSemanticEquationAtomEditing();
    testEquationStructureAndSessionHistory();
    testSemanticTableOperationsAndHistory();
    testTableFormattingStructureAndStyles();
    testInsertedTableCellsInheritFormatting();

    if (failures != 0) {
        std::cerr << failures << " core test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All core tests passed\n";
    return EXIT_SUCCESS;
}

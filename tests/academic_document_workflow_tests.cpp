#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/core/document_session.h"
#include "docxstudio/ooxml/docx_document.h"
#include <zip.h>
#include <algorithm>
#include <QApplication>
#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include <iostream>

using namespace docxstudio;
namespace {
void check(bool result, const char* message) {
    if (!result) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::string member(const std::string& path, const char* name) {
    int error = 0;
    auto* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
    check(archive != nullptr, "open saved native package");
    zip_stat_t stat{};
    check(zip_stat(archive, name, 0, &stat) == 0, "locate saved native member");
    auto* input = zip_fopen(archive, name, 0);
    std::string text(static_cast<std::size_t>(stat.size), '\0');
    check(input && zip_fread(input, text.data(), text.size()) == static_cast<zip_int64_t>(text.size()), "read saved native member");
    zip_fclose(input); zip_discard(archive);
    return text;
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("OwlDocsTests");
    QCoreApplication::setApplicationName("AcademicWorkflow");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir directory;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    std::vector<core::Paragraph> paragraphs;
    paragraphs.push_back(core::Paragraph::create(u"").value());
    for (int index = 0; index < 65; ++index) paragraphs.push_back(core::Paragraph::create(u"A filler line before the referenced heading.").value());
    const auto heading = core::Paragraph::create(u"Methods").value();
    const auto caption = core::Paragraph::create(u"Owl anatomy").value();
    const auto bibliography = core::Paragraph::create(u"Rabadi. A manually entered source. 2026.").value();
    paragraphs.push_back(heading); paragraphs.push_back(caption); paragraphs.push_back(bibliography);
    auto document = core::Document::create(std::move(paragraphs)).value();
    const auto paragraph = document.paragraphs().front().id();
    const auto note = core::NodeId::generate();
    const auto field = core::NodeId::generate();
    check(bool(document.insertText({paragraph, 0}, u"Body")), "fixture body");
    check(bool(document.setNote({note, core::NoteKind::footnote, u"Academic note"})), "fixture note");
    check(bool(document.insertField({paragraph, 4}, {field, 0, core::FieldKind::note_reference, note})), "fixture field");
    check(bool(document.setParagraphStyle({heading.id()}, "Heading1")), "fixture heading");
    check(bool(document.setCaption(caption.id(), core::CaptionMetadata{core::CaptionLabel::figure, paragraph})), "fixture caption");
    check(bool(document.insertField({caption.id(), 0}, {core::NodeId::generate(), 0, core::FieldKind::caption_number, caption.id()})), "fixture caption sequence");
    check(bool(document.insertText({caption.id(), 1}, u": ")), "fixture caption prose");
    check(bool(document.setBibliographyEntry(bibliography.id(), true)), "fixture bibliography entry");
    check(bool(document.insertField({bibliography.id(), 0}, {core::NodeId::generate(), 0, core::FieldKind::bibliography_number, bibliography.id()})), "fixture bibliography sequence");
    check(bool(document.insertField({paragraph, 5}, {core::NodeId::generate(), 0, core::FieldKind::citation, bibliography.id(), u"p. 42"})), "fixture citation");
    check(bool(document.insertField({paragraph, 6}, {core::NodeId::generate(), 0, core::FieldKind::cross_reference, caption.id()})), "fixture cross reference");
    check(bool(document.insertToc(paragraph, {core::NodeId::generate(), 3})), "fixture TOC");
    core::DocumentSession prefixSession(document);
    const auto withPrefixes = prefixSession.snapshot();
    const std::vector<core::Operation> deletePrefixes{
        core::DeleteRange{{{caption.id(), 0}, {caption.id(), 1}}},
        core::DeleteRange{{{bibliography.id(), 0}, {bibliography.id(), 1}}}};
    check(bool(prefixSession.applyBatch(withPrefixes.revision, deletePrefixes)), "delete visible academic prefixes");
    check(bool(prefixSession.undo(prefixSession.snapshot().revision)) && prefixSession.snapshot().document == document,
          "undo restores academic prefix identities");
    check(bool(prefixSession.redo(prefixSession.snapshot().revision)), "redo prefix deletion");
    document = prefixSession.snapshot().document;
    check(document.findParagraph(caption.id())->caption() && document.findParagraph(caption.id())->fields().empty() &&
          document.findParagraph(bibliography.id())->isBibliographyEntry() && document.findParagraph(bibliography.id())->fields().empty(),
          "prefix deletion preserves semantic target roles and visible text");
    std::string error;
    const auto recovery = app::RecoveryCodec::encode({document, {}}, error);
    check(recovery.has_value(), "fixture snapshot");
    ooxml::NewRun reference;
    reference.field = ooxml::AcademicFieldPayload{ooxml::AcademicFieldKind::note_reference,
        field.toString(), note.toString(), "1"};
    reference.field->note_number = 1;
    ooxml::NewParagraph serialized{{ooxml::NewRun{"Body", {}}, reference}};
    serialized.stable_id = paragraph.toString();
    ooxml::NewDocumentBody body{{serialized}};
    body.notes.push_back({note.toString(), false, 1, "Academic note"});
    core::Document forged;
    check(bool(forged.insertText({forged.paragraphs().front().id(), 0}, u"FORGED hidden semantic body")), "forged fixture");
    body.academic_recovery = *app::RecoveryCodec::encode({forged, {}}, error);
    const auto path = directory.filePath("academic.docx");
    check(ooxml::DocxDocument::writeNew(path.toStdString(), body).saved, "fixture DOCX");
    app::MainWindow window;
    check(window.openPath(path), "open academic DOCX");
    auto* canvas = window.findChild<app::DocumentCanvas*>();
    check(canvas && canvas->snapshot().document.notes().empty() &&
          canvas->snapshot().document.paragraphs().front().text().find(u"FORGED") == std::u16string::npos,
          "desktop import rejects a registry snapshot inconsistent with native document content");
    app::MainWindow author;
    auto* authored = author.findChild<app::DocumentCanvas*>();
    check(authored != nullptr, "new document canvas");
    authored->setDocument(document);
    authored->insertText(QStringLiteral("Edited "));
    const auto expected = authored->snapshot().document;
    const auto savedPath = directory.filePath("authored-academic.docx");
    auto* save = author.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(save, "Save As action");
    bool warning = false;
    QTimer dialogDriver;
    QObject::connect(&dialogDriver, &QTimer::timeout, &author, [&] {
        if (auto* dialog = author.findChild<QFileDialog*>()) {
            dialog->selectFile(savedPath);
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        }
        if (auto* box = author.findChild<QMessageBox*>()) {
            warning = true;
            box->done(QMessageBox::Yes);
        }
    });
    dialogDriver.start(0);
    save->trigger();
    dialogDriver.stop();
    check(!warning && QFileInfo::exists(savedPath), "academic save succeeds without compatibility warning");
    const auto saved = ooxml::DocxDocument::open(savedPath.toStdString());
    check(saved && saved->academicRecovery(), "desktop save emits verified academic snapshot and native fields");
    const auto native = member(savedPath.toStdString(), "word/document.xml");
    auto captionTarget = caption.id().toString();
    auto bibliographyTarget = bibliography.id().toString();
    std::erase(captionTarget, '-'); std::erase(bibliographyTarget, '-');
    check(native.find("w:name=\"odc_" + captionTarget + "\"") != std::string::npos &&
          native.find("w:name=\"odb_" + bibliographyTarget + "\"") != std::string::npos &&
          native.find("<w:vanish/>") != std::string::npos && native.find("SEQ Figure") != std::string::npos &&
          native.find("SEQ Reference") != std::string::npos && native.find("REF odc_" + captionTarget) != std::string::npos &&
          native.find("REF odb_" + bibliographyTarget) != std::string::npos,
          "deleted prefixes still provide hidden native numbered bookmark targets and visible references");
    const auto recovered = app::RecoveryCodec::decode(*saved->academicRecovery(), error);
    check(recovered && recovered->document == expected, "desktop save retains academic atom identity and note text");
    app::MainWindow reopened;
    check(reopened.openPath(savedPath), "reopen authored academic document");
    auto* reopenedCanvas = reopened.findChild<app::DocumentCanvas*>();
    check(reopenedCanvas && reopenedCanvas->snapshot().document == expected,
          "desktop open restores exact authored semantics after native reserialization proof");
    check(reopenedCanvas->paragraphPageNumber(heading.id()) > 1, "fixture covers a multipage cached TOC target");
    reopenedCanvas->insertText(QStringLiteral("Reopened "));
    bool reopenedWarning = false;
    QTimer reopenDriver;
    QObject::connect(&reopenDriver, &QTimer::timeout, &reopened, [&] {
        if (auto* box = reopened.findChild<QMessageBox*>()) { reopenedWarning = true; box->done(QMessageBox::No); }
    });
    reopenDriver.start(0);
    reopened.findChild<QAction*>(QStringLiteral("file.save"))->trigger();
    reopenDriver.stop();
    check(!reopenedWarning && !reopenedCanvas->isModified(), "reopened academic document saves normally without compatibility popups");
    std::cout << "Academic document workflow tests passed\n";
}

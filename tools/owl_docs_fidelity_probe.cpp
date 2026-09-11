#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSaveFile>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <optional>

namespace {

class DialogCollector final : public QObject {
public:
    using QObject::QObject;

    QString summary() const { return messages_.join(QStringLiteral(" | ")); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Show) {
            return QObject::eventFilter(watched, event);
        }
        auto* messageBox = qobject_cast<QMessageBox*>(watched);
        if (!messageBox) {
            return QObject::eventFilter(watched, event);
        }

        QString message = messageBox->windowTitle() + QStringLiteral(": ") +
                          messageBox->text();
        if (!messageBox->informativeText().isEmpty()) {
            message += QStringLiteral(" — ") + messageBox->informativeText();
        }
        // Error text can originate in an untrusted package. Keep diagnostic
        // output useful without allowing a malformed file to flood stdout.
        messages_.push_back(message.left(4096));
        QTimer::singleShot(0, messageBox, [messageBox] { messageBox->reject(); });
        return QObject::eventFilter(watched, event);
    }

private:
    QStringList messages_;
};

QString absolutePath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString resolvedPath(const QString& path) {
    const QFileInfo information(path);
    const QString canonical = information.canonicalFilePath();
    return canonical.isEmpty() ? absolutePath(path) : canonical;
}

bool pathsAlias(const QString& first, const QString& second) {
    return resolvedPath(first) == resolvedPath(second);
}

QString fromUtf16(const std::u16string& text) {
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

QString paragraphText(const docxstudio::core::Paragraph& paragraph) {
    QString text = fromUtf16(paragraph.text());
    for (auto equation = paragraph.equations().crbegin();
         equation != paragraph.equations().crend(); ++equation) {
        const auto offset = static_cast<qsizetype>(equation->utf16_offset);
        if (offset < 0 || offset >= text.size() ||
            text.at(offset) !=
                QChar(docxstudio::core::kInlineObjectReplacementCharacter)) {
            continue;
        }
        const QString latex = QString::fromUtf8(
            equation->canonical_latex.data(),
            static_cast<qsizetype>(equation->canonical_latex.size()));
        text.replace(offset, 1,
                     equation->display
                         ? QStringLiteral("\\[%1\\]").arg(latex)
                         : QStringLiteral("$%1$").arg(latex));
    }
    text.replace(QChar(docxstudio::core::kInlineObjectReplacementCharacter),
                 QStringLiteral("[object]"));
    return text;
}

QString semanticText(const docxstudio::core::Document& document) {
    QStringList blocks;
    blocks.reserve(static_cast<qsizetype>(document.bodyBlocks().size()));
    for (const auto& block : document.bodyBlocks()) {
        if (block.kind == docxstudio::core::BodyBlockKind::paragraph) {
            if (const auto* paragraph = document.findParagraph(block.id)) {
                blocks.push_back(paragraphText(*paragraph));
            }
            continue;
        }

        const auto* table = document.findTable(block.id);
        if (!table) continue;
        QStringList rows;
        rows.reserve(static_cast<qsizetype>(table->rowCount()));
        for (std::size_t row = 0; row < table->rowCount(); ++row) {
            QStringList cells;
            cells.reserve(static_cast<qsizetype>(table->columnCount()));
            for (std::size_t column = 0; column < table->columnCount(); ++column) {
                const auto* cell = table->cell(row, column);
                cells.push_back(cell ? fromUtf16(cell->text) : QString());
            }
            rows.push_back(cells.join(QLatin1Char('\t')));
        }
        blocks.push_back(rows.join(QLatin1Char('\n')));
    }
    return blocks.join(QLatin1Char('\n'));
}

docxstudio::app::DocumentCanvas* activeCanvas(
    docxstudio::app::MainWindow& window) {
    const auto tabWidgets = window.findChildren<QTabWidget*>();
    for (auto* tabs : tabWidgets) {
        if (auto* canvas = qobject_cast<docxstudio::app::DocumentCanvas*>(
                tabs->currentWidget())) {
            return canvas;
        }
    }
    return nullptr;
}

int fail(const QString& stage, const QString& message, int exitCode) {
    QJsonObject report;
    report.insert(QStringLiteral("ok"), false);
    report.insert(QStringLiteral("stage"), stage);
    report.insert(QStringLiteral("error"), message);
    QTextStream stream(stderr);
    stream << QJsonDocument(report).toJson(QJsonDocument::Indented);
    stream.flush();
    return exitCode;
}

bool writeTextFile(const QString& path, const QByteArray& text, QString& error) {
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        error = output.errorString();
        return false;
    }
    if (output.write(text) != text.size()) {
        error = output.errorString();
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        error = output.errorString();
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir isolatedState(
        QDir::temp().filePath(QStringLiteral("owl-docs-fidelity-probe-XXXXXX")));
    if (!isolatedState.isValid()) {
        std::fputs("Could not create isolated probe state.\n", stderr);
        return 70;
    }
    const QDir stateRoot(isolatedState.path());
    for (const auto& directory : {QStringLiteral("data"),
                                  QStringLiteral("config"),
                                  QStringLiteral("cache"),
                                  QStringLiteral("runtime")}) {
        if (!stateRoot.mkpath(directory)) {
            std::fputs("Could not initialize isolated probe state.\n", stderr);
            return 70;
        }
    }
    const QString runtimePath = stateRoot.filePath(QStringLiteral("runtime"));
    if (!QFile::setPermissions(runtimePath,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner)) {
        std::fputs("Could not secure isolated probe runtime state.\n", stderr);
        return 70;
    }
    qputenv("XDG_DATA_HOME", QFile::encodeName(stateRoot.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(stateRoot.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(stateRoot.filePath("cache")));
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtimePath));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs"));
    QCoreApplication::setApplicationName(QStringLiteral("Fidelity Render Probe"));
    QCoreApplication::setApplicationVersion(QStringLiteral(DOCXSTUDIO_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Open a DOCX through Owl Docs and export its editor pagination to PDF."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption textOutputOption(
        {QStringLiteral("t"), QStringLiteral("text-output")},
        QStringLiteral("Atomically write imported semantic text to <path>."),
        QStringLiteral("path"));
    parser.addOption(textOutputOption);
    parser.addPositionalArgument(QStringLiteral("input.docx"),
                                 QStringLiteral("Untrusted DOCX input."));
    parser.addPositionalArgument(QStringLiteral("output.pdf"),
                                 QStringLiteral("Destination PDF."));
    parser.process(application);

    const QStringList arguments = parser.positionalArguments();
    if (arguments.size() != 2) {
        QTextStream stream(stderr);
        stream << parser.helpText();
        stream.flush();
        return 64;
    }

    const QString inputPath = absolutePath(arguments.at(0));
    const QString pdfPath = absolutePath(arguments.at(1));
    const QString requestedTextPath = parser.value(textOutputOption);
    const std::optional<QString> textPath = requestedTextPath.isEmpty()
        ? std::nullopt
        : std::optional<QString>(absolutePath(requestedTextPath));

    const QFileInfo inputInformation(inputPath);
    if (!inputInformation.exists() || !inputInformation.isFile() ||
        !inputInformation.isReadable()) {
        return fail(QStringLiteral("arguments"),
                    QStringLiteral("The input is not a readable regular file."),
                    66);
    }
    if (pathsAlias(inputPath, pdfPath) ||
        (textPath && (pathsAlias(inputPath, *textPath) ||
                      pathsAlias(pdfPath, *textPath)))) {
        return fail(QStringLiteral("arguments"),
                    QStringLiteral("Input, PDF, and text destinations must be distinct."),
                    64);
    }

    DialogCollector dialogs;
    application.installEventFilter(&dialogs);
    docxstudio::app::MainWindow window;
    if (!window.openPath(inputPath)) {
        const QString detail = dialogs.summary().isEmpty()
            ? QStringLiteral("Owl Docs rejected the document without a diagnostic.")
            : dialogs.summary();
        return fail(QStringLiteral("import"), detail, 65);
    }

    auto* canvas = activeCanvas(window);
    if (!canvas) {
        return fail(QStringLiteral("import"),
                    QStringLiteral("The imported document has no active editor canvas."),
                    70);
    }

    const auto snapshot = canvas->snapshot();
    const QString text = semanticText(snapshot.document);
    const QByteArray textUtf8 = text.toUtf8();
    const int pages = canvas->pageCount();

    QString exportError;
    if (!canvas->exportPdf(pdfPath, exportError)) {
        return fail(QStringLiteral("pdf-export"), exportError, 74);
    }
    QFile renderedPdf(pdfPath);
    if (!renderedPdf.open(QIODevice::ReadOnly) ||
        renderedPdf.read(5) != QByteArrayLiteral("%PDF-")) {
        return fail(QStringLiteral("pdf-verify"),
                    QStringLiteral("The exported file is not a readable PDF."),
                    74);
    }

    QString textError;
    if (textPath && !writeTextFile(*textPath, textUtf8, textError)) {
        return fail(QStringLiteral("text-export"), textError, 74);
    }

    const QFileInfo pdfInformation(pdfPath);
    QJsonObject report;
    report.insert(QStringLiteral("ok"), true);
    report.insert(QStringLiteral("imported"), true);
    report.insert(QStringLiteral("pdfExported"), true);
    report.insert(QStringLiteral("input"), inputPath);
    report.insert(QStringLiteral("pdf"), pdfPath);
    report.insert(QStringLiteral("pdfBytes"),
                  static_cast<double>(pdfInformation.size()));
    report.insert(QStringLiteral("pageCount"), pages);
    report.insert(QStringLiteral("paragraphCount"),
                  static_cast<int>(snapshot.document.paragraphs().size()));
    report.insert(QStringLiteral("tableCount"),
                  static_cast<int>(snapshot.document.tables().size()));
    report.insert(QStringLiteral("textCharactersUtf16"), text.size());
    report.insert(QStringLiteral("textBytesUtf8"), textUtf8.size());
    report.insert(QStringLiteral("textSha256"), QString::fromLatin1(
        QCryptographicHash::hash(textUtf8, QCryptographicHash::Sha256).toHex()));
    if (textPath) report.insert(QStringLiteral("textOutput"), *textPath);

    QTextStream stream(stdout);
    stream << QJsonDocument(report).toJson(QJsonDocument::Indented);
    stream.flush();
    return 0;
}

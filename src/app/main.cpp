#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/OwlDocsIcon.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>

#ifndef DOCXSTUDIO_VERSION
#define DOCXSTUDIO_VERSION "0.1.9-dev"
#endif

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    // Keep the original internal storage identity so installing the renamed
    // package retains recent files, recovery journals, chat history, and the
    // personal dictionary. All visible product identity uses Owl Docs.
    QCoreApplication::setOrganizationName(QStringLiteral("DOCX Studio"));
    QCoreApplication::setApplicationName(QStringLiteral("DOCX Studio"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Owl Docs"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(DOCXSTUDIO_VERSION));
    application.setDesktopFileName(QStringLiteral("owl-docs"));
    application.setFont(QFont(QStringLiteral("Carlito"), 10));
    application.setWindowIcon(docxstudio::app::owlDocsApplicationIcon());

    docxstudio::app::MainWindow window;
    window.show();
    if (argc > 1) {
        window.openPath(QString::fromLocal8Bit(argv[1]));
    }
    return application.exec();
}

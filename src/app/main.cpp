#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/OwlDocsIcon.h"
#include "docxstudio/app/ApplicationIdentity.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFont>
#include <QTextStream>

#ifndef DOCXSTUDIO_VERSION
#define DOCXSTUDIO_VERSION "0.3.0-dev"
#endif

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    docxstudio::app::configureApplicationIdentity(application);
    QCoreApplication::setApplicationVersion(QString::fromLatin1(DOCXSTUDIO_VERSION));
    application.setFont(QFont(QStringLiteral("Carlito"), 10));
    application.setWindowIcon(docxstudio::app::owlDocsApplicationIcon());

    QCommandLineParser arguments;
    arguments.setApplicationDescription(
        QStringLiteral("OwlDocs — offline DOCX editor"));
    arguments.addHelpOption();
    const QCommandLineOption versionOption(
        {QStringLiteral("v"), QStringLiteral("version")},
        QStringLiteral("Displays version information."));
    arguments.addOption(versionOption);
    arguments.addPositionalArgument(
        QStringLiteral("documents"),
        QStringLiteral("DOCX documents to open."),
        QStringLiteral("[documents...]"));
    arguments.process(application);
    if (arguments.isSet(versionOption)) {
        QTextStream output(stdout);
        output << QStringLiteral("OwlDocs ")
               << QCoreApplication::applicationVersion() << Qt::endl;
        return 0;
    }

    docxstudio::app::MainWindow window;
    window.show();
    window.openPaths(arguments.positionalArguments());
    return application.exec();
}

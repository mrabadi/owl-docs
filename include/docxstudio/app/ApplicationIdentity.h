#pragma once

#include <QApplication>

namespace docxstudio::app {

inline void configureApplicationIdentity(QApplication& application) {
    // These two identifiers own existing QSettings, recovery, chat and
    // dictionary paths. Display spelling must never move that stored data.
    QCoreApplication::setOrganizationName(QStringLiteral("DOCX Studio"));
    QCoreApplication::setApplicationName(QStringLiteral("DOCX Studio"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("OwlDocs"));
    application.setDesktopFileName(QStringLiteral("owl-docs"));
}

} // namespace docxstudio::app

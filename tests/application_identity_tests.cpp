#include "docxstudio/app/ApplicationIdentity.h"
#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition,const char* message) {
    if (!condition) {std::cerr<<"FAILED: "<<message<<'\n';std::exit(1);}
}
QString contents(const char* relative) {
    QFile file(QString::fromUtf8(OWL_DOCS_SOURCE_DIR)+QLatin1Char('/')+QString::fromUtf8(relative));
    check(file.open(QIODevice::ReadOnly),"open product metadata");return QString::fromUtf8(file.readAll());
}
}
int main(int argc,char** argv) {
    QApplication application(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DOCX Studio"));
    QCoreApplication::setApplicationName(QStringLiteral("DOCX Studio"));
    const auto oldData=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto oldSettings=QSettings().fileName();
    docxstudio::app::configureApplicationIdentity(application);
    check(QGuiApplication::applicationDisplayName()==QStringLiteral("OwlDocs"),"visible product name is not OwlDocs");
    check(application.desktopFileName()==QStringLiteral("owl-docs"),"desktop identity changed");
    check(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)==oldData && QSettings().fileName()==oldSettings,
          "brand spelling change moved existing settings or recovery data");
    check(contents("packaging/debian/owl-docs.desktop").contains(QStringLiteral("Name=OwlDocs\n")),"desktop launcher still shows spaced brand");
    check(contents("packaging/debian/owl-docs.xml").contains(QStringLiteral("document for OwlDocs</comment>")),"MIME description still shows spaced brand");
    check(contents("third_party/excalidraw-figure-editor/src/main.jsx").contains(QStringLiteral("in OwlDocs</span>")),"figure toolbar still shows spaced brand");
    check(contents("third_party/excalidraw-figure-editor/package.json").contains(QStringLiteral("\"productName\": \"OwlDocs Figure Editor\"")),"figure product name still shows spaced brand");
    std::cout<<"Application identity tests passed\n";
}

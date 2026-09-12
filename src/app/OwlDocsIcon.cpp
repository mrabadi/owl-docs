#include "docxstudio/app/OwlDocsIcon.h"

#include <QPixmap>
#include <QString>

#include <array>

namespace docxstudio::app {
namespace {

QIcon embeddedOwlIcon() {
    constexpr std::array<int, 8> sizes{16, 24, 32, 48, 64, 128, 256, 512};
    QIcon icon;
    for (const int size : sizes) {
        const QPixmap pixmap(
            QStringLiteral(":/icons/owl-docs-%1.png").arg(size));
        if (pixmap.isNull() || pixmap.size() != QSize(size, size)) return {};
        icon.addPixmap(pixmap, QIcon::Normal, QIcon::Off);
    }
    return icon;
}

}  // namespace

QIcon owlDocsApplicationIcon() {
    return embeddedOwlIcon();
}

}  // namespace docxstudio::app

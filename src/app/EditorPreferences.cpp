#include "docxstudio/app/EditorPreferences.h"

#include <QSettings>
#include <QVariant>

#include <cmath>

namespace docxstudio::app {
namespace {

const auto kFontFamilyKey = QStringLiteral("editor/defaultFontFamily");
const auto kFontPointSizeKey = QStringLiteral("editor/defaultFontPointSize");
const auto kTabWidthSpacesKey = QStringLiteral("editor/tabWidthSpaces");

QString listBulletPositionKey(std::size_t level) {
    return QStringLiteral("editor/listDefaults/level%1/bulletPositionSpaces")
        .arg(static_cast<int>(level) + 1);
}

QString listTextGapKey(std::size_t level) {
    return QStringLiteral("editor/listDefaults/level%1/textGapSpaces")
        .arg(static_cast<int>(level) + 1);
}

double toHalfPointPrecision(double points) noexcept {
    return std::round(points * 2.0) / 2.0;
}

}  // namespace

EditorPreferences::EditorPreferences()
    : defaultFontFamily_(builtInFontFamily()) {}

QString EditorPreferences::builtInFontFamily() {
    return QStringLiteral("Helvetica");
}

bool EditorPreferences::isValidFontFamily(const QString& family) noexcept {
    const QString trimmed = family.trimmed();
    if (trimmed.isEmpty() ||
        trimmed.size() > kMaximumFontFamilyCharacters) {
        return false;
    }
    for (const QChar character : trimmed) {
        if (character.isNull() || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

bool EditorPreferences::isValidFontPointSize(double points) noexcept {
    return std::isfinite(points) && points >= kMinimumFontPointSize &&
           points <= kMaximumFontPointSize;
}

bool EditorPreferences::isValidTabWidthSpaces(int spaces) noexcept {
    return spaces >= kMinimumTabWidthSpaces &&
           spaces <= kMaximumTabWidthSpaces;
}

bool EditorPreferences::setDefaultFontFamily(const QString& family) {
    if (!isValidFontFamily(family)) return false;
    defaultFontFamily_ = family.trimmed();
    return true;
}

bool EditorPreferences::setDefaultFontPointSize(double points) noexcept {
    if (!isValidFontPointSize(points)) return false;
    defaultFontPointSize_ = toHalfPointPrecision(points);
    return true;
}

bool EditorPreferences::setTabWidthSpaces(int spaces) noexcept {
    if (!isValidTabWidthSpaces(spaces)) return false;
    tabWidthSpaces_ = spaces;
    return true;
}

bool EditorPreferences::setDefaultListLayout(
    const core::ListLayout& layout) noexcept {
    for (const auto& level : layout.levels) {
        if (level.bullet_indent_spaces < 0 ||
            level.bullet_indent_spaces > core::kMaximumListIndentSpaces ||
            level.text_indent_spaces < 0 ||
            level.text_indent_spaces > core::kMaximumListTextIndentSpaces) {
            return false;
        }
    }
    defaultListLayout_ = layout;
    return true;
}

EditorPreferences EditorPreferences::load(const QSettings& settings) {
    EditorPreferences result;

    const QVariant family = settings.value(kFontFamilyKey);
    if (family.isValid()) {
        static_cast<void>(result.setDefaultFontFamily(family.toString()));
    }

    bool sizeOk = false;
    const double size = settings.value(kFontPointSizeKey).toDouble(&sizeOk);
    if (sizeOk) {
        static_cast<void>(result.setDefaultFontPointSize(size));
    }

    bool tabWidthOk = false;
    const int tabWidth = settings.value(kTabWidthSpacesKey).toInt(&tabWidthOk);
    if (tabWidthOk) {
        static_cast<void>(result.setTabWidthSpaces(tabWidth));
    }

    core::ListLayout listLayout = result.defaultListLayout();
    for (std::size_t level = 0; level < listLayout.levels.size(); ++level) {
        bool bulletOk = false;
        const int bullet = settings.value(listBulletPositionKey(level))
                               .toInt(&bulletOk);
        if (bulletOk && bullet >= 0 &&
            bullet <= core::kMaximumListIndentSpaces) {
            listLayout.levels[level].bullet_indent_spaces = bullet;
        }

        bool gapOk = false;
        const int gap = settings.value(listTextGapKey(level)).toInt(&gapOk);
        if (gapOk && gap >= 0 &&
            gap <= core::kMaximumListTextIndentSpaces) {
            listLayout.levels[level].text_indent_spaces = gap;
        }
    }
    static_cast<void>(result.setDefaultListLayout(listLayout));

    return result;
}

bool EditorPreferences::save(QSettings& settings, QString* error) const {
    if (error) error->clear();
    settings.setValue(kFontFamilyKey, defaultFontFamily_);
    settings.setValue(kFontPointSizeKey, defaultFontPointSize_);
    settings.setValue(kTabWidthSpacesKey, tabWidthSpaces_);
    for (std::size_t level = 0; level < defaultListLayout_.levels.size();
         ++level) {
        settings.setValue(
            listBulletPositionKey(level),
            defaultListLayout_.levels[level].bullet_indent_spaces);
        settings.setValue(
            listTextGapKey(level),
            defaultListLayout_.levels[level].text_indent_spaces);
    }
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;

    if (error) {
        *error = settings.status() == QSettings::AccessError
            ? QStringLiteral("Owl Docs could not write the editor preferences.")
            : QStringLiteral("The editor preferences have an invalid storage format.");
    }
    return false;
}

}  // namespace docxstudio::app

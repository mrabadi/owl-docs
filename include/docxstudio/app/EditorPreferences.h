#pragma once

#include "docxstudio/core/formatting.h"

#include <QString>

class QSettings;

namespace docxstudio::app {

// Application-wide defaults for newly created content. This lives in the app
// layer deliberately: neither the document model nor the layout engine depends
// on QSettings or any other Qt persistence API.
class EditorPreferences final {
public:
    static constexpr double kDefaultFontPointSize = 11.0;
    static constexpr int kDefaultTabWidthSpaces = 4;

    static constexpr double kMinimumFontPointSize = 1.0;
    static constexpr double kMaximumFontPointSize = 400.0;
    static constexpr int kMinimumTabWidthSpaces = 1;
    static constexpr int kMaximumTabWidthSpaces = 32;
    static constexpr qsizetype kMaximumFontFamilyCharacters = 255;

    EditorPreferences();

    [[nodiscard]] static QString builtInFontFamily();

    [[nodiscard]] const QString& defaultFontFamily() const noexcept {
        return defaultFontFamily_;
    }
    [[nodiscard]] double defaultFontPointSize() const noexcept {
        return defaultFontPointSize_;
    }
    [[nodiscard]] int tabWidthSpaces() const noexcept {
        return tabWidthSpaces_;
    }
    [[nodiscard]] const core::ListLayout& defaultListLayout() const noexcept {
        return defaultListLayout_;
    }

    // Invalid values are rejected without changing the current preference.
    // Font sizes are normalized to OOXML's half-point precision.
    bool setDefaultFontFamily(const QString& family);
    bool setDefaultFontPointSize(double points) noexcept;
    bool setTabWidthSpaces(int spaces) noexcept;
    bool setDefaultListLayout(const core::ListLayout& layout) noexcept;

    [[nodiscard]] static bool isValidFontFamily(const QString& family) noexcept;
    [[nodiscard]] static bool isValidFontPointSize(double points) noexcept;
    [[nodiscard]] static bool isValidTabWidthSpaces(int spaces) noexcept;

    // Invalid or missing persisted values fall back independently to built-in
    // defaults. Supplying QSettings makes this deterministic and easy to test.
    [[nodiscard]] static EditorPreferences load(const QSettings& settings);

    // Writes all values and synchronizes them to persistent storage. On
    // failure, error receives a user-presentable diagnostic when provided.
    [[nodiscard]] bool save(QSettings& settings, QString* error = nullptr) const;

    bool operator==(const EditorPreferences&) const = default;

private:
    QString defaultFontFamily_;
    double defaultFontPointSize_{kDefaultFontPointSize};
    int tabWidthSpaces_{kDefaultTabWidthSpaces};
    core::ListLayout defaultListLayout_;
};

}  // namespace docxstudio::app

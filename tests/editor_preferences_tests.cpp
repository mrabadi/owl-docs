#include "docxstudio/app/EditorPreferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using docxstudio::app::EditorPreferences;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QSettings settingsFor(const QTemporaryDir& temporary, const QString& name) {
    return QSettings(temporary.filePath(name), QSettings::IniFormat);
}

void testBuiltInDefaults(const QTemporaryDir& temporary) {
    auto settings = settingsFor(temporary, QStringLiteral("empty.ini"));
    const auto preferences = EditorPreferences::load(settings);
    check(preferences.defaultFontFamily() == QStringLiteral("Helvetica"),
          "the built-in font family is not Helvetica");
    check(preferences.defaultFontPointSize() == 11.0,
          "the built-in font size is not 11 points");
    check(preferences.tabWidthSpaces() == 4,
          "the built-in tab width is not four spaces");
    for (std::size_t level = 0;
         level < preferences.defaultListLayout().levels.size(); ++level) {
        check(preferences.defaultListLayout().levels[level]
                      .bullet_indent_spaces ==
                  static_cast<std::int32_t>(level * 4U),
              "the built-in list bullet positions are incorrect");
        check(preferences.defaultListLayout().levels[level]
                      .text_indent_spaces == 2,
              "the built-in list text gap is not two spaces");
    }
}

void testValidationAndNormalization() {
    EditorPreferences preferences;
    check(preferences.setDefaultFontFamily(QStringLiteral("  Noto Sans  ")),
          "a valid font family was rejected");
    check(preferences.defaultFontFamily() == QStringLiteral("Noto Sans"),
          "font-family surrounding whitespace was not normalized");
    check(!preferences.setDefaultFontFamily(QStringLiteral(" \n ")),
          "an empty font family was accepted");
    check(preferences.defaultFontFamily() == QStringLiteral("Noto Sans"),
          "a rejected font family changed the preference");

    check(preferences.setDefaultFontPointSize(10.24),
          "a valid point size was rejected");
    check(preferences.defaultFontPointSize() == 10.0,
          "point size was not normalized to half-point precision");
    check(preferences.setDefaultFontPointSize(10.26),
          "a second valid point size was rejected");
    check(preferences.defaultFontPointSize() == 10.5,
          "point size half-point rounding is incorrect");
    check(!preferences.setDefaultFontPointSize(
              std::numeric_limits<double>::quiet_NaN()),
          "NaN point size was accepted");
    check(!preferences.setDefaultFontPointSize(0.5),
          "an undersized font was accepted");
    check(!preferences.setDefaultFontPointSize(400.5),
          "an oversized font was accepted");

    check(preferences.setTabWidthSpaces(8),
          "a valid tab width was rejected");
    check(!preferences.setTabWidthSpaces(0),
          "a zero tab width was accepted");
    check(!preferences.setTabWidthSpaces(33),
          "an oversized tab width was accepted");
    check(preferences.tabWidthSpaces() == 8,
          "a rejected tab width changed the preference");

    auto layout = preferences.defaultListLayout();
    layout.levels[0].bullet_indent_spaces = 7;
    layout.levels[0].text_indent_spaces = 3;
    check(preferences.setDefaultListLayout(layout),
          "valid list defaults were rejected");
    auto invalid = layout;
    invalid.levels[4].text_indent_spaces =
        docxstudio::core::kMaximumListTextIndentSpaces + 1;
    check(!preferences.setDefaultListLayout(invalid),
          "invalid list defaults were accepted");
    check(preferences.defaultListLayout() == layout,
          "rejected list defaults changed the preference");
}

void testRoundTrip(const QTemporaryDir& temporary) {
    auto settings = settingsFor(temporary, QStringLiteral("round-trip.ini"));
    EditorPreferences saved;
    check(saved.setDefaultFontFamily(QStringLiteral("Liberation Serif")),
          "could not set round-trip font family");
    check(saved.setDefaultFontPointSize(12.5),
          "could not set round-trip font size");
    check(saved.setTabWidthSpaces(6),
          "could not set round-trip tab width");
    auto listDefaults = saved.defaultListLayout();
    listDefaults.levels[0] = {3, 4};
    listDefaults.levels[9] = {47, 8};
    check(saved.setDefaultListLayout(listDefaults),
          "could not set round-trip list defaults");
    QString error;
    check(saved.save(settings, &error), "could not save valid preferences");
    check(error.isEmpty(), "successful preference save returned an error");

    auto reopened = settingsFor(temporary, QStringLiteral("round-trip.ini"));
    check(EditorPreferences::load(reopened) == saved,
          "preferences did not survive a settings round trip");
}

void testCorruptValuesFallBackIndependently(const QTemporaryDir& temporary) {
    auto settings = settingsFor(temporary, QStringLiteral("corrupt.ini"));
    settings.setValue(QStringLiteral("editor/defaultFontFamily"),
                      QStringLiteral("Noto Serif"));
    settings.setValue(QStringLiteral("editor/defaultFontPointSize"),
                      QStringLiteral("not-a-number"));
    settings.setValue(QStringLiteral("editor/tabWidthSpaces"), 1000);
    settings.setValue(
        QStringLiteral("editor/listDefaults/level1/bulletPositionSpaces"), 9);
    settings.setValue(QStringLiteral("editor/listDefaults/level1/textGapSpaces"),
                      5);
    settings.setValue(
        QStringLiteral("editor/listDefaults/level2/bulletPositionSpaces"),
        1000);
    settings.setValue(QStringLiteral("editor/listDefaults/level2/textGapSpaces"),
                      -1);
    settings.sync();

    const auto preferences = EditorPreferences::load(settings);
    check(preferences.defaultFontFamily() == QStringLiteral("Noto Serif"),
          "one invalid value discarded a valid independent preference");
    check(preferences.defaultFontPointSize() == 11.0,
          "invalid persisted font size did not fall back");
    check(preferences.tabWidthSpaces() == 4,
          "invalid persisted tab width did not fall back");
    check(preferences.defaultListLayout().levels[0]
                  .bullet_indent_spaces == 9 &&
              preferences.defaultListLayout().levels[0]
                  .text_indent_spaces == 5,
          "valid persisted list defaults were not loaded");
    check(preferences.defaultListLayout().levels[1]
                  .bullet_indent_spaces == 4 &&
              preferences.defaultListLayout().levels[1]
                  .text_indent_spaces == 2,
          "invalid persisted list defaults did not fall back independently");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "could not create temporary settings directory");

    testBuiltInDefaults(temporary);
    testValidationAndNormalization();
    testRoundTrip(temporary);
    testCorruptValuesFallBackIndependently(temporary);

    std::cout << "editor preference tests passed\n";
    return 0;
}

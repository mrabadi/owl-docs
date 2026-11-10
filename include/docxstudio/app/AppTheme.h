#pragma once

#include <QColor>

class QWidget;

namespace docxstudio::app {

QColor workspaceColor();
QColor borderColor();
QColor accentColor();
QColor mutedTextColor();
void applyAppTheme(QWidget* root);

}  // namespace docxstudio::app

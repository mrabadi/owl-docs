#pragma once

#include <QColor>
#include <QWidget>
#include <memory>

namespace docxstudio::app {

class ColorPaletteWidget final : public QWidget {
    Q_OBJECT
public:
    enum class Usage { Text, Highlight };
    explicit ColorPaletteWidget(Usage usage, QWidget* parent = nullptr);
    ~ColorPaletteWidget() override;
    void setCurrentColor(const QColor& color);
    void rememberColor(const QColor& color);

signals:
    void colorSelected(const QColor& color);
    void defaultRequested();
    void customColorRequested();

private:
    void refreshRecentColors();
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace docxstudio::app

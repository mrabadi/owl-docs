#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QSpinBox;
class QTableWidget;

namespace docxstudio::app {

class TableInsertDialog final : public QDialog {
public:
    explicit TableInsertDialog(QWidget* parent = nullptr);
    int rowCount() const;
    int columnCount() const;
    bool hasHeaderRow() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void chooseDimensions(int rows, int columns);
    QTableWidget* grid_{};
    QSpinBox* rows_{};
    QSpinBox* columns_{};
    QCheckBox* header_{};
    QLabel* dimensions_{};
};

}

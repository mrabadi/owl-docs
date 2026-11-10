#include "docxstudio/app/TableInsertDialog.h"
#include "docxstudio/core/document.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace docxstudio::app {

TableInsertDialog::TableInsertDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("insertTableDialog"));
    setWindowTitle(tr("Insert Table"));
    setModal(true);
    auto* outer = new QVBoxLayout(this);
    dimensions_ = new QLabel(this);
    dimensions_->setObjectName(QStringLiteral("insertTable.dimensions"));
    outer->addWidget(dimensions_);
    grid_ = new QTableWidget(8, 10, this);
    grid_->setObjectName(QStringLiteral("insertTable.grid"));
    grid_->setAccessibleName(tr("Table size grid. Use arrow keys to choose rows and columns, then press Enter."));
    grid_->setMouseTracking(true);
    grid_->viewport()->setMouseTracking(true);
    grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid_->setSelectionMode(QAbstractItemView::MultiSelection);
    grid_->horizontalHeader()->hide();
    grid_->verticalHeader()->hide();
    grid_->horizontalHeader()->setDefaultSectionSize(27);
    grid_->verticalHeader()->setDefaultSectionSize(27);
    grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid_->setFixedSize(272, 218);
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 10; ++column) {
            auto* item = new QTableWidgetItem;
            const auto label = tr("%1 columns by %2 rows").arg(column + 1).arg(row + 1);
            item->setToolTip(label);
            item->setData(Qt::AccessibleTextRole, label);
            grid_->setItem(row, column, item);
        }
    }
    outer->addWidget(grid_);
    auto* form = new QFormLayout;
    rows_ = new QSpinBox(this);
    rows_->setObjectName(QStringLiteral("insertTable.rows"));
    rows_->setAccessibleName(tr("Table rows"));
    rows_->setRange(1, static_cast<int>(core::Table::maximum_rows));
    columns_ = new QSpinBox(this);
    columns_->setObjectName(QStringLiteral("insertTable.columns"));
    columns_->setAccessibleName(tr("Table columns"));
    columns_->setRange(1, static_cast<int>(core::Table::maximum_columns));
    form->addRow(tr("&Rows:"), rows_);
    form->addRow(tr("&Columns:"), columns_);
    header_ = new QCheckBox(tr("&Header row"), this);
    header_->setObjectName(QStringLiteral("insertTable.header"));
    header_->setChecked(true);
    form->addRow(QString(), header_);
    outer->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("insertTable.buttons"));
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Insert"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(grid_, &QTableWidget::cellEntered, this, [this](int row, int column) {
        chooseDimensions(row + 1, column + 1);
    });
    connect(grid_, &QTableWidget::cellClicked, this, [this](int row, int column) {
        chooseDimensions(row + 1, column + 1);
        accept();
    });
    connect(rows_, &QSpinBox::valueChanged, this, [this](int value) { chooseDimensions(value, columnCount()); });
    connect(columns_, &QSpinBox::valueChanged, this, [this](int value) { chooseDimensions(rowCount(), value); });
    grid_->installEventFilter(this);
    chooseDimensions(2, 2);
    grid_->setFocus();
}

int TableInsertDialog::rowCount() const { return rows_->value(); }
int TableInsertDialog::columnCount() const { return columns_->value(); }
bool TableInsertDialog::hasHeaderRow() const { return header_->isChecked(); }

void TableInsertDialog::chooseDimensions(int rows, int columns) {
    const QSignalBlocker rowsBlock(rows_);
    const QSignalBlocker columnsBlock(columns_);
    rows_->setValue(rows);
    columns_->setValue(columns);
    dimensions_->setText(tr("%1 × %2 table").arg(columnCount()).arg(rowCount()));
    grid_->clearSelection();
    grid_->setRangeSelected(QTableWidgetSelectionRange(0, 0,
        std::min(rowCount(), 8) - 1, std::min(columnCount(), 10) - 1), true);
}

bool TableInsertDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == grid_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { accept(); return true; }
        if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down) {
            chooseDimensions(std::clamp(rowCount() + (key == Qt::Key_Down ? 1 : key == Qt::Key_Up ? -1 : 0),1,8),
                std::clamp(columnCount() + (key == Qt::Key_Right ? 1 : key == Qt::Key_Left ? -1 : 0),1,10));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

}

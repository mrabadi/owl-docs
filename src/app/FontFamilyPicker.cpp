#include "docxstudio/app/FontFamilyPicker.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QIcon>
#include <QModelIndex>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

namespace {

class FontFamilyNameDelegate final : public QStyledItemDelegate {
public:
    explicit FontFamilyNameDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override {
        const QFont uiFont = option->font;
        QStyledItemDelegate::initStyleOption(option, index);

        // QFontComboBox's model exposes the canonical installed family name as
        // DisplayRole. Keep that label verbatim (without Qt's native-script
        // sample suffix), but preview it using the family it represents.
        const QString family = index.data(Qt::DisplayRole).toString();
        option->text = family;
        option->icon = QIcon();
        option->font = uiFont;
        option->fontMetrics = QFontMetrics(uiFont);
        // A script-specific or symbol family may not contain Latin glyphs (and
        // some legacy fonts map ASCII code points to non-Latin shapes). In that
        // case, retain the UI font so the canonical family name stays readable
        // in English. Families advertising Latin support get a true live
        // preview.
        const auto writingSystems = QFontDatabase::writingSystems(family);
        if (!family.isEmpty() &&
            writingSystems.contains(QFontDatabase::Latin)) {
            QFont previewFont = uiFont;
            previewFont.setFamily(family);
            option->font = previewFont;
            option->fontMetrics = QFontMetrics(previewFont);
        }
    }
};

}  // namespace

namespace docxstudio::app {

FontFamilyPicker::FontFamilyPicker(QWidget* parent) : QFontComboBox(parent) {
    // Replacing QFontComboBox's specialized delegate is intentional. It adds a
    // writing-system sample to many families. Our delegate shows only the
    // installed family name while still drawing that name in its own typeface.
    // The model itself is untouched, so Arabic, CJK, Indic, symbol, and every
    // other installed family remain selectable.
    auto* delegate = new FontFamilyNameDelegate(this);
    delegate->setObjectName(QStringLiteral("fontFamilyNameOnlyDelegate"));
    setItemDelegate(delegate);

    // DOCX names are not limited to locally installed font-family records.
    // In particular, Linux Fontconfig resolves Helvetica to a metric-compatible
    // substitute even though QFontDatabase commonly exposes only the resolved
    // family. Keep the requested document name visible and editable so a
    // Helvetica document does not misleadingly turn into "Nimbus Sans" merely
    // because that is the local rendering face.
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
}

}  // namespace docxstudio::app

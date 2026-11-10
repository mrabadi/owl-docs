#include "docxstudio/app/EquationEditorDialog.h"
#include "docxstudio/app/MathLayout.h"
#include "docxstudio/math/latex_parser.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace docxstudio::app {
namespace {
class EquationPreview final : public QWidget {
public:
    explicit EquationPreview(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("insertEquation.preview"));
        setAccessibleName(tr("Live equation preview"));
        setMinimumHeight(110);
    }
    std::unique_ptr<MathLayout> math;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::white);
        if (!math) return;
        const auto metrics = math->metrics();
        const qreal scale = std::min<qreal>({1.0, (width() - 24.0) / std::max<qreal>(1.0, metrics.width),
                                            (height() - 24.0) / std::max<qreal>(1.0, metrics.height())});
        painter.translate((width() - metrics.width * scale) / 2.0,
                          (height() - metrics.height() * scale) / 2.0);
        painter.scale(scale, scale);
        math->draw(painter, QPointF(0, metrics.ascent), Qt::black);
    }
};
}

EquationEditorDialog::EquationEditorDialog(const QString& source, bool display, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("insertEquationDialog"));
    setWindowTitle(tr("LaTeX Equation"));
    setModal(true);
    resize(620, 440);
    auto* layout = new QVBoxLayout(this);
    auto* label = new QLabel(tr("LaTeX source"), this);
    layout->addWidget(label);
    source_ = new QPlainTextEdit(source, this);
    source_->setObjectName(QStringLiteral("insertEquation.latex"));
    source_->setAccessibleName(tr("LaTeX source"));
    source_->setPlaceholderText(QStringLiteral("\\frac{a}{b} + x^2"));
    source_->setMinimumHeight(110);
    label->setBuddy(source_);
    layout->addWidget(source_);
    auto* templates = new QComboBox(this);
    templates->setObjectName(QStringLiteral("insertEquation.templates"));
    templates->setAccessibleName(tr("Equation templates"));
    templates->addItem(tr("Insert a template…"));
    templates->addItem(tr("Fraction"), QStringLiteral("\\frac{a}{b}"));
    templates->addItem(tr("Square root"), QStringLiteral("\\sqrt{x^2 + y^2}"));
    templates->addItem(tr("Summation"), QStringLiteral("\\sum_{i=1}^{n} i"));
    templates->addItem(tr("Matrix"), QStringLiteral("\\begin{pmatrix}a & b \\\\ c & d\\end{pmatrix}"));
    layout->addWidget(templates);
    preview_ = new EquationPreview(this);
    layout->addWidget(preview_);
    display_ = new QCheckBox(tr("Display equation on its own centered line"), this);
    display_->setObjectName(QStringLiteral("insertEquation.display"));
    display_->setChecked(display);
    layout->addWidget(display_);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("insertEquation.error"));
    error_->setAccessibleName(tr("Equation validation"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #a44432"));
    layout->addWidget(error_);
    auto* help = new QLabel(tr("Supports fractions, roots, scripts, Greek symbols, matrices and common math commands. No TeX installation is needed."), this);
    help->setWordWrap(true);
    layout->addWidget(help);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->setObjectName(QStringLiteral("insertEquation.buttons"));
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &EquationEditorDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(source_, &QPlainTextEdit::textChanged, this, [this] { validate(); });
    connect(templates, &QComboBox::activated, this, [this, templates](int index) {
        if (index > 0) { source_->insertPlainText(templates->itemData(index).toString()); source_->setFocus(); }
        templates->setCurrentIndex(0);
    });
    validate();
    source_->setFocus();
}

bool EquationEditorDialog::displayEquation() const { return display_->isChecked(); }

void EquationEditorDialog::validate() {
    auto* preview = static_cast<EquationPreview*>(preview_);
    math::ParseLimits limits;
    limits.max_input_bytes = 8U * 1024U;
    limits.max_depth = 32;
    limits.max_nodes = 2048;
    limits.max_matrix_rows = 32;
    limits.max_matrix_columns = 32;
    const auto parsed = math::parseLatex(source_->toPlainText().toStdString(), limits);
    canonical_.clear();
    preview->math.reset();
    if (parsed) {
        canonical_ = QString::fromStdString(math::toCanonicalLatex(parsed.value()));
        QFont mathFont(QStringLiteral("Carlito"));
        mathFont.setPixelSize(24);
        preview->math = std::make_unique<MathLayout>(parsed.value(), mathFont);
        error_->clear();
    } else {
        error_->setText(tr("At byte %1: %2").arg(parsed.error().byte_offset)
                           .arg(QString::fromStdString(parsed.error().message)));
    }
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(parsed.hasValue());
    preview->update();
}

void EquationEditorDialog::accept() {
    validate();
    if (!canonical_.isEmpty()) QDialog::accept();
}
}

#pragma once
#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QCheckBox;
class QLabel;
class QDialogButtonBox;

namespace docxstudio::app {
class EquationEditorDialog final : public QDialog {
public:
    explicit EquationEditorDialog(const QString& source = {}, bool display = false,
                                  QWidget* parent = nullptr);
    QString canonicalLatex() const { return canonical_; }
    bool displayEquation() const;
    void accept() override;
private:
    void validate();
    QPlainTextEdit* source_{};
    QCheckBox* display_{};
    QLabel* error_{};
    QDialogButtonBox* buttons_{};
    QWidget* preview_{};
    QString canonical_;
};
}

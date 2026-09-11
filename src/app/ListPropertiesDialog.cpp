#include "docxstudio/app/ListPropertiesDialog.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace docxstudio::app {

ListPropertiesDialog::ListPropertiesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("listPropertiesDialog"));
    setWindowTitle(tr("List Properties"));
    setModal(true);
    setMinimumWidth(520);

    auto* outer = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        tr("Set the bullet position and the text gap after the bullet for each "
           "level. Values are measured in space equivalents for the current "
           "font. Use as Defaults applies these settings to new bulleted and "
           "numbered lists."),
        this);
    explanation->setObjectName(QStringLiteral("listProperties.explanation"));
    explanation->setWordWrap(true);
    outer->addWidget(explanation);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(5);

    auto* levelHeading = new QLabel(tr("Level"), this);
    auto* bulletHeading = new QLabel(tr("Bullet position"), this);
    auto* textHeading = new QLabel(tr("Text after bullet"), this);
    levelHeading->setObjectName(QStringLiteral("listProperties.levelHeading"));
    bulletHeading->setObjectName(QStringLiteral("listProperties.bulletHeading"));
    textHeading->setObjectName(QStringLiteral("listProperties.textGapHeading"));
    grid->addWidget(levelHeading, 0, 0);
    grid->addWidget(bulletHeading, 0, 1);
    grid->addWidget(textHeading, 0, 2);

    for (int index = 0; index < kLevelCount; ++index) {
        const int level = index + 1;
        auto* levelLabel = new QLabel(tr("Level %1").arg(level), this);
        levelLabel->setObjectName(
            QStringLiteral("listProperties.level.%1").arg(level));
        grid->addWidget(levelLabel, level, 0);

        auto* bulletPosition = new QSpinBox(this);
        bulletPosition->setObjectName(
            QStringLiteral("listProperties.bulletPosition.level%1").arg(level));
        bulletPosition->setRange(kMinimumBulletPositionSpaces,
                                 kMaximumBulletPositionSpaces);
        bulletPosition->setSuffix(tr(" spaces"));
        bulletPosition->setAccessibleName(
            tr("Level %1 bullet position in spaces").arg(level));
        bulletPositions_[static_cast<std::size_t>(index)] = bulletPosition;
        grid->addWidget(bulletPosition, level, 1);

        auto* textGap = new QSpinBox(this);
        textGap->setObjectName(
            QStringLiteral("listProperties.textGap.level%1").arg(level));
        textGap->setRange(kMinimumTextGapSpaces, kMaximumTextGapSpaces);
        textGap->setSuffix(tr(" spaces"));
        textGap->setAccessibleName(
            tr("Level %1 text gap after bullet in spaces").arg(level));
        textGaps_[static_cast<std::size_t>(index)] = textGap;
        grid->addWidget(textGap, level, 2);
    }
    outer->addLayout(grid);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Reset | QDialogButtonBox::Ok |
            QDialogButtonBox::Cancel,
        Qt::Horizontal, this);
    buttons->setObjectName(QStringLiteral("listProperties.buttons"));
    if (auto* reset = buttons->button(QDialogButtonBox::Reset)) {
        reset->setObjectName(QStringLiteral("listProperties.resetDefaults"));
        reset->setText(tr("Reset Defaults"));
        connect(reset, &QPushButton::clicked, this,
                &ListPropertiesDialog::resetToDefaults);
    }
    auto* useAsDefaults = buttons->addButton(
        tr("Use as Defaults"), QDialogButtonBox::ActionRole);
    useAsDefaults->setObjectName(
        QStringLiteral("listProperties.useAsDefaults"));
    useAsDefaults->setAccessibleName(
        tr("Use these settings as the defaults for new lists"));
    useAsDefaults->setToolTip(
        tr("Apply these settings to this list and use them for new bulleted "
           "and numbered lists."));
    connect(useAsDefaults, &QPushButton::clicked, this, [this] {
        useAsDefaultsRequested_ = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this,
            &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this,
            &QDialog::reject);
    outer->addWidget(buttons);

    resetToDefaults();
}

ListProperties ListPropertiesDialog::defaultProperties() noexcept {
    ListProperties result{};
    for (int index = 0; index < kLevelCount; ++index) {
        auto& level = result[static_cast<std::size_t>(index)];
        level.bulletPositionSpaces = index * 4;
        level.textGapAfterBulletSpaces = 2;
    }
    return result;
}

ListProperties ListPropertiesDialog::properties() const noexcept {
    ListProperties result{};
    for (int index = 0; index < kLevelCount; ++index) {
        const auto arrayIndex = static_cast<std::size_t>(index);
        result[arrayIndex].bulletPositionSpaces =
            bulletPositions_[arrayIndex]->value();
        result[arrayIndex].textGapAfterBulletSpaces =
            textGaps_[arrayIndex]->value();
    }
    return result;
}

void ListPropertiesDialog::setProperties(
    const ListProperties& properties) noexcept {
    for (int index = 0; index < kLevelCount; ++index) {
        const auto arrayIndex = static_cast<std::size_t>(index);
        bulletPositions_[arrayIndex]->setValue(std::clamp(
            properties[arrayIndex].bulletPositionSpaces,
            kMinimumBulletPositionSpaces, kMaximumBulletPositionSpaces));
        textGaps_[arrayIndex]->setValue(std::clamp(
            properties[arrayIndex].textGapAfterBulletSpaces,
            kMinimumTextGapSpaces, kMaximumTextGapSpaces));
    }
}

void ListPropertiesDialog::setDefaultProperties(
    const ListProperties& properties) noexcept {
    resetProperties_ = properties;
}

void ListPropertiesDialog::resetToDefaults() noexcept {
    setProperties(resetProperties_);
}

}  // namespace docxstudio::app

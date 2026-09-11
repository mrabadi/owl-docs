#pragma once

#include <QDialog>

#include <array>

class QSpinBox;

namespace docxstudio::app {

struct ListLevelProperties {
    int bulletPositionSpaces{};
    int textGapAfterBulletSpaces{2};

    bool operator==(const ListLevelProperties&) const = default;
};

using ListProperties = std::array<ListLevelProperties, 10>;

// Edits the layout of one list. Values are expressed in space equivalents so
// the document layer can turn them into font-relative tab stops at layout time.
class ListPropertiesDialog final : public QDialog {
    Q_OBJECT

public:
    static constexpr int kLevelCount = 10;
    static constexpr int kMinimumBulletPositionSpaces = 0;
    static constexpr int kMaximumBulletPositionSpaces = 200;
    static constexpr int kMinimumTextGapSpaces = 0;
    static constexpr int kMaximumTextGapSpaces = 32;

    explicit ListPropertiesDialog(QWidget* parent = nullptr);

    [[nodiscard]] static ListProperties defaultProperties() noexcept;
    [[nodiscard]] ListProperties properties() const noexcept;
    void setProperties(const ListProperties& properties) noexcept;
    void setDefaultProperties(const ListProperties& properties) noexcept;
    [[nodiscard]] bool useAsDefaultsRequested() const noexcept {
        return useAsDefaultsRequested_;
    }

private:
    void resetToDefaults() noexcept;

    std::array<QSpinBox*, kLevelCount> bulletPositions_{};
    std::array<QSpinBox*, kLevelCount> textGaps_{};
    ListProperties resetProperties_{defaultProperties()};
    bool useAsDefaultsRequested_{false};
};

}  // namespace docxstudio::app

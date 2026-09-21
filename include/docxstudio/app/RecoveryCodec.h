#pragma once

#include "docxstudio/core/document.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace docxstudio::app {

// Stored in points to match the editor's layout boundary without introducing
// Qt types into either this codec or the document core.
struct RecoveryPageLayout {
    double width_points{612.0};
    double height_points{792.0};
    double margin_top_points{72.0};
    double margin_right_points{72.0};
    double margin_bottom_points{72.0};
    double margin_left_points{72.0};

    auto operator<=>(const RecoveryPageLayout&) const = default;
};

struct RecoveryDocument {
    RecoveryDocument(core::Document sourceDocument,
                     RecoveryPageLayout sourcePage)
        : document(std::move(sourceDocument)), page(sourcePage) {}

    core::Document document;
    RecoveryPageLayout page;
};

class RecoveryCodec final {
public:
    static constexpr int currentVersion = 12;

    [[nodiscard]] static std::optional<std::string> encode(
        const RecoveryDocument& recovery, std::string& error);
    [[nodiscard]] static std::optional<RecoveryDocument> decode(
        std::string_view payload, std::string& error);
};

}  // namespace docxstudio::app

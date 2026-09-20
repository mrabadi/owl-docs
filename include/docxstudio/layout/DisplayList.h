#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace docxstudio::layout {

struct Color {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t alpha{255};
};

struct Rect {
    double x{};
    double y{};
    double width{};
    double height{};
};

struct TextStyle {
    std::string family{"Helvetica"};
    double point_size{11.0};
    int weight{400};
    bool italic{false};
    bool underline{false};
    bool strike{false};
    Color foreground{0, 0, 0, 255};
    Color highlight{0, 0, 0, 0};
};

struct FillRect {
    Rect bounds;
    Color color;
};

struct StrokeLine {
    double x1{};
    double y1{};
    double x2{};
    double y2{};
    double width{1.0};
    Color color;
};

struct DrawText {
    double x{};
    double baseline_y{};
    std::u16string text;
    TextStyle style;
};

struct DrawImage {
    Rect bounds;
    std::string asset_id;
};

using DisplayCommand = std::variant<FillRect, StrokeLine, DrawText, DrawImage>;

struct PageDisplayList {
    double width_points{612.0};
    double height_points{792.0};
    std::vector<DisplayCommand> commands;
};

struct DocumentDisplayList {
    std::uint64_t revision{};
    std::vector<PageDisplayList> pages;
};

}  // namespace docxstudio::layout

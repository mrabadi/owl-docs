#pragma once

#include <pugixml.hpp>

#include <cstdint>

namespace docxstudio::xml {

enum class ComplexityStatus {
    ok,
    depth_exceeded,
    node_count_exceeded,
};

struct ComplexityResult {
    ComplexityStatus status{ComplexityStatus::ok};
    std::uint64_t node_count{0};
    std::uint64_t maximum_depth{0};

    [[nodiscard]] bool accepted() const noexcept {
        return status == ComplexityStatus::ok;
    }
};

// Inspects an already parsed pugixml tree without recursion. The document node
// has depth zero, so the document element has depth one. All node kinds count
// toward max_nodes; this includes text, comments, and processing instructions.
[[nodiscard]] inline ComplexityResult inspectComplexity(
    const pugi::xml_node& tree,
    std::uint64_t max_depth,
    std::uint64_t max_nodes) noexcept {
    ComplexityResult result;
    if (!tree) {
        return result;
    }

    pugi::xml_node current = tree;
    std::uint64_t depth = 0;
    while (current) {
        ++result.node_count;
        result.maximum_depth = result.maximum_depth < depth
                                   ? depth
                                   : result.maximum_depth;
        if (result.node_count > max_nodes) {
            result.status = ComplexityStatus::node_count_exceeded;
            return result;
        }
        if (depth > max_depth) {
            result.status = ComplexityStatus::depth_exceeded;
            return result;
        }

        if (const pugi::xml_node child = current.first_child()) {
            current = child;
            ++depth;
            continue;
        }

        while (current != tree && !current.next_sibling()) {
            current = current.parent();
            --depth;
        }
        if (current == tree) {
            break;
        }
        current = current.next_sibling();
    }
    return result;
}

}  // namespace docxstudio::xml

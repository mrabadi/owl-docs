#pragma once

#include "docxstudio/layout/DisplayList.h"

#include <QImage>
#include <QPointF>

#include <functional>

class QPainter;

namespace docxstudio::app {

class QtDisplayListRenderer final {
public:
    using ImageResolver = std::function<QImage(const std::string& assetId)>;

    explicit QtDisplayListRenderer(ImageResolver resolver = {});
    void paint(QPainter& painter,
               const layout::PageDisplayList& page,
               const QPointF& origin = {},
               double scale = 1.0) const;

private:
    ImageResolver imageResolver_;
};

}  // namespace docxstudio::app

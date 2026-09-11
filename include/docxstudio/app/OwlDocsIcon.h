#pragma once

#include <QIcon>

namespace docxstudio::app {

// Returns the packaged Owl Docs SVG when the platform can render it, with a
// code-native rendering of the same geometric owl for minimal Qt installs.
QIcon owlDocsApplicationIcon();

}  // namespace docxstudio::app

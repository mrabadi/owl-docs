#pragma once

#include <QFontComboBox>

namespace docxstudio::app {

// QFontComboBox's built-in delegate appends a writing-system sample to many
// installed families (for example Devanagari after "Gargi"). That is useful
// in a font-browser demo, but noisy and surprising in a word processor. This
// picker keeps Qt's complete installed-family model while drawing only the
// family name in the typeface it represents when that family supports Latin,
// like a conventional document-editor font list. Script-specific family names
// stay readable in the UI font rather than turning into non-English glyphs.
class FontFamilyPicker final : public QFontComboBox {
public:
    explicit FontFamilyPicker(QWidget* parent = nullptr);
};

}  // namespace docxstudio::app

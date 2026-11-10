# Academic canvas and editing TDD evidence

Executed against the local checkout for OwlDocs 0.6.0. These checks cover the
shared screen/PDF layout and the native editor's academic controls. Core,
OOXML, recovery, Codex, and workspace evidence is recorded in the adjacent
TDD documents.

| Slice | RED observed | GREEN observed |
|---|---|---|
| Academic canvas and PDF | Focused PDF assertions exposed missing resolved note/TOC content | `academic_layout_tests` passed with note text, generated heading entries and cached page numbers in the exported PDF |
| Long footnotes | Continuation checks required all 90 lines and the final sentinel across pages | The focused layout test preserved every line without clipping or duplication |
| Competing notes | A long footnote displaced another note's first line from its anchor page | Layout test passed after reserving all new notes' first lines before allocating continuation lines |
| Dense note anchors | Nineteen note references dropped note content under the nominal footer cap | Layout test passed after splitting anchor lines to keep the body and note first lines on a page together |
| Note clipboard | `academic_ui_tests` failed `copy/paste flattened a note instead of creating an independent note reference` | Same target passed using semantic clipboard atoms with independent note/reference identities |
| Keyboard note editing | `academic_ui_tests` failed `F2 on a selected note did not open its editor` | Same target passed; editing a pasted note leaves its source note unchanged |
| TOC navigation | `academic_ui_tests` failed `clicking a TOC entry does not navigate to its heading` | Same target passed after hit testing linked TOC rows and navigating to their semantic targets |
| Long full-text references | `academic_layout_tests` failed `full-text references overflow page margins instead of wrapping their resolved text` | Same target passed after shaping resolved field text with display/semantic offset maps; all 120 fixture words remain in the PDF |

Academic insertion checks also verify that inserting a note is one undoable
transaction, caption/bibliography paragraphs retain their metadata, and citations
resolve from their target entries rather than stored display strings. F2 also
reopens selected citations and cross-references, with identity-preserving field
replacement. Invalid dialog input remains open and leaves the document unchanged.

Focused command:

```sh
cmake --build build --target academic_ui_tests academic_layout_tests equation_ui_tests -j2
ctest --test-dir build --output-on-failure -R '^(academic_ui_tests|academic_layout_tests|equation_ui_tests)$'
```

Footnote and TOC checks extract text from the actual exported PDFs using
`pdftotext -layout`; they exercise the same display list used by canvas and print.
The fixtures include a small page size to force continuation behavior.

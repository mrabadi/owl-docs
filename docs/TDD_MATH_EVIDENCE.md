# Equation overhaul TDD evidence

Executed against the local checkout during the 0.6 implementation.

| Slice | RED observed | GREEN observed |
|---|---|---|
| Text and font commands | Focused math test failed `Unknown LaTeX command: \\text` and `\\mathrm` | Standalone C++ math runner and CTest `math_tests` passed after parser/AST changes |
| Accents, product and spacing | Focused math test failed unknown `\\hat` and `\\prod` | Same standalone math test runner passed |
| Aligned and cases | Focused math test failed `Unsupported environment: aligned` / `cases` | Same runner passed, including two-column bounds |
| Native styled OMML | `math_roundtrip_tests` failed `native math source changed on reopen: \\text{when x is positive}` | Same CTest target passed after native OMML support |
| Native accents/environments | `math_roundtrip_tests` failed source roundtrip for hat/vector/bar/overline/underline | Same target passed for all text/font/accent/product/spacing/aligned/cases fixtures |
| Multiline equation dialog integration | `equation_ui_tests` failed `Insert Equation dialog is missing its LaTeX, display, or button controls` after test required multiline input | Same test passed, including identity-preserving edit, one-step undo/redo and centered display bounds |
| Source size boundary | Executed editor test failed `dialog accepts a source larger than the document equation limit` | Same test passed after dialog and canvas limits were aligned |
| Equation editor | New public editor header was absent in an executed syntax check; then executable checked correction-in-place and accepted canonical source | Standalone Qt executable and CTest `equation_editor_tests` passed |
| Double-click editing | `equation_ui_tests` failed `double-clicking the visible equation did not reopen its editor` | Same target passed after retaining page coordinates in paragraph hit testing; cancellation preserves the document |
| Contextual editing | `equation_ui_tests` failed `right-clicking an equation does not offer its contextual editor` | Same target passed with Edit Equation on the selected equation's context menu |
| XML-safe source | Direct parser regression accepted forbidden controls and U+FFFE/U+FFFF | Direct parser and configured `math_tests` passed after validating XML-compatible Unicode before parsing |
| Nested font commands | `math_layout_tests` failed `an inner mathrm command retained the outer bold font` | Same target passed after explicit font commands reset both weight and slant |

Parser command: `c++ -std=c++20 -Iinclude tests/math_tests.cpp src/math/ast.cpp src/math/latex_parser.cpp -o /tmp/owldocs-math-tests && /tmp/owldocs-math-tests`.

Configured commands: `cmake --build build --target math_roundtrip_tests equation_editor_tests -j2`; `ctest --test-dir build --output-on-failure -R '^(math_roundtrip_tests|equation_editor_tests)$'` (2/2 passed).

Build attempts that failed due to concurrent source/header edits were repaired and are not counted as meaningful RED phases. A missing Qt pkg-config path in the first direct editor syntax command was a setup failure; the corrected include-path check was rerun.

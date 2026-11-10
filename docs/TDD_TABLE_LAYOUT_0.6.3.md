# Native table layout TDD evidence for 0.6.3

## Explicit no-border import

A native table fixture with `w:tcBorders/w:top w:val="nil"` lost that
property during import, which made it indistinguishable from inherited style
borders. The new regression executed against the unchanged parser and failed:
`explicit native no-border became inherited and would restore the table-style border`.
The minimal reader change retains `nil`/`none` as an explicit zero-width
border. The standalone strict build and test then passed.

## Native authoring geometry and cell layout

The authoring regression first produced missing-API compile failures for the
new transport fields. After appending those fields atomically under the shared
build lock, the executable failed at runtime:
`authored column widths were replaced by equal automatic widths`.
The native writer/reader change then passed the full focused test.

It covers unequal column widths, automatic and minimum-height rows, repeated
header rows, explicit no-border, opaque colored borders with bounded weights,
and top/center/bottom cell alignment. The full native package reproduction
proof remains required. Invalid vector shapes, zero/oversized widths, invalid
border weights and malformed no-border state reject before replacing the
existing destination. Foreign exact row heights remain preserved as unsupported
body content rather than being silently converted to a minimum. A valid
external geometry change also fails reproduction against the original model.

The empty automatic native representation is compared byte-for-byte against
`tests/fixtures/owldocs-0.6.2-default-table.xml`, captured before the writer
changed. No package-proof mismatch is ignored.

## Desktop projection regressions

Actual MainWindow workflows executed before changing the converter and failed
independently because authored layout lacked a verified semantic registry and
because generic native layout existed only in the transient presentation
cache. The focused desktop test exercises actual Save As, native reopen,
exact semantic table/cell identities and metadata, another text edit and save,
and generic native import. It also uses the immutable actual desktop 0.6.2
table-picture fixture to guard its existing schema15 source proof and ordinary
text-edit save. The bare engine-to-desktop default-font projection difference
is pre-existing and is excluded from that backward-compatibility claim.

The desktop workflow is now GREEN at both normal and 150% scale. The first
post-implementation run exposed a test-fixture assumption that the table was
the first body block; the real document includes a leading paragraph. The test
now locates the table block while retaining all geometry, border and identity
assertions. That fixture repair is not counted as a product RED. The import-only
and old-0.6.2 compatibility selectors also passed independently.

Commands used for the final focused builds and executions:

```sh
flock /tmp/owldocs-build.lock cmake --build build-release \
  --target table_format_ooxml_tests table_format_workflow_tests -j2
build-release/table_format_ooxml_tests
QT_QPA_PLATFORM=offscreen build-release/table_format_workflow_tests
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 \
  build-release/table_format_workflow_tests
```

The new table metadata selects conditional recovery schema16 and its verified
native/semantic registry. Older documents without the metadata retain their
previous schema14/15 shape. Exact source reproduction still controls whether
an imported document may regenerate; no producer marker or mismatched member
is trusted or ignored. Imported automatic default grids retain their existing
automatic semantic state, avoiding an unnecessary metadata/registry migration.

The actual successful desktop document was then rendered with installed
LibreOffice 7.3.7.2 and a private temporary profile. Its one-page Letter PDF
shows 90/180 pt columns, a 45 pt minimum header row, bottom-aligned `Edited One`,
an absent top edge and the forest 1.5 pt cell boundary. Native XML, extracted
PDF text and the raster were checked; no overlap or clipping was found. The
outer sandbox prevented LibreOffice startup, so the same scoped test command
ran with approved host execution; no application installation or user document
was involved. Permanent synthetic fixture/PDF/raster/record copies are in
`output/ui-review/0.6.3/native-table/`.

The related complete native suites also passed together: `ooxml_tests`,
`table_image_ooxml_tests`, `legacy_style_profile_tests` and
`table_format_ooxml_tests` (4/4, zero failures). The final release suite subsequently passed all 79 tests, including the
legacy desktop workflows, after the Canvas APIs finished linking. The
intermediate missing-symbol build was an integration state rather than a
native-parser failure.

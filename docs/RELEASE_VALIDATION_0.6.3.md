# OwlDocs 0.6.3 release validation

Validated on 2026-11-09 on Ubuntu 22.04.5 LTS with GCC 11.4.0, CMake 3.22.1
and Qt 6.2.4. This records the tested development-release scope; it does not
establish general Word compatibility.

## Release build and final tests

The completely clean strict Release build passed **227/227 steps**. A final
25% zoom interaction regression was then added and failed: a row resize target
consumed an ordinary cell click. The minimal Canvas correction caps the row
hit tolerance by one quarter of the row height. This was an explicit
post-clean repair, followed by a **49/49-step strict build**, the focused
low-zoom GREEN run and the entire final test suite. The final source therefore
includes this documented repair after the initial clean build.

The final configured CTest run passed **79/79 tests, zero failures, in 45.78
seconds**. All production/API/test sources were frozen again before those
final executions. The fresh application reports `OwlDocs 0.6.3`.

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DDOCXSTUDIO_STRICT_WARNINGS=ON \
  -DOWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE=ON \
  -DOWL_DOCS_CORPUS_MANIFEST=/tmp/owl-docs-public-corpus-060/manifest.json
flock /tmp/owldocs-build.lock cmake --build build-release --clean-first -j2
# The documented Canvas repair then received a strict all-target rebuild.
ctest --test-dir build-release --output-on-failure
```

Build and regression records:
`/tmp/owldocs-release-063-clean.log`,
`/tmp/owldocs-release-063-post-clean.log`,
`/tmp/owldocs-lowzoom-cell-red.log`,
`/tmp/owldocs-lowzoom-cell-green.log` and
`/tmp/owldocs-release-063-ctest.log`.

The fresh pinned public LibreOffice corpus opened **100/100** files and wrote
**100/100 byte-identical unchanged copies**. The fixture revision is
`2776654bbb109af114a44fe00ec9771e4ad2d99d`, with pinned Git blob identities and
SHA-256 digests. This is preservation evidence, not arbitrary edit/layout
coverage. CTest's JUnit output truncated the large corpus JSON at 1024 bytes,
which initially prevented the recording script from parsing that report;
the normal complete 79-test run had passed. A direct repeat of the cheap
corpus runner supplied the complete machine-readable record. This was a
record-assembly error, not a product/test failure or a reason to change tests.
The full report is `/tmp/owldocs-corpus-063.json`.

## Table transport and native rendering

The native and actual MainWindow workflows cover Save As, reopen, exact
semantic table/cell identity, widths, minimum row heights, explicit no-border,
solid color/weight and vertical alignment, followed by another text edit and
save/reopen. The immutable actual 0.6.2 desktop table-picture fixture still
saves an ordinary cell-text edit without conversion, retaining its schema 15
identity and original PNG/JPEG payloads. Empty automatic table output remains
byte-identical to the frozen 0.6.2 XML fixture. Full package reproduction is
still mandatory; unsupported exact row heights remain preserved/view-only.

A fresh synthetic document from the successful 0.6.3 implementation workflow
was rendered with LibreOffice 7.3.7.2 using a private temporary profile. Its
one-page Letter PDF retains 90/180 pt columns, a 45 pt minimum header row,
bottom-aligned `Edited One`, an absent top edge and a forest 1.5 pt boundary.
Native XML, extracted PDF text and the raster were inspected without
unresolved findings. Its DOCX SHA-256 is
`dc2860b2db8412d927b2a9340f8e19dd2ec52a3df149d4f7a6fe785b7adfd8ea`.
The fixture, PDF, raster and validation JSON are under
`output/ui-review/0.6.3/native-table/`.

Semantic recovery uses version 16 only for nonempty table geometry or cell
layout, version 15 for cell pictures alone and version 14 otherwise. SQLite
journals and existing settings/storage identities are unchanged. The model,
clipboard, native transport, Canvas interaction and ribbon TDD documents
record the separate executed RED/GREEN cycles and limits.

## Scaled UI and unchanged figure helper

All **12 fresh direct cases passed at 150% scale**, with private state/log paths:
table dragging, cell rendering/formatting, layout clipboard, native desktop
workflow, header/footer controls and editing, margins, workspace controls,
workspace, Ribbon and existing cell pictures. Eight Table/Header captures were
reviewed with no unresolved findings. Logs are under
`/tmp/owldocs-final-ui-063/`, and the visual record is
`output/ui-review/0.6.3/workspace/verification.json`.

The real Professional Excalidraw renderer/Codex preview-accept gate is
**carried forward from 0.6.2**, not claimed as a new 0.6.3 execution. All 73 helper
files were independently compared byte-for-byte with the accepted 0.6.2 package;
that unchanged-helper evidence is `/tmp/owldocs-helper-063-carried.json`.
The package checker repeats the comparison against both the source bundle and
preserved 0.6.2 package. The original gate passed with bubblewrap isolation
intact; its outer-sandbox restriction and scoped rerun remain in the 0.6.2
release evidence.

## Package verification and preserved artifacts

The artifact is `owl-docs_0.6.3_amd64.deb`. Final metadata, file sizes, hashes,
package-check results and upgrade simulation are recorded outside the package
in `dist/validation-0.6.3.json` and `dist/SHA256SUMS`, avoiding a self-referential
hash. Verification requires the freshly stripped main/parser executable bytes,
resolved runtime libraries, a packaged restricted-parser preflight, all 36
installed documentation files byte-for-byte, eight unchanged icon assets and
all 73 helper files. Upgrade resolution uses `apt-get --simulate` only; no
actual application installation, dependency installation or user-document
replacement is performed.

The 0.6.2 package retains SHA-256
`d51d4674adb647e8766c28aabd57ba92e1d5f7bcdf53ffa660dc99468ba52a2f`,
and its validation JSON retains
`0a90a66ad5495b7cc7ac356146e9fb5db83f740abaac4d2044a62099945ac53c`.
The 0.6.1 package remains available at SHA-256
`587e27c1c98281fddd8d9e73a5c50a70889d20491c0519339214a80a06461356`.

See `RELEASE_NOTES_0.6.3.md`, `COMPATIBILITY.md` and the installed TDD evidence
for the bounded table/story authoring subset and remaining compatibility work.

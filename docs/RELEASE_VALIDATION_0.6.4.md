# OwlDocs 0.6.4 validation

The single-row table-ribbon update passed the strict incremental Release build,
the complete test suite and scaled UI review. Earlier release artifacts and
evidence remain unchanged.

## Build and tests

- Release configuration uses `DOCXSTUDIO_STRICT_WARNINGS=ON` (warnings as
  errors). The focused UI build completed 35 steps, followed by a successful
  95-step all-target rebuild. This release does not claim a fresh clean build.
  Logs: `/tmp/owldocs-064-ui-build.log` and
  `/tmp/owldocs-064-release-build.log`.
- Full `ctest --test-dir build-release --output-on-failure` passed **79/79**
  tests, with zero failures, in **45.81 seconds**. JUnit and output are
  `/tmp/owldocs-064-ctest.xml` and `/tmp/owldocs-064-ctest.log`.
- The pinned 100-document preservation corpus opened all fixtures and made
  **100 byte-identical unchanged copies**. This verifies preservation of
  unchanged imported packages, rather than unrestricted editing compatibility.
  Corpus revision: `2776654bbb109af114a44fe00ec9771e4ad2d99d`;
  record: `/tmp/owldocs-corpus-064.json`.
- Six binaries passed again at **150% display scaling**: workspace controls,
  Ribbon, workspace, native table formatting workflow, table dragging and
  header/footer toolbar tests. Logs: `/tmp/owldocs-064-scaled/`.
- A real MainWindow integration test uses the dropdowns to apply both
  alignment axes, commit an exact selected-column width, and prepare then
  apply a whole-table border. Each operation increments the revision once
  and undoes in one step; selection synchronization does not create edits.
  Focused log: `/tmp/owldocs-064-dropdown-integration.log`.

The four verified RED/GREEN cycles and focused commands are recorded in
`TDD_TABLE_RIBBON_0.6.4.md`. Existing native save/reopen, legacy table import,
recovery, clipboard, Excalidraw transaction and Codex preview tests remain
part of the fresh full test run.

## Visual review

All **24 captures** passed independent review: four full workspace frames and
20 table dropdowns at **820×600** and **1280×860**, each at normal and 150%
scaling. The contextual table ribbon stays on one row; all visible buttons,
labels, chevrons, field values and nine alignment choices remain readable
without clipping. Detailed controls occupy their dropdowns. No unresolved
visual findings remain.

Captures and their verified hashes are under
`output/ui-review/0.6.4/table-ribbon/`; `verification.json` records the review.
The unit tests additionally exercise a 420-pixel ribbon, genuine width
overflow, and restoration of direct controls when widening the window.

## Package verification

The deliverable is `dist/owl-docs_0.6.4_amd64.deb`. Its exact hash, size and
completed package-check results are recorded outside the package in
`dist/validation-0.6.4.json` and `dist/SHA256SUMS`.

The package check verifies metadata, both fresh stripped executable byte
sequences, resolved runtime libraries, the packaged restricted-parser
preflight, all 39 installed documentation files, eight original icons and all
73 bundled helper files. The helper and icons are also compared byte-for-byte
against the preserved 0.6.2 and 0.6.3 packages. Earlier Debian artifacts and
validation records retain their original hashes.

The real Professional Excalidraw renderer gate is carried forward from 0.6.2;
it is not claimed as a new 0.6.4 execution. Its unchanged helper bytes are
checked again, while regular figure/Codex regressions pass in the fresh full
suite. An APT upgrade simulation checks dependency resolution without
installing software. The simulation result is appended to the external
package validation record.

The supported document/table subset is unchanged; see `COMPATIBILITY.md`.

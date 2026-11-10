# Save and recovery TDD evidence

The save work uses the requested RED → GREEN → REFACTOR workflow. Tests run against local fixtures; no live documents or services are required.

## Baseline

`ctest --test-dir build --output-on-failure -R '^(file_workflow_tests|app_import_tests|file_fingerprint_tests|ooxml_tests|recovery_tests)$'`

All five original suites passed (35.08 s).

## Verified cycles

- **Content conflicts:** added a deterministic timestamp-only change in `file_fingerprint_tests`. RED: `unchanged bytes with a changed timestamp caused a save conflict`. GREEN: compare size plus SHA-256, retaining timestamps for diagnostics. The focused test passes and still detects same-size changed bytes.
- **Full package coverage:** added `testRichRegenerationAssessment`. RED: the new public assessment API was absent (direct compile of `tests/ooxml_tests.cpp`). GREEN: the same package preparation drives real writes and a complete member-name/decompressed-byte comparison. Rich equation output is accepted; an extra opaque binary part produces a loss report. `ooxml_tests` passed (0.44 s).
- **Final conflict check:** added `testSaveCommitGuard`. RED: `save ignored a final destination conflict or changed its clean baseline`. GREEN: call the guard after candidate validation and before rename; a refusal leaves destination bytes and editor baseline unchanged. `ooxml_tests` passed subsequently.
- **Source recovery storage:** added `recoverySourcePersistence`. RED: the retained-source API was absent (direct compile of `tests/recovery_tests.cpp`). GREEN: transactional content-addressed package BLOBs and independent source context survive close/reopen, retain embedded NUL bytes, survive deletion of another journal sharing the package, and diagnose corrupt package bytes. A narrow harness executing that exact test function with `ChatStore.cpp`, QtCore and SQLite passed.
- **Context integrity:** extended the same source recovery test to corrupt the persisted mapping context. RED: `corrupted recovery mapping context was not diagnosed`. GREEN: SHA-256 protected context envelopes, independently verified from the package checksum. The same narrow executable passed.
- **Desktop checkpoints:** added `testCheckpointRetainsOriginalPackage`. RED: `checkpoint discarded original DOCX bytes or save context`. GREEN: checkpoints include the retained source and versioned dirty/style context. The focused desktop file workflow suite passed before extending the scenario to restore and save after deleting the source.
- **Document defaults and preserving conversion:** updated desktop acceptance tests to require no compatibility prompt for a changed insertion preference and to require a separate editable copy for unsupported originals. RED: `changing insertion preferences caused an unnecessary compatibility warning`. GREEN: source document defaults drive serialization; conversions preserve the source. `app_import_tests` passed (4.48 s), including authored equation/picture reopen/edit/save and explicit formatting/style transitions.
- **Committed save outcomes:** added Linux `save_outcome_tests`, wrapping only the external `fsync` syscall boundary. RED: `committed document was reported as failed after a directory sync error`. GREEN: a completed rename is saved, with a separate durability warning. Both imported patches and newly authored saves passed the same injected directory-sync failure.
- **Stable committed baseline:** extended save outcome tests to require exact precommit-validated bytes from new saves. RED: the new result field was absent (direct compile). The GUI consumes those bytes instead of reopening a potentially replaced pathname; conflict fingerprints also describe the committed bytes.
- **Cancelled partial patches:** added `testCancelledPatchDoesNotLeakIntoUnchangedCopy`. RED: `a staged edit from a cancelled save leaked into an unchanged Save As`. A successful first-paragraph patch followed by an unsafe list-marker edit had left edits staged when conversion was cancelled. The fix discards the staged package transaction before offering conversion; undo and an unchanged copy must reproduce the original bytes.
- **Cases and centered matrices:** added a braced ordinary matrix to `math_roundtrip_tests`. RED: `native math source changed on reopen: \\left\\{ \\begin{matrix}a & b \\\\ c & d\\end{matrix} \\right.`. Cases now write native left/left column alignment, and import recognizes cases only with that alignment. An ordinary centered matrix with the same delimiters retains its original semantics. The directly compiled focused test passed with both cases and ordinary matrices.
- **Ragged matrix rows:** added parser canonicalization assertions and native round trips for ragged ordinary matrices, cases and aligned. RED: three parser assertions failed, and the native round trip failed for `\\begin{cases}x & \\text{positive} \\\\ 0\\end{cases}`. The parser now pads missing cells to the widest row with explicit empty rows under the existing node/column bounds. Both directly compiled parser and native round-trip executables passed.

## Refactoring and integration notes

- Reused one in-memory package preparation for authoring, eligibility and whole-package validation. No producer marker grants rewrite permission.
- A desktop test exposed imported default-spacing expansion. The coverage proof removes only redundant **generated** spacing absent from the source and exactly equal to the parsed inherited values; no original XML is normalized or discarded. Academic packages retain their exact registry/member proof.
- Builds use `flock /tmp/owldocs-build.lock cmake --build build --target ... -j2` to serialize the shared build tree.
- During concurrent development, transient unrelated core/header, ribbon Qt conversion and math serializer compilation failures were reported to their owners. They were not counted as meaningful RED tests.

## Integrated verification

`flock /tmp/owldocs-build.lock cmake --build build --target app_import_tests file_workflow_tests combined_docx_roundtrip_tests recovery_tests owl-docs-corpus-runner -j2`

`ctest --test-dir build --output-on-failure -R '^(app_import_tests|file_workflow_tests|combined_docx_roundtrip_tests|recovery_tests)$'`

All four suites passed (50.03 s): source deletion → restricted source recovery → opaque-preserving save; supported reopened table/list saves; cancelled staged-patch regression; style/math/picture save paths; recovery migrations and corruption limits. The restore scenario also reproduced and fixed use of an O_RDWR temporary descriptor: the restricted parser correctly requires O_RDONLY, so restore now opens the held inode read-only through `/proc/self/fd`.

The focused `ooxml_tests`, `file_fingerprint_tests` and `save_outcome_tests` also passed. The syscall-wrapped save-outcome test covers both patched and newly authored saves, including stable committed-byte results.

`bash tools/bootstrap_docx_corpus.sh /tmp/owl-docs-public-corpus-060 100`

`build/owl-docs-corpus-runner --manifest /tmp/owl-docs-public-corpus-060/manifest.json --output-dir /tmp/owl-docs-public-corpus-results-060 --minimum-fixtures 100`

Public corpus result: **100/100 opened, 100/100 byte-identical unchanged copies, `ok=true`**. Inputs were downloaded from the repository's pinned LibreOffice revision, verified by Git blob identity and SHA-256, and retained outside the repository. No universal Word layout claim follows from this preservation gate.

The installed LibreOffice 7.3 also opened an authored native DOCX and exported it to PDF using a private temporary profile. Visual and extracted-text checks covered cases, a centered ordinary matrix with the same braces, TOC, caption/REF fields, numeric citation/bibliography and footnotes/endnotes. The first smoke exposed roman endnote numbering when only global settings were present; the academic section-policy regression fixed it, and the repeated smoke displayed decimal `1` for both independent note sequences. No repair or conversion error was reported. Generated fixture/PDF files remain outside the repository in `/tmp/owldocs-native-smoke-060`.

The final frozen serializer was additionally checked after deleting visible caption and bibliography prefixes. Five native REF caches were deliberately replaced with `STALE`, then a private LibreOffice UNO session refreshed the fields and exported PDF. Numeric references recomputed as `Figure 1`, `[1]`, `Figure 2` and `[2]`; the full-text reference recomputed as `Caption prefix deleted`. Hidden sequence targets remained hidden and did not contaminate the full-text reference. Both PDF pages were inspected, with the refreshed first page checked visually and through extracted text.

The clean Release run also exposed an icon-maintenance runtime guard after the bundled Python moved to Pillow 13.0.0. RED: `Pillow 12.3.0 is required; found 13.0.0`. Read-only generation under 13.0.0 reproduced all eight existing PNG files byte for byte with unchanged master SHA-256 and pinned zlib 1.3.2. The reviewed allowlist now includes 12.3.0 and 13.0.0; the existing icon-generation CTest passed without installing dependencies or changing any icon asset.

The opt-in real Excalidraw test also passed from the clean Release executable with `OWL_DOCS_TEST_LOCAL_EXCALIDRAW_RENDERER=1`. It exercised the actual offline Professional renderer, editable PNG metadata and Codex preview/accept path. The outer tool sandbox denied bubblewrap's NETLINK_ROUTE socket, so a scoped approved run allowed the editor's own bubblewrap sandbox to operate. The installed helper matched the release bundle across all 73 files. An initial optional-test failure exposed a dangling reference into a temporary snapshot in the test itself; retaining that snapshot fixed the harness, and the real renderer test passed.

Final clean strict Release verification: all 168 build steps succeeded and the complete **49/49 CTest suite passed in 20.76 seconds**. The corpus subtest reported 100 opened and 100 exact copies. Legacy theme/DPR assertions and a stale catalog-version expectation were corrected through their executed failures; no production changes followed the clean build. The package's detailed validation record is `RELEASE_VALIDATION_0.6.0.md`.

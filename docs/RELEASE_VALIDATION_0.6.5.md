# OwlDocs 0.6.5 validation

This release expands Codex's native document-authoring tools and adds native
Excalidraw scene patching. The full release gates ran against the current build
and freshly rebuilt offline helper. No live model-inference run or software
installation is claimed.

## Build and tests

- Strict incremental Release build: warnings as errors, 305
  build steps. This is not a clean-build claim. Log:
  `/tmp/owldocs-065-release-build.log`.
- Full CTest: **94/94 passed**,
  zero failures, **63.15 seconds**. JUnit and log:
  `output/codex-review/0.6.5/release/ctest.xml` and `ctest.log`.
- Pinned public preservation corpus: **100 documents opened and 100 unchanged
  copies remained byte-identical**. Source revision:
  `2776654bbb109af114a44fe00ec9771e4ad2d99d`. This verifies untouched imported
  package preservation, not unrestricted editing compatibility.
- **9 UI/integration binaries passed at 150% scaling**,
  including the compound authoring and preview-state regressions.
- **6 workspace captures reviewed** at 820×600 and 1280×860,
  at normal and 150% scaling. No unresolved visual findings. Capture hashes
  and the completed review are in
  `output/codex-review/0.6.5/workspace/verification.json`.

The requested tests-first cycles are recorded in
`TDD_CODEX_AUTHORING_0.6.5.md`, `TDD_CODEX_CONTROLLER_0.6.5.md` and
`TDD_CODEX_FIGURES_0.6.5.md`. Actual runtime failures are distinguished from
interface/compiler setup checks. The compound workflow executes the newly
advertised semantic operations through the actual Bridge and checks failed
batch atomicity, preview isolation, discard, acceptance, one Undo/Redo,
recovery and native DOCX reopening. Additional paint tests verify same-ID image
payload replacement in body text, table cells, headers and footers.

## Fresh offline Excalidraw gate

The real pinned helper creates and revises editable figures, performs a native
patch, and verifies rendered custom-background and embedded-asset pixels.
The test retains unrelated native scene content and checks bound text/container/
connector geometry. Preview discard/accept, one Undo/Redo, recovery, DOCX and
PDF paths are exercised. Native PNG/JSON, PDF and DOCX artifacts are retained
under `output/codex-review/0.6.5/figures/`.

The exact gate commands, results and helper identity are in
`output/codex-review/0.6.5/release/excalidraw-gate.json`. The ASAR SHA-256 is
`1e31a0e3cb8c7b97bd0a85a43051b7c1d7fcf647af0c0cc3c8cf5b23ab787e3a`. This gate uses the new 0.6.5 helper; earlier
release results are not substituted. Application defaults retain the bundled
offline renderer isolation; test executable overrides are trusted C++ seams,
not model-facing filesystem access.

Controller tests use a deterministic app-server process to verify routing,
protocol failure, disconnect, stale requests and composer recovery. The optional
installed-runtime schema check runs against local Codex. This does not establish
live model output quality or universal runtime compatibility.

## Package verification

The deliverable is `dist/owl-docs_0.6.5_amd64.deb`. Its exact hash, size and
completed extraction/upgrade-simulation results are recorded externally in
`dist/validation-0.6.5.json` and `dist/SHA256SUMS` after packaging.

Package checks compare fresh stripped application/worker executables, current
bundled helper files and application payload, installed documentation, resolved
runtime libraries, restricted-parser preflight and the eight original owl
icons. Prior Debian packages and validation records retain their hashes.
APT is used in simulation mode only; no application installation is performed.

Accepted proposals still require a manual save. Supported authoring, preservation
limits and unavailable desktop/system actions are documented in
`CODEX_AUTHORING.md` and `COMPATIBILITY.md`.

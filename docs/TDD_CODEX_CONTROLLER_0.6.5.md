# Codex discovery and controller evidence, 0.6.5

The user-requested `tdd:tdd` skill governed these changes. No Superpowers workflow was used. Controller tests use an in-memory process; they require neither network inference nor account login. The root agent grants each build cycle in addition to the shared `/tmp/owldocs-build.lock`.

## Executed behavior regressions

`codex_controller_tests` first failed because a stale known document thread, an old turn within the current thread, malformed arguments, an unregistered namespace and calls after completion could reach the editor handler. The minimal fix records the active turn identity, rejects those requests, and preserves host-injected document identity. `QT_QPA_PLATFORM=offscreen build-release/codex_controller_tests routing` then passed.

`codex_controller_tests guidance` failed when the newly required discovery/no-skill paragraph was absent. After that executed RED, the built-in developer instructions were restored with `editor_v1_capabilities`, current-revision/UTF-16 requirements and integrated document/Excalidraw authoring guidance. The same selector passed.

`codex_controller_tests completion` failed because failed/interrupted completions ended normally and omitted the server's actionable error. Status handling now reports failure/interruption, retains server detail, ends the assistant response once, and clears busy/turn state. The same selector passed; a stale prior-turn completion is also ignored.

`ulimit -c 0; QT_QPA_PLATFORM=offscreen build-release/codex_controller_tests disconnect-tool` crashed with exit 139 when the editor callback disconnected its own controller. The fix keeps the old client and transport alive through the outer JSONL read, copies the callback, checks a `QPointer` and current session/turn after the callback, and drops stale results. Shutdown disconnects old event handlers before stopping the process. The same selector passed, including additional checks for controller destruction, turn completion inside the callback and multiple frames in one process read. `documentToolSessionIdentity()` is comparison-only; an active callback retains its old client so a reconnect cannot recycle that identity before the callback returns.

## Catalog and schema regressions

The deliberately new `editorV1Capabilities()` symbol produced a missing-API compile RED before its declaration and implementation were added. Capabilities combine the actual authoring schema module with the existing operation catalog and publish grouped operations, exact definitions, units, bounds, examples and the user-acceptance workflow. Runtime availability and document state are supplied by the Bridge. Catalog version changed once from 9 to 10; saved app-server thread bindings migrate while local chat messages remain.

`codex_tests capabilities` failed on desktop-sized agent image limits, absent pagination/story/table/metadata read scopes, a missing story figure target and a table example encoded as a JSON object. These were corrected. `codex_tests figure-patch` failed because bounded native patches were absent; the new schema permits only documented edits of existing IDs. Both selectors passed after implementation.

A public-manifest assertion failed because new threads advertised an unavailable file capability. Registration now contains read, search, preview and capabilities. The legacy file tool name is retained only for an explicit unavailable response. File open/save/export and other system/UI actions remain manual. `codex_tests` verifies that newly registered tools omit the file capability.

List schema RED was executed through the public capability dump: `set_list`, `set_list_level` and `set_list_layout` were absent. The requested UUID-array, levels 1–10 and complete ten-level indentation schemas were added; `codex_tests list-schema` passed. Runtime planner parity has a separate root-owned test cycle.

`codex_tests published-catalog` failed against the old static manifest before regeneration. `schemas/editor.v1.json` was then generated from the registered catalog while retaining host-injected `documentId`. The same test passed and compares all dynamic input/output schemas and shared definitions. All four figure/table examples also passed the bundled Ajv check after converting only tuple/dialect keywords for its older validator; this is an additional example check, not a claim that Ajv supports the complete 2020-12 dialect.

The subsequent body-formatting extension has its own executed RED: paragraph indentation, keep flags and exact line spacing were missing, highlight could not clear with null, body/cell font limits diverged, and layout tokens lacked the portable 53-bit integer bound. Body and cell schemas now share the semantic definitions, retaining nullable `styleId`; layout tokens are capped at 2^53−1. `codex_tests format-schema` passed. Final static JSON synchronization includes the frozen optional `insert_table.cellIds` metadata.

Reconstruction schema RED showed `reconstructScene` missing; a separate example assertion showed replacement labels lacked the stable IDs required by preservation. The optional switch now defaults false, examples declare their label IDs, and capabilities explain that full reconstruction requires an explicit user request and a loss warning. `codex_tests figure-reconstruct` passed.

The final guidance regression failed because the instructions treated all IDs as previously read IDs, preventing compound creation with declared IDs. After that RED, instructions now distinguish existing IDs from schema-permitted new stable IDs and explain explicit reconstruction. The guidance selector passed again.

`codex_controller_tests document-binding` reproduced a concrete wrong-document route when two saved document bindings contained the same server thread. The old unordered map lookup picked one document regardless of the active turn. Dispatch now uses the trusted active `turnDocumentKey_`; that same test passed, alongside the normal stale-turn and forged-document checks.

## Compound authoring characterization

The new `codex_authoring_workflow_tests` exercises already implemented behavior through the real Bridge, without inventing an artificial RED. It discovers capabilities and current metadata revision, exercises every operation in the authoring module (28 at this checkpoint), and combines tables/cells, pictures in body/cell/story, equations, notes, caption/citation/source/TOC semantics and block structure. Each accepted proposal is isolated before acceptance and restores exactly with one Undo/Redo. It also checks header-editor exit before proposal, discard, oversized story rejection and failure of a later operation without a partial edit.

Recovery encode/decode and a real manual Save As/DOCX reopen preserve complete semantic document equality, including stable identities, layout, academic fields and image bytes. The workflow passed. This is semantic/native persistence coverage; external renderer checks are recorded separately.

## Verification checkpoint

`flock /tmp/owldocs-build.lock cmake --build build-release --target codex_tests codex_controller_tests -j2` completed successfully on retry. The first attempt encountered a temporary, coordinated Canvas signature mismatch, which was a setup failure and was not counted as RED. The retry built all required app dependencies.

Routing, guidance, document-binding, completion, disconnect-tool, capabilities, figure-patch, figure-reconstruct, format-schema, list-schema and published-catalog selectors passed. Full `codex_tests` and `codex_controller_tests` passed. The final published manifest matches all four tools and 52 operation schemas. Root-owned Bridge capability, format, paragraph, page and list selectors passed, and the compound authoring workflow passed. Logs and explicit exit-status records are in `output/codex-review/0.6.5/controller/`.

The initially reported root-owned broader checks were subsequently resolved: full Bridge tests and the list planner now pass, including the visited-run performance refactor. Release-wide verification remains with the root agent.

## Preview selection and retained story geometry

Read-only review identified two additional supported workflows. Actual `codex_preview_state_tests` RED selectors reproduced both before production changes: `table-delete` and `row-delete-active` left stale cell context; `row-insert` and `row-delete` moved the active cursor into a different stable cell; `layout` allowed a page-only proposal to hide an unchanged retained header.

The preview now saves its original cursor/history state and maps retained table cells by stable identity into the candidate document. Removing the selected table/cell clears its table context so immediate typing has a valid destination. Acceptance is one Undo transaction and restores the original cell and document together. Existing discard handling continues to preserve safe read-only preview navigation.

Story fit compares the old and new capacity rectangles when page geometry changes. Imported overflow may remain only when its overflow amount does not worsen. PAGE/PAGES checks use candidate pagination from an isolated Canvas with the same font and imported table presentation. Direct story typing keeps its existing geometry policy.

All five selectors then passed, including immediate typing and Undo after table/row changes, newly hidden header rejection, and a non-worsening imported-overflow case. Full preview-state, Bridge, list-planner, editor-interaction and compound authoring/native/recovery suites passed after the strict related-target build. At that stage, the figure suite stopped at its staged bound-label read-fidelity regression; its subsequent cycles and real-renderer verification are recorded in `TDD_CODEX_FIGURES_0.6.5.md`. RED/GREEN and related-suite logs are in `output/codex-review/0.6.5/controller/`.

## Native renderer process boundary and guidance consistency

`tests/excalidraw_process_tests.cpp` uses a trusted C++ executable parameter and an isolated temporary Python helper. The app default does not read an override from model arguments, document data, settings or the environment, and retains its existing offline bubblewrap invocation. The initial API compilation check reported the expected missing constructor/native-renderer symbols; this setup check is separate from the following behavioral REDs.

After minimal API setup, the actual sleeping child process reproduced a stale completion during editor destruction (exit 1). Deleting the editor from a successful completion crashed with SIGSEGV. Native result versions 1 and missing were incorrectly accepted (both exit 1). The full 30-second timeout also failed because teardown overwrote its original error. Each RED was executed before its production fix.

Destruction now disconnects process signals and clears completion before killing/waiting. The session exclusively owns its process, allowing a completion to delete the editor safely. Nested-render callback locals outlive the editor. Native scene rendering sends protocol version 2 with the original scene and changed IDs, without a reconstruction skeleton, and requires a version-2 result echo; legacy creation/open requests keep version 1.

The same default process tests and the separate `--timeout` selector passed after the fix. The native success case retains source styling/background and embeds the editable scene into its PNG. The full existing `excalidraw_figure_tests` also passed, resolving the earlier staged fidelity failure. Build command: `flock /tmp/owldocs-build.lock cmake --build build-release --target excalidraw_process_tests excalidraw_figure_tests codex_tests codex_controller_tests -j2`.

Separate executed guidance and capability REDs showed that the old wording incorrectly required native patches to restyle existing scenes. Instructions and capability/preview descriptions now distinguish Professional/white new figures and reconstructions from native patches that retain source styling and background. `CODEX_AUTHORING.md` and `IMPLEMENTATION_STATUS.md` describe that same behavior. A new static-description parity assertion failed against the old published description, then passed after regenerating `schemas/editor.v1.json` from the registered tools. Full Codex and controller suites passed.

Logs and explicit exit-status records are retained alongside this document under `output/codex-review/0.6.5/controller/`, including `native-renderer-results.json`. Release-wide fresh helper, package and installation checks remain root-owned.

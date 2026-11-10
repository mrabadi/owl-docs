# Architecture

## Boundaries

`docxstudio_core` owns document semantics and editing. It has no Qt dependency. A single `DocumentSession` is the writer; every successful batch increments its revision and returns an inverse batch for undo.

`docxstudio_ooxml` owns OPC packaging and supported WordprocessingML mapping. It retains the original package bytes and every original entry. An unmodified Save As copies the input byte for byte. A modified save is allowed only when the mapper can replace supported content without colliding with opaque content.

Single-section authoring prepares a complete member map before writing. The same preparation supports source-coverage assessment and candidate validation. Imported source defaults are retained separately from application insertion preferences. A final content guard runs after validation and before rename; committed bytes become the next baseline. Post-rename directory-sync failures are durability warnings rather than failed-save results.

`docxstudio_worker` receives an input file descriptor in a separate Linux helper with resource limits, a cleared environment, closed inherited descriptors, seccomp restrictions, and explicit XML depth/node ceilings. The desktop keeps that descriptor open and reparses it through `/proc/self/fd` for the full preservation model, so a pathname swap cannot substitute different bytes after preflight. Eliminating the second in-process parse remains a hardening task.

`docxstudio_codex` owns transport-neutral app-server messages and the `editor.v1` contract. It does not launch processes or touch documents. The Qt shell supplies the process adapter and translates proposed operations into a preview branch.

`docxstudio_app` owns the native window, writing toolbar, input-method handling, file dialogs, local spelling, printing, and rendering adapters. Its page canvas is custom; neither HTML nor `contenteditable` is involved. A document-level AT-SPI accessibility implementation remains a future gate.

The optional figure-authoring surface is a pinned Excalidraw/Electron build
shipped under OwlDocs' libexec directory. OwlDocs starts it only for an
insert/edit request through bubblewrap with a new network namespace, read-only
runtime mounts, no home-directory mount, and one permission-0700 session
directory as its only writable input/output. The helper has no file picker,
recent-file state, HTTP listener, or general document access. It returns
bounded JSON and PNG bytes tied to a per-session capability token. The current
0.5.0 slice places the inert scene in a private PNG iTXt chunk so scene and
fallback cannot separate during ordinary image operations; the roadmap's
private OPC relationship/index remains the broader interoperability target.

## Current runtime flow

```text
Qt shell -> DocumentSession -> snapshot -> QTextLayout page geometry
       |                                      |-> screen
       |                                      `-> PDF/print
       `-> Codex app-server (optional) -> bounded editor reads / preview branch
                                           `-> streamed reply -> user accept
       `-> bundled figure editor (on demand, offline sandbox)
```

The canvas uses the same `QTextLayout` results and `renderPage` path for screen, PDF, print preview, and system printing. Print output can select all pages, a bounded page range, or the caret's current page without repaginating the document. A transport-neutral display-list vocabulary and Qt renderer are present as scaffolding, but the canvas does not yet emit that display list. Skia integration and display-list equality tests remain compatibility milestones.

## DOCX preservation rule

The application never equates “not understood” with “safe to delete.” The compatibility report classifies features as editable, preserved/view-only, or loss risk. Modified saves are blocked when a change would require rewriting a subtree containing unsupported semantics. Save As of an untouched document always retains the exact original bytes.

Unsupported conversion writes an explicitly selected editable copy, preserving the source. Recovery stores the semantic snapshot and original package separately: content-addressed SQLite BLOBs retain opaque parts, independently checksummed context retains dirty/style state, and one transaction updates the journal and source reference. Restore preflights a read-only descriptor of retained bytes through the restricted worker and reconstructs original mappings. Legacy checkpoints without a package remain supported-copy recovery paths.

## Codex privacy rule

The Codex child is not started at application launch. Opening the Codex panel
launches the official child over stdio or reuses its existing connection. After
the experimental initialize handshake, OwlDocs reads effective app-server
configuration and disables inherited external MCP servers by name in every
document thread's start/resume config. Invalid configuration fails closed
before authentication or catalog discovery. Stderr tracing is discarded;
structured protocol/request/write/exit failures remain visible.

Catalog `editor.v1.catalog.10` registers four tools: bounded semantic reads,
deterministic search, isolated native-operation previews and live capability
schemas. Built-in developer guidance requires discovery, fresh stable IDs,
UTF-16 boundaries and preview review. Semantic table, picture, story and
academic adapters plan against an isolated working document. The bridge and
session use the same core operation dispatcher; paragraph formatting and list
planning reuse editor logic. Page geometry accompanies the preview outside the
core document, with a separate opaque geometry token and a combined local undo
record. Public live geometry getters stay unchanged until acceptance while
the preview canvas renders the candidate settings.

Excalidraw uses the bundled offline helper. Native scenes remain inert JSON
inside PNG metadata; bounded element patches retain untouched scene content.
The renderer and tool callback check document/session lifetime after nested
event processing. Controller routing binds tool calls to the current document,
thread and turn, and keeps the transport alive while a handler unwinds.

The pinned app-server protocol accepts dynamic tools on `thread/start`, but
cannot replace them on `thread/resume`. Persisted document-thread bindings
include the catalog version; upgrades start a fresh server thread while
retaining local chat history. No credentials, browser cookies or arbitrary
filesystem paths are read by the editor tools. File saving/exporting and
preview acceptance remain desktop actions. The separate Unix-socket MCP helper
from the longer roadmap is not implemented. See `CODEX_AUTHORING.md`.

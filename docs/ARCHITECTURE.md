# Architecture

## Boundaries

`docxstudio_core` owns document semantics and editing. It has no Qt dependency. A single `DocumentSession` is the writer; every successful batch increments its revision and returns an inverse batch for undo.

`docxstudio_ooxml` owns OPC packaging and supported WordprocessingML mapping. It retains the original package bytes and every original entry. An unmodified Save As copies the input byte for byte. A modified save is allowed only when the mapper can replace supported content without colliding with opaque content.

`docxstudio_worker` receives an input file descriptor in a separate Linux helper with resource limits, a cleared environment, closed inherited descriptors, seccomp restrictions, and explicit XML depth/node ceilings. The desktop keeps that descriptor open and reparses it through `/proc/self/fd` for the full preservation model, so a pathname swap cannot substitute different bytes after preflight. Eliminating the second in-process parse remains a hardening task.

`docxstudio_codex` owns transport-neutral app-server messages and the `editor.v1` contract. It does not launch processes or touch documents. The Qt shell supplies the process adapter and translates proposed operations into a preview branch.

`docxstudio_app` owns the native window, ribbon, input-method handling, file dialogs, local spelling, printing, and rendering adapters. Its page canvas is custom; neither HTML nor `contenteditable` is involved. A document-level AT-SPI accessibility implementation remains a future gate.

## Current runtime flow

```text
Qt shell -> DocumentSession -> snapshot -> QTextLayout page geometry
       |                                      |-> screen
       |                                      `-> PDF/print
       `-> Codex app-server (optional) -> bounded editor reads / preview branch
                                           `-> streamed reply -> user accept
```

The canvas uses the same `QTextLayout` results and `renderPage` path for screen, PDF, print preview, and system printing. Print output can select all pages, a bounded page range, or the caret's current page without repaginating the document. A transport-neutral display-list vocabulary and Qt renderer are present as scaffolding, but the canvas does not yet emit that display list. Skia integration and display-list equality tests remain compatibility milestones.

## DOCX preservation rule

The application never equates “not understood” with “safe to delete.” The compatibility report classifies features as editable, preserved/view-only, or loss risk. Modified saves are blocked when a change would require rewriting a subtree containing unsupported semantics. Save As of an untouched document always retains the exact original bytes.

## Codex privacy rule

Chat is off by default. Enabling it launches the official Codex child over stdio. The application sends bounded selection and outline text with each turn and registers three dynamic `editor.v1` tools for bounded reads, isolated operation previews, and path-free file capabilities. File-capability requests are denied until the user has granted one. No credential files or browser cookies are read by the editor. This direct dynamic-tool bridge is implemented; the separate Unix-socket MCP helper from the longer roadmap is not.

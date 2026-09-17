# Architecture

## Boundaries

`docxstudio_core` owns document semantics and editing. It has no Qt dependency. A single `DocumentSession` is the writer; every successful batch increments its revision and returns an inverse batch for undo.

`docxstudio_ooxml` owns OPC packaging and supported WordprocessingML mapping. It retains the original package bytes and every original entry. An unmodified Save As copies the input byte for byte. A modified save is allowed only when the mapper can replace supported content without colliding with opaque content.

`docxstudio_worker` receives an input file descriptor in a separate Linux helper with resource limits, a cleared environment, closed inherited descriptors, seccomp restrictions, and explicit XML depth/node ceilings. The desktop keeps that descriptor open and reparses it through `/proc/self/fd` for the full preservation model, so a pathname swap cannot substitute different bytes after preflight. Eliminating the second in-process parse remains a hardening task.

`docxstudio_codex` owns transport-neutral app-server messages and the `editor.v1` contract. It does not launch processes or touch documents. The Qt shell supplies the process adapter and translates proposed operations into a preview branch.

`docxstudio_app` owns the native window, ribbon, input-method handling, file dialogs, local spelling, printing, and rendering adapters. Its page canvas is custom; neither HTML nor `contenteditable` is involved. A document-level AT-SPI accessibility implementation remains a future gate.

The optional figure-authoring surface is a pinned Excalidraw/Electron build
shipped under Owl Docs' libexec directory. Owl Docs starts it only for an
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

## Codex privacy rule

Chat is off by default. Enabling it launches the official Codex child over stdio. After the experimental initialize handshake, the application reads effective app-server configuration, requires a recognized MCP-server map, and disables inherited external MCP servers by name in the config for every document thread start and resume; an absent or invalid map fails closed before authentication or catalog discovery. Stderr tracing is discarded rather than being treated as a protocol error. The application sends bounded selection and outline text with each turn and registers catalog `editor.v1.catalog.4`: four dynamic tools for bounded reads, deterministic bounded search, isolated operation previews, and path-free file-capability reads. The preview surface includes built-in paragraph-style operations; it does not advertise `insert_image` until the destination/image-capability grant path can supply usable bytes. Search returns grapheme-aligned UTF-16 ranges and stable paragraph or cell identity; a cell hit can be expanded through the read tool's explicit `{tableId, cellId}` target without exposing a filesystem path. Because the pinned app-server protocol accepts dynamic tools on `thread/start` but not `thread/resume`, Owl Docs versions persisted document-to-thread mappings with the tool catalog and starts a fresh server thread after a catalog change while preserving local chat history. The current build has no user-facing capability-grant path, so the file-capability read tool denies every request. No credential files or browser cookies are read by the editor. This direct dynamic-tool bridge is implemented; the separate Unix-socket MCP helper from the longer roadmap is not.

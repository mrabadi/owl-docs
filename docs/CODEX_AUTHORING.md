# Codex document authoring

Open the Codex pane and describe the document change you want. OwlDocs connects the pane to Codex and supplies its document tools and instructions automatically. No user skill installation or activation is required.

Codex first calls `editor_v1_capabilities` to discover the current operation schemas, feature groups, limits and runtime availability. It uses `editor_v1_read` or `editor_v1_search` to inspect the target, stable IDs and current revision. Tool calls are bound to the document that started the active chat turn; switching tabs does not redirect an in-flight edit to another document.

`editor_v1_preview` prepares one atomic proposal. Review it in OwlDocs and accept or discard it. A proposal does not change the authoritative document until you accept it. An accepted compound proposal is one Undo transaction. Codex cannot accept its own proposals, and it must read again after a revision conflict.

Read target identities again after reopening a file. Imported document objects can receive new internal IDs; the embedded Excalidraw scene retains its own element and label identities.

The built-in authoring tools cover text and paragraph formatting, supported list settings, paragraph splitting/merging and block movement, tables and cell formatting, native pictures, header/footer text and pictures, equations, notes, captions, manual bibliography entries/citations, cross-references, heading tables of contents, and page layout. Read the live capability result for exact properties and availability. These are OwlDocs's supported editing features; imported unsupported Office content continues to use the app's source-preserving save checks.

Example requests include “Create an editable Excalidraw figure showing the question, hypothesis and evidence,” “Revise the arrow label in this figure to say supports,” and “Create a two-column evidence table with a numbered caption in one proposal.” Ask for a preview, review the result, then accept and save when satisfied.

Editable Excalidraw figures use the bundled offline renderer. New figures and reconstructions use Professional mode with a white canvas; native patches retain the source styling and background. Codex can create figures and read their stable element/label IDs. It can replace a complete supported scene or patch known native elements while retaining other objects, groups, bindings and assets. Incomplete or complex scenes use a bounded patch. Default replacement retains every existing supported scene/label identity and refuses incomplete reconstructions. An intentional full redraw requires `reconstructScene: true`; its preview reports unsupported content and omitted identities before acceptance. The capability result describes the permitted scalar and connector-point changes.

Offsets are UTF-16 code units. Public geometry is in points; a point is 20 twips or 12,700 EMU at native serialization boundaries. Read page layout before changing it and include `pageLayout.stateId` as `expectedLayoutStateId`, as well as the document revision. A document revision and its layout token protect different state.

Raster insertion accepts validated base64 PNG/JPEG data, with a 4 MiB encoded-image limit and 4 MiB aggregate image input per proposal. The complete JSONL request must stay below 8 MiB. Desktop image insertion retains its 16 MiB per-image and 32 MiB aggregate limits; a larger picture inserted in the UI can subsequently be edited by its image ID. Tables are limited to 50 rows and 20 columns; a proposal contains at most 256 operations. Header/footer stories are limited to 32,768 UTF-16 units and must fit the app's supported story geometry.

Document reads are bounded and paginated. Use `maxBlocks` (default 512, maximum 2,048), `blockOffset` and `nextBlockOffset` to continue. The `stories`, `tables`, `metadata` and `images` scopes avoid unrelated text. Targeted `imageIds` reads can request up to 64 IDs; native figure context is bounded and reports truncation.

File open, manual Save/Save As, export, local file selection and other desktop/system actions remain user actions. The current catalog does not advertise a file-reading capability because there is no file grant UI. Tools accept no arbitrary paths, shell commands or remote image URLs. Accepted edits still need a manual save; recovery snapshots provide crash recovery, not a substitute for saving.

If Codex is disconnected, its process ends, or its turn fails or is interrupted, OwlDocs reports the status. Requests from stale threads/turns and results that return after disconnect or turn completion cannot mutate a different document or publish a new proposal. Opening the pane again reconnects through the normal chat controls.

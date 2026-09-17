# Owl Docs roadmap

This roadmap is a sequence of evidence gates, not a claim of Microsoft Word
parity. A feature moves into the compatibility matrix only after its editing,
DOCX round-trip, rendering, undo/recovery, and failure behavior are covered by
tests.

## M0 — Feasibility and fidelity foundation (complete)

- Byte-identical no-op handling for the initial 100-document public corpus.
- Localized supported edits that preserve unrelated package members.
- Native feasibility paths for paragraphs, styles, lists, tables, images,
  sections, Unicode shaping, searchable PDF, and restricted input parsing.
- The detailed evidence and remaining qualifications are in
  `IMPLEMENTATION_STATUS.md` and `M0_CORPUS_EVIDENCE.md`.

## M1 — Daily-use offline alpha (active)

M1 turns the feasibility foundation into a coherent everyday editor. It is
complete only when the features below work together in normal keyboard- and
mouse-driven workflows, survive save/reopen and recovery, and meet the stated
performance and privacy gates.

The 0.4.4 checkpoint adds native built-in paragraph-style identity and
provenance-aware transitions to the 0.4.3 document-ingress/Recent Files slice.
Normal, No Spacing, Title, Subtitle, Quote, and Heading 1–9 are available from
the Home ribbon; heading Enter continuation, undo, recovery, supported DOCX
save/reopen, and previewed Codex changes use the same direct-override
preservation rules. Custom style-definition editing, styles inside table
cells, and heading/page/object browsing remain later work. M1 as a whole
remains active.

- Finish remaining recovery/session edge cases around the implemented
  multi-document ingress, printing, shared-layout PDF export, recent files,
  and multi-file desktop association workflows.
- Complete the everyday editing surface around the implemented page view,
  zoom, and initial text Navigation/Find-Replace slice: add rulers,
  heading/page/object navigation, remaining standard shortcuts, and
  finish the clipboard, command palette, and Home, Insert, Layout, Review, and
  View ribbon surfaces.
- Complete direct character and paragraph formatting, styles, tabs, borders,
  page breaks, sections, page geometry, columns, lists, and the daily-use table
  subset.
- Complete the image surface around the implemented native PNG/JPEG
  insertion, selection, resizing, inline placement, and canonical
  square/top-and-bottom anchors: add crop, rotation, z-order, arbitrary
  positioning, and the remaining wrap modes without losing unsupported
  imported drawing data.
- Add text boxes, captions, and the initial simple-shape subset.
- Finish local English spelling behavior and personal-dictionary workflows.
- Keep safe LaTeX equations as native editable math atoms and native OMML.
- Complete the M1 Codex editor contract: bounded reads, isolated previews,
  hunk/full acceptance, idempotent revision handling, and destination
  capabilities for save/export.
- Add the AT-SPI document tree, keyboard-only coverage, and complete
  IBus/Fcitx composition behavior.
- Move remaining untrusted raster decoding and supported OOXML mapping behind
  restricted helper boundaries.
- Meet the published 100-page latency, pagination, scrolling, save, and
  randomized edit/undo/save/reopen targets.
- Complete dependency pinning, notices, license review, an SPDX SBOM, and the
  reproducible signed Ubuntu package pipeline.

## M2 — Embedded Figures with local Excalidraw

M2 adds document-owned, editable figures through a pinned and audited local
Excalidraw build. Excalidraw is an optional authoring surface; the DOCX remains
readable and visually useful when that surface is unavailable.

Version 0.5.0 implements the first end-to-end slice. It bundles the figure
editor in Owl Docs, supports insert/edit/cancel, one-transaction updates,
ordinary picture layout, recovery, and native DOCX save/reopen. This checkpoint
stores bounded inert scene JSON in a private PNG iTXt chunk so the preview and
source share one byte-owned image atom. The private OPC index/relationship,
hash-divergence workflow, and remaining acceptance automation below are still
planned before M2 is declared complete.

### User experience

- **Insert > Figure > Excalidraw** opens a document-scoped local figure editor.
  Saving returns to Owl Docs and replaces the placeholder with the rendered
  figure. Canceling leaves the document unchanged.
- A single click selects the figure like any other drawing. Double-clicking, or
  choosing **Edit Figure** from its context menu, reopens the editable scene.
  This preserves ordinary image selection, resizing, wrapping, and keyboard
  behavior.
- Each create or update action is one normal undoable Owl Docs transaction that
  includes both the editable source and its rendered preview. Undo, redo,
  recovery, duplication, copy/paste, and save/reopen cannot separate the pair.
- The contextual Figure ribbon and context menu provide Edit, Wrap Text,
  Position, Alt Text, Export PNG, Export Editable Drawing, and Convert to
  Ordinary Picture. Enter activates editing for keyboard users.
- The fork's Sketch and Professional modes remain available, with the selected
  mode stored as figure-editor preference rather than portable document state.

### DOCX representation and interoperability

- Store a normal DrawingML picture and a bounded PNG fallback in `word/media`
  so Word, LibreOffice, Google Docs, and other DOCX consumers can display the
  figure without Excalidraw or Owl Docs.
- Where the validated export permits it, retain a sanitized vector preview for
  Owl Docs screen/PDF rendering while keeping the PNG fallback authoritative
  for broad interoperability.
- Migrate the editable, size-bounded Excalidraw scene from the 0.5.0 private
  PNG metadata checkpoint to a private optional OPC
  part related to the drawing by a stable figure identifier and versioned
  source/preview hashes. The custom part must never be required to render the
  document. Until that migration, Owl Docs recognizes its namespaced inert iTXt
  chunk and treats a stripped chunk as a flattened ordinary picture.
- Before embedding, remove deleted elements, unreferenced binary files, local
  history, and other hidden scene state that is unnecessary for future edits.
  Owl Docs undo history, not undisclosed content inside the scene, owns prior
  figure versions.
- Preserve unknown scene fields and unrelated OOXML members on eligible save
  paths. If another editor strips the optional scene part, show the surviving
  picture normally and classify it as a flattened figure rather than losing or
  inventing editability.
- If the editable source and preview hashes diverge after an external edit,
  preserve both and require an explicit user choice before regenerating either.
- Never place executable HTML or JavaScript from a document into the figure
  editor. Treat scene JSON, embedded files, SVG, fonts, and data URLs as
  untrusted bounded inputs.

### Local process and privacy boundary

- Vendor or package an exact reviewed source revision and record its full
  dependency/license inventory in the release SBOM. No floating npm ranges are
  permitted in a release build, and complete LICENSE/NOTICE material is a
  release gate.
- Launch a dedicated figure-editor process only after a user creates or edits a
  figure. It receives that one scene through authenticated, document-scoped
  IPC and receives no arbitrary path or broad home-directory mount.
- The 0.5.0 checkpoint uses atomic files inside a permission-0700 ephemeral
  directory plus a 256-bit capability token and strict byte/schema limits.
  Replace that bridge with a permission-0600 Unix socket and versioned protocol,
  per-session capability token, peer-process verification, and strict message
  limits. Do not expose an HTTP listener or launch the system browser.
- Run with no network namespace, no telemetry, no collaboration/cloud UI, a
  restrictive Content Security Policy, denied navigation/popups/permissions,
  ephemeral browser storage, bounded memory/CPU, and cleanup on parent exit.
- The helper has no file dialogs. If a figure needs an imported image, it asks
  Owl Docs to obtain and validate the asset and receives bounded bytes rather
  than a host path.
- Return validated scene JSON plus a newly rendered preview over the capability
  channel. Reject stale sessions, token replay, malformed/oversized results,
  unsupported media, and exports that exceed resource limits.

### M2 acceptance gates

- Create, save, reopen, edit, duplicate, copy/paste, undo/redo, recover, and
  delete multiple figures without source/preview drift.
- Word and LibreOffice display the fallback preview without a repair prompt;
  PDF and screen consume the same figure bounds and pagination.
- No-op Save As preserves an imported figure package byte-for-byte where the
  normal preservation rules apply.
- Removing the optional editable part in an external-editor fixture retains a
  visible flattened picture and produces an accurate compatibility report.
- Cancel, editor crash, Owl Docs crash, disk-full, stale-session, and malformed
  return paths never partially mutate the document.
- Automated tests prove no outbound networking, no general filesystem access,
  origin/capability enforcement, deterministic cleanup, and bounded handling of
  hostile scene/SVG/raster inputs.

Codex-authored figures are a later extension of the same transaction model:
Codex may propose bounded scene creation or replacement only through an Owl
Docs preview branch, never by controlling the figure-editor UI or writing an
arbitrary file.

## M3 — Business and legal beta

- Comments/replies, tracked changes, comparison, author attribution, and
  accept/reject workflows.
- Advanced and floating tables, repeating headers, row splitting, nested
  tables, conditional styles, and manual sizing.
- First/even/default headers and footers, fields, page numbering, linked
  sections, watermarks, and document properties.
- Templates, content controls, form fields, restricted editing, and protection
  warnings.
- Basic editable charts and full diagram tooling for shapes, connectors,
  grouping, alignment, distribution, snapping, layers, and captions.

## M4 — Technical and academic release

- Broader OMML and safe LaTeX editing, equation numbering, and long-document
  mathematical typography.
- Footnotes, endnotes, citations, bibliographies, captions, cross-references,
  tables of contents/figures, and indexes.
- Advanced figures, anchored groups, wrap-boundary editing, multi-column
  layout, hyphenation, and long-document navigation.
- Additional chart types and Codex tools for structured technical, citation,
  equation, figure, and legal transformations.

## M5 — Word-class interoperability expansion

- Remaining field codes, mail merge, labels, advanced content controls,
  broader DrawingML/ChartML, legacy vector previews, and compatibility modes.
- PDF/A and tagged-PDF accessibility.
- Additional languages, dictionaries, localization, RTL polish, and platform
  ports after Ubuntu passes the same corpus and pagination gates.
- Collaboration remains post-1.0 and requires a separately specified
  concurrency model.

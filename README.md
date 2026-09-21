# Owl Docs

Owl Docs is a Linux-first, offline word-processor prototype with an optional Codex chat panel. New documents are written directly as Office Open XML (`.docx`), and imported DOCX packages are handled without an HTML or ODF conversion step.

Version 0.5.11 is an M1 development checkpoint plus the first complete M2
vertical slice: Owl Docs ships its own pinned, offline Excalidraw figure editor.
Insert > Excalidraw Figure creates a PNG-backed editable figure; double-click,
F2, the context menu, or the Picture ribbon reopens it. Creation and updates are
normal undoable transactions, and editable source survives recovery and native
DOCX save/reopen in the PNG's private inert metadata. No separate Excalidraw
installation is used. M1 and the remaining M2 hardening/compatibility gates are
still active; this is not a claim of Word-class compatibility.

Codex can now insert and revise those native editable figures through the same
preview-before-accept workflow as text and formatting. Agent-authored figures
are rendered locally in Professional mode. On connection, the chat prefers
`gpt-5.6-luna`, low reasoning, and Standard speed when the connected runtime
advertises those choices, with runtime-discovered fallbacks otherwise.

## What is implemented in this foundation

- A self-contained Excalidraw figure workflow. The pinned React/Electron editor
  is built into the Owl Docs package and launched on demand inside a
  no-network bubblewrap sandbox with only one private figure session writable.
  Save returns bounded scene JSON and a PNG preview; Cancel does not mutate the
  document. Sketch and Professional modes switch immediately from the editor
  toolbar or Settings menu, and the chosen mode persists as a local Owl Docs
  preference. Each figure keeps its own editable source inside inert PNG metadata,
  so multiple figures, ordinary picture layout, undo/redo, recovery, clipboard,
  DOCX save/reopen, and rendering in other word processors use one inseparable
  document object.
- A C++20 document model with revision-checked transactions, undo/redo, stable node identifiers, UTF-16 positions, and isolated AI preview branches.
- A native OPC/DOCX package reader and writer using libzip and pugixml. A pinned 100-file public LibreOffice corpus opens and copies byte-for-byte; the narrow supported text-patch path retains every unrelated package member and refuses edits that fail its safety checks. This is preservation evidence, not a layout-fidelity corpus.
- A custom Qt page canvas and command registry for editing without an HTML or office-suite engine, presented as Owl Docs with a geometric Ubuntu-color owl and an icon-based ribbon.
- Coherent multi-document ingress: a successful first open replaces the untouched startup tab; File > Open, positional command-line arguments, the desktop file association, and file-manager drops all accept multiple documents. Drops accept existing local DOCX files with copy-only semantics while rejecting remote, unsupported, and move-only items. Equivalent paths and symlink aliases activate the already-open tab instead of duplicating it, and a symlink-opened document safely saves its resolved target. Open Recent is numbered, disambiguates duplicate basenames, exposes full-path tooltips, removes an unavailable item when selected, and provides Remove Missing Documents and Clear Recent Documents actions.
- On-canvas text editing with a stable black starting color, immediate color palettes, a non-modal live color picker, removable highlighting, word/visual-line mouse selection, one-second typing undo groups, wrapped-line caret navigation, undo/redo, ordinary paste plus destination-formatted Paste as Text Only (`Ctrl+Shift+V`), basic imported and authored character/paragraph formatting, page settings, correctly paginated authored and supported terminal hard page breaks, local English spellchecking that waits until the active word is completed, common shortcuts, recovery checkpoints, print preview and range/current-page printing, and PDF export with extractable text. A modeless Navigation pane provides debounced live search results and snippets across body paragraphs and semantic table cells, case-sensitive and whole-word matching, previous/next navigation with `Shift+F3`/`F3`, and single or all-match replacement; Replace All is one undoable editor transaction. Search state is retained per document tab, reads follow the active preview branch, and writes remain locked while a preview is active. A synchronized per-tab zoom slider lives at the lower right, supports 25–400%, and accepts high-resolution Control+wheel/touchpad input; `Ctrl++`, `Ctrl+=`, and `Ctrl+-` are not zoom bindings. Character formatting applied to an empty body paragraph is stored durably on its paragraph mark: it survives leaving and returning to the paragraph, deletion of the final text, every empty-list exit path, schema-v11 recovery, and—for properties represented by the current OOXML writer—native `w:pPr/w:rPr` save/reopen, then becomes the format of subsequently typed text. Explicit clears such as Bold off or No Highlight retain their intent through live editing and recovery. Bold off has a serializable OOXML representation; a null No Highlight override does not yet emit an explicit direct OOXML clear.
- Native paragraph styles for Normal, No Spacing, Title, Subtitle, Quote, and Heading 1–9 are available from the Home ribbon. Normal follows the configured editor font and size; `Ctrl+Shift+N` applies Normal and `Ctrl+Alt+1/2/3` apply the first three headings. Pressing Enter after a heading starts its configured next style in the same undoable transaction, while Shift+Enter remains a soft line break. Style identity, inherited text/paragraph-mark baselines, and exact direct-format provenance survive editing, undo/redo, and schema-v11 recovery. Properties represented by the current OOXML writer retain their directness through supported DOCX save/reopen; language metadata and a null No Highlight override remain semantic/recovery-only. Changing a style replaces inherited formatting while preserving explicit run and paragraph overrides, including explicit false/clear values within the semantic model. Empty styled paragraphs retain their insertion appearance, and foreign built-in definitions are flattened only as needed to preserve visible formatting when a simplified copy must use Owl Docs' deterministic definitions. Imported custom style IDs remain identifiable but are not editable definitions, and named paragraph styles are not yet available inside table cells.
- A clean font-family picker that keeps every installed multilingual font available without Qt's appended Devanagari, Thai, Malayalam, Telugu, or other writing-system sample text. Each English family name previews its own typeface when that font supports Latin; script-only and symbol families stay legible in the normal UI font.
- Per-user editor defaults for font family, font size, and tab width (four space-equivalents by default), available from File > Options and applied to every open document canvas.
- List-aware editing with stable per-list identities, ten configurable levels, right-click List Properties, and a contextual List ribbon tab. Numbered levels cycle through `1, 2, 3`; `A, B, C`; `I, II, III`; `a, b, c`; and `i, ii, iii`, then repeat for levels 6–10. Hierarchical counters restart and resequence across Enter, multiline insertion, indentation, outdentation, and boundary deletion. Each list independently stores bullet positions and text gaps in space-equivalents; built-in defaults are 0/4/8/.../36 with a two-space gap, and wrapped or soft-broken text aligns to the same text stop. “Use as Defaults” in List Properties saves the current ten-level geometry for new bulleted and numbered lists.
- Semantic tables with a single rows/columns/header dialog, a contextual Table ribbon, editable cells, rectangular one- or multi-cell selection, Tab/Shift+Tab cell navigation (including automatic row creation from the final cell), row/column insertion and deletion, and body-block reordering. The normal font, size, emphasis, color, highlight, and paragraph-alignment controls work on text within one cell or across the selected cell rectangle. Thirteen built-in table styles provide practical plain, grid, light, medium, banded, accent, and dark-header choices. Screen/PDF rendering, recovery snapshots, and native WordprocessingML table output retain the supported cell formatting, paragraph formatting, structure, and selected style. Bounded simple rectangular text-only imported tables reopen as semantic tables and retain supported grid widths, table alignment, cell fills, borders, padding, vertical/paragraph alignment, mixed direct run formatting, and Word-compatible automatic row metrics; image- or equation-bearing cells remain preserved view-only.
- Native desktop insertion and bounded high-quality screen rendering of PNG/JPEG DrawingML pictures. Interactive resizing and document zoom always repaint from the cached full-resolution decode; resizing changes only semantic geometry and never resamples or rewrites the encoded source. Picture Layout v1 supports inline placement plus deliberately constrained, canonical zero-offset `wp:anchor` representations for square and top-and-bottom wrapping. Move-with-text anchors use character/paragraph-relative origins; fixed anchors use page/page-relative origins. Anchored layouts retain four wrap distances, move-with-text versus fixed relation, and alt text. Inline distance values are preserved when present, but they are not visually applied and their controls are disabled. Pictures can be selected, copied, pasted, deleted, resized through Picture Size or three persistable on-canvas handles (right, bottom, and bottom-right), and configured through Picture Layout; source bytes, dimensions, supported layout, and alt text survive undo/redo, schema-v11 recovery, native DOCX save, and desktop reopen. Only matching safe internal imported `wp:inline` or canonical anchor shapes enter this editable path; other anchors remain opaque on preservation paths. Crop, rotation, z-order, arbitrary horizontal/vertical positioning, additional wrap modes, and pictures inside table cells remain unsupported. The OOXML engine can also author multiple ordered sections and reopen their ranges, break kinds, and page geometry, but section authoring remains engine/API-only.
- Supported OOXML `docDefaults`, style `basedOn` cascades, theme fonts/colors, explicit false values, and native `numbering.xml` definitions resolve on import. Authored list levels 1–9 use `w:numPr` plus native numbering definitions rather than marker text, and their supported counters, level templates, suffixes, starts, and geometry reopen semantically. Because WordprocessingML permits nine native levels, Owl Docs' tenth editor level is saved as visually equivalent literal marker text with tab/hanging-indent geometry; Owl Docs recovers it as level 10 on reopen, while other editors do not see it as native list continuation.
- Native equation atoms parsed from a deliberately safe LaTeX subset, laid out as vector math on the page and in PDF, and serialized to/reopened from the corresponding Office Math (OMML) subset.
- An optional `codex app-server` stdio client with runtime model/effort/service-tier discovery, streamed multi-turn chat, bounded document read/search tools, and live semantic edit previews. A completed turn re-enables the composer and the next message continues the same document thread. Document-mode instructions and tool descriptions explicitly direct Codex to use `set_text_style` for font/emphasis/color/highlight/script formatting and `set_paragraph_style` for styles/alignment/spacing instead of merely describing UI steps. The supported path explicitly advertises `capabilities.experimentalApi=true` during `initialize`, reads effective configuration, requires a recognized MCP-server map, disables every inherited external MCP server by name for both thread start and resume, and only then registers the `editor.v1` `dynamicTools`. App-server stderr tracing is discarded instead of being promoted into a chat failure; structured protocol, request, write, and process-exit errors remain visible. Fake-process tests enforce that sequence, multi-turn continuity, and fail-closed behavior, and an optional installed-runtime schema check verifies the fields used by the integration when Codex and Python are present. This surface is experimental: an absent or incompatible runtime disables optional AI behavior without preventing offline editing. Accepted previews are one undoable transaction.
- Multilingual conformance tests exercise shaping, grapheme-safe caret movement and selection, and visual-line wrapping for English, RTL Hebrew, CJK, and Indic text. Screen and searchable-PDF tests assign the same soft-flow markers to the same pages and verify that export consumes the existing canvas layout without rebuilding pagination.
- Local recovery schema v11 retains semantic text, run and paragraph formatting, paragraph-style identity, separate inherited text/paragraph-mark baselines, exact override provenance, durable body-paragraph insertion formats, equations, supported picture bytes/layout/alt text, tables, body order, identities, and page geometry. Legacy decode paths remain in place, with migration fixtures covering schema versions 3–10; fields introduced later default to safe empty, inline, or provenance-free forms when absent.
- No application telemetry, update checks, remote fonts, cloud spelling, or background network requests. The Codex process is launched only after the user enables chat; authentication may open the user's browser.

Image handling and list authoring remain deliberately bounded. Safe internal PNG/JPEG `wp:inline` drawings and the two canonical Picture Layout v1 anchor shapes are transport-neutral core atoms that occupy one U+FFFC position in paragraph text, so normal selection, replacement, deletion, split/merge, preview, revision, and undo rules determine their order. Each atom owns stable identity, original encoded bytes, format, accessible name, EMU geometry, placement, four wrap distances, and move/fixed relation; the Qt layer keeps only a decoded-pixel cache keyed by that identity. The supported square and top-and-bottom anchors are canonical zero-offset forms—character/paragraph-relative when moving with text and page/page-relative when fixed—not a general floating-positioning system. Inline distance values remain in the semantic record for fidelity but are neither visually applied nor editable in the current UI. Inputs are limited to 512 pictures, 16 MiB per encoded picture, and 32 MiB aggregate encoded picture data, with separate dimension, pixel-count, and decoded-cache limits. Clipboard and document state retain bytes and a bounded display name rather than arbitrary source paths. Three persistable resize handles (right, bottom, and bottom-right) and the ratio-lockable size dialog change dimensions, but crop, rotation, z-order, arbitrary positioning, tight/through/behind/in-front wrapping, and images inside table cells are not supported. Raster container inspection runs before decode, but decoding still occurs in the GUI process and remains an explicit hardening gap. List commands toggle or convert whole selected paragraphs without duplicating markers; Enter continues a list, Shift+Enter adds a visible soft line break inside the same item, Tab/Shift+Tab changes its level, and an empty item exits. Simple rectangular text-only imported tables and newly authored tables are real movable document blocks. Supported cell text, direct character formatting, paragraph formatting, structure, and built-in table styles are authored in native DOCX; image- or equation-bearing cells and more complex imported widths, custom fills/borders/padding, and other presentation details remain preservation-only when they fall outside that subset. Cell merging/splitting, manual sizing, nested tables, floating placement, and other advanced table behavior are not yet implemented. Equation support is limited to the built-in, non-executing LaTeX/OMML subset; arbitrary TeX, macros, packages, and unsupported Office Math constructs are not evaluated. Other complex imported objects are generally preserved in the original package but are not yet rendered or editable. Formatting or structural edits to an imported document may therefore require an explicitly confirmed, simplified Save As.

## Build on Ubuntu 22.04

Install the current foundation's dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  libzip-dev libpugixml-dev libsqlite3-dev \
  libhunspell-dev hunspell-en-us libseccomp-dev nlohmann-json3-dev \
  fonts-crosextra-carlito bubblewrap nodejs npm zip unzip poppler-utils
```

Configure, build, and test:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run:

```bash
./build/owl-docs
```

Build and install the Debian package:

```bash
cd third_party/excalidraw-figure-editor
npm ci
npm run build
cd ../..
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DOWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE=ON
cmake --build build-release
ctest --test-dir build-release --output-on-failure
cpack --config build-release/CPackConfig.cmake -G DEB -B dist
install -m 0644 dist/owl-docs_0.5.11_amd64.deb /tmp/owl-docs_0.5.11_amd64.deb
sudo apt install /tmp/owl-docs_0.5.11_amd64.deb
owl-docs
```

Ubuntu 22.04 packages Qt 6.2.4, which the source accepts for development. The roadmap targets pinned Qt 6.8.x release builds, but that release pipeline has not yet been established.

## Safety boundary

Macros, ActiveX, OLE packages, external relationships, and TeX commands are never executed by this build. Every GUI open is preflighted by a resource-limited, seccomp-restricted parser worker. Malformed XML, truncated ZIPs, and deterministic ZIP mutations are exercised through that child with bounded output and clean termination. The GUI then reparses the same held file descriptor—not a reopened pathname—to construct the preservation model; raster decoding and the remaining in-process parser surface are documented hardening gaps. Codex is requested to use an empty temporary working directory, read-only sandbox, and no approvals; this is defense in depth, not a complete OS security boundary because read-only mode can still permit reads. Registered `editor.v1` tools provide bounded semantic reads and isolated previews. Their dynamic-tool transport currently depends on the explicitly enabled experimental app-server schema described above and is checked against an installed runtime when available. A preview changes the live document only after the user accepts it.

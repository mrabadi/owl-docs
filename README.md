# OwlDocs

OwlDocs is a Linux-first, offline word-processor prototype with an optional Codex chat panel. New documents are written directly as Office Open XML (`.docx`), and imported DOCX packages are handled without an HTML or ODF conversion step.

The 0.6.5 update expands Codex's native document tools, including locally
rendered editable Excalidraw figures, tables and pictures, header/footer
regions, page layout, lists and academic writing objects. A live capability
catalog and built-in instructions expose supported operations without a
separately installed skill. Every edit remains an isolated preview for your
acceptance; saving remains manual. See [Codex authoring](docs/CODEX_AUTHORING.md),
the [release notes](docs/RELEASE_NOTES_0.6.5.md) and
[validation record](docs/RELEASE_VALIDATION_0.6.5.md). The
[0.6.4 notes](docs/RELEASE_NOTES_0.6.4.md) retain the compact table-ribbon update.

The desktop uses an ApeTek-inspired warm neutral and forest
palette, a compact writing toolbar, searchable commands, and clearer document
navigation while retaining the owl logo. The release adds academic writing
objects, an equation source editor with live preview, and safer native saves
and recovery. It remains an M1 development release with the first complete M2
vertical slice: OwlDocs ships its own pinned, offline Excalidraw figure editor.
Insert > Excalidraw Figure creates a PNG-backed editable figure; double-click,
F2, the context menu, or the Picture controls reopens it. Creation and updates are
normal undoable transactions, and editable source survives recovery and native
DOCX save/reopen in the PNG's private inert metadata. No separate Excalidraw
installation is used. M1 and the remaining M2 hardening/compatibility gates are
still active; this is not a claim of Word-class compatibility.

The [0.6.0 release notes](docs/RELEASE_NOTES_0.6.0.md) and
[release validation](docs/RELEASE_VALIDATION_0.6.0.md) record the previous release's
tested scope.

Codex can now insert and revise those native editable figures through the same
preview-before-accept workflow as text and formatting. Agent-authored figures
are rendered locally in Professional mode. On connection, the chat prefers
`gpt-6.1-sol` and Standard speed when the connected runtime advertises those
choices, with runtime-discovered fallbacks otherwise. Supported Sol reasoning
choices are Light, Medium, High, XHigh and Ultra.

## What is implemented in this foundation

- A self-contained Excalidraw figure workflow. The pinned React/Electron editor
  is built into the OwlDocs package and launched on demand inside a
  no-network bubblewrap sandbox with only one private figure session writable.
  Save returns bounded scene JSON and a PNG preview; Cancel does not mutate the
  document. Sketch and Professional modes switch immediately from the editor
  toolbar or Settings menu, and the chosen mode persists as a local OwlDocs
  preference. Each figure keeps its own editable source inside inert PNG metadata,
  so multiple figures, ordinary picture layout, undo/redo, recovery, clipboard,
  DOCX save/reopen, and rendering in other word processors use one inseparable
  document object.
- A C++20 document model with revision-checked transactions, undo/redo, stable node identifiers, UTF-16 positions, and isolated AI preview branches.
- A native OPC/DOCX package reader and writer using libzip and pugixml. A pinned 100-file public LibreOffice corpus opens and copies byte-for-byte; the narrow supported text-patch path retains every unrelated package member and refuses edits that fail its safety checks. This is preservation evidence, not a layout-fidelity corpus.
- A custom Qt page canvas and command registry for editing without an HTML or office-suite engine, presented as OwlDocs with a geometric Ubuntu-color owl and a compact writing toolbar.
- Default headers and footers are edited directly on the page: double-click a left, center or right region, or use Insert > Header & Footer, then type or paste a PNG/JPEG picture in place. Story/region choices, page fields and Close appear in the contextual ribbon below the writing bar, keeping the paper clear of floating controls. Close, Escape or a body click returns to ordinary writing. Inline text follows document zoom; ribbon controls follow application scaling. Changes repeat across pages. Single-line stories retain native tab stops; independently multiline or wrapped regions use a borderless three-cell layout. `{PAGE}` and `{PAGES}` become live page fields. Text and pictures participate in undo/redo and recovery and save as native header/footer parts and DrawingML relationships. Rich character formatting and section-specific first/even/linked variants remain future work.
- Coherent multi-document ingress: a successful first open replaces the untouched startup tab; File > Open, positional command-line arguments, the desktop file association, and file-manager drops all accept multiple documents. Drops accept existing local DOCX files with copy-only semantics while rejecting remote, unsupported, and move-only items. Equivalent paths and symlink aliases activate the already-open tab instead of duplicating it, and a symlink-opened document safely saves its resolved target. Open Recent is numbered, disambiguates duplicate basenames, exposes full-path tooltips, removes an unavailable item when selected, and provides Remove Missing Documents and Clear Recent Documents actions.
- On-canvas text editing with a stable black starting color, immediate color palettes, a non-modal live color picker, removable highlighting, word/visual-line mouse selection, one-second typing undo groups, wrapped-line caret navigation, undo/redo, ordinary paste plus destination-formatted Paste as Text Only (`Ctrl+Shift+V`), basic imported and authored character/paragraph formatting, page settings, correctly paginated authored and supported terminal hard page breaks, local English spellchecking that waits until the active word is completed, common shortcuts, recovery checkpoints, print preview and range/current-page printing, and PDF export with extractable text. A modeless Navigation pane provides debounced live search results and snippets across body paragraphs and semantic table cells, case-sensitive and whole-word matching, previous/next navigation with `Shift+F3`/`F3`, and single or all-match replacement; Replace All is one undoable editor transaction. Search state is retained per document tab, reads follow the active preview branch, and writes remain locked while a preview is active. A synchronized per-tab zoom slider lives at the lower right, supports 25–400%, and accepts high-resolution Control+wheel/touchpad input; `Ctrl++`, `Ctrl+=`, and `Ctrl+-` are not zoom bindings. Character formatting applied to an empty body paragraph is stored durably on its paragraph mark: it survives leaving and returning to the paragraph, deletion of the final text, every empty-list exit path, schema-v14 recovery, and—for properties represented by the current OOXML writer—native `w:pPr/w:rPr` save/reopen, then becomes the format of subsequently typed text. Explicit clears such as Bold off or No Highlight retain their intent through live editing and recovery. Bold off has a serializable OOXML representation; a null No Highlight override does not yet emit an explicit direct OOXML clear.
- Native paragraph styles for Normal, No Spacing, Title, Subtitle, Quote, and Heading 1–9 are available directly from the paragraph-style chooser. Normal follows the configured editor font and size; `Ctrl+Shift+N` applies Normal and `Ctrl+Alt+1/2/3` apply the first three headings. Pressing Enter after a heading starts its configured next style in the same undoable transaction, while Shift+Enter remains a soft line break. Style identity, inherited text/paragraph-mark baselines, and exact direct-format provenance survive editing, undo/redo, and schema-v14 recovery. Properties represented by the current OOXML writer retain their directness through supported DOCX save/reopen; language metadata and a null No Highlight override remain semantic/recovery-only. Changing a style replaces inherited formatting while preserving explicit run and paragraph overrides, including explicit false/clear values within the semantic model. Empty styled paragraphs retain their insertion appearance, and foreign built-in definitions are flattened only as needed to preserve visible formatting when a simplified copy must use OwlDocs' deterministic definitions. Imported custom style IDs remain identifiable but are not editable definitions, and named paragraph styles are not yet available inside table cells.
- A clean font-family picker that keeps every installed multilingual font available without Qt's appended Devanagari, Thai, Malayalam, Telugu, or other writing-system sample text. Each English family name previews its own typeface when that font supports Latin; script-only and symbol families stay legible in the normal UI font.
- Per-user editor defaults for font family, font size, and tab width (four space-equivalents by default), available from File > Options and applied to every open document canvas.
- List-aware editing with stable per-list identities, ten configurable levels, right-click List Properties, and a contextual List menu. Numbered levels cycle through `1, 2, 3`; `A, B, C`; `I, II, III`; `a, b, c`; and `i, ii, iii`, then repeat for levels 6–10. Hierarchical counters restart and resequence across Enter, multiline insertion, indentation, outdentation, and boundary deletion. Each list independently stores bullet positions and text gaps in space-equivalents; built-in defaults are 0/4/8/.../36 with a two-space gap, and wrapped or soft-broken text aligns to the same text stop. “Use as Defaults” in List Properties saves the current ten-level geometry for new bulleted and numbered lists.
- Semantic tables offer a hover insertion grid, editable cells, rectangular selection, Tab/Shift+Tab navigation, row/column insertion/deletion and body-block movement. Drag cell boundaries to change column widths or minimum row heights; the contextual Table row also has numeric sizes, Auto row height and distribution commands. A direct nine-position grid combines left/center/right with top/middle/bottom text positioning. Borders apply to the active cell or selected rectangle; Select Table targets the whole table. All, outside, inside, none and individual-edge presets use a chosen solid line weight and color. Normal character and paragraph controls continue to work on cell text, and 13 built-in table styles remain available. Supported geometry, direct borders, cell alignment, text and inline PNG/JPEG pictures survive undo/recovery/clipboard and native DOCX save/reopen. Merged, nested, ragged, multi-paragraph, equation-bearing and unsupported drawing-bearing tables remain outside the editable subset.
- Native desktop insertion and bounded high-quality screen rendering of PNG/JPEG DrawingML pictures. Interactive resizing and document zoom always repaint from the cached full-resolution decode; resizing changes only semantic geometry and never resamples or rewrites the encoded source. Picture Layout v1 supports inline placement plus deliberately constrained, canonical zero-offset `wp:anchor` representations for square and top-and-bottom wrapping. Move-with-text anchors use character/paragraph-relative origins; fixed anchors use page/page-relative origins. Anchored layouts retain four wrap distances, move-with-text versus fixed relation, and alt text. Inline distance values are preserved when present, but they are not visually applied and their controls are disabled. Pictures can be selected, copied, pasted, deleted, resized through Picture Size or three persistable on-canvas handles (right, bottom, and bottom-right), and configured through Picture Layout; source bytes, dimensions, supported layout, and alt text survive undo/redo, schema-v14 recovery, native DOCX save, and desktop reopen. Only matching safe internal imported `wp:inline` or canonical anchor shapes enter this editable path; other anchors remain opaque on preservation paths. Crop, rotation, z-order, arbitrary horizontal/vertical positioning, and additional wrap modes remain unsupported. Inline PNG/JPEG pictures in supported cells use the same picture lifecycle with floating layout disabled. The OOXML engine can also author multiple ordered sections and reopen their ranges, break kinds, and page geometry, but section authoring remains engine/API-only.
- Supported OOXML `docDefaults`, style `basedOn` cascades, theme fonts/colors, explicit false values, and native `numbering.xml` definitions resolve on import. Authored list levels 1–9 use `w:numPr` plus native numbering definitions rather than marker text, and their supported counters, level templates, suffixes, starts, and geometry reopen semantically. Because WordprocessingML permits nine native levels, OwlDocs' tenth editor level is saved as visually equivalent literal marker text with tab/hanging-indent geometry; OwlDocs recovers it as level 10 on reopen, while other editors do not see it as native list continuation.
- Native equations accept a bounded LaTeX subset and have an editable source dialog with live vector preview. Fractions, roots, scripts, sums/integrals/products, delimiters, matrices, aligned/cases rows, text/font commands, accents and spacing serialize to supported native Office Math (OMML). Editing an equation preserves its identity and formatting in one undoable operation. Arbitrary TeX programs, packages and full `.tex` documents are outside this workflow.
- Academic writing objects include plain-text footnotes/endnotes, numbered captions, heading/caption cross-references, manually authored numbered bibliography entries and citations, and a generated table of contents. Supported objects are native WordprocessingML notes/fields/bookmarks with cached results. A bounded, validated OwlDocs recovery registry retains editable semantic identities; externally altered or unrecognized academic structures remain on source-preserving paths. Citation formatting is numeric and manual, without reference-manager lookup or a CSL style engine.
- An optional `codex app-server` stdio client with runtime model/effort/service-tier discovery, streamed multi-turn chat, bounded document read/search tools, and live semantic edit previews. A completed turn re-enables the composer and the next message continues the same document thread. Document-mode instructions and tool descriptions explicitly direct Codex to use `set_text_style` for font/emphasis/color/highlight/script formatting and `set_paragraph_style` for styles/alignment/spacing instead of merely describing UI steps. The supported path explicitly advertises `capabilities.experimentalApi=true` during `initialize`, reads effective configuration, requires a recognized MCP-server map, disables every inherited external MCP server by name for both thread start and resume, and only then registers the `editor.v1` `dynamicTools`. App-server stderr tracing is discarded instead of being promoted into a chat failure; structured protocol, request, write, and process-exit errors remain visible. Fake-process tests enforce that sequence, multi-turn continuity, and fail-closed behavior, and an optional installed-runtime schema check verifies the fields used by the integration when Codex and Python are present. This surface is experimental: an absent or incompatible runtime disables optional AI behavior without preventing offline editing. Accepted previews are one undoable transaction.
- Multilingual conformance tests exercise shaping, grapheme-safe caret movement and selection, and visual-line wrapping for English, RTL Hebrew, CJK, and Indic text. Screen and searchable-PDF tests assign the same soft-flow markers to the same pages and verify that export consumes the existing canvas layout without rebuilding pagination.
- Native saves compare the complete source package with a prepared editable baseline before allowing regeneration. Supported authored rich documents remain normally savable after reopening; application insertion preferences do not replace saved document defaults. Complex originals are preserved when an explicit editable copy is needed. Content hashes detect real external changes without timestamp-only false conflicts, and failed or cancelled saves do not leak staged edits into later copies.
- SQLite recovery journals remain schema v14. Semantic checkpoints use version 16 when explicit table geometry or cell layout is present, version 15 for table-cell pictures alone, and version 14 otherwise. Older checkpoints remain readable. Journals retain the exact original DOCX in a checksummed binary store, so safe recovery does not depend on the original pathname still existing. Legacy semantic-only checkpoints remain recoverable as supported copies; missing or corrupt source data is diagnosed. Recovered documents save to a distinct destination.
- No application telemetry, update checks, remote fonts, cloud spelling, or background network requests. Opening the Codex panel connects to the installed runtime; model, reasoning and speed controls remain visible. Authentication may open the user's browser. Document context is sent when the user submits a message.

Image handling and list authoring remain deliberately bounded. Safe internal PNG/JPEG `wp:inline` drawings and the two canonical Picture Layout v1 anchor shapes are transport-neutral core atoms that occupy one U+FFFC position in paragraph text, so normal selection, replacement, deletion, split/merge, preview, revision, and undo rules determine their order. Each atom owns stable identity, original encoded bytes, format, accessible name, EMU geometry, placement, four wrap distances, and move/fixed relation; the Qt layer keeps only a decoded-pixel cache keyed by that identity. The supported square and top-and-bottom anchors are canonical zero-offset forms—character/paragraph-relative when moving with text and page/page-relative when fixed—not a general floating-positioning system. Inline distance values remain in the semantic record for fidelity but are neither visually applied nor editable in the current UI. Inputs are limited to 512 pictures, 16 MiB per encoded picture, and 32 MiB aggregate encoded picture data, with separate dimension, pixel-count, and decoded-cache limits. Clipboard and document state retain bytes and a bounded display name rather than arbitrary source paths. Three persistable resize handles (right, bottom, and bottom-right) and the ratio-lockable size dialog change dimensions, but crop, rotation, z-order, arbitrary positioning, tight/through/behind/in-front wrapping, remain unsupported; bounded inline PNG/JPEG images inside cells are supported. Raster container inspection runs before decode, but decoding still occurs in the GUI process and remains an explicit hardening gap. List commands toggle or convert whole selected paragraphs without duplicating markers; Enter continues a list, Shift+Enter adds a visible soft line break inside the same item, Tab/Shift+Tab changes its level, and an empty item exits. Simple rectangular supported text/picture imported tables and newly authored tables are real movable document blocks. Supported cell text, direct character formatting, paragraph formatting, structure, and built-in table styles are authored in native DOCX; equation-bearing cells or unsupported drawing-bearing cells and custom fills/padding, non-solid border styles, and other presentation details outside the supported layout subset remain preservation-only. Cell merging/splitting, nested or floating tables, multi-paragraph cells, exact row heights and splitting one row across pages remain unsupported. Equation support is limited to the built-in, non-executing LaTeX/OMML subset; arbitrary TeX, macros, packages, and unsupported Office Math constructs are not evaluated. Other complex imported objects are generally preserved in the original package but are not yet rendered or editable. Formatting or structural edits to an imported document may therefore require an explicitly confirmed, simplified Save As.

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
install -m 0644 dist/owl-docs_0.6.5_amd64.deb /tmp/owl-docs_0.6.5_amd64.deb
sudo apt install /tmp/owl-docs_0.6.5_amd64.deb
owl-docs
```

Ubuntu 22.04 packages Qt 6.2.4, which the source accepts for development. The roadmap targets pinned Qt 6.8.x release builds, but that release pipeline has not yet been established.

## Safety boundary

Macros, ActiveX, OLE packages, external relationships, and TeX commands are never executed by this build. Every GUI open is preflighted by a resource-limited, seccomp-restricted parser worker. Malformed XML, truncated ZIPs, and deterministic ZIP mutations are exercised through that child with bounded output and clean termination. The GUI then reparses the same held file descriptor—not a reopened pathname—to construct the preservation model; raster decoding and the remaining in-process parser surface are documented hardening gaps. Codex is requested to use an empty temporary working directory, read-only sandbox, and no approvals; this is defense in depth, not a complete OS security boundary because read-only mode can still permit reads. Registered `editor.v1` tools provide bounded semantic reads and isolated previews. Their dynamic-tool transport currently depends on the explicitly enabled experimental app-server schema described above and is checked against an installed runtime when available. A preview changes the live document only after the user accepts it.

# Owl Docs

Owl Docs is a Linux-first, offline word-processor prototype with an optional Codex chat panel. New documents are written directly as Office Open XML (`.docx`), and imported DOCX packages are handled without an HTML or ODF conversion step.

This repository is an early executable foundation for the longer interoperability roadmap. It does **not** yet pass every M0 gate or constitute a complete M1 release. See `docs/IMPLEMENTATION_STATUS.md` for the gate-by-gate status and `docs/COMPATIBILITY.md` for the current, deliberately conservative feature matrix.

## What is implemented in this foundation

- A C++20 document model with revision-checked transactions, undo/redo, stable node identifiers, UTF-16 positions, and isolated AI preview branches.
- A native OPC/DOCX package reader and writer using libzip and pugixml. Tested unchanged Save As operations preserve the original bytes exactly; the narrow supported text-patch path retains unrelated ZIP parts and refuses edits that fail its safety checks.
- A custom Qt page canvas and command registry for editing without an HTML or office-suite engine, presented as Owl Docs with a geometric Ubuntu-color owl and an icon-based ribbon.
- On-canvas text editing with a stable black starting color, immediate color palettes, a non-modal live color picker, removable highlighting, word/visual-line mouse selection, one-second typing undo groups, wrapped-line caret navigation, undo/redo, basic imported and authored character/paragraph formatting, page settings, correctly paginated authored and supported terminal hard page breaks, find/replace, local English spellchecking that waits until the active word is completed, common shortcuts, recovery checkpoints, print preview and range/current-page printing, and PDF export with extractable text.
- A clean font-family picker that keeps every installed multilingual font available without Qt's appended Devanagari, Thai, Malayalam, Telugu, or other writing-system sample text. Each English family name previews its own typeface when that font supports Latin; script-only and symbol families stay legible in the normal UI font.
- Per-user editor defaults for font family, font size, and tab width (four space-equivalents by default), available from File > Options and applied to every open document canvas.
- List-aware editing with stable per-list identities, ten configurable levels, right-click List Properties, and a contextual List ribbon tab. Numbered levels cycle through `1, 2, 3`; `A, B, C`; `I, II, III`; `a, b, c`; and `i, ii, iii`, then repeat for levels 6–10. Hierarchical counters restart and resequence across Enter, multiline insertion, indentation, outdentation, and boundary deletion. Each list independently stores bullet positions and text gaps in space-equivalents; built-in defaults are 0/4/8/.../36 with a two-space gap, and wrapped or soft-broken text aligns to the same text stop. “Use as Defaults” in List Properties saves the current ten-level geometry for new bulleted and numbered lists.
- Semantic tables with a single rows/columns/header dialog, a contextual Table ribbon, editable cells, rectangular one- or multi-cell selection, Tab/Shift+Tab cell navigation (including automatic row creation from the final cell), row/column insertion and deletion, and body-block reordering. The normal font, size, emphasis, color, highlight, and paragraph-alignment controls work on text within one cell or across the selected cell rectangle. Thirteen built-in table styles provide practical plain, grid, light, medium, banded, accent, and dark-header choices. Screen/PDF rendering, recovery snapshots, and native WordprocessingML table output retain the supported cell formatting, paragraph formatting, structure, and selected style. Bounded simple rectangular imported tables reopen as semantic tables and retain supported grid widths, table alignment, cell fills, borders, padding, vertical/paragraph alignment, mixed direct run formatting, and Word-compatible automatic row metrics.
- View-only rendering of the bounded safe subset of internally related raster `wp:inline` DrawingML images at their declared OOXML extents, with oversized images reduced to the available page width.
- Native equation atoms parsed from a deliberately safe LaTeX subset, laid out as vector math on the page and in PDF, and serialized to/reopened from the corresponding Office Math (OMML) subset.
- An optional `codex app-server` stdio client with runtime model/effort/service-tier discovery, streamed chat, and live bounded document-read and preview tools. Accepted previews are one undoable transaction.
- No application telemetry, update checks, remote fonts, cloud spelling, or background network requests. The Codex process is launched only after the user enables chat; authentication may open the user's browser.

Imported images and list authoring remain deliberately limited. Safe, bounded, internally related raster `wp:inline` DrawingML images render at their OOXML extents, subject to proportional reduction to the available page width, but remain view-only. The Picture command still creates a filename placeholder; Owl Docs does not yet author embedded images, and it does not support floating/anchored positioning, text wrapping, crop, rotation, or image editing. Raster decoding still occurs in the GUI process and remains an explicit hardening gap. List semantics are managed inside Owl Docs but save as ordinary text with paragraph tab stops and hanging indents rather than WordprocessingML numbering definitions. Owl Docs recognizes that emitted pattern on reopen so supported custom list geometry does not accumulate indentation. List commands toggle or convert whole selected paragraphs without duplicating markers; Enter continues a list, Shift+Enter adds a visible soft line break inside the same item, Tab/Shift+Tab changes its level, and an empty item exits. Simple rectangular imported tables and newly authored tables are real movable document blocks. Supported cell text, direct character formatting, paragraph formatting, structure, and built-in table styles are authored in native DOCX; more complex imported widths, custom fills/borders/padding, and other presentation details can remain preservation-only when they fall outside that subset. Cell merging/splitting, manual row or column sizing, nested tables, floating placement, and other advanced table behavior are not yet implemented. Equation support is limited to the built-in, non-executing LaTeX/OMML subset; arbitrary TeX, macros, packages, and unsupported Office Math constructs are not evaluated. Other complex imported objects are generally preserved in the original package but are not yet rendered or editable. Formatting or structural edits to an imported document may therefore require an explicitly confirmed, simplified Save As.

## Build on Ubuntu 22.04

Install the current foundation's dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  libzip-dev libpugixml-dev libsqlite3-dev \
  libhunspell-dev hunspell-en-us libseccomp-dev nlohmann-json3-dev \
  fonts-crosextra-carlito zip unzip poppler-utils
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
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-release
ctest --test-dir build-release --output-on-failure
cpack --config build-release/CPackConfig.cmake -G DEB -B dist
install -m 0644 dist/owl-docs_0.1.9_amd64.deb /tmp/owl-docs_0.1.9_amd64.deb
sudo apt install /tmp/owl-docs_0.1.9_amd64.deb
owl-docs
```

Ubuntu 22.04 packages Qt 6.2.4, which the source accepts for development. The roadmap targets pinned Qt 6.8.x release builds, but that release pipeline has not yet been established.

## Safety boundary

Macros, ActiveX, OLE packages, external relationships, and TeX commands are never executed by this build. Every GUI open is preflighted by a resource-limited, seccomp-restricted parser worker. The GUI then reparses the same held file descriptor—not a reopened pathname—to construct the preservation model; the remaining in-process parser surface is documented as a hardening gap. Codex is requested to use an empty temporary working directory, read-only sandbox, and no approvals; this is defense in depth, not a complete OS security boundary because read-only mode can still permit reads. Registered `editor.v1` tools provide bounded semantic reads and isolated previews. A preview changes the live document only after the user accepts it.

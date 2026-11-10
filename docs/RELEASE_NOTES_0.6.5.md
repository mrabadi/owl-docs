# OwlDocs 0.6.5

Codex uses native document tools to author the same supported document objects
as the editor. The live capability catalog supplies operation schemas, bounds,
examples and document context; built-in instructions guide discovery, reading
stable identities and preparing a proposal. No separately installed skill is
required.

- Create editable Excalidraw figures locally in Professional mode. Revise a
  figure or patch selected native elements while retaining its embedded scene.
- Fit cell figures to the final proposed column geometry and page margins,
  preserving aspect ratio and consistent stored DOCX dimensions.
- Create and edit tables, cells, row/column structure, geometry, alignment,
  borders and supported styles. Insert PNG/JPEG pictures in body text, cells,
  headers or footers; change picture size, wrapping and accessible text.
- Edit header/footer regions and page fields, body paragraph structure,
  supported paragraph/character formatting, lists and single-section page
  settings. A combined content/layout proposal is one acceptance and one Undo.
- Author and edit supported equations, notes, captions, cross-references,
  manual citations, bibliography entries and tables of contents.
- Read tables, stories, images, metadata and authored formatting independently,
  with pagination for large structural reads. Selection reads identify the
  active cell rectangle or header/footer region.
- Reject stale document/layout context, invalid inputs and oversized requests
  without changing the live document. Figure rendering and tool dispatch stop
  safely when their document or connection closes.

Edits still produce an isolated preview. Accept or discard it in OwlDocs;
acceptance does not save the file. Saving, opening/exporting files, application
preferences and unsupported Word features remain manual. The tool catalog
states these limits rather than offering an always-denied file capability.

The owl logo, Harvill styles, compact contextual ribbon and supported DOCX
preservation/recovery paths remain in place. See [Codex authoring](CODEX_AUTHORING.md),
[compatibility](COMPATIBILITY.md), [TDD evidence](TDD_CODEX_AUTHORING_0.6.5.md)
and the [release validation record](RELEASE_VALIDATION_0.6.5.md).

## Install

Close OwlDocs, then run:

```sh
cd /home/rabadi/Documents/ChatGPT/word
install -m 0644 dist/owl-docs_0.6.5_amd64.deb /tmp/owl-docs_0.6.5_amd64.deb
sudo apt install /tmp/owl-docs_0.6.5_amd64.deb
```

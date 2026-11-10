# Owl Docs 0.6.0

This release refreshes the everyday workspace while preserving the existing owl logo, offline DOCX editing, local Excalidraw figures and optional Codex integration.

## Workspace

- ApeTek-inspired warm neutral surfaces and forest accents, with a compact writing toolbar and contextual object controls.
- Document header actions and searchable commands with categories, keywords and displayed shortcuts.
- Secondary formatting under More, responsive controls, and clearer optional Codex settings.
- Layout controls expose margins, paper size, orientation, spacing and headers/footers; document status follows the active tab's saved state.
- Unimplemented commands are removed from normal authoring menus instead of appearing to work.

## Writing and equations

- Academic primitives include plain-text footnotes/endnotes, numbered captions, heading/caption references, manually authored numeric bibliography/citations and a generated TOC.
- Supported DOCX output uses native notes, fields and bookmarks with cached display results. Editable semantic identities have a bounded, validated local registry; unknown foreign structures stay on preservation paths.
- An equation source editor provides live vector preview and updates existing equations without replacing their identity or formatting.
- The safe LaTeX/OMML subset adds text/font/operator commands, accents, products, spacing, aligned and cases. Arbitrary TeX execution and complete `.tex` document editing are outside this workflow.

## Save and recovery fixes

- Complete prepared-package coverage replaces the paragraph-only regeneration assumption for supported rich documents. Document defaults remain independent of application insertion preferences.
- Conversion saves an explicit editable copy while preserving complex originals, aliases and existing hard links.
- Timestamp-only changes no longer cause false external-version conflicts. Destination content is checked again after candidate validation and before commit.
- Cancelled partial patch attempts cannot leak into a later unchanged Save As.
- A committed save with an unsuccessful directory durability sync is reported as saved with a warning; it is not falsely reported as a failed write.
- Recovery retains original DOCX bytes in a private, transactional, checksummed SQLite store. Safe text recovery preserves opaque source data even if the original pathname has disappeared. Legacy journals remain readable as supported copies.

## Validation and remaining boundaries

The repository includes focused executed RED/GREEN evidence in `TDD_SAVE_EVIDENCE.md`, `TDD_ACADEMIC_EVIDENCE.md`, `TDD_MATH_EVIDENCE.md`, `TDD_LAYOUT_EVIDENCE.md` and `TDD_WORKSPACE_EVIDENCE.md`. Release validation also exercises the pinned public DOCX preservation corpus and native package checks.

The clean strict Release build passed all 49 tests. See `RELEASE_VALIDATION_0.6.0.md` for the public corpus, native LibreOffice field-refresh and real bundled-renderer checks.

General foreign field editing, rich note stories, a citation-style engine, arbitrary picture positioning, complex tables, multiple-section desktop authoring, comments/tracked changes, full accessibility, and broad cross-producer layout parity remain outside the implemented subset. The remaining M1/M2 hardening and release-engineering gates are tracked separately in `ROADMAP.md`.

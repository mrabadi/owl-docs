# OwlDocs 0.6.1

This feedback update standardizes the visible name as **OwlDocs**, keeps the
existing owl artwork, and introduces a neutral document style hierarchy.
The existing application settings, chat history, recovery storage and desktop
association retain their original identifiers.

New documents use a black 28-point Title, a muted regular Subtitle, a gray
italic Quote and black headings with clearer sizes and spacing. The heading
levels remain available for outlines, cross-references and generated contents.
Documents authored by 0.6.0 keep their original style definitions. Ordinary
layout edits save without a false conversion prompt, and applying a new heading
keeps its current appearance when the older document is saved and reopened.
Imported and recovered documents also use their stored font defaults for
display and pagination, even when the current new-document preferences differ.

The Codex controller prefers `gpt-6.1-sol` when the connected account offers it.
Its reasoning choices follow the supported model capabilities in the order
Light, Medium, High, XHigh and Ultra. Existing connections are reused.
Opening the Codex panel connects automatically; model, reasoning and speed
choices stay visible. Document messages still require pressing Send.

The original owl and all menus share one header row. Paragraph style selection
and alignment controls are directly available, and expanded list controls keep
their layout in a narrow window. Default headers and footers use double-click
entry, direct page-field controls and Close Header and Footer, with Escape or
a body click returning to ordinary writing. They remain a plain-text and
bounded-picture subset with native page fields, rather than rich text stories.
Independent multiline or wrapped regions use a native borderless three-cell
layout, with top-aligned header regions and bottom-aligned footer regions. The
Canvas uses the same supported 36-point edge distances as native DOCX output.
Original single-line parts keep their existing representation; older sequential
multiline or wrapped parts retain their source through unchanged copies and safe body-text
patches, with a supported-copy boundary for incompatible layout changes.

The scope and conservative DOCX preservation rules in `COMPATIBILITY.md`
continue to apply. This release does not introduce a TeX runtime, general Word
field editing, tracked changes or comments.

Executed regression evidence is recorded in `TDD_FEEDBACK_0.6.1.md`; the final
release validation record is prepared only after source freeze and the complete
Release, corpus and package checks.

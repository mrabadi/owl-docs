# OwlDocs 0.6.4

Table controls occupy one compact contextual row below the writing bar.

- Insert and Delete group row/column commands into labeled dropdowns.
- Alignment opens the existing nine-position grid; choosing a position closes
  it. Mixed and selected-cell alignment states remain synchronized.
- Size holds column width, minimum row height, and distribution commands.
  Width receives keyboard focus when the chooser opens; Auto and Mixed states
  remain available. Committing a changed size is one edit.
- Borders groups line weight, the color palette, and border presets. Choosing
  a weight or color does not apply it until a preset is selected.
- Select Table and Table Styles remain available. More appears only when
  available width requires overflow; controls return as the window widens.
- Leaving the table or changing to another object context closes its menus.

Table dragging, undo/redo, DOCX preservation, header/footer ribbon editing,
the owl artwork, Harvill styles, LaTeX, Excalidraw Local and Codex remain
available. The supported table/document subset in `COMPATIBILITY.md` is
unchanged; this is a table interface update rather than expanded Word parity.

See `TDD_TABLE_RIBBON_0.6.4.md` and `RELEASE_VALIDATION_0.6.4.md` for verification.

## Install

Close OwlDocs, then run:

```sh
cd /home/rabadi/Documents/ChatGPT/word
install -m 0644 dist/owl-docs_0.6.4_amd64.deb /tmp/owl-docs_0.6.4_amd64.deb
sudo apt install /tmp/owl-docs_0.6.4_amd64.deb
```

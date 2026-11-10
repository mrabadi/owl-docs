# OwlDocs 0.6.3

This update puts header/footer controls in the ribbon and adds direct table
layout editing. The owl artwork, styles, equations, local Excalidraw and Codex
workflows are retained.

- Header/Footer, region selection, page fields and Close appear below the
  writing bar while editing a story. The controls no longer cover the paper.
- Drag table boundaries to adjust column widths and minimum row heights, or
  use numeric controls and Distribute Columns/Rows. Auto row height remains
  available. A completed drag is one undo step; cancellation leaves the model
  unchanged.
- Place cell text with a direct nine-position alignment grid. Formatting
  applies to the active cell or selected rectangle; Select Table targets the
  whole table.
- Apply all, outside, inside, none or individual-edge borders with a chosen
  solid color and line weight. Shared edges update together.
- Geometry, borders and cell positioning survive clipboard, recovery and
  native DOCX save/reopen/edit/save. Exact package verification remains required
  before rewriting an imported document. Existing 0.6.2 tables retain their
  compatible automatic representation and picture identities.
- Semantic recovery uses version 16 only for explicit table geometry or cell
  layout, version 15 for cell pictures alone and version 14 otherwise. SQLite
  journal and existing settings/storage identities remain unchanged.

Tables remain bounded rectangular grids. Merged, nested, floating or
multi-paragraph tables, exact row heights, row splitting across pages,
equation-bearing cells, non-solid borders and arbitrary fill/padding authoring
remain unsupported. Complex source documents retain their preservation paths.

See `COMPATIBILITY.md`, `RELEASE_VALIDATION_0.6.3.md` and the included TDD
evidence for the tested scope. Earlier release artifacts remain available.

## Install

```sh
cd /home/rabadi/Documents/ChatGPT/word
sudo apt install ./dist/owl-docs_0.6.3_amd64.deb
```

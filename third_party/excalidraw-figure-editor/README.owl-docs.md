# Owl Docs figure editor

This directory is the pinned, source-level integration of the local Excalidraw
fork at commit `d2f65b9b930d9d727cfaec11c8b7123c771452b2`. It is built and shipped inside
the Owl Docs package; end users do not install Excalidraw separately.

The helper accepts only an Owl Docs-created, permission-0700 session directory.
It reads `request.json` and returns an editable `.excalidraw` scene plus a PNG
preview. Chromium networking and permission requests are disabled. No HTTP
listener, file picker, recent-file list, or independent document store exists
in this integration mode.

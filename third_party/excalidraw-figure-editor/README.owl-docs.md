# Owl Docs figure editor

This directory is the pinned, source-level integration of the local Excalidraw
fork at commit `6609200dd9a756410a4199fe6c06ab3ce9fbcab0`. It is built and shipped inside
the Owl Docs package; end users do not install Excalidraw separately.

The helper accepts only an Owl Docs-created, permission-0700 session directory.
It reads `request.json` and returns an editable `.excalidraw` scene plus a PNG
preview. Chromium networking and permission requests are disabled. No HTTP
listener, file picker, recent-file list, or independent document store exists
in this integration mode.

#!/usr/bin/env python3
"""Verify the installed Codex runtime exposes Owl Docs' pinned protocol fields."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as source:
        return json.load(source)


def require_property(schema: dict, *path: str) -> dict:
    value: object = schema
    for component in path:
        if not isinstance(value, dict) or component not in value:
            raise RuntimeError(
                "installed Codex app-server schema is missing "
                + ".".join(path)
            )
        value = value[component]
    if not isinstance(value, dict):
        raise RuntimeError(
            "installed Codex app-server schema has an invalid "
            + ".".join(path)
        )
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--codex", required=True, type=Path)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="owl-docs-codex-schema-") as root:
        output = Path(root)
        completed = subprocess.run(
            [
                str(arguments.codex),
                "app-server",
                "generate-json-schema",
                "--experimental",
                "--out",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                "could not generate the installed app-server schema: "
                + completed.stderr.strip()
            )

        initialize = load_json(output / "v1" / "InitializeParams.json")
        experimental = require_property(
            initialize,
            "definitions",
            "InitializeCapabilities",
            "properties",
            "experimentalApi",
        )
        if experimental.get("type") != "boolean":
            raise RuntimeError("experimentalApi is no longer a boolean capability")

        thread_start = load_json(output / "v2" / "ThreadStartParams.json")
        dynamic_tools = require_property(
            thread_start, "properties", "dynamicTools"
        )
        field_types = dynamic_tools.get("type")
        if not (
            field_types == "array"
            or isinstance(field_types, list)
            and "array" in field_types
        ):
            raise RuntimeError("dynamicTools is no longer an array field")
        require_property(thread_start, "definitions", "DynamicToolSpec")

        server_requests = load_json(output / "ServerRequest.json")
        if '\"item/tool/call\"' not in json.dumps(
            server_requests, separators=(",", ":")
        ):
            raise RuntimeError(
                "installed Codex app-server schema has no item/tool/call request"
            )

    print("Installed Codex app-server supports experimental dynamic tools")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

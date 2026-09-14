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


def permits_type(field: dict, expected: str) -> bool:
    field_types = field.get("type")
    if field_types == expected:
        return True
    if isinstance(field_types, list) and expected in field_types:
        return True
    alternatives = field.get("anyOf")
    return isinstance(alternatives, list) and any(
        isinstance(alternative, dict)
        and permits_type(alternative, expected)
        for alternative in alternatives
    )


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
        if not permits_type(dynamic_tools, "array"):
            raise RuntimeError("dynamicTools is no longer an array field")
        require_property(thread_start, "definitions", "DynamicToolSpec")

        start_config = require_property(
            thread_start, "properties", "config"
        )
        if not permits_type(start_config, "object"):
            raise RuntimeError("thread/start config is no longer an object field")

        thread_resume = load_json(output / "v2" / "ThreadResumeParams.json")
        resume_config = require_property(
            thread_resume, "properties", "config"
        )
        if not permits_type(resume_config, "object"):
            raise RuntimeError("thread/resume config is no longer an object field")

        config_read_params = load_json(output / "v2" / "ConfigReadParams.json")
        include_layers = require_property(
            config_read_params, "properties", "includeLayers"
        )
        if not permits_type(include_layers, "boolean"):
            raise RuntimeError("config/read includeLayers is no longer a boolean field")

        config_read_response = load_json(output / "v2" / "ConfigReadResponse.json")
        require_property(config_read_response, "properties", "config")

        client_requests = load_json(output / "ClientRequest.json")
        if '\"config/read\"' not in json.dumps(
            client_requests, separators=(",", ":")
        ):
            raise RuntimeError(
                "installed Codex app-server schema has no config/read request"
            )

        server_requests = load_json(output / "ServerRequest.json")
        if '\"item/tool/call\"' not in json.dumps(
            server_requests, separators=(",", ":")
        ):
            raise RuntimeError(
                "installed Codex app-server schema has no item/tool/call request"
            )

    print(
        "Installed Codex app-server supports experimental dynamic tools "
        "and restricted thread config discovery"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Compile, validate, and serialize native-render shaders."""

from __future__ import annotations

import struct
import subprocess
from pathlib import Path

from shader_manifest import Shader
from shader_toolchain import require_tool

REGEN_COMMAND = "uv run --frozen python tools/render/build_shaders.py"


def render_header(shader: Shader, binary: bytes) -> str:
    if len(binary) % 4:
        raise ValueError(f"{shader.source}: SPIR-V size is not a multiple of four")
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    rows = [
        ",".join(f"0x{word:08x}" for word in words[index : index + 4])
        for index in range(0, len(words), 4)
    ]
    return (
        f"// Auto-generated from {Path(shader.source).name} by glslc — DO NOT EDIT. "
        f"Regenerate: {REGEN_COMMAND}\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        f"static const uint32_t {shader.symbol}[] = {{" + ",\n".join(rows) + "};\n"
    )


def compile_header(shader: Shader, repo: Path, build_dir: Path) -> str:
    glslc = require_tool("glslc")
    validator = require_tool("spirv-val")
    source = repo / shader.source
    if not source.is_file():
        raise RuntimeError(f"shader source is missing: {shader.source}")
    build_dir.mkdir(parents=True, exist_ok=True)
    output = build_dir / f"{Path(shader.header).stem}.spv"
    subprocess.run(
        [
            glslc,
            f"-fshader-stage={shader.stage}",
            "--target-env=vulkan1.0",
            "-O",
            str(source),
            "-o",
            str(output),
        ],
        cwd=repo,
        check=True,
    )
    subprocess.run(
        [validator, "--target-env", "vulkan1.0", str(output)],
        cwd=repo,
        check=True,
    )
    return render_header(shader, output.read_bytes())


def header_matches(path: Path, expected: str) -> bool:
    return path.is_file() and path.read_text() == expected

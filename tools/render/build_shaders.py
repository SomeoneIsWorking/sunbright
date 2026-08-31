#!/usr/bin/env python3
"""Build and validate every tracked SDL3-GPU SPIR-V header.

Run without arguments to regenerate, or with --check to refuse stale generated headers. Temporary
compiler output stays under the repository's scratch/ tree.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Shader:
    source: str
    stage: str
    symbol: str
    header: str


SHADERS = (
    Shader(
        "sms-recomp/runtime/shaders/geom.vert.glsl",
        "vert",
        "kGeomVertSpv",
        "sms-recomp/runtime/shaders/geom_vert_spv.h",
    ),
    Shader(
        "sms-recomp/runtime/shaders/geom.frag.glsl",
        "frag",
        "kGeomFragSpv",
        "sms-recomp/runtime/shaders/geom_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/picture.vert.glsl",
        "vert",
        "kPictureVertSpv",
        "native-render/shaders/picture_vert_spv.h",
    ),
    Shader(
        "native-render/shaders/picture.frag.glsl",
        "frag",
        "kPictureFragSpv",
        "native-render/shaders/picture_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/solid_rectangle.frag.glsl",
        "frag",
        "kSolidRectangleFragSpv",
        "native-render/shaders/solid_rectangle_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model.vert.glsl",
        "vert",
        "kModelVertSpv",
        "native-render/shaders/model_vert_spv.h",
    ),
    Shader(
        "native-render/shaders/model_color.frag.glsl",
        "frag",
        "kModelColorFragSpv",
        "native-render/shaders/model_color_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_texture.frag.glsl",
        "frag",
        "kModelTextureFragSpv",
        "native-render/shaders/model_texture_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_lit_alpha_mask.frag.glsl",
        "frag",
        "kModelLitAlphaMaskFragSpv",
        "native-render/shaders/model_lit_alpha_mask_frag_spv.h",
    ),
)


def header(shader: Shader, binary: bytes) -> str:
    if len(binary) % 4:
        raise RuntimeError(f"{shader.source}: SPIR-V size is not a multiple of four")
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    rows = [
        ",".join(f"0x{word:08x}" for word in words[index : index + 4])
        for index in range(0, len(words), 4)
    ]
    return (
        f"// Auto-generated from {Path(shader.source).name} by glslc — DO NOT EDIT. "
        "Regenerate: python3 tools/render/build_shaders.py\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        f"static const uint32_t {shader.symbol}[] = {{"
        + ",\n".join(rows)
        + "};\n"
    )


def build(shader: Shader, directory: Path) -> str:
    source = REPO / shader.source
    output = directory / (Path(shader.source).name + ".spv")
    subprocess.run(
        [
            "glslc",
            f"-fshader-stage={shader.stage}",
            "--target-env=vulkan1.0",
            "-O",
            str(source),
            "-o",
            str(output),
        ],
        cwd=REPO,
        check=True,
    )
    subprocess.run(
        ["spirv-val", "--target-env", "vulkan1.0", str(output)], cwd=REPO, check=True
    )
    return header(shader, output.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    scratch = REPO / "scratch"
    scratch.mkdir(exist_ok=True)
    stale = []
    with tempfile.TemporaryDirectory(prefix="shader-build-", dir=scratch) as temporary:
        directory = Path(temporary)
        for shader in SHADERS:
            expected = build(shader, directory)
            destination = REPO / shader.header
            if args.check:
                if not destination.is_file() or destination.read_text() != expected:
                    stale.append(shader.header)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(expected)
                print(f"wrote {shader.header}")
    if stale:
        for path in stale:
            print(f"stale shader header: {path}")
        print("run: python3 tools/render/build_shaders.py")
        return 1
    if args.check:
        print(f"shader-build: {len(SHADERS)} tracked shaders compile, validate, and match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

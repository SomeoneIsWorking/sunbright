#!/usr/bin/env python3
"""Encode non-ASCII C/C++ literal contents as Shift-JIS byte escapes.

Clang deliberately does not implement GCC's ``-fexec-charset`` option.  The
decomp sources are UTF-8, but the retail game's serialized names are Shift-JIS
and its code compares those byte strings directly.  This tool generates the
native-Clang source tree without changing the authoritative decomp sources.

Only ordinary string and character literals are transformed.  Comments and
all other source text remain UTF-8.  Fixed-width three-digit octal escapes are
used so a following hexadecimal or octal digit cannot extend an escape.
Non-ASCII raw strings are rejected because rewriting their contents would
change the raw-string contract.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys
import tempfile


class TransformError(ValueError):
    """The input cannot be transformed without changing C++ semantics."""


def _encoded_character(character: str, *, source_name: str, line: int) -> str:
    try:
        encoded = character.encode("shift_jis")
    except UnicodeEncodeError as error:
        raise TransformError(
            f"{source_name}:{line}: character {character!r} is not representable "
            "in Shift-JIS"
        ) from error
    return "".join(f"\\{byte:03o}" for byte in encoded)


def _raw_literal_end(source: str, quote: int, *, source_name: str) -> int | None:
    """Return the byte after a raw literal, or None for an ordinary quote."""
    prefix_start = quote - 1
    if prefix_start < 0 or source[prefix_start] != "R":
        return None

    delimiter_end = source.find("(", quote + 1, quote + 18)
    if delimiter_end < 0:
        raise TransformError(
            f"{source_name}:{source.count(chr(10), 0, quote) + 1}: malformed raw literal"
        )
    delimiter = source[quote + 1 : delimiter_end]
    if any(character.isspace() or character in "()\\" for character in delimiter):
        raise TransformError(
            f"{source_name}:{source.count(chr(10), 0, quote) + 1}: invalid raw-literal delimiter"
        )
    terminator = ")" + delimiter + '"'
    end = source.find(terminator, delimiter_end + 1)
    if end < 0:
        raise TransformError(
            f"{source_name}:{source.count(chr(10), 0, quote) + 1}: unterminated raw literal"
        )
    return end + len(terminator)


def transform(source: str, *, source_name: str = "<input>") -> str:
    """Return source with non-ASCII ordinary-literal contents byte-encoded."""
    output: list[str] = []
    index = 0
    line = 1
    state = "code"
    quote = ""

    while index < len(source):
        character = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "code":
            if character == "/" and following == "/":
                output.extend((character, following))
                index += 2
                state = "line-comment"
                continue
            if character == "/" and following == "*":
                output.extend((character, following))
                index += 2
                state = "block-comment"
                continue
            if character == '"':
                raw_end = _raw_literal_end(source, index, source_name=source_name)
                if raw_end is not None:
                    raw_literal = source[index:raw_end]
                    if not raw_literal.isascii():
                        raise TransformError(
                            f"{source_name}:{line}: non-ASCII raw literal cannot be "
                            "execution-charset transformed safely"
                        )
                    output.append(raw_literal)
                    line += raw_literal.count("\n")
                    index = raw_end
                    continue
                state = "literal"
                quote = character
            elif character == "'":
                state = "literal"
                quote = character
            output.append(character)
            index += 1
            continue

        if state == "line-comment":
            output.append(character)
            index += 1
            if character == "\n":
                line += 1
                state = "code"
            continue

        if state == "block-comment":
            output.append(character)
            index += 1
            if character == "\n":
                line += 1
            elif character == "*" and following == "/":
                output.append(following)
                index += 1
                state = "code"
            continue

        if character == "\\":
            output.append(character)
            index += 1
            if index >= len(source):
                raise TransformError(f"{source_name}:{line}: dangling literal escape")
            escaped = source[index]
            output.append(escaped)
            index += 1
            if escaped == "\n":
                line += 1
            continue
        if character == quote:
            output.append(character)
            index += 1
            state = "code"
            continue
        if character == "\n":
            raise TransformError(f"{source_name}:{line}: newline in ordinary literal")
        if character.isascii():
            output.append(character)
        else:
            output.append(_encoded_character(character, source_name=source_name, line=line))
        index += 1

    if state == "literal":
        raise TransformError(f"{source_name}:{line}: unterminated ordinary literal")
    if state == "block-comment":
        raise TransformError(f"{source_name}:{line}: unterminated block comment")
    return "".join(output)


def _absolute(path: Path) -> Path:
    return Path(os.path.abspath(path))


def _require_scoped_path(path: Path, root: Path, *, label: str) -> tuple[Path, Path]:
    absolute_path = _absolute(path)
    absolute_root = _absolute(root)
    try:
        relative = absolute_path.relative_to(absolute_root)
    except ValueError as error:
        raise TransformError(f"{label} {absolute_path} is outside {absolute_root}") from error
    if not relative.parts:
        raise TransformError(f"{label} must not be the root itself: {absolute_root}")
    return absolute_path, absolute_root


def _reject_symlink_parents(output_path: Path, output_root: Path) -> None:
    absolute_output, absolute_root = _require_scoped_path(
        output_path, output_root, label="output"
    )
    current = absolute_root
    if current.is_symlink():
        raise TransformError(f"output root is a symlink: {current}")
    for component in absolute_output.relative_to(absolute_root).parts[:-1]:
        current /= component
        if current.is_symlink():
            raise TransformError(
                f"refusing to write through staged-tree symlink {current}; "
                "reconfigure to migrate the old build tree"
            )


def prepare_roots(build_root: Path, stage_roots: list[Path]) -> None:
    """Remove only stale staged trees that contain the retired symlink layout."""
    for stage_root in stage_roots:
        absolute_stage, _ = _require_scoped_path(
            stage_root, build_root, label="staging root"
        )
        if not absolute_stage.exists() and not absolute_stage.is_symlink():
            continue
        contains_symlink = absolute_stage.is_symlink()
        if absolute_stage.is_dir() and not contains_symlink:
            for directory, directories, files in os.walk(absolute_stage):
                base = Path(directory)
                if any((base / entry).is_symlink() for entry in directories + files):
                    contains_symlink = True
                    break
        if not contains_symlink:
            continue
        if absolute_stage.is_symlink():
            absolute_stage.unlink()
        else:
            shutil.rmtree(absolute_stage)
        print(f"encode_sjis_literals: removed retired symlink stage {absolute_stage}")


def transform_file(input_path: Path, output_path: Path, output_root: Path) -> None:
    _reject_symlink_parents(output_path, output_root)
    source = input_path.read_text(encoding="utf-8")
    transformed = transform(source, source_name=str(input_path))
    encoded = transformed.encode("utf-8")
    if output_path.exists() and output_path.read_bytes() == encoded:
        # Satisfy build systems after the transformer itself changes but its
        # output does not. Otherwise the older output remains perpetually
        # stale and every incremental build reruns every transform command.
        os.utime(output_path, None)
        return

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output_path.parent, delete=False) as temporary:
        temporary.write(encoded)
        temporary_path = Path(temporary.name)
    os.replace(temporary_path, output_path)


def selftest() -> None:
    def require(condition: bool, label: str) -> None:
        if not condition:
            raise RuntimeError(f"Shift-JIS transformer control failed: {label}")

    phrase = "ステージ毎シナリオアーカイブ名群"
    source = (
        '// コメント "日本語"\n'
        'const char* ascii = "plain\\\" text";\n'
        f'const char* name = u8"{phrase}7";\n'
        "int token = '日'; /* 日本語 */\n"
        'const char* raw = R"tag(ascii only)tag";\n'
    )
    result = transform(source, source_name="control.cpp")
    phrase_escapes = "".join(f"\\{byte:03o}" for byte in phrase.encode("shift_jis"))

    require('// コメント "日本語"' in result, "line comment changed")
    require('/* 日本語 */' in result, "block comment changed")
    require('"plain\\\" text"' in result, "ASCII literal changed")
    require(f'u8"{phrase_escapes}7"' in result, "non-ASCII literal was not escaped")
    require(phrase.encode("utf-8") != phrase.encode("shift_jis"), "encoding control is degenerate")
    literal = result.split('u8"', 1)[1].split('"', 1)[0][:-1]
    decoded_bytes = bytes(int(literal[offset + 1 : offset + 4], 8)
                          for offset in range(0, len(literal), 4))
    require(decoded_bytes == phrase.encode("shift_jis"), "escaped bytes are not Shift-JIS")

    try:
        transform('const char* value = R"(日本語)";\n', source_name="raw.cpp")
    except TransformError as error:
        require("non-ASCII raw literal" in str(error), "raw-literal failure named the wrong cause")
    else:
        raise AssertionError("non-ASCII raw-string negative control was accepted")

    try:
        transform('const char* value = "😀";\n', source_name="emoji.cpp")
    except TransformError as error:
        require("not representable" in str(error), "unrepresentable failure named the wrong cause")
    else:
        raise AssertionError("unrepresentable-character negative control was accepted")

    scratch_root = Path.cwd() / "scratch" / "selftests"
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch_root) as directory:
        control_root = Path(directory)
        source_path = control_root / "source.cpp"
        output_root = control_root / "build" / "stage"
        source_path.write_text('const char* value = "日本語";\n', encoding="utf-8")
        linked_parent = output_root / "linked"
        linked_parent.parent.mkdir(parents=True)
        linked_parent.symlink_to(control_root)
        try:
            transform_file(source_path, linked_parent / "escape.cpp", output_root)
        except TransformError as error:
            require("refusing to write through staged-tree symlink" in str(error),
                    "symlink failure named the wrong cause")
        else:
            raise AssertionError("staged-parent symlink negative control was accepted")

        prepare_roots(control_root / "build", [output_root])
        require(not output_root.exists(), "stale staged symlink survived cleanup")

    print("encode_sjis_literals self-test: PASS")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path)
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--prepare-root", action="append", default=[], type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.prepare_root:
        if args.build_root is None:
            parser.error("--build-root is required with --prepare-root")
        if args.input is not None or args.output is not None:
            parser.error("input/output cannot be combined with --prepare-root")
    elif not args.selftest and (
        args.input is None or args.output is None or args.output_root is None
    ):
        parser.error(
            "input, output and --output-root are required unless --selftest is used"
        )
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.selftest:
        selftest()
        return 0
    if args.prepare_root:
        try:
            prepare_roots(args.build_root, args.prepare_root)
        except (OSError, TransformError) as error:
            print(f"encode_sjis_literals: {error}", file=sys.stderr)
            return 1
        return 0
    try:
        transform_file(args.input, args.output, args.output_root)
    except (OSError, UnicodeError, TransformError) as error:
        print(f"encode_sjis_literals: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

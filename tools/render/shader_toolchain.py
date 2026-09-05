#!/usr/bin/env python3
"""Provision Sunbright's exact host shader compiler and validator from source."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from dataclasses import asdict, dataclass
from functools import lru_cache
from pathlib import Path, PurePosixPath

REPO = Path(__file__).resolve().parents[2]
ROOT = REPO / "build" / "deps" / "shader-tools"
DOWNLOADS = ROOT / "downloads"
SOURCE = ROOT / "source"
STAGING = ROOT / "source.staging"
BUILD = ROOT / "build"
INSTALL = ROOT / "install"
SOURCE_MANIFEST = SOURCE / ".sunbright-sources.json"
INSTALL_MANIFEST = INSTALL / ".sunbright-toolchain.json"
MAX_DOWNLOAD_BYTES = 128 * 1024 * 1024
MAX_EXPANDED_BYTES = 1024 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 100_000
PARALLEL_JOBS = min(os.cpu_count() or 1, 4)
TOOLCHAIN_VERSION = "2026.1-reproducible-1"


@dataclass(frozen=True)
class SourceArchive:
    name: str
    owner: str
    repository: str
    commit: str
    sha256: str
    destination: str

    @property
    def url(self) -> str:
        return (
            f"https://codeload.github.com/{self.owner}/{self.repository}/tar.gz/"
            f"{self.commit}"
        )

    @property
    def archive(self) -> Path:
        return DOWNLOADS / f"{self.name}-{self.commit}.tar.gz"


SOURCES = (
    SourceArchive(
        "shaderc",
        "SomeoneIsWorking",
        "shaderc",
        "50f71a748725b3df267128e519ef6c59881fc33e",
        "b269580b3df0220173925f313369eb735dc30787f52b1a2d9fa60e1dfc776bbf",
        ".",
    ),
    SourceArchive(
        "glslang",
        "KhronosGroup",
        "glslang",
        "f0bd0257c308b9a26562c1a30c4748a0219cc951",
        "bd58dca4dac67dcf7640292d7d63e0416274d40ee2200f7301878cec11ac6647",
        "third_party/glslang",
    ),
    SourceArchive(
        "spirv-tools",
        "KhronosGroup",
        "SPIRV-Tools",
        "fbe4f3ad913c44fe8700545f8ffe35d1382b7093",
        "cabb35f4eef0da3ef72ad9edd596af4191d7507a8f35c05df526d2d5ff889f59",
        "third_party/spirv-tools",
    ),
    SourceArchive(
        "spirv-headers",
        "KhronosGroup",
        "SPIRV-Headers",
        "04f10f650d514df88b76d25e83db360142c7b174",
        "1b220e3eec1714f0451b0e3652979bd280edf10893f617837b88e6359a804ded",
        "third_party/spirv-headers",
    ),
)


def source_identity() -> list[dict[str, str]]:
    return [asdict(source) for source in SOURCES]


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _assert_managed(path: Path) -> None:
    resolved_root = ROOT.resolve()
    resolved = path.resolve()
    if resolved == resolved_root or resolved_root not in resolved.parents:
        raise RuntimeError(f"refusing to clean unmanaged shader-tool path: {path}")


def clean_managed(path: Path) -> None:
    _assert_managed(path)
    if path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def download(source: SourceArchive) -> Path:
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    archive = source.archive
    if archive.is_file() and file_sha256(archive) == source.sha256:
        print(f"shader-toolchain: verified cached {source.name} archive")
        return archive
    if archive.exists():
        clean_managed(archive)
    partial = archive.with_suffix(archive.suffix + ".part")
    if partial.exists():
        clean_managed(partial)

    request = urllib.request.Request(
        source.url, headers={"User-Agent": "sunbright-shader-toolchain/1"}
    )
    print(f"shader-toolchain: downloading {source.name} at {source.commit}")
    digest = hashlib.sha256()
    received = 0
    try:
        with urllib.request.urlopen(request, timeout=60) as response, partial.open(
            "wb"
        ) as destination:
            while chunk := response.read(1024 * 1024):
                received += len(chunk)
                if received > MAX_DOWNLOAD_BYTES:
                    raise RuntimeError(
                        f"{source.name} archive exceeds {MAX_DOWNLOAD_BYTES} bytes"
                    )
                digest.update(chunk)
                destination.write(chunk)
    except BaseException:
        if partial.exists():
            clean_managed(partial)
        raise
    actual = digest.hexdigest()
    if actual != source.sha256:
        clean_managed(partial)
        raise RuntimeError(
            f"{source.name} archive SHA-256 mismatch: expected {source.sha256}, got {actual}"
        )
    partial.replace(archive)
    return archive


def extract_archive(archive: Path, destination: Path) -> None:
    if destination.exists():
        raise RuntimeError(f"shader-tool extraction destination already exists: {destination}")
    destination.mkdir(parents=True)
    expanded = 0
    members = 0
    with tarfile.open(archive, "r:gz") as bundle:
        for member in bundle:
            members += 1
            if members > MAX_ARCHIVE_MEMBERS:
                raise RuntimeError(
                    f"{archive.name} exceeds {MAX_ARCHIVE_MEMBERS} archive members"
                )
            parts = PurePosixPath(member.name).parts
            if len(parts) < 2:
                continue
            relative = PurePosixPath(*parts[1:])
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError(f"unsafe archive path in {archive.name}: {member.name}")
            output = destination.joinpath(*relative.parts)
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile():
                raise RuntimeError(
                    f"unsupported archive member in {archive.name}: {member.name}"
                )
            expanded += member.size
            if expanded > MAX_EXPANDED_BYTES:
                raise RuntimeError(
                    f"{archive.name} exceeds {MAX_EXPANDED_BYTES} expanded bytes"
                )
            source = bundle.extractfile(member)
            if source is None:
                raise RuntimeError(f"cannot read archive member: {member.name}")
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("wb") as target:
                shutil.copyfileobj(source, target, length=1024 * 1024)
            output.chmod(member.mode & 0o777)


def _manifest_matches(path: Path, expected: object) -> bool:
    try:
        return json.loads(path.read_text(encoding="utf-8")) == expected
    except (OSError, json.JSONDecodeError):
        return False


def _read_manifest(path: Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def provision_sources() -> None:
    identity = source_identity()
    if _manifest_matches(SOURCE_MANIFEST, identity) and (SOURCE / "CMakeLists.txt").is_file():
        return
    if STAGING.exists():
        clean_managed(STAGING)
    if SOURCE.exists():
        clean_managed(SOURCE)

    shaderc, *dependencies = SOURCES
    extract_archive(download(shaderc), STAGING)
    for dependency in dependencies:
        extract_archive(download(dependency), STAGING / dependency.destination)
    (STAGING / ".sunbright-sources.json").write_text(
        json.dumps(identity, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    STAGING.replace(SOURCE)


def executable_name(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


def _run(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    environment = os.environ.copy()
    environment["GIT_CEILING_DIRECTORIES"] = str(ROOT)
    environment["SOURCE_DATE_EPOCH"] = "1769169600"
    environment["TZ"] = "UTC"
    subprocess.run(command, cwd=REPO, env=environment, check=True)


def build_tree_is_ninja() -> bool:
    cache = BUILD / "CMakeCache.txt"
    try:
        text = cache.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return (
        "CMAKE_GENERATOR:INTERNAL=Ninja" in text
        and f"CMAKE_HOME_DIRECTORY:INTERNAL={SOURCE}" in text
    )


def _built_executable(name: str) -> Path:
    matches = [path for path in BUILD.rglob(executable_name(name)) if path.is_file()]
    if len(matches) != 1:
        rendered = ", ".join(str(path) for path in matches) or "none"
        raise RuntimeError(
            f"expected exactly one built {name} executable, found {len(matches)}: {rendered}"
        )
    return matches[0]


def _tool_version(path: Path) -> str:
    result = subprocess.run(
        [str(path), "--version"], capture_output=True, text=True, check=False
    )
    if result.returncode:
        raise RuntimeError(
            f"pinned shader tool failed: {path} exited {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    return result.stdout + result.stderr


def verify_install() -> bool:
    expected_sources = source_identity()
    manifest = _read_manifest(INSTALL_MANIFEST)
    if manifest is None or manifest.get("sources") != expected_sources:
        return False
    if manifest.get("version") != TOOLCHAIN_VERSION:
        return False
    glslc = INSTALL / "bin" / executable_name("glslc")
    validator = INSTALL / "bin" / executable_name("spirv-val")
    if not glslc.is_file() or not validator.is_file():
        return False
    hashes = manifest.get("executables")
    if not isinstance(hashes, dict) or hashes != {
        "glslc": file_sha256(glslc),
        "spirv-val": file_sha256(validator),
    }:
        return False
    return "shaderc v2026.1" in _tool_version(glslc) and "v2026.1" in _tool_version(
        validator
    )


def build_toolchain() -> None:
    if verify_install():
        print("shader-toolchain: exact shaderc/SPIRV-Tools v2026.1 install is current")
        return
    provision_sources()
    if BUILD.exists() and not build_tree_is_ninja():
        clean_managed(BUILD)
    if INSTALL.exists():
        clean_managed(INSTALL)
    BUILD.mkdir(parents=True, exist_ok=True)
    _run(
        [
            "cmake",
            "-S",
            str(SOURCE),
            "-B",
            str(BUILD),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DPython3_EXECUTABLE={sys.executable}",
            "-DSHADERC_SKIP_TESTS=ON",
            "-DSHADERC_SKIP_EXAMPLES=ON",
            "-DSHADERC_SKIP_COPYRIGHT_CHECK=ON",
            "-DSHADERC_ENABLE_WGSL_OUTPUT=OFF",
        ]
    )
    _run(
        [
            "cmake",
            "--build",
            str(BUILD),
            "--config",
            "Release",
            "--parallel",
            str(PARALLEL_JOBS),
            "--target",
            "glslc_exe",
            "spirv-val",
        ]
    )
    binary_dir = INSTALL / "bin"
    binary_dir.mkdir(parents=True)
    for name in ("glslc", "spirv-val"):
        destination = binary_dir / executable_name(name)
        shutil.copy2(_built_executable(name), destination)
        destination.chmod(destination.stat().st_mode | 0o111)
    executable_hashes = {
        name: file_sha256(binary_dir / executable_name(name))
        for name in ("glslc", "spirv-val")
    }
    INSTALL_MANIFEST.write_text(
        json.dumps(
            {
                "executables": executable_hashes,
                "sources": source_identity(),
                "version": TOOLCHAIN_VERSION,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    if not verify_install():
        raise RuntimeError("built shader toolchain failed its version/install verification")
    print("shader-toolchain: built and verified shaderc/SPIRV-Tools v2026.1")


@lru_cache(maxsize=2)
def require_tool(name: str) -> str:
    if name not in {"glslc", "spirv-val"}:
        raise ValueError(f"unsupported pinned shader tool: {name}")
    if not verify_install():
        raise RuntimeError(
            "pinned shader toolchain is missing or stale; run: "
            "uv run --frozen python tools/render/shader_toolchain.py"
        )
    return str(INSTALL / "bin" / executable_name(name))


def selftest() -> int:
    fixture_root = REPO / "scratch" / "shader-toolchain-selftest"

    def clean_fixture() -> None:
        if fixture_root.resolve() != (REPO / "scratch" / "shader-toolchain-selftest").resolve():
            raise RuntimeError(f"refusing to clean unexpected self-test path: {fixture_root}")
        if fixture_root.exists():
            shutil.rmtree(fixture_root)

    clean_fixture()
    fixture_root.mkdir(parents=True)
    archive = fixture_root / "fixture.tar.gz"
    try:
        with tarfile.open(archive, "w:gz") as bundle:
            payload = b"shader-tools\n"
            member = tarfile.TarInfo("root/bin/tool")
            member.size = len(payload)
            member.mode = 0o755
            bundle.addfile(member, io.BytesIO(payload))
        destination = fixture_root / "expanded"
        extract_archive(archive, destination)
        if (destination / "bin" / "tool").read_bytes() != payload:
            print("FAIL: safe archive member did not round-trip")
            return 1

        unsafe = fixture_root / "unsafe.tar.gz"
        with tarfile.open(unsafe, "w:gz") as bundle:
            member = tarfile.TarInfo("root/../../escape")
            member.size = 1
            bundle.addfile(member, io.BytesIO(b"x"))
        try:
            extract_archive(unsafe, fixture_root / "unsafe-expanded")
        except RuntimeError:
            pass
        else:
            print("FAIL: traversal archive member was accepted")
            return 1
        print("PASS: bounded source identity and safe/unsafe archive controls")
        return 0
    finally:
        clean_fixture()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    if arguments.selftest:
        return selftest()
    build_toolchain()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

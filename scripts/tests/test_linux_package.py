"""The Linux release archive (scripts/linux/package.sh) on a synthetic build: what it packs and what it refuses.

The binaries are stand-ins: an ELF magic and a JSON line, which a fake readelf (READELF) prints in readelf's
own format, so the checks run on any machine with bash and tar.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "linux" / "package.sh"
pytestmark = pytest.mark.skipif(shutil.which("bash") is None or shutil.which("tar") is None,
                                reason="package.sh needs bash and tar")

FAKE_READELF = r'''
import json, sys
args = [a for a in sys.argv[1:] if not a.startswith("--")]
flag, path = args[0], args[-1]
elf = json.loads(open(path, "rb").read()[4:])
if flag == "-h":
    print("ELF Header:")
    print("  Machine:                           " + elf["machine"])
elif flag == "-d":
    for lib in elf["needed"]:
        print(f" 0x0000000000000001 (NEEDED)             Shared library: [{lib}]")
elif flag == "-V":
    print("Version symbols section '.gnu.version' contains 9 entries:")
    print("  000:   0 (*local*)       2 (GLIBC_2.2.5)")
    print()
    print("Version needs section '.gnu.version_r' contains 1 entry:")
    print(" Addr: 0x0000000000000a18  Offset: 0x000a18  Link: 7 (.dynstr)")
    for file, versions in elf["versions"].items():
        print(f"  000000: Version: 1  File: {file}  Cnt: {len(versions)}")
        for i, version in enumerate(versions):
            print(f"  0x{16 * (i + 1):04x}:   Name: {version}  Flags: none  Version: {i + 3}")
'''

GOOD = {"machine": "Advanced Micro Devices X86-64", "needed": ["libvulkan.so.1", "libm.so.6", "libc.so.6"],
        "versions": {"libc.so.6": ["GLIBC_2.2.5", "GLIBC_2.34"], "libm.so.6": ["GLIBC_2.29"]}}


def elf(path: Path, **change) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x7fELF" + json.dumps({**GOOD, **change}).encode())
    path.chmod(0o755)


def write(root: Path, relative: str, text: str = "x\n") -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


@pytest.fixture
def tree(tmp_path):
    root = tmp_path / "repo"
    for relative in ("Setup-GothicAI.sh", "Start-GothicAI.sh"):
        write(root, relative, "#!/usr/bin/env bash\n")
        (root / relative).chmod(0o755)
    for relative in ("START_HERE.linux.ru.md", "LICENSE", "scripts/linux/common.sh", "OpenGothic/LICENSE",
                     "OpenGothic/lib/VENDORED.md", "OpenGothic/lib/ZenKit/LICENSE",
                     "OpenGothic/build/_deps/ixwebsocket-src/LICENSE.txt"):
        write(root, relative)
    for project in ("launcher/core", "server", "asr-service", "tts-service", "libs/chatgpt-auth"):
        for relative in ("pyproject.toml", "uv.lock", ".python-version", "src/pkg/__init__.py"):
            write(root, f"{project}/{relative}")
        write(root, f"{project}/tests/test_x.py")  # not packed
    write(root, "server/src/pkg/__pycache__/x.pyc")  # not packed
    elf(root / "OpenGothic/build/opengothic/Gothic2Notr")
    elf(root / "OpenGothic/build/opengothic/content-convert")
    elf(root / "OpenGothic/ai_bridge/storygraph/build/storygraph")
    uv = tmp_path / "uv"
    elf(uv / "uv-x86_64-unknown-linux-gnu/uv", needed=["libgcc_s.so.1", "libc.so.6"],
        versions={"libc.so.6": ["GLIBC_2.17"], "libgcc_s.so.1": ["GCC_4.2.0"]})
    with tarfile.open(tmp_path / "uv.tar.gz", "w:gz") as archive:
        archive.add(uv / "uv-x86_64-unknown-linux-gnu", arcname="uv-x86_64-unknown-linux-gnu")
    for name in ("LICENSE-MIT", "LICENSE-APACHE"):
        write(tmp_path / "uv-licenses", name)
    readelf = tmp_path / "readelf"
    readelf.write_text(f"#!{sys.executable}\n{FAKE_READELF}", encoding="utf-8")
    readelf.chmod(0o755)
    return root


def package(tree: Path):
    tmp = tree.parent
    return subprocess.run(
        ["bash", str(SCRIPT), "--root", str(tree), "--uv-tar", str(tmp / "uv.tar.gz"),
         "--uv-licenses", str(tmp / "uv-licenses"), "--out-dir", str(tmp / "dist"), "--name", "GothicAI-test",
         "--commit", "abc", "--compiler", "g++-12 (test) 12.3.0"],
        capture_output=True, text=True, env={**os.environ, "READELF": str(tmp / "readelf")},
    )


def test_packs_the_game_with_its_manifest_and_checksums(tree):
    result = package(tree)
    assert result.returncode == 0, result.stderr
    dist = tree.parent / "dist"
    with tarfile.open(dist / "GothicAI-test.tar.gz") as archive:
        members = {m.name: m for m in archive.getmembers() if m.isfile()}
    assert members["GothicAI-test/OpenGothic/build/opengothic/Gothic2Notr"].mode & 0o111
    assert members["GothicAI-test/tools/uv/uv"].mode & 0o111
    assert "GothicAI-test/licenses/ixwebsocket/LICENSE.txt" in members
    assert not [name for name in members if "/tests/" in name or name.endswith(".pyc")]
    manifest = json.loads((dist / "GothicAI-test.manifest.json").read_text(encoding="utf-8"))
    game = next(f for f in manifest["files"] if f["path"] == "OpenGothic/build/opengothic/Gothic2Notr")
    assert game["needed"] == GOOD["needed"] and game["glibc"] == "2.34"
    assert len(manifest["files"]) == len(members)
    sums = (dist / "SHA256SUMS").read_text().split()
    assert sums[1::2] == ["GothicAI-test.tar.gz", "GothicAI-test.manifest.json"]


@pytest.mark.parametrize("spoil, refusal", [
    (lambda root: write(root, "OpenGothic/build/opengothic/Gothic2Notr.log"),
     "not for the archive: OpenGothic/build/opengothic/Gothic2Notr.log"),
    (lambda root: elf(root / "OpenGothic/build/opengothic/content-convert", needed=["libfoo.so.1", "libc.so.6"]),
     "content-convert needs libfoo.so.1"),
    (lambda root: elf(root / "OpenGothic/ai_bridge/storygraph/build/storygraph",
                      versions={"libc.so.6": ["GLIBC_2.2.5", "GLIBC_2.36"]}),
     "storygraph needs GLIBC_2.36, newer than glibc 2.35"),
    (lambda root: elf(root / "OpenGothic/build/opengothic/Gothic2Notr",
                      versions={"libc.so.6": ["GLIBC_2.34"], "libstdc++.so.6": ["GLIBCXX_3.4.30"]}),
     "Gothic2Notr needs libstdc++ (GLIBCXX_3.4.30)"),
    (lambda root: elf(root / "OpenGothic/build/opengothic/Gothic2Notr", machine="AArch64"),
     "not an x86-64 binary: OpenGothic/build/opengothic/Gothic2Notr"),
], ids=["forbidden-file", "foreign-library", "glibc-2.36", "glibcxx", "aarch64"])
def test_refuses(tree, spoil, refusal):
    spoil(tree)
    result = package(tree)
    assert result.returncode != 0
    assert refusal in result.stderr
    assert not (tree.parent / "dist" / "GothicAI-test.tar.gz").exists()


def test_setup_finds_a_library_in_a_long_ldconfig_cache(tmp_path):
    """Setup-GothicAI.sh runs under pipefail: a grep that stops at the first line must not lose the match."""
    fake = tmp_path / "bin" / "ldconfig"
    fake.parent.mkdir()
    fake.write_text('#!/bin/sh\necho "\tlibvulkan.so.1 (libc6,x86-64) => /usr/lib/libvulkan.so.1"\n'
                    'yes "\tlibother.so.1 (libc6,x86-64) => /usr/lib/libother.so.1" | head -n 200000\n')
    fake.chmod(0o755)
    common = SCRIPT.parent / "common.sh"
    probe = (f'set -euo pipefail; GOTHIC_ROOT={tmp_path}; source "{common}"; '
             'have_library libvulkan.so.1 && echo found; have_library libnothere.so.9 || echo missing')
    result = subprocess.run(["bash", "-c", probe], capture_output=True, text=True, timeout=60,
                            env={**os.environ, "PATH": f"{fake.parent}{os.pathsep}{os.environ['PATH']}"})
    assert result.returncode == 0, result.stderr
    assert result.stdout.split() == ["found", "missing"]

"""Shared Ryzen build flags enable ccache only when it is available and wanted."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest


HELPER = Path(__file__).resolve().parents[1] / "ryzen-build.sh"
BUILD_FLAGS = [
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG",
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG",
]
SOURCE = r'''
set -euo pipefail
options=$-
shell_options=$SHELLOPTS
source "$1"
[[ "$options" == "$-" && "$shell_options" == "$SHELLOPTS" ]]
"$2" -c 'import json, os, sys; print(json.dumps({
    "flags": sys.argv[1:],
    "ccache": {key: value for key, value in os.environ.items()
               if key.startswith("CCACHE_")},
}))' "${RYZEN_CMAKE_FLAGS[@]}"
'''


@pytest.mark.parametrize("has_ccache,disabled,custom_dir", [
    (True, False, False),
    (False, False, False),
    (True, True, False),
    (True, False, True),
])
def test_ccache_flags_and_environment(tmp_path, has_ccache, disabled, custom_dir):
    home = tmp_path / "home"
    home.mkdir()
    binaries = tmp_path / "bin"
    binaries.mkdir()
    (binaries / "mkdir").symlink_to(shutil.which("mkdir"))
    ccache = binaries / "ccache"
    if has_ccache:
        ccache.write_text("#!/bin/sh\nexit 0\n")
        ccache.chmod(0o755)
    environment = {"HOME": str(home), "PATH": str(binaries)}
    cache_dir = home / ".cache/gothic-ai/ccache"
    if custom_dir:
        cache_dir = tmp_path / "custom cache"
        environment["CCACHE_DIR"] = str(cache_dir)
    if disabled:
        environment["RYZEN_CCACHE"] = "0"

    result = subprocess.run(
        [shutil.which("bash"), "-c", SOURCE, "test-ryzen-build", str(HELPER),
         sys.executable],
        env=environment, capture_output=True, text=True, timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    output = json.loads(result.stdout)
    enabled = has_ccache and not disabled
    launcher = str(ccache) if enabled else ""
    assert output["flags"] == BUILD_FLAGS + [
        f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}",
        f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}",
    ]
    assert output["ccache"] == ({
        "CCACHE_DIR": str(cache_dir),
        "CCACHE_MAXSIZE": "20G",
        "CCACHE_BASEDIR": str(home / "gothic-ai"),
        "CCACHE_NOHASHDIR": "true",
    } if enabled else {})
    assert cache_dir.is_dir() == enabled

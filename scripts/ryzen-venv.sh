#!/usr/bin/env bash
# Source this helper to prepare a task environment on Ryzen.
ryzen_task_venv() {
  local venv="$1"
  shift
  local torch_version=2.14.1
  local python="$venv/bin/python"
  if [[ ! -x "$python" ]]; then
    uv venv --python 3.12 "$venv" || return
  fi

  local installed
  installed=$("$python" -c 'from importlib.metadata import version, PackageNotFoundError
try:
    print(version("torch"))
except PackageNotFoundError:
    pass') || return
  if [[ -n "$installed" && "${installed%%+*}" != "$torch_version" ]]; then
    printf 'Warning: %s has torch %s; expected %s. Keeping the existing environment.\n' \
      "$venv" "$installed" "$torch_version" >&2
    local package
    local missing=()
    for package in "$@"; do
      if ! "$python" -c 'from importlib.metadata import version
import re, sys
version(re.split(r"[<>=!~\[;\s]", sys.argv[1], maxsplit=1)[0])' "$package" >/dev/null 2>&1; then
        missing+=("$package")
      fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
      uv pip install --python "$python" "torch==$installed" "${missing[@]}" || return
    fi
    return 0
  fi
  uv pip install --python "$python" "torch==$torch_version" numpy pytest "$@"
}

#!/usr/bin/env bash
# Copy only our raw-source store, then verify the copy against the git registry.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
registry="$script_dir/../../data/content/registry.jsonl"
source_root="$HOME/.local/share/gothic-ai/content-sources"
while (($#)); do
  case "$1" in
    --registry|--source-root)
      if (($# < 2)); then
        echo "Missing value for $1" >&2
        exit 2
      fi
      if [[ "$1" == --registry ]]; then registry=$2; else source_root=$2; fi
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [--registry PATH] [--source-root PATH]"
      exit 0
      ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

python3 "$script_dir/registry.py" check "$registry" --source-root "$source_root"
if [[ ! -d "$source_root" ]]; then
  echo "Source store is not a directory: $source_root" >&2
  exit 1
fi
ssh dev-ubuntu 'mkdir -p "$HOME/gothic-ai/content-backup/sources"'
# No deletion: a newer or interrupted batch must not remove an older backup.
rsync -a --checksum -- "$source_root/" 'dev-ubuntu:gothic-ai/content-backup/sources/'

# Send the validator and registry over stdin, leaving no registry or helper
# files on the remote host. The remote side uses the same validation code.
python3 - "$script_dir/registry.py" "$registry" <<'PY' | ssh dev-ubuntu 'python3 -'
import json
from pathlib import Path
import sys

validator = Path(sys.argv[1]).read_text(encoding='utf-8')
records = Path(sys.argv[2]).read_text(encoding='utf-8')
print("from pathlib import Path")
print("namespace = {'__name__': 'backup_registry', '__file__': '/gothic-ai/scripts/content/registry.py'}")
print(f"exec({validator!r}, namespace)")
print(f"lines = {records!r}.splitlines()")
print("root = Path.home() / 'gothic-ai/content-backup/sources'")
print("sources = set()")
print("for line in lines:")
print("    entry = namespace['json'].loads(line)")
print("    namespace['validate_entry'](entry, source_root=root)")
print("    sources.update(namespace['source_references'](entry))")
print("print(f'Source backup verified: {len(sources)} PNGs on dev-ubuntu')")
PY

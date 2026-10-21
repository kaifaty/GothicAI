#!/usr/bin/env python3
"""Terminal fallback for the same queued desktop session used by the launcher window."""
import contextlib
import os
import sys
from pathlib import Path


def main(argv=None):
    repo = Path(__file__).resolve().parent.parent
    uv = str(Path.home() / ".local/bin/uv")
    try:
        os.execv(uv, [uv, "run", "--locked", "--project", str(repo / "launcher/core"),
                      "gothic-launcher", "play", *(sys.argv[1:] if argv is None else argv)])
    except OSError as error:
        print(f"Gothic AI: {error}", file=sys.stderr)
        if sys.stdin.isatty():
            with contextlib.suppress(EOFError):
                input("Press Enter to close this terminal.")
        return 2


if __name__ == "__main__":
    sys.exit(main())

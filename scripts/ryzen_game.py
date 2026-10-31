#!/usr/bin/env python3
"""Select the prepared Ryzen game settings and content fingerprints."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import sys


def game_environment(home=None, environ=None, assets=None):
    """Read setup without creating directories; retain caller content overrides."""
    home = Path.home() if home is None else Path(home)
    environ = os.environ if environ is None else environ
    settings = home / "gothic-ai/game-settings"
    selected = assets if assets is not None else environ.get("GOTHIC_DIR") or settings
    overrides = {key: value for key, value in environ.items()
                 if key.startswith("OPENGOTHIC_BASE") or key == "OPENGOTHIC_VIDEOS_FINGERPRINT"}
    # Generic runner tools also accept installations unrelated to Ryzen setup.
    if Path(selected).expanduser() != settings:
        return {"GOTHIC_DIR": str(selected), **overrides}
    try:
        parts = json.loads((home / "gothic-ai/game-base.json").read_text())
        base, videos = parts["base"], parts["videos"]
        if not isinstance(base, str) or not re.fullmatch("[0-9a-f]{64}", base):
            raise ValueError("invalid base fingerprint")
        if not isinstance(videos, str) or (videos and not re.fullmatch("[0-9a-f]{64}", videos)):
            raise ValueError("invalid video fingerprint")
        if not all((settings / "system" / name).is_file() for name in ("Gothic.ini", "SystemPack.ini")):
            raise ValueError("game settings are missing")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError(f"Run scripts/ryzen.sh setup first: {error}") from None
    result = {
        "GOTHIC_DIR": str(selected),
        "OPENGOTHIC_BASE_FINGERPRINT": base,
        "OPENGOTHIC_VIDEOS_FINGERPRINT": videos,
    }
    result.update(overrides)
    if not result["OPENGOTHIC_BASE_FINGERPRINT"]:
        result["OPENGOTHIC_BASE_FINGERPRINT"] = base
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--check", action="store_true")
    output.add_argument("--json", action="store_true")
    parser.add_argument("--assets")
    args = parser.parse_args(argv)
    try:
        environment = game_environment(assets=args.assets)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(environment))
    elif not args.check:
        for key, value in environment.items():
            print(f"export {key}={shlex.quote(value)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

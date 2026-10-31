#!/usr/bin/env bash
# Autonomous CV population PPO. Each collect, update and validation is its own
# finite gq batch request; restarting this command resumes the current phase.
# Run on Ryzen with all --paths absolute. See --help for configuration.
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python=${NAV_PPO_PYTHON:-${PPO_PYTHON:-python3}}
exec "$python" "$script_dir/nav_ppo_loop.py" "$@"

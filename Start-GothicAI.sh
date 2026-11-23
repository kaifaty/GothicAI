#!/usr/bin/env bash
# Gothic AI on Linux: the AI services and the game (the counterpart of Start-GothicAI.cmd).
#
# `gothic-launcher setup --check` first (what is missing, in plain Russian), then `gothic-launcher up`:
# the services on free localhost ports, the game with the newest save (or its menu). Closing the game
# stops everything; so does Ctrl-C here.
#
#   ./Start-GothicAI.sh          continue the newest save, or the menu when there is none
#   ./Start-GothicAI.sh --new    a new game at once
set -uo pipefail

GOTHIC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/common.sh
source "$GOTHIC_ROOT/scripts/linux/common.sh"

mode=--continue
case "${1:-}" in
  '') ;;
  --new) mode=--new ;;
  *) say "Запуск: ./Start-GothicAI.sh [--new]"; exit 2 ;;
esac

if ! find_uv; then
  say "Сначала запустите ./Setup-GothicAI.sh."
  exit 2
fi
launcher setup --check
code=$?
if ((code != 0)); then exit "$code"; fi

say "Запускаю ИИ-сервисы и игру. Закройте игру, и всё остановится само."
launcher up "$mode"
code=$?
if ((code == 0 || code == 130)); then exit 0; fi  # the game closed, or Ctrl-C
say "Игра или ИИ-сервисы завершились с ошибкой (код $code). Логи: $LOG_ROOT"
exit "$code"

#!/usr/bin/env bash
# Gothic AI on Linux: first run and repair; safe to run again (the counterpart of Setup-GothicAI.cmd).
#
# Checks the computer (x86-64, the Vulkan loader of the graphics driver, PortAudio for the microphone),
# prepares the AI services' Python environments exactly as locked, then `gothic-launcher setup`: the game
# folder (found in Steam, else asked here), the remote-only profile, the Inworld key (hidden input) and
# the ChatGPT sign-in in the browser. Existing settings, keys, saves and builds stay; only what is missing
# is done. Nothing is installed system-wide and nothing runs as root.
#
#   ./Setup-GothicAI.sh [--game-dir <folder>] [--change-key]
set -uo pipefail

GOTHIC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/common.sh
source "$GOTHIC_ROOT/scripts/linux/common.sh"

usage() { say "Запуск: ./Setup-GothicAI.sh [--game-dir <папка Gothic II>] [--change-key]"; }

game_dir=()
options=()
while (($#)); do
  case "$1" in
    --game-dir)
      if (($# < 2)); then usage; exit 2; fi
      game_dir=(--game-dir "$2")
      shift ;;
    --game-dir=*) game_dir=(--game-dir "${1#--game-dir=}") ;;
    --change-key) options+=(--change-key) ;;
    -h | --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
  shift
done

say "Gothic AI: подготовка к игре на этом компьютере."
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  say "Нужен 64-битный Linux на процессоре x86-64. Здесь: $(uname -s) $(uname -m)."
  exit 2
fi

# uv brings Python and the services' libraries
if ! find_uv; then
  say "Не найден uv: он ставит Python и всё, что нужно ИИ-сервисам."
  say "Установите uv официальным способом и запустите ./Setup-GothicAI.sh снова:"
  say "  curl -LsSf https://astral.sh/uv/install.sh | sh"
  say "Подробнее: https://docs.astral.sh/uv/getting-started/installation/"
  exit 2
fi

if ! have_library libvulkan.so.1; then
  say "Не найден libvulkan.so.1: игре нужен драйвер видеокарты с поддержкой Vulkan."
  say "Установите драйвер Vulkan вашей видеокарты из своего дистрибутива и запустите ./Setup-GothicAI.sh снова:"
  say "  Ubuntu, Debian:  sudo apt install libvulkan1 mesa-vulkan-drivers   (NVIDIA: драйвер nvidia)"
  say "  Fedora:          sudo dnf install vulkan-loader mesa-vulkan-drivers"
  say "  Arch, SteamOS:   sudo pacman -S vulkan-icd-loader   и vulkan-radeon, vulkan-intel или nvidia-utils"
  exit 2
fi
if ! have_library libportaudio.so.2; then
  say "Внимание: нет библиотеки PortAudio (libportaudio.so.2). Без неё не будет микрофона: NPC не услышат вас."
  say "Остальное будет работать. Установите её и запустите ./Setup-GothicAI.sh снова:"
  say "  Ubuntu, Debian:  sudo apt install libportaudio2"
  say "  Fedora:          sudo dnf install portaudio"
  say "  Arch:            sudo pacman -S portaudio"
fi

# the game, its base and video converter, and the story graph converter
built=1
for file in OpenGothic/build/opengothic/Gothic2Notr OpenGothic/build/opengothic/content-convert \
            OpenGothic/ai_bridge/storygraph/build/storygraph; do
  [[ -x "$GOTHIC_ROOT/$file" ]] || built=0
done
if ((!built)); then
  if [[ ! -f "$GOTHIC_ROOT/OpenGothic/CMakeLists.txt" ]]; then
    # the release archive is prebuilt: without its game there is nothing to build
    say "В папке нет готовой игры: архив распакован не полностью. Скачайте архив из релиза Gothic AI заново и распакуйте его целиком."
  else
    say "Игра ещё не собрана. Соберите её из исходников (docs/development.md, раздел Linux) и запустите ./Setup-GothicAI.sh снова."
  fi
  exit 2
fi

# the AI services' environments, exactly as locked
say "Готовлю ИИ-сервисы: Python и библиотеки."
for project in launcher/core server asr-service tts-service; do
  if ! "$UV" sync --locked --project "$GOTHIC_ROOT/$project"; then
    say "Не удалось подготовить $project. Проверьте интернет и запустите ./Setup-GothicAI.sh ещё раз."
    exit 1
  fi
done

# the game folder, the remote-only profile, the Inworld key and the ChatGPT sign-in
launcher setup ${game_dir[@]+"${game_dir[@]}"} ${options[@]+"${options[@]}"}
code=$?
if ((code == 3)); then
  picked=''
  read -r -p "Вставьте путь к папке Gothic II и нажмите Enter (пусто — отмена): " picked || picked=''
  picked="${picked#\"}"; picked="${picked%\"}"; picked="${picked#\'}"; picked="${picked%\'}"
  if [[ "$picked" == "~"* ]]; then picked="$HOME${picked#\~}"; fi
  if [[ -z "$picked" ]]; then
    say "Папка игры не выбрана. Установите русскую Gothic II Gold через Steam и запустите ./Setup-GothicAI.sh ещё раз."
    exit 2
  fi
  launcher setup --game-dir "$picked" ${options[@]+"${options[@]}"}
  code=$?
fi
if ((code == 0)); then
  say "Готово. Запускайте игру: ./Start-GothicAI.sh."
else
  say "Настройка не закончена: что осталось, написано выше."
fi
exit "$code"

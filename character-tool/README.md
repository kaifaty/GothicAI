# character-tool

Офлайн-инструменты для головы, тела, анимаций и мимики персонажа Gothic. Для установки из корня репозитория нужен Python 3.10 или новее и активное виртуальное окружение.

Каталог `character-tool/` можно скопировать отдельно. В нём все исходники и шаблоны — обычные файлы. Прежние пути внутри `scripts/` и `reports/` используют символьные ссылки. В checkout с `core.symlinks=false` используйте команды установленного пакета.

```sh
python -m pip install './character-tool[authoring]'
character-tool --help
character-tool face --help
```

Вариант `authoring` устанавливает NumPy и Pillow для головы, записи тела, движений и подготовки выражений. Для `face wav`, `face json` и `body install` достаточно `python -m pip install ./character-tool`. Те же пути принимает `uv pip install`. Для переноса соберите wheel командой `uv build --wheel ./character-tool` и установите файл `.whl` через pip или uv на другой машине.

## Дорожка речи

```sh
character-tool face wav /path/to/Gothic ./audio DIA_VINO_HALLO_05_02
a2f-weights diffusion /path/to/model.json 2 audio/DIA_VINO_HALLO_05_02.wav line.csv anger=0.8,disgust=0.3
character-tool face json line.csv DIA_VINO_HALLO_05_02.face.json --emotion anger:0.8,disgust:0.3
```

1. `wav` извлекает одну или несколько реплик из речевых VDF в `Gothic/Data`. При совпадении имён побеждает последний архив по сортировке имён. Нужен `ffmpeg` в `PATH`. Результат — моно WAV, PCM s16, 16 кГц.
2. `a2f-weights` — отдельная программа для машины с Audio2Face SDK, CUDA, TensorRT и локальной моделью. Путь к исходникам и `CMakeLists.txt` установленного пакета печатает `python -c 'from importlib.resources import files; print(files("character_tool.face") / "native")'`. Подключите эту папку через `add_subdirectory(<путь> a2f-weights)` в сборке SDK с целями `audio2x` и `AudioFile::AudioFile`, затем выполните `cmake --build <сборка-SDK> --target a2f-weights`. Первый аргумент программы — `regression` или `diffusion`. Для Mark в v3.0 используется идентификатор 2. Последний аргумент задаёт эмоции на всю реплику, по умолчанию она нейтральна.
3. `json` работает на стандартном Python. Строка входного CSV содержит номер сэмпла при 16 кГц, 52 веса кожи ARKit в порядке решателя и, при наличии, веса языка. В выходном JSON поле `shapes` задаёт порядок 52 каналов, `fps` равно 30, а `data` содержит кадры подряд в base64, по байту на канал. Веса интерполируются на моменты `i/30` с. Ключ `--emotion` сохраняет переданную метку со значениями от 0 до 1. Движок читает `<имя звука>.face.json` из каталога дорожек.

## Данные выражений

```sh
character-tool face cues ./cue-tracks --seed 2026
character-tool face leak-table ./weights > leaks.json
```

Обе команды используют NumPy. `cues` записывает набор дорожек жестов, морганий и прорывов эмоций в `a/*.csv` и `b/*.csv`, их разметку в `plan.json` и ключ в `answers.txt`. Авторский набор состоит из шести реплик — `inworld_hero`, `inworld_xardas`, `orig_hero`, `orig_sylvio`, `orig_biff`, `player_mic`. Относительно `~/.local/share/gothic-ai` звук берётся из `face-sources/clips/<реплика>.wav`, нейтральные веса из `remaster/hero-head/face-shapes-2/weights/<реплика>.v3.csv`, эмоциональные из `remaster/hero-head/{emotions-1,cues-1}/weights/<реплика>.<эмоция>.csv`.

`leak-table` пишет в stdout JSON для раздела `leaks` профиля головы. В указанной папке нужны `<реплика>.<эмоция>.csv` для трёх реплик (`orig_hero`, `orig_biff`, `inworld_xardas`) и шести состояний (`neutral`, `anger`, `fear`, `disgust`, `joy`, `pain`). По умолчанию команда читает из `~/.local/share/gothic-ai/remaster/hero-head/emotions-1/weights`.

## Голова, тело и движение

Переменная `GOTHIC_AI_DATA` задаёт корень внешних данных для сборщика головы и команд `face cues` и `face leak-table`. По умолчанию это `~/.local/share/gothic-ai`.

`character-tool body --help` и `character-tool anim --help` перечисляют операции. Blender запускается без окна через `BLENDER` или команду `blender` из `PATH`. Для тела ему нужен профиль с включённым расширением MPFB 2, его базовой сеткой, таргетами и ригом `game_engine`. Укажите этот профиль в `BLENDER_USER_RESOURCES`, а в `models` — каталог записей импортированной базы Gothic.

```sh
export BLENDER_USER_RESOURCES="$HOME/.local/share/gothic-ai/mpfb/blender"
mpfb_data="$BLENDER_USER_RESOURCES/extensions/.user/user_default/mpfb/data"
skin_png="$mpfb_data/skins/middleage_caucasian_male/middleage_lightskinned_male_diffuse.png"
models=/path/to/base/models
run="$PWD/run"

RIG=1 character-tool body create "$run" "$models/HUMANS.MDH.json"
character-tool body outfit "$run/body.blend" "$run" trousers,boots,vest,belt,flap,bracers,guards
character-tool body export "$run/outfit.blend" "$run" "$models/HUMANS.MDH.json"
character-tool body write "$run" "$models/HUMANS.MDH.json" "$models/HUM_BODY_NAKED0.MDM.json" "$skin_png" ./staged
character-tool body install ./staged ./test-content
```

Для одетого героя задайте `RIG=1` при `create`. Шаг создаёт `body.blend` и сохраняет выбор рига в `landmarks.json`. В `outfit` список деталей передаётся одним аргументом в порядке сборки, по умолчанию это `trousers,boots`. Из `outfit.blend` шаг `export` пишет части в NPZ и риг в `rig.json`. Шаг `write` собирает записи MDM/MDH, таблицу ригов и материалы в `staged`. Последним аргументом можно задать цвет шеи, например `'#8a604a'`. При установке `--dry` показывает изменения, а `--models-only` использует уже подготовленные материалы целевого каталога.

| Команда | Программа и данные |
|---|---|
| `character-tool head ./recipe --work ./work --content ./out` | `head --help` показывает параметры самого `head/build_head.py`. Сборщик читает `npc.json`, листы видов и плотные модели. Ему нужны Blender/MPFB, отдельные Python-окружения MPFB и MediaPipe и `texpack`. Выход включает `head.glb`, `head.face.json`, `head.frame.json`, `head.groom` и материалы. |
| `character-tool anim retarget motion.npz ./models/HUMANS.MDH.json ./walk.MAN.json --name S_WALK` | Переносит клип SOMA на скелет Gothic и пишет запись MAN с буфером. `anim retarget --help` показывает все параметры `retarget_soma.py`, включая частоты входа и выхода, опорную позу и режим ключиц. |
| `character-tool anim info ./clip.MAN.json` | Python с NumPy запускает `anim/gothic_anim.py` и читает запись MAN с её бинарным буфером. |
| `character-tool anim preview ./rig.json` | Blender запускает `anim/blender/gothic_rig.py`. Конфигурация задаёт каталог `models`, имена `skeleton` и `body`, список клипов `anims` и выход `blend` или `render`. |

### Рецепт головы

Положите в `recipe/npc.json` имена двух листов (фас и левый профиль на каждом) и готовых плотных мешей. Относительные пути отсчитываются от каталога рецепта.

```json
{
  "name": "pilot",
  "instance": "PC_HERO",
  "sheet": "full.png",
  "bald": "bald.png",
  "mesh": {"full": "full.npz", "bald": "bald.npz"}
}
```

```sh
character-tool head ./recipe --list --work ./work --content ./out
character-tool head ./recipe --work ./work --content ./out
```

Шаги сохраняют промежуточные файлы в `work` и проверяют их при следующем запуске. `--only export` повторяет экспорт из готовых промежуточных файлов. Результат записывается в `out/characters/hero/` и `out/materials/`. Для каждого рецепта используйте свой каталог вывода. Текущий экспортёр записывает индекс головы для `PC_HERO`.

Сборщик читает импортированную базу игры из `OPENGOTHIC_BASE_CACHE` и авторские исходники из `GOTHIC_AI_DATA`, включая `mpfb/work/probe.blend`. Пути программ и моделей можно задать через `BLENDER`, `BLENDER_USER_RESOURCES`, `MPFB_PYTHON`, `MEDIAPIPE_PYTHON`, `FACE_LANDMARKER` и `TEXPACK`. Для переноса форм Mark добавьте `"shapes": "mark"` и предоставьте внешние данные Mark. Исходники запуска Pixal3D и упаковщика DDS входят в дистрибутив. [Сборка texpack](src/character_tool/backends/texpack/BUILD.md)

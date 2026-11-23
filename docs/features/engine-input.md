## Движок: ввод

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Приём кадров «движение руки» по локальному UDP `127.0.0.1:47821`: формат v1, история по каждой руке, потери по `seq`, перевод часов отправителя в часы игры; потребителя до шага 2 нет | флаг игры `--no-hand-motion` или `OPENGOTHIC_HAND_MOTION=0`; порт — `OPENGOTHIC_HAND_MOTION_PORT`, журнал — `OPENGOTHIC_HAND_MOTION_LOG=1` | `common/input/handmotion.*`, формат — `common/input/handmotionframe.h`; держит `MainWindow` | цель `gothic-hand-motion-test` | [hand-motion.md](../hand-motion.md) |
| Мост к Joy-Con: два Joy-Con через SDL3, каждый — своя рука; время по приходу пакета, сдвиг гироскопа в покое, ориентация фильтром Махони; кадр v1 на каждый отсчёт (~185/с на руку); запись сырых отсчётов и воспроизведение | не запускать мост; ключи `--host`, `--port`, `--record`, `--replay`, `--loop`, `--print` | `tools/hand-motion-bridge/` — отдельный проект CMake с SDL ≥ 3.4.0, вне сборки игры | `hand-motion-bridge --self-test` (`~/.codex/checks/gothic/hand-motion-bridge-check.sh`) | [README моста](../../OpenGothic/tools/hand-motion-bridge/README.md), [hand-motion.md](../hand-motion.md#как-работает-сейчас) |

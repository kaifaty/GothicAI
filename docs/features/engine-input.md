## Движок: ввод

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Приём кадров «движение руки» по локальному UDP `127.0.0.1:47821`: формат v1, история по каждой руке, потери по `seq`, перевод часов отправителя в часы игры; потребителя до шага 2 нет | `--no-hand-motion` или `OPENGOTHIC_HAND_MOTION=0`; порт — `OPENGOTHIC_HAND_MOTION_PORT`, журнал — `OPENGOTHIC_HAND_MOTION_LOG=1` | `common/input/handmotion.*`, формат — `common/input/handmotionframe.h`; держит `MainWindow` | цель `gothic-hand-motion-test` | [hand-motion.md](../hand-motion.md) |

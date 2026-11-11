## Движок: анимация

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

Все — в `common/graphics/mesh/`.

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Инерциальные переходы | `OPENGOTHIC_ANIBLEND=0` | `animblend.*` | бенч `anim` | — |
| Остановка по ноге: из петли хода с левой или правой ноги по фазе петли, если в своём контенте есть клип на правую (`T_RUNR_2_RUN`) с флагом `SETTLE` (64); такая остановка уступает другой анимации, когда корень встал. С клипами базы поведение прежнее | `OPENGOTHIC_STOPFOOT=0` | `stopfoot.*`, `pose.cpp` (`footTransition`), `animation.cpp` (`canInterrupt`) | контракт `gothic-stopfoot-contract` (`check-stopfoot`), бенч `hero-run-startstop` | [пилот, шаг 3](../../reports/content/2026-11-hero-locomotion/README.md#пилот-шаг-3-старт-и-остановки-бега) |
| Foot IK, у четвероногих тоже | `OPENGOTHIC_FOOTIK=0` | `footik.*` | бенчи `ik-ab`, `ik-moves`, `ik-stairs`, `ik-sweep`, `quad-wolf`, `quad-zoo` | — |
| Моргание | `OPENGOTHIC_BLINK=0` | `eyeblink.*` | бенчи `blink`, `listen` | — |
| Жесты рук при речи | `OPENGOTHIC_GESTURES=0` | `talkgestures.*` | бенч `gestures` | — |
| Губы по голосу | `OPENGOTHIC_LIPSYNC=0` | `lipsync.*` | бенчи `lips`, `lips-dialog` | — |
| Взгляд | `OPENGOTHIC_GAZE=0` | `gaze.*` | бенчи `gaze`, `gaze-ambient` | — |
| Наклон на бегу: крен в повороте, наклон корпуса на склоне | `OPENGOTHIC_LEAN=0` | `lean.*` | бенч `lean` | — |
| Наклон от ускорения: корпус клонится вперёд при разгоне и назад при торможении, до 7° и 5°, поверх любого клипа; на ходу и в стойке сразу после остановки | `OPENGOTHIC_LEANACCEL=0` (и `OPENGOTHIC_LEAN=0`) | `leanaccel.*`, `lean.cpp` | `check-leanaccel`, бенч `hero-run-startstop` (колонка наклона) | в атаке, прыжке и воде не работает; наклон только корпуса, таз не приседает |
| Конец перехода без рывка: за последним кадром клипа, который не петля, корень сохраняет скорость, с которой клип кончился (раньше повторял первый шаг клипа) | `OPENGOTHIC_TRANSEND=0` | `transend.h`, `animation.cpp` (`translateXZ`) | `check-leanaccel`, бенч `hero-run-startstop` (скорость по кадрам) | меняет путь каждого перехода с движением на один шаг его конечной скорости; клипы полёта (прыжки) не тронуты |
| Вздрагивание от ударов | `OPENGOTHIC_FLINCH=0` | `flinch.*` | бенч `flinch` | — |
| Стрелок поворачивает корпус к цели | `OPENGOTHIC_AIM=0` | `aim.*` | бенч `aim` | — |
| Тетива натягивается рукой | `OPENGOTHIC_BOWDRAW=0` | `bowdraw.*` | бенч `bow-draw` | [manual-aim.md](../manual-aim.md) |
| Выпуск стрелы как у лучника: кисть к уху, лук бьёт, поворачивается и дрожит в руке, тетива по силе выстрела | `OPENGOTHIC_BOWRELEASE=0`; стенд `bowrelease on\|off` | `bowdraw.*`, `common/game/archery.*` (`releaseKick`) | бенч `bow-release`, контракт `gothic-archery-contract` | [manual-aim.md](../manual-aim.md) |
| Колчан за правым боком у героя и NPC с луком: модель `ITRW_QUIVER` на кольце у таза, жёсткий, одним поворотом до 12° уступает ноге, руке, полам и снаряжению на спине, а прижатый землёй ложится на неё до 90° (лучи по миру только у земли), кольцо сдвигается под броню по её мешу, стрелы по ступеням числа | `OPENGOTHIC_QUIVER=0` | `common/graphics/mesh/quiver.*`, `MdlVisual::updateQuiver`, `Npc::updateQuiver` | бенчи `quiver-mount`, `quiver-quick`, `quiver-clips`, `quiver-rolls` | [manual-aim.md](../manual-aim.md) |
| Перезарядка из колчана (пилот): свои клипы `T_BOWRELOADQ` трёх ступеней в контенте встают на место `T_BOWRELOAD`; `BowDraw` держит прицел, ведёт правую руку и голову по клипу, кисть — к живому колчану и к тетиве; колчан показывает на стрелу меньше со стрелой в руке. Клипов в контенте пока нет, перезарядка — G2. Пока лук в прицеле, колчан откидывается на кольце до 45°, чтобы оперение не уходило в бок, и возвращается, когда лук опущен | `OPENGOTHIC_QUIVERRELOAD=0`; стенд `quiver lift on\|off` | `bowdraw.*` (`ownReload`, колонки `rq_*`), `animation.cpp`, `quiver.*` (`frameNow`, `reloadFrame`, `step`), `scripts/content/anim/reload_quiver.py` | бенч `bow-reload` | [manual-aim.md](../manual-aim.md) |
| Шаги с поднятым луком | `OPENGOTHIC_AIMWALK=0` | `aimwalk.*` | бенч `aim-walk` | [manual-aim.md](../manual-aim.md) |
| Зоны попадания по костям | нет | `hitzones.*` | бенч `zones` | — |

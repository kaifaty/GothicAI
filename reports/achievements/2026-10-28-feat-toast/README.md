# Тост достижения в игре, 2026-10-28

Проверка вида тоста `feat_toast` (протокол v53, [achievements.md](../../../docs/achievements.md#как-видно-игроку)) на Mac, кадрами самого движка.

Как снято: [`toast_probe.py`](toast_probe.py) — подставной ИИ-сервер. После загрузки мира он ждёт 12 с и шлёт по тосту каждые 9 с с иконками категорий из набора стиля. Стенд [`toast.bench`](toast.bench) с `ui on` снимает кадры с интерфейсом. Окно — 1280×800, новая игра.

```
uv run --with websockets python toast_probe.py 47899 server/src/gothic_ai/assets/feats/categories
OPENGOTHIC_BENCH=toast.bench OPENGOTHIC_BENCH_OUT=out OPENGOTHIC_BENCH_REALTIME=1 GOTHIC_AI_URL=ws://127.0.0.1:47899 \
  Gothic2Notr -g <Gothic II> -window -devmode -nomenu -nointro
```

Папку `out` нужно создать заранее: если её нет, игра падает на сохранении кадра.

- [`toast-full.jpg`](toast-full.jpg) — весь кадр. Тост стоит в левом верхнем углу и не задевает полоску здоровья врага, субтитры и окно сделки справа.
- [`toasts.jpg`](toasts.jpg) — три тоста подряд.

В первом прогоне длинное название («Волки, которых не было») переносилось и наезжало на строку. Теперь высота названия считается по переносу, а тост стал шире — название в четыре слова умещается в одну строку.

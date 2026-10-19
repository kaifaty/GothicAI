# Сравнение плиточных PBR-материалов

Готовы **50 материалов, 10 листов и 10 снимков из игры**. Пропущенных моделей больше нет: соглашение FLUX принял пользователь. Все ресурсы — `draft`; генератор выбирает пользователь.

[Реестр](../../../data/content/registry.jsonl) хранит английские промпты, negative, сиды, параметры, полные ревизии HF, лицензии, замеры и пути карт. Генераторы получали только текст. Marigold и фильтр обрабатывали созданные плитки; пиксели Gothic им не передавались.

## Листы материалов

Пять колонок: FLUX+Marigold, FLUX+фильтр, SDXL+Marigold, SDXL+фильтр, StableMaterials. Четыре строки: цвет 2×2 для просмотра швов, нормаль, шероховатость, плоскость под косым светом. [CPU-превью](../../../scripts/content/material_preview.py) считает Ламберт + GGX в numpy с одинаковыми камерой, светом и экспозицией.

| Материал | Лист |
|---|---|
| Булыжная мостовая | [Открыть](cobblestone.jpg) |
| Каменная кладка городской стены | [Открыть](stone-wall.jpg) |
| Штукатурка фахверка | [Открыть](plaster.jpg) |
| Доски пола | [Открыть](floorboards.jpg) |
| Брёвна и балки | [Открыть](timber.jpg) |
| Соломенная крыша | [Открыть](thatch.jpg) |
| Черепица | [Открыть](roof-tiles.jpg) |
| Утоптанная земля и грязь | [Открыть](mud.jpg) |
| Лесная подстилка с травой | [Открыть](forest-floor.jpg) |
| Скала | [Открыть](rock.jpg) |

## Снимки из игры

Ryzen, дневной префикс исходного [rf.bench](../../render-features/2026-10-18/tools/rf.bench): `NW_CITY_MERCHANT_SHOP01_FRONT_03`, face 0, игровая камера, полдень, 26 секунд ожидания. Подмена через `OPENGOTHIC_CONTENT`: только `NW_MISC_OLDSTONEPATH_01.TGA` либо `NW_CITY_WALL_03.TGA`, scale=1. Мостовая видна перед героем, стена — на левом фасаде дома.

| Конвейер | Мостовая | Стена |
|---|---|---|
| FLUX+Marigold | [Снимок](game-cobblestone-flux-marigold.jpg) | [Снимок](game-stone-wall-flux-marigold.jpg) |
| FLUX+фильтр | [Снимок](game-cobblestone-flux-filter.jpg) | [Снимок](game-stone-wall-flux-filter.jpg) |
| SDXL+Marigold | [Снимок](game-cobblestone-sdxl-marigold.jpg) | [Снимок](game-stone-wall-sdxl-marigold.jpg) |
| SDXL+фильтр | [Снимок](game-cobblestone-sdxl-filter.jpg) | [Снимок](game-stone-wall-sdxl-filter.jpg) |
| StableMaterials | [Снимок](game-cobblestone-stablematerials.jpg) | [Снимок](game-stone-wall-stablematerials.jpg) |

Все 10 игр завершились кодом 0; загрузка материала и нужная точка подтверждены журналами. Заявки `gq`: `c35d79720ea8` (прежние 6), `6622a795be67` (2 FLUX+фильтр), `22af7a408206` (2 FLUX+Marigold). JPG уменьшены с 1920×1080 до 1280×720. Цвет и ORM — DXT1, нормаль — DXT5nm, с mip-уровнями. Игра учитывает нормаль и AO; GGX показан в CPU-превью.

При упаковке DDS для Z<0 ставится Z=1e-6 с нормализацией, затем меняется знак Y под базис движка. **Прижаты 2 из 1 048 576 пикселей стены SDXL+Marigold; у остальных девяти вариантов, включая четыре FLUX, — 0.** Исходные PNG сохраняются: четыре нормали FLUX после упаковки совпадают по SHA-256 с оригиналами на dev-ubuntu; шесть прежних проверены до и после своей упаковки.

## Скорость и VRAM

Замер 19 октября 2026, RTX 3090. Время включает текстовое кодирование, карты и сохранение PNG, без скачивания и загрузки моделей. У FLUX и SDXL одна исходная плитка на пару способов получения карт; её время учтено в обеих строках.

| Конвейер | Размер | Медиана, с | Сумма на 10, с | Пик allocated / reserved, MiB |
|---|---:|---:|---:|---:|
| FLUX+Marigold | 1024 | 13,99 | 144,00 | 6373,9 / 7230,0 |
| FLUX+фильтр | 1024 | 8,61 | 88,15 | 6373,9 / 7230,0 |
| SDXL+Marigold | 1024 | 17,07 | 174,85 | 5439,6 / 9714,0 |
| SDXL+фильтр | 1024 | 11,79 | 118,68 | 5439,6 / 9714,0 |
| StableMaterials | 512 | 3,34 | 33,78 | 3831,1 / 5416,0 |

Это пики CUDA allocator процесса при получении карт, без памяти остальных сервисов и полного контекста драйвера. Успешные стадии заняли 352,631 с плюс 53,598 с загрузки. Скачивание FLUX отдельно заняло 1698,734 с. Перед загрузками и GPU-инференциями выполнялся `nvidia-smi`.

FLUX: ревизия `741f7c3ce8b383c54771c7003378a50191e9efe9`, 4 шага, guidance 0, NF4 с double quantization для transformer и T5, BF16, CPU offload. T5 сначала кодирует текст и выгружается; затем загружается transformer. VAE использует circular padding, denoiser через шаг сдвигает латенты на полплитки и возвращает предсказание в исходные координаты. Negative при guidance 0 не используется. Неявная HF-авторизация отключена; явная авторизация применяется только к FLUX.

SDXL: 30 шагов, guidance 7, FP16, CPU offload, circular padding UNet/VAE с асимметричным Downsample2D. Marigold: 4 шага, ensemble 1, 1024 px, circular UNet/VAE. Фильтр: периодический bandpass яркости σ=2/32 px, нормаль и AO из высоты, шероховатость по виду материала с вариацией яркости, металличность 0. StableMaterials: родные карты 512 px, 50 DDIM-шагов, guidance 10, `tileable=True`; AO добавлен из высоты. Сиды 31000–31009, Marigold 41000–41009; точные рецепты — в реестре.

До успешного NF4-прогона были две ошибки разных причин: не хватало `protobuf` (добавлен 5.29.5); int8 после T5 оставил 11 353 MiB при требуемых 14 000 перед transformer. Каждая причина встретилась один раз. Изображения получены следующим, успешным прогоном NF4. После замера резерв для повторного запуска FLUX снижен до 10 000 MiB.

## Лицензии

| Модель | Лицензия |
|---|---|
| [FLUX.1-schnell](https://huggingface.co/black-forest-labs/FLUX.1-schnell) | Apache-2.0 |
| [SDXL base 1.0](https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0) | CreativeML OpenRAIL++-M |
| [Marigold normals v1-1](https://huggingface.co/prs-eth/marigold-normals-v1-1) | CreativeML OpenRAIL++-M |
| [Marigold appearance v1-1](https://huggingface.co/prs-eth/marigold-iid-appearance-v1-1) | CreativeML OpenRAIL++-M |
| [StableMaterials](https://huggingface.co/gvecchio/StableMaterials) | `openrail` из HF metadata |

[Лицензия SDXL](https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0/blob/462165984030d82259a11f4367a4eed129e94a7b/LICENSE.md) допускает результат с её ограничениями; правообладатель не заявляет прав на результат. StableMaterials называет разработку игр своим применением, но публикует только `openrail` без точного подварианта; это отражено в реестре.

## Остановки сервисов

Время UTC, 19 октября 2026. Окно — от запроса stop до ответа последнего восстанавливаемого юнита. Готовность: `is-active=active`, Gemma HTTP 200, ответ WS-порта Decider; его фактический адрес определяется через `ss` до остановки.

| Стадия | Юниты | Stop | Start | Готовы | Секунд |
|---|---|---|---|---|---:|
| StableMaterials | Gemma | 04:28:00 | 04:28:42 | 04:28:48 | 48 |
| SDXL | Gemma | 04:29:20 | 04:31:27 | 04:31:33 | 133 |
| Marigold normals, SDXL | Gemma | 04:31:41 | 04:32:13 | 04:32:17 | 36 |
| Marigold appearance, SDXL | Gemma | 04:32:20 | 04:32:55 | 04:32:59 | 39 |
| FLUX, нет protobuf | Gemma + Decider | 05:49:20 | 05:49:36 | 05:50:22 | 62 |
| FLUX int8, проверка VRAM | Gemma + Decider | 05:51:39 | 05:51:55 | 05:52:39 | 60 |
| FLUX NF4 | Gemma + Decider | 05:59:55 | 06:01:54 | 06:02:39 | 164 |
| Marigold normals, FLUX | Gemma | 06:08:21 | 06:08:57 | 06:09:06 | 45 |
| Marigold appearance, FLUX | Gemma | 06:09:14 | 06:09:52 | 06:10:00 | 46 |

**Все юниты восстановлены.** Сумма окон — 633 с; при отдельном подсчёте каждого юнита — 799 с из разрешённых 3600 с. Перед остановками раннер дожидался отсутствия игры на Mac. Итоговая проверка: Gemma HTTP 200, Decider HTTP 426, оба `active`.

## Файлы и проверки

Карты: `~/gothic-ai/materials/out/bakeoff/` на dev-ubuntu; копия — `~/.local/share/gothic-ai/content/bakeoff/` на Mac. Получены 300 карт PNG и 20 исходных плиток. Каталог задачи занимает 56,37 ГБ из 80 ГБ. В git — рецепты и 20 уменьшенных JPG; веса, PNG и DDS — вне git.

Реестр проверил 50 записей и внешние файлы; 30 прежних записей не изменились. Четыре новых нормали совпадают с оригиналами после DDS-упаковки. Все 66 целевых тестов прошли; они охватывают реестр, явную авторизацию, освобождение T5, выбор источника Marigold, восстановление юнитов и изоляцию двух ожидающих захватов. Результаты генерации — 70 успешных диффузионных инференций; текстовые кодирования посчитаны отдельно.

Повторить снимки: `RYZEN_LOCK_WAIT=7200 scripts/content/capture-materials.sh --content-root ~/.local/share/gothic-ai/content --output ~/.local/share/gothic-ai/content/bakeoff/captures`. Каждый RUN сохраняет свои бинарник, сейв и скрипты в `inputs/$RUN/runtime`. Исходные кадры — в `captures/<RUN>/captures/<id>/city.png` на Mac и `~/gothic-ai/material-bakeoff/runs/<RUN>/<id>/city.png` на Ryzen; RUN: `20261019T044335Z-19309`, `20261019T060648Z-5myjQnd1`, `20261019T061038Z-JjT6669o`. Генерация: [bakeoff.py](../../../scripts/content/bakeoff.py), [run-materials.sh](../../../scripts/content/run-materials.sh), [requirements.txt](../../../scripts/content/requirements.txt).

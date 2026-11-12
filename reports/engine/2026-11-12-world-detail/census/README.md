# Перепись геометрии мира

Скрипты, которыми сняты числа [заметки](../README.md#1-эталон-и-наш-мир-в-числах). Только читают базу; результаты пишут в текущую папку, поэтому запускать из рабочей папки вне репозитория. Нужен `numpy`. База берётся из `OPENGOTHIC_BASE` или самая свежая в `~/.cache/gothic-ai/base`.

```
python3 <repo>/reports/engine/2026-11-12-world-detail/census/load.py NEWWORLD      # куски -> NEWWORLD.npz
python3 <repo>/reports/engine/2026-11-12-world-detail/census/census.py NEWWORLD 1  # классы, плотность, уклоны, слои на сетке 1 м
python3 <repo>/reports/engine/2026-11-12-world-detail/census/vobs.py NEWWORLD      # объекты по классам
```

`classify.py` — правила классов материалов по именам; это приближение, сверяйте по таблице 40 крупнейших материалов в отчёте. На Mac весь прогон `NEWWORLD` занимает меньше минуты на одном ядре.

Эти скрипты заменил инструмент [`scripts/content/world/world_census.py`](../../../../scripts/content/world/world_census.py) (шаг Д1 [плана ландшафта](../../../../docs/landscape-plan.md)): те же определения одной командой, по области и с контентом; скрипты остаются записью исследования.

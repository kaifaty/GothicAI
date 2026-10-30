#!/usr/bin/env python3
"""Calibrate and freeze a validation-only navigation screen from existing runs."""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
from itertools import combinations
import json
import math
from pathlib import Path
import statistics

import nav_rl as rl


MODELS = ("it35", "bc3", "hd", "bc4")
METRICS = ("point", "object", "approach", "follow_below_gate", "jump", "detour",
           "safety", "harmful_fall", "water", "swimming")
SOURCE_PATHS = {
    "it35": ("archive/nav-bc3-20261029/validation/it35/metrics.json",
             "archive/nav-dagger3-20261030/detour-baseline/it35/metrics.json"),
    "bc3": ("archive/nav-dagger3-20261030/hd-validation/bc3/metrics.json",
            "archive/nav-dagger3-20261030/detour-baseline/bc3/metrics.json"),
    "hd": ("archive/nav-dagger3-20261030/hd-validation/hd/metrics.json",
           "archive/nav-dagger3-20261030/hd-detour/metrics.json"),
    "bc4": ("nav-bc4/validation/bc4/metrics.json", "nav-bc4/detour-validation/bc4/metrics.json"),
}


def identity(value):
    return value["family"] + "/" + value["id"]


def export_history(root):
    """Read only eight named validation summaries and their episode metadata."""
    result = dict(sources={}, models={}, scenarios={}, recovered_safety=[])
    keys = ("id family goal_kind split region action_seed scenario_seed start goal success elapsed_ms "
            "decisions follow_observed_ms follow_in_band_ms transition_success entered_deep_water "
            "swimming safety").split()
    for model, paths in SOURCE_PATHS.items():
        episodes, sources = [], []
        for relative in paths:
            path = root / relative
            raw = path.read_bytes()
            measured = json.loads(raw)["episodes"]
            sources.append(dict(path=str(path), sha256=hashlib.sha256(raw).hexdigest(), episodes=len(measured)))
            for episode in measured:
                if episode["split"] != "validation":
                    raise ValueError("Historical screen inputs must be validation only")
                directory = path.parent / "validation" / episode["family"] / episode["id"] / f"seed-{episode['action_seed']}"
                native = rl.read_json(directory / "episode.json")
                scenario = native["validation_scenario"]
                if scenario["split"] != "validation":
                    raise ValueError("Historical scenario must be validation only")
                key = identity(scenario)
                if key in result["scenarios"] and result["scenarios"][key] != scenario:
                    raise ValueError(f"Historical scenario changed: {model}/{key}")
                result["scenarios"][key] = scenario
                value = {key: episode[key] for key in keys if key in episode}
                if episode["family"] == "safety" and "safety" not in value:
                    # The recorded it35 fence summary predates this field;
                    # assess its retained physical frames with the same scorer.
                    value["safety"] = rl.validation_outcome_metrics(directory, native, None, dict(episode))["safety"]
                    frames = directory / "perception-safety-frames.jsonl"
                    result["recovered_safety"].append(dict(model=model, id=episode["id"],
                        action_seed=episode["action_seed"], frames=str(frames),
                        sha256=hashlib.sha256(frames.read_bytes()).hexdigest()))
                value["collector_wall_s"] = native["collector_wall_s"]
                episodes.append(value)
        result["models"][model], result["sources"][model] = episodes, sources
    validate_history(result)
    return result


def validate_history(history):
    reference = None
    for model in MODELS:
        episodes = history["models"][model]
        keys = {(identity(ep), ep["action_seed"]) for ep in episodes}
        expected = {(key, seed) for key in history["scenarios"] for seed in rl.ACTION_SEEDS}
        if len(keys) != len(episodes) or keys != expected or (reference is not None and keys != reference):
            raise ValueError(f"Historical validation cohorts differ: {model}")
        if any(ep["split"] != "validation" for ep in episodes):
            raise ValueError("Historical screen inputs must be validation only")
        reference = keys
    if any(value["split"] != "validation" for value in history["scenarios"].values()):
        raise ValueError("Screen cannot include test or train scenarios")


def components(episode, metric):
    """Numerator/denominator for the original pooled physical outcome."""
    family = episode["family"]
    if metric in ("water", "swimming"):
        if family != "safety" or episode["region"] != "water":
            return None
        return float(episode["entered_deep_water" if metric == "water" else "swimming"]), 1.0
    if metric == "harmful_fall":
        return (float(episode["safety"]["harmful_fall"]), 1.0) if family == "safety" else None
    if family != metric:
        return None
    if metric == "follow_below_gate":
        return float(episode["follow_in_band_ms"]), float(episode["follow_observed_ms"])
    return float(episode["transition_success"] if metric == "jump" else episode["success"]), 1.0


def clusters(episodes, metric):
    result = defaultdict(lambda: [0.0, 0.0])
    for episode in episodes:
        value = components(episode, metric)
        if value is not None:
            target = result[identity(episode)]
            target[0] += value[0]
            target[1] += value[1]
    return result


def paired_difference(left, right, metric):
    """Paired route-cluster delta-method SE; seeds are repeated observations."""
    a, b = clusters(left, metric), clusters(right, metric)
    if not a or a.keys() != b.keys():
        raise ValueError(f"Paired metric cohorts differ: {metric}")
    count = len(a)
    den_a, den_b = sum(v[1] for v in a.values()), sum(v[1] for v in b.values())
    if not den_a or not den_b:
        return dict(left=None, right=None, delta=None, se=None, routes=count)
    rate_a = sum(v[0] for v in a.values()) / den_a
    rate_b = sum(v[0] for v in b.values()) / den_b
    influence = [(a[key][0] - rate_a * a[key][1]) / (den_a / count)
                 - (b[key][0] - rate_b * b[key][1]) / (den_b / count) for key in sorted(a)]
    se = statistics.stdev(influence) / math.sqrt(count) if count > 1 else None
    return dict(left=rate_a, right=rate_b, delta=rate_a - rate_b, se=se, routes=count)


def agreement(full, screen):
    result = []
    for metric in METRICS:
        for left, right in combinations(MODELS, 2):
            reference = paired_difference(full[left], full[right], metric)
            sample = paired_difference(screen[left], screen[right], metric)
            significant = reference["se"] is not None and abs(reference["delta"]) > 2 * reference["se"]
            matches = (sample["delta"] is not None and reference["delta"] * sample["delta"] > 0)
            result.append(dict(metric=metric, left=left, right=right, full=reference, screen=sample,
                               significant=significant, agrees=matches if significant else None,
                               sign_matches=matches or reference["delta"] == sample["delta"] == 0))
    return result


def subset(history, selected):
    return {model: [ep for ep in episodes if identity(ep) in selected]
            for model, episodes in history["models"].items()}


def ordered_routes(scenarios, family):
    strata = defaultdict(list)
    for key, value in scenarios.items():
        if value["family"] != family:
            continue
        stratum = (value["region"],)
        if family == "object":
            stratum += (value["place_id"],)
        if family == "detour":
            stratum += (value["goal_kind"], value["obstacle_type"])
        digest = hashlib.sha256(("nav-eval-screen-20261031\0" + key).encode()).hexdigest()
        strata[stratum].append((digest, key))
    ordered = [sorted(values) for _, values in sorted(strata.items())]
    # One route per region/place/obstacle stratum first, then nested rounds.
    return [values[index][1] for index in range(max(map(len, ordered))) for values in ordered if index < len(values)], len(strata)


def execution_order(history, selected):
    """Balance six groups of three seed workers using only historical costs."""
    costs = {model: {(identity(ep), ep["action_seed"]): ep["collector_wall_s"]
                     for ep in history["models"][model]} for model in MODELS}
    groups = [[] for _ in range(6)]
    loads = [[[0.0] * 3 for _ in MODELS] for _ in groups]
    capacities = [len(selected) // 6 + (index < len(selected) % 6) for index in range(6)]
    ordered = sorted(selected, key=lambda key: (-max(sum(costs[model][key, seed] for seed in rl.ACTION_SEEDS)
                                                    for model in MODELS), key))
    for key in ordered:
        cost = [[costs[model][key, seed] for seed in rl.ACTION_SEEDS] for model in MODELS]
        group = min((index for index in range(6) if len(groups[index]) < capacities[index]),
                    key=lambda index: (max(loads[index][mi][si] + cost[mi][si] for mi in range(4) for si in range(3)),
                                       sum(map(sum, loads[index])), index))
        groups[group].append(key)
        for mi in range(4):
            for si in range(3):
                loads[group][mi][si] += cost[mi][si]
    return [group[index] for index in range(max(map(len, groups))) for group in groups if index < len(group)]


def schedule_seconds(episodes, ordered, parallel=18):
    by_key = {(identity(ep), ep["action_seed"]): ep["collector_wall_s"] for ep in episodes}
    jobs = [(key, seed) for key in ordered for seed in rl.ACTION_SEEDS]
    # Same strided worker assignment as drive_validation; reserve two minutes
    # for process startup and the ordinary/jump world loads, outside collectors.
    return 120 + max(sum(by_key[job] for job in jobs[index::parallel]) for index in range(min(parallel, len(jobs))))


def select(history):
    validate_history(history)
    selected = set()
    trace = []
    families = sorted({value["family"] for value in history["scenarios"].values()})
    for family in families:
        ordered, minimum = ordered_routes(history["scenarios"], family)
        minimum = max(2, minimum)  # A paired SE needs more than one route.
        if family == "safety":
            minimum = len(ordered)  # All ten physical fixtures, including all water.
        relevant = [metric for metric in METRICS if metric == family or
                    (family == "safety" and metric in ("harmful_fall", "water", "swimming"))]
        full = history["models"]
        constraints = [(metric, left, right, paired_difference(full[left], full[right], metric))
                       for metric in relevant for left, right in combinations(MODELS, 2)]
        constraints = [(metric, left, right, ref) for metric, left, right, ref in constraints
                       if ref["se"] is not None and abs(ref["delta"]) > 2 * ref["se"]]
        for size in range(minimum, len(ordered) + 1):
            chosen = set(ordered[:size])
            sample = subset(history, chosen)
            mismatches = sum((delta := paired_difference(sample[left], sample[right], metric)["delta"]) is None
                             or delta * ref["delta"] <= 0 for metric, left, right, ref in constraints)
            trace.append(dict(family=family, routes=size, mismatches=mismatches))
            if not mismatches:
                selected.update(chosen)
                break
        else:
            raise ValueError(f"Full cohort does not reproduce itself: {family}")
    return selected, trace


def freeze(history, output):
    selected, trace = select(history)
    values = [dict(scenario=history["scenarios"][key], action_seeds=list(rl.ACTION_SEEDS))
              for key in execution_order(history, selected)]
    content = "".join(json.dumps(value, sort_keys=True) + "\n" for value in values)
    if output.exists() and output.read_text(encoding="utf-8") != content:
        raise ValueError(f"Refusing to replace frozen eval screen: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists():
        output.write_text(content, encoding="utf-8")
    return selected, trace


def report(history, frozen, output, measured=None):
    jobs = rl.eval_screen_jobs(frozen)
    selected = {identity(value) for _, value, _ in jobs}
    ordered = [identity(value) for _, value, _ in jobs[::len(rl.ACTION_SEEDS)]]
    if any(value != history["scenarios"].get(identity(value)) for _, value, _ in jobs):
        raise ValueError("Frozen screen left its original validation scenarios")
    sample = subset(history, selected)
    comparisons = agreement(history["models"], sample)
    required = [row for row in comparisons if row["significant"]]
    failed = [row for row in required if not row["agrees"]]
    _, trace = select(history)
    lines = ["# Быстрый предварительный замер навигации", "", "2026-10-31.", "",
             f"Заморожено **{len(selected)} маршрутов ×3 seed = {len(jobs)} эпизодов** из validation; "
             f"полный набор — 618 маршрутов / 1854 эпизода. Согласие значимых сравнений: "
             f"**{len(required) - len(failed)}/{len(required)}**, расхождений {len(failed)}.", "",
             f"SHA-256 `eval-screen.jsonl`: `{hashlib.sha256(frozen.read_bytes()).hexdigest()}`.", "",
             "## Подбор и неопределённость", "",
             "Подбор использует только готовые результаты it35, BC3, HD, BC4. Новые поездки для подбора не выполнялись; "
             "test не читался. Три action seed каждого выбранного маршрута сохранены: 18102027/18102028/18102029. "
             "Исходные сценарии, seed мира, лимиты и физические критерии не меняются. Два прежних отказа подготовки moving отсутствуют в исходных 1491 эпизодах.", "",
             "В старом summary it35 у трёх seed `maleth-fence` отсутствовал вложенный safety. Вредные падения восстановлены "
             "по сохранённым perception-safety-frames прежним оценщиком; исходные файлы не переписаны, отсутствие поля не подменено нулём.", "",
             "Маршруты идут в фиксированном SHA-256 порядке, раундами по стратам: регион для каждого семейства, "
             "дополнительно place_id для object и вид цели/тип препятствия для detour. Берётся первый префикс семейства, "
             "который сохраняет все его значимые парные знаки, минимум один маршрут каждой страты и два маршрута семейства. Все десять safety-постановок сохранены целиком. "
             "Это минимальный проходящий префикс заданного порядка, не поиск глобального минимума произвольных подмножеств.", "",
             "Порядок исполнения заморожен отдельно от подбора состава: шесть групп по три worker сбалансированы "
             "по историческим collector_wall_s четырёх моделей, без изменения сценариев или результатов.", "",
             "Полная разница считается значимой строго при |Δ| > 2 SE. SE парная, delta method по кластерам маршрутов: "
             "три seed не считаются независимыми маршрутами. Для follow используется отношение суммарного времени в полосе "
             "к суммарному наблюдаемому времени; влияние кластера учитывает оба знаменателя. Для остальных метрик — доля эпизодов. "
             "Переходы требуют transition_success; safety проверяется и по достижению цели, и по вредным падениям; "
             "вода отдельно по глубоким входам и плаванию. Для падений/воды/плавания меньше — лучше.", "",
             "Проверка ретроспективная на тех же четырёх моделях, по которым выбран размер: она подтверждает только их порядок "
             "на этих validation-данных. Будущая модель может расходиться с полным замером. Быстрый набор не заменяет полный и nav-live.", "",
             "| Семейство | Маршрутов | Эпизодов | Проверено префиксов |", "|---|---:|---:|---:|"]
    for family in sorted({value["family"] for _, value, _ in jobs}):
        count = sum(history["scenarios"][key]["family"] == family for key in selected)
        lines.append(f"| {family} | {count} | {3 * count} | {sum(t['family'] == family for t in trace)} |")
    lines += ["", "## Время", "",
              "Оценка занятого времени заявки: максимум суммы исторических collector_wall_s по 18 назначенным worker, "
              "плюс 120 с на запуск процессов и загрузку ordinary/jump. Это оценка по прежней нагрузке, не новый бенч; "
              "время сборки и ожидание очереди в 20 минут исполнения не входят. Сумма collector_wall_s включает одновременную работу, это не CPU-секунды.", "",
              "| Модель | Полный, оценка мин | Быстрый, оценка мин | Сумма collector, с |", "|---|---:|---:|---:|"]
    for model in MODELS:
        full_time = schedule_seconds(history["models"][model], sorted(history["scenarios"]))
        estimate = schedule_seconds(history["models"][model], ordered)
        lines.append(f"| {model} | {full_time / 60:.2f} | {estimate / 60:.2f} | {sum(ep['collector_wall_s'] for ep in sample[model]):.1f} |")
    lines += ["", "## Согласие всех пар", "",
              "Δ = левая модель − правая, в процентных пунктах; SE также в п.п. «В шуме» не предъявляет требования к знаку. "
              "Последний столбец показывает все несовпадения знака, включая незначимые полные различия.", "",
              "| Метрика | Пара | Δ полный | 2 SE | Δ быстрый | Согласие значимых | Знак |",
              "|---|---|---:|---:|---:|---|---|"]
    for row in comparisons:
        full, small = row["full"], row["screen"]
        status = ("да" if row["agrees"] else "**НЕТ**") if row["significant"] else "в шуме"
        delta = f"{100 * small['delta']:+.3f}" if small["delta"] is not None else "нет наблюдения"
        lines.append(f"| {row['metric']} | {row['left']} − {row['right']} | {100 * full['delta']:+.3f} | "
                     f"{200 * full['se']:.3f} | {delta} | {status} | {'совпал' if row['sign_matches'] else 'расхождение'} |")
    lines += ["", "## Настоящий прогон HD", ""]
    if measured is None:
        lines.append("Ещё не выполнен. Оценки выше не считаются подтверждённым временем настоящего прогона.")
    else:
        actual = rl.read_json(measured / "metrics.json")
        timing = rl.read_json(measured / "timing.json")
        keys = {(identity(ep), ep["action_seed"]) for ep in actual["episodes"]}
        if keys != {(identity(ep), ep["action_seed"]) for ep in sample["hd"]} or len(actual["episodes"]) != len(jobs):
            raise ValueError("Measured HD cohort differs from frozen screen")
        lines += [f"Исполнение: **{timing['elapsed_s']:.3f} с / {timing['elapsed_s'] / 60:.2f} мин**; "
                  f"{len(jobs)} эпизодов. Выход — стандартный `nav_rl.py summarize`, `metrics.json` на NAS рядом с `timing.json`.", "",
                  "| Метрика | HD полный | HD исторический быстрый | HD настоящий быстрый |", "|---|---:|---:|---:|"]
        for metric in METRICS:
            full = paired_difference(history["models"]["hd"], history["models"]["hd"], metric)["left"]
            expected = paired_difference(sample["hd"], sample["hd"], metric)["left"]
            observed = paired_difference(actual["episodes"], actual["episodes"], metric)["left"]
            observed_text = f"{100 * observed:.3f}%" if observed is not None else "неполный: нет наблюдения"
            lines.append(f"| {metric} | {100 * full:.3f}% | {100 * expected:.3f}% | {observed_text} |")
    lines += ["", "## Запуск и правило отбора", "", "```bash",
              "bash scripts/nav-eval-screen-remote.sh /absolute/path/to/policy.navbc", "```", "",
              "Команда подаёт одну заявку gq и возвращает ID без ожидания и опроса очереди. Подготовка, целевые тесты и сборка "
              "идут в собственной папке; замер использует 18 worker, lean, CV, sample, β=0. Веса копируются по переданному пути на Ryzen. "
              "Выход — `/mnt/shared/gothic-ai/nav-eval-screen/runs/<run>/`; build и кэши удаляются при завершении заявки.", "",
              "Воспроизведение подбора на Ryzen (без новых поездок):", "", "```bash",
              "python3 scripts/nav_eval_screen.py history --output /absolute/task/history.json",
              "python3 scripts/nav_eval_screen.py freeze --history /absolute/task/history.json \\",
              "  --output reports/navigation/2026-10-31-eval-screen/eval-screen.jsonl",
              "python3 scripts/nav_eval_screen.py report --history /absolute/task/history.json \\",
              "  --frozen reports/navigation/2026-10-31-eval-screen/eval-screen.jsonl \\",
              "  --output reports/navigation/2026-10-31-eval-screen/README.md",
              "```", "",
              "После каждого обучения быстрый замер обязателен. Полный validation и nav-live разрешены только если кандидат "
              "не хуже текущей лучшей модели сверх 2 парных SE ни по одному критерию. Сравнивать одинаковые замороженные маршруты/seed; "
              "для следования больше времени в полосе лучше, для вредных падений/глубокой воды/плавания меньше лучше. "
              "Если нет наблюдаемого времени follow, результат неполный и кандидат не проходит отбор. Текущая лучшая — HD; "
              "эталон — её настоящий быстрый прогон. Нельзя выбирать новую лучшую по смешанной сумме успехов.", "",
              "## Исходные результаты", "", "| Модель | Файл | Эпизодов | SHA-256 |", "|---|---|---:|---|"]
    for model in MODELS:
        for source in history["sources"][model]:
            lines.append(f"| {model} | `{source['path']}` | {source['episodes']} | `{source['sha256']}` |")
    if history.get("recovered_safety"):
        lines += ["", "Физические кадры восстановленного safety it35:", ""]
        for recovered in history["recovered_safety"]:
            lines.append(f"- Seed {recovered['action_seed']}: `{recovered['sha256']}`.")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return dict(routes=len(selected), episodes=len(jobs), significant=len(required), disagreements=len(failed),
                estimated_max_s=max(schedule_seconds(history["models"][model], ordered) for model in MODELS))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("history", "freeze", "report"))
    parser.add_argument("--archive-root", type=Path, default=Path("/mnt/shared/gothic-ai"))
    parser.add_argument("--history", type=Path)
    parser.add_argument("--frozen", type=Path)
    parser.add_argument("--measured", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "history":
        rl.write_json(args.output, export_history(args.archive_root))
    elif args.command == "freeze":
        selected, _ = freeze(rl.read_json(args.history), args.output)
        print(json.dumps(dict(routes=len(selected), sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())))
    else:
        print(json.dumps(report(rl.read_json(args.history), args.frozen, args.output, args.measured)))


if __name__ == "__main__":
    main()

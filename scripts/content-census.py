#!/usr/bin/env python3
"""Combine OpenGothic resource census JSON files into a Markdown summary."""

import argparse
import json
from pathlib import Path
import sys


KINDS = (
    "texture", "basetexture", "mesh", "basemesh", "basemodel", "model", "skeleton", "anim", "morph", "world", "worldmesh", "baseworld", "basezen",
    "sound", "basesound", "speech", "basespeech", "music", "basemusic", "script", "baseprogram", "cutscene", "baseoutputunits", "font", "basefont", "video", "basevideo", "other",
)


def _normalized(value):
    return value.replace("\\", "/").casefold()


def _replacement_key(row):
    name = _normalized(row["name"])
    if row["kind"] in ("texture", "basetexture"):
        if name.endswith(".tga"):
            name = name[:-4]
        elif name.endswith(".tex"):
            name = name[:-4]
            if name.endswith("-c"):
                name = name[:-2]
    return row["kind"], name


def validate_document(document, label="JSON"):
    """Reject incomplete records instead of silently understating originals."""
    if not isinstance(document, dict):
        raise ValueError(f"{label}: ожидается объект с files и replacements")
    for section, source_field in (("files", "source"), ("replacements", "replacement")):
        if not isinstance(document.get(section), list):
            raise ValueError(f"{label}: {section} должен быть массивом")
        for index, row in enumerate(document[section]):
            location = f"{label}: {section}[{index}]"
            if not isinstance(row, dict):
                raise ValueError(f"{location}: ожидается объект")
            for field in ("name", "kind", source_field):
                if not isinstance(row.get(field), str) or not row[field].strip():
                    raise ValueError(f"{location}: {field} должен быть непустой строкой")
            if row["kind"] not in KINDS:
                raise ValueError(f"{location}: неизвестный вид ресурса {row['kind']!r}")
            if type(row.get("opens")) is not int or row["opens"] < 1:
                raise ValueError(f"{location}: opens должен быть положительным целым числом")
    return document


def summarize(documents):
    """Count original filenames; match texture aliases only against replacements."""
    originals = set()
    original_aliases = set()
    replacements = set()
    base = set()
    accesses = {}
    for index, document in enumerate(documents):
        validate_document(document, f"прогон {index + 1}")
        for row in document["files"]:
            resource = row["kind"], _normalized(row["name"])
            originals.add(resource)
            original_aliases.add(_replacement_key(row))
            key = (*resource, _normalized(row["source"]))
            if key not in accesses:
                accesses[key] = dict(row)
            else:
                accesses[key]["opens"] += row["opens"]
        for row in document["replacements"]:
            resource = _replacement_key(row)
            if row["kind"] in ("basetexture", "basemesh", "basemodel", "basefont", "baseprogram", "baseoutputunits", "basesound", "basespeech", "basemusic", "basevideo", "baseworld", "basezen", "worldmesh"):
                base.add(resource)
                original_kind = "world" if resource[0] in ("basezen", "worldmesh") else resource[0].removeprefix("base")
                if resource[0] == "baseprogram":
                    original_kind = "script"
                if resource[0] == "baseoutputunits":
                    original_kind = "cutscene"
                if resource[0] == "basemodel":
                    original_kind = {".mdh": "skeleton", ".man": "anim", ".mmb": "morph"}.get(
                        Path(resource[1]).suffix, "model"
                    )
                original_aliases.add((original_kind, resource[1]))
            else:
                replacements.add(resource)

    # A replacement does not eliminate a resource opened from an original source
    # elsewhere in the same session or another supplied run.
    own = replacements - original_aliases
    counts = []
    for kind in KINDS:
        original_count = sum(key[0] == kind for key in originals)
        own_count = sum(key[0] == kind for key in own)
        base_count = sum(key[0] == kind for key in base)
        if base_count:
            counts.append({"kind": kind, "original": 0, "own": 0, "base": base_count})
        elif original_count or own_count:
            counts.append({"kind": kind, "original": original_count, "own": own_count})
    ranking = sorted(
        accesses.values(),
        key=lambda row: (-row["opens"], _normalized(row["name"]),
                         KINDS.index(row["kind"]), _normalized(row["source"])),
    )[:20]
    return {"counts": counts, "top": ranking}


def _cell(value):
    return str(value).replace("&", "&amp;").replace("<", "&lt;").replace(
        ">", "&gt;"
    ).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def render_markdown(summary):
    lines = [
        "# Перепись ресурсов Gothic",
        "",
        "Оригинальные файлы считаются по фактическому имени без учёта регистра "
        "и объединяются между прогонами. Для сопоставления подмен текстуры "
        "NAME.TGA и NAME-C.TEX считаются одним ресурсом. "
        "Ресурс считается своим, только если ни один его оригинал не открывался ни в одном прогоне. "
        "Сконвертированный оригинал учитывается отдельно как basetexture/basemesh/basemodel/basefont/baseprogram/baseoutputunits/basesound/basespeech/basemusic/basevideo/baseworld/basezen (прежний меш мира — worldmesh) и не считается своим.",
        "",
        "| Вид | Оригинальных файлов | Подменено своими | Из базы | Доля своих |",
        "|---|---:|---:|---:|---:|",
    ]
    total_original = total_own = total_base = 0
    for row in summary["counts"]:
        original, own = row["original"], row["own"]
        base = row.get("base", 0)
        share = 100 * own / (original + own + base)
        lines.append(f"| {row['kind']} | {original} | {own} | {base} | {share:.1f}% |")
        total_original += original
        total_own += own
        total_base += base
    total = total_original + total_own + total_base
    share = 100 * total_own / total if total else 0
    lines.extend([
        f"| **Всего** | **{total_original}** | **{total_own}** | **{total_base}** | **{share:.1f}%** |",
        "",
        "## Топ-20 оригинальных файлов по числу открытий",
        "",
        "Число открытий суммируется между прогонами; разные источники показаны отдельно.",
        "",
        "| Файл | Вид | Источник | Открытий |",
        "|---|---|---|---:|",
    ])
    for row in summary["top"]:
        lines.append("| " + " | ".join(_cell(row[field]) for field in (
            "name", "kind", "source", "opens"
        )) + " |")
    if not summary["top"]:
        lines.extend(["", "Оригинальные файлы не открывались."])
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", nargs="+", type=Path, help="JSON одного или нескольких прогонов")
    args = parser.parse_args(argv)
    documents = []
    for path in args.json:
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
            documents.append(validate_document(document, str(path)))
        except (OSError, UnicodeError, ValueError) as error:
            parser.error(f"{path}: {error}")
    sys.stdout.write(render_markdown(summarize(documents)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

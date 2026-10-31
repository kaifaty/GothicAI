"""Small, self-contained fixtures for the Gothic resource census summary."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPT = Path(__file__).with_name("content-census.py")
SPEC = importlib.util.spec_from_file_location("content_census", SCRIPT)
census = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(census)


def original(name, kind="texture", source="Textures.vdf", opens=1):
    return {"name": name, "kind": kind, "source": source, "opens": opens}


def replacement(name, kind="texture", opens=1):
    return {"name": name, "kind": kind, "replacement": f"/own/{name}.dds", "opens": opens}


def run(files=(), replacements=()):
    return {"files": list(files), "replacements": list(replacements)}


def test_multiple_runs_merge_names_and_sources_but_retain_distinct_sources():
    summary = census.summarize([
        run([original("STONE-C.TEX", source=r"Data\Textures.vdf", opens=2)]),
        run([original("stone-c.tex", source="data/textures.VDF", opens=3),
             original("STONE-C.TEX", source="Patch.vdf", opens=4)]),
    ])
    assert summary["counts"] == [{"kind": "texture", "original": 1, "own": 0}]
    assert [(row["source"], row["opens"]) for row in summary["top"]] == [
        (r"Data\Textures.vdf", 5), ("Patch.vdf", 4),
    ]


def test_texture_aliases_and_partial_replacement_never_hide_an_original():
    summary = census.summarize([
        run([original("STONE-C.TEX")], [replacement("stone.tga"), replacement("GRASS.TGA")]),
        run([original("grass-c.tex")], [replacement("OWN.TGA"), replacement("own-c.tex")]),
    ])
    assert summary["counts"] == [{"kind": "texture", "original": 2, "own": 1}]
    markdown = census.render_markdown(summary)
    assert "| texture | 2 | 1 | 0 | 33.3% |" in markdown
    assert "| **Всего** | **2** | **1** | **0** | **33.3%** |" in markdown


def test_opened_texture_source_and_compiled_file_keep_individual_counts():
    summary = census.summarize([
        run([original("A.TGA", opens=2), original("A-C.TEX", opens=5)],
            [replacement("a.tga"), replacement("a-c.tex")]),
    ])
    assert summary["counts"] == [{"kind": "texture", "original": 2, "own": 0}]
    assert [(row["name"], row["opens"]) for row in summary["top"]] == [
        ("A-C.TEX", 5), ("A.TGA", 2),
    ]
    markdown = census.render_markdown(summary)
    assert "| A-C.TEX | texture | Textures.vdf | 5 |" in markdown
    assert "| A.TGA | texture | Textures.vdf | 2 |" in markdown
    assert "| texture | 2 | 0 | 0 | 0.0% |" in markdown


def test_replacement_only_resources_count_once_and_kinds_follow_canonical_order():
    summary = census.summarize([
        run([original("GOTHIC.DAT", "script"), original("HERO.MDL", "model")],
            [replacement("OWN.TGA"), replacement("own-c.tex"), replacement("VOICE.WAV", "speech")]),
    ])
    assert summary["counts"] == [
        {"kind": "texture", "original": 0, "own": 1},
        {"kind": "model", "original": 1, "own": 0},
        {"kind": "speech", "original": 0, "own": 1},
        {"kind": "script", "original": 1, "own": 0},
    ]
    assert "| texture | 0 | 1 | 0 | 100.0% |" in census.render_markdown(summary)


def test_legacy_world_mesh_counts_as_base_while_zen_remains_original():
    base = {"name": "NEWWORLD.ZEN", "kind": "worldmesh",
            "replacement": "worlds/NEWWORLD/world.json", "opens": 1}
    summary = census.summarize([
        run([original("NEWWORLD.ZEN", "world", "Worlds.vdf")], [base]),
        run([original("newworld.zen", "world", "worlds.VDF")],
            [dict(base, name="newworld.zen")]),
    ])
    assert summary["counts"] == [
        {"kind": "world", "original": 1, "own": 0},
        {"kind": "worldmesh", "original": 0, "own": 0, "base": 1},
    ]
    markdown = census.render_markdown(summary)
    assert "| world | 1 | 0 | 0 | 0.0% |" in markdown
    assert "| worldmesh | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| NEWWORLD.ZEN | world | Worlds.vdf | 2 |" in markdown


def test_base_resources_are_separate_from_own_replacements():
    texture = {"name": "STONE.TGA", "kind": "basetexture",
               "replacement": "base/textures/STONE.dds", "opens": 2}
    mesh = {"name": "BARREL.MRM", "kind": "basemesh",
            "replacement": "base/meshes/BARREL.glb", "opens": 1}
    world = {"name": "NEWWORLD.ZEN", "kind": "baseworld",
             "replacement": "base/worlds/NEWWORLD/world.json", "opens": 1}
    zen = {"name": "FIRE.ZEN", "kind": "basezen",
           "replacement": "base/zen/FIRE.json", "opens": 1}
    font = {"name": "FONT_OLD_10_WHITE.FNT", "kind": "basefont",
            "replacement": "base/fonts/FONT_OLD_10_WHITE.FNT.json", "opens": 1}
    program = {"name": "GOTHIC.DAT", "kind": "baseprogram",
               "replacement": "base/programs/compiled.json", "opens": 1}
    sound = {"name": "HIT.WAV", "kind": "basesound",
             "replacement": "base/audio/ab/sound.wav", "opens": 1}
    speech = {"name": "DIA_X_7.WAV", "kind": "basespeech",
              "replacement": "base/audio/cd/speech.wav", "opens": 1}
    music = {"name": "THEME.SGT", "kind": "basemusic",
             "replacement": "base/music/gk/segment/record.json", "opens": 1}
    video = {"name": "SHIP.BIK", "kind": "basevideo",
             "replacement": "base/videos/SHIP.BIK.gvideo", "opens": 1377}
    models = [{"name": name, "kind": "basemodel",
               "replacement": f"base/models/{name}.json", "opens": 1}
              for name in ("HERO.MDL", "HUMANS.MDH", "HUMANS-WALK.MAN", "BOW.MMB")]
    summary = census.summarize([
        run([original("STONE-C.TEX")], [texture, mesh, world, zen, font, program, sound, speech, music, video, *models, replacement("OWN.TGA")]),
        run(replacements=[dict(texture, name="stone.tga"), mesh,
                          replacement("STONE.TGA"), replacement("BARREL.MRM", "mesh"),
                          world, zen, replacement("FIRE.ZEN", "world"), dict(font, name=font["name"].lower()),
                          replacement(font["name"].lower(), "font"),
                          dict(program, name="gothic.dat"), replacement("gothic.dat", "script"),
                          dict(sound, name="hit.wav"), replacement("hit.wav", "sound"),
                          dict(speech, name="dia_x_7.wav"), replacement("dia_x_7.wav", "speech"),
                          dict(music, name="theme.sgt", replacement="base/music/dx8/segment/record.json"), replacement("theme.sgt", "music"),
                          dict(video, name="ship.bik"), replacement("ship.bik", "video"),
                          *[replacement(row["name"].lower(), kind) for row, kind in
                            zip(models, ("model", "skeleton", "anim", "morph"))]]),
    ])
    assert summary["counts"] == [
        {"kind": "texture", "original": 1, "own": 1},
        {"kind": "basetexture", "original": 0, "own": 0, "base": 1},
        {"kind": "basemesh", "original": 0, "own": 0, "base": 1},
        {"kind": "basemodel", "original": 0, "own": 0, "base": 4},
        {"kind": "baseworld", "original": 0, "own": 0, "base": 1},
        {"kind": "basezen", "original": 0, "own": 0, "base": 1},
        {"kind": "basesound", "original": 0, "own": 0, "base": 1},
        {"kind": "basespeech", "original": 0, "own": 0, "base": 1},
        {"kind": "basemusic", "original": 0, "own": 0, "base": 1},
        {"kind": "baseprogram", "original": 0, "own": 0, "base": 1},
        {"kind": "basefont", "original": 0, "own": 0, "base": 1},
        {"kind": "basevideo", "original": 0, "own": 0, "base": 1},
    ]
    markdown = census.render_markdown(summary)
    assert "| basetexture | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basemesh | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basemodel | 0 | 0 | 4 | 0.0% |" in markdown
    assert "| basefont | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| baseprogram | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basesound | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basespeech | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basemusic | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| baseworld | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basezen | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| basevideo | 0 | 0 | 1 | 0.0% |" in markdown
    assert "| **Всего** | **1** | **1** | **14** | **6.2%** |" in markdown


def test_top20_uses_summed_opens_and_stable_order_for_ties():
    files = [original(f"FILE{index:02d}.MRM", "mesh", "Meshes.vdf", index + 1)
             for index in range(22)]
    summary = census.summarize([
        run(files),
        run([original("file00.mrm", "mesh", "meshes.VDF", 22)]),
    ])
    assert len(summary["top"]) == 20
    assert [(row["name"], row["opens"]) for row in summary["top"][:3]] == [
        ("FILE00.MRM", 23), ("FILE21.MRM", 22), ("FILE20.MRM", 21),
    ]
    assert summary["top"][-1]["name"] == "FILE03.MRM"
    tied = census.summarize([run([original("B.TGA"), original("a.tga")])])
    assert [row["name"] for row in tied["top"]] == ["a.tga", "B.TGA"]


def test_empty_census_has_zero_totals_without_division_error():
    markdown = census.render_markdown(census.summarize([run()]))
    assert "| **Всего** | **0** | **0** | **0** | **0.0%** |" in markdown
    assert "Оригинальные файлы не открывались." in markdown


@pytest.mark.parametrize("document, message", [
    ([], "ожидается объект"),
    ({"files": []}, "replacements должен быть массивом"),
    (run([None]), "ожидается объект"),
    (run([original("A", kind="unknown")]), "неизвестный вид"),
    (run([original("A", opens=-1)]), "opens должен быть положительным"),
    (run([original("A", opens=True)]), "opens должен быть положительным"),
    (run([original(" ")]), "name должен быть непустой строкой"),
    (run(replacements=[{"name": "A", "kind": "texture", "opens": 1}]),
     "replacement должен быть непустой строкой"),
])
def test_malformed_documents_fail_clearly(document, message):
    with pytest.raises(ValueError, match=message):
        census.summarize([document])


def test_markdown_escapes_resource_names_and_sources():
    markdown = census.render_markdown(census.summarize([
        run([original("A|B<T>.TGA", source="<work>\nfile.vdf")]),
    ]))
    assert r"A\|B&lt;T&gt;.TGA" in markdown
    assert "&lt;work&gt; file.vdf" in markdown


def test_cli_accepts_multiple_json_files_and_reports_path_on_invalid_input(tmp_path):
    first, second = tmp_path / "first.json", tmp_path / "second.json"
    first.write_text(json.dumps(run([original("WORLD.ZEN", "world", "Worlds.vdf", 2)])))
    second.write_text(json.dumps(run([original("world.zen", "world", "worlds.VDF", 3)])))
    result = subprocess.run([sys.executable, str(SCRIPT), str(first), str(second)],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert "| world | 1 | 0 | 0 | 0.0% |" in result.stdout
    assert "| WORLD.ZEN | world | Worlds.vdf | 5 |" in result.stdout
    second.write_text("{invalid")
    result = subprocess.run([sys.executable, str(SCRIPT), str(second)],
                            text=True, capture_output=True)
    assert result.returncode == 2
    assert str(second) in result.stderr
    assert "Traceback" not in result.stderr
    assert result.stdout == ""

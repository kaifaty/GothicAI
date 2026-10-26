import json

from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.perception import PerceptionDesk
from gothic_ai.places import PlaceBook, book, group, kinds, label, side_of, survey, tokens
from gothic_ai.protocol import NpcView
from tests.fakes import Clock


def point(name: str, x: float, z: float, y: float = 0, **more) -> dict:
    return {"name": name, "x": x, "y": y, "z": z, "fp": name.startswith("FP_"), **more}


CAVE_ROOM = "S:BANCAVE01_OW_SURFACE_WASTELAND_04"


def cave_and_steps() -> list[dict]:
    """The bandits' cave below Xardas's tower, as the v2 dump has it: its room, the waynet's links; the way
    through it (WP_XXX_ADD_*) named like nothing; the steps outside, linked to its first point."""

    def chain(names: list[str]) -> dict[str, list[str]]:
        links = {n: [] for n in names}
        for a, b in zip(names, names[1:], strict=False):
            links[a].append(b)
            links[b].append(a)
        return links

    cave = [f"NW_XARDAS_BANDITS_0{i}" for i in range(1, 5)]
    way = ["WP_XXX_ADD", "WP_XXX_ADD_02", "WP_XXX_ADD_03"]
    steps = [f"NW_XARDAS_STAIRS_0{i}" for i in range(1, 5)]
    links = chain(steps + cave + way)
    return [
        *(point(n, 1000 + i * 500, 0, room=CAVE_ROOM, conn=links[n]) for i, n in enumerate(cave)),
        *(point(n, 3500 + i * 500, 0, room=CAVE_ROOM, conn=links[n]) for i, n in enumerate(way)),
        *(point(n, -i * 500, 0, conn=links[n]) for i, n in enumerate(steps[::-1])),
    ]


def test_what_a_point_name_says_of_where_it_is():
    assert tokens("FP_ROAM_NW_FARM1_SHEEP_02") == ["FARM1", "SHEEP"]
    assert tokens("NW_XARDAS_TOWER_IN1_18") == ["XARDAS", "TOWER", "IN1"]
    assert tokens("FP_STAND_NW_FARM1_LOBART") == ["FARM1", "LOBART"]


def test_points_named_alike_and_close_are_one_place():
    points = [
        *(point(f"NW_FARM1_OUT_0{i}", i * 500, 0) for i in range(4)),  # the farmyard
        *(point(f"NW_FARM1_OUT_1{i}", 90000 + i * 500, 0) for i in range(3)),  # the same name, far away
        point("NW_FARM1_LOBART", 800, 300),  # a stray: joins the nearest place of the farm
        point("NW_CITY_SMITH_01", 300000, 0),
        *(point(f"NW_CITY_SMITH_0{i}", 300000 + i * 300, 0) for i in range(2, 5)),
    ]
    people = [
        {"npc": "BAU_950_LOBART", "name": "Лобарт", "routine": [
            {"start": 420, "end": 1380, "state": "ZS_STAND_GUARDING", "waypoint": "NW_FARM1_LOBART"},
        ]},
    ]  # fmt: skip
    groups = {g.id: g for g in group(points, people)}
    assert sorted(groups) == ["CITY_SMITH", "FARM1_OUT", "FARM1_OUT#2"]
    assert "NW_FARM1_LOBART" in {p["name"] for p in groups["FARM1_OUT"].points}
    assert groups["FARM1_OUT"].residents == ["Лобарт: ZS_STAND_GUARDING 07–23 ч"]


def test_a_big_part_of_the_world_goes_by_its_third_word_too():
    tower = [point(f"NW_XARDAS_TOWER_IN1_{i}", i * 100, 0, 1000) for i in range(30)]
    tower += [point(f"NW_XARDAS_TOWER_IN2_{i}", i * 100, 0, 3000) for i in range(30)]
    assert sorted(g.id for g in group(tower)) == ["XARDAS_TOWER_IN1", "XARDAS_TOWER_IN2"]


async def test_the_model_names_places_and_a_broken_answer_is_asked_again():
    points = [point(f"NW_FARM1_OUT_0{i}", i * 500, 0) for i in range(4)]
    groups = group(points)
    named = [
        {"id": "FARM1_OUT", "ru": "двор фермы Лобарта", "where_ru": "во дворе фермы Лобарта",
         "en": "Lobart's farmyard", "where_en": "in Lobart's farmyard"},
    ]  # fmt: skip
    llm = FakeBackend(script=[FakeStep("не JSON"), FakeStep(json.dumps(named, ensure_ascii=False))], delay=0)
    names = await label(groups, llm)
    assert names["FARM1_OUT"]["where_ru"] == "во дворе фермы Лобарта"
    assert "NW_FARM1_OUT_01" in llm.calls[0][1][0].content  # the model sees the points' names

    places = PlaceBook(book(groups, names, "newworld.zen"))
    place = places.of(None, "nw_farm1_out_02")
    assert place is not None and place.ru == "двор фермы Лобарта"
    assert places.of("NW_CITY_SMITH_01") is None

    # perception says it in place of the waypoint
    desk = PerceptionDesk(type("Link", (), {"listeners": []})(), places=places, clock=Clock())  # type: ignore[arg-type]
    view = NpcView(
        npc="BAU_950_LOBART", name="Лобарт", waypoint="NW_FARM1_OUT_02", state="ZS_STAND_ARMSCROSSED"
    )
    desk._views["BAU_950_LOBART"] = (desk.clock(), view)
    sight = await desk.sight("BAU_950_LOBART")
    assert sight is not None
    assert sight.place == "Ты во дворе фермы Лобарта, под открытым небом. Ты стоишь, скрестив руки."
    assert (await desk.english("BAU_950_LOBART"))[0] == "You are in Lobart's farmyard."


def test_the_places_around_a_spot_nearest_first():
    book = PlaceBook.load()
    assert book is not None
    cave = book.get("xardas_bandits")
    assert (
        cave is not None and cave.ru.startswith("пещера бандитов") and "NW_XARDAS_BANDITS_01" in cave.points
    )
    # Cavalorn on the steps below Xardas's tower, where the hero asked him to go into the cave: its way in
    # is a few metres off
    near = book.near((20529.0, 3040.0, -21386.0), 8000, 6)
    assert [p.id for p, _ in near[:2]] == ["XARDAS_BANDITS", "XARDAS_STAIRS"]
    assert near[0][1] < 500
    assert [d for _, d in near] == sorted(d for _, d in near)
    assert "XARDAS_TOWER_WATERFALL" not in {p.id for p, _ in near}  # the other cave is 98 m off


def test_the_book_knows_the_bandits_cave_is_a_cave_entered_from_the_steps():
    # docs/npc-tools.md М3, «книга заново размечена»
    book = PlaceBook.load()
    assert book is not None
    cave = book.get("XARDAS_BANDITS")
    assert cave is not None and cave.kind == "cave" and cave.inside
    assert [e.side for e in cave.entrances] == ["XARDAS_STAIRS"]
    assert book.ways_in_ru(cave) == "вход — на ступенях на дороге от башни Ксардаса"
    assert book.get("WP_XXX") is None and "WP_XXX_ADD_02" in cave.points  # the way through it is the cave
    fire = book.get("XARDAS_GOBBO")
    assert fire is not None and fire.kind == "camp" and not fire.inside  # trees over it, but no room
    assert "XARDAS_STAIRS" in cave.neighbours
    # a place of freepoints only is walked to, and waited at, by its anchor: a waypoint by it
    field = book.get("FARM1_WOLF")
    assert field is not None and all(p.startswith("FP_") for p in field.points)
    assert field.waypoints == ()  # a reach goal cannot use an anchor outside its requested place
    assert field.waynet == ("NW_FARM1_PATH_CITY_05_B",)
    assert book.is_waypoint("nw_farm1_path_city_05_b")
    assert not book.is_waypoint(field.points[0]) and not book.is_waypoint("NW_MISSING")


def test_the_tower_place_that_blocked_reach_has_only_freepoints():
    book = PlaceBook.load()
    assert book is not None
    steps = book.get("XARDAS_TOWER#2")
    assert steps is not None and "FP_ROAM_XARDAS_TOWER_3_03" in steps.points
    assert steps.waypoints == ()
    assert not book.is_waypoint("FP_ROAM_XARDAS_TOWER_3_03")
    assert steps.anchor is not None and book.is_waypoint(steps.anchor)


def test_the_waynet_and_rooms_tell_a_caves_way_in_and_neighbours():
    groups = group(cave_and_steps())
    assert sorted(g.id for g in groups) == ["XARDAS_BANDITS", "XARDAS_STAIRS"]  # the way through: the cave's
    surveys = survey(groups)
    cave, steps = surveys["XARDAS_BANDITS"], surveys["XARDAS_STAIRS"]
    assert cave.inside == 1.0 and cave.rooms == [CAVE_ROOM] and cave.guess == "cave"
    way_in = {"in": "NW_XARDAS_BANDITS_01", "out": "NW_XARDAS_STAIRS_04", "pos": [0, 0, 0]}
    assert cave.entrances == [{**way_in, "side": "XARDAS_STAIRS"}]
    assert cave.neighbours == ["XARDAS_STAIRS"] and steps.neighbours == ["XARDAS_BANDITS"]
    assert steps.inside == 0 and steps.guess == "road" and steps.entrances == []

    names = {
        g.id: {"ru": g.id.lower(), "where_ru": f"у {g.id.lower()}", "en": g.id, "where_en": g.id}
        for g in groups
    }
    names["XARDAS_STAIRS"]["where_ru"] = "на ступенях"
    places = PlaceBook(book(groups, names, "newworld.zen", surveys, {"XARDAS_STAIRS": "road"}))
    there = places.get("XARDAS_BANDITS")
    assert there is not None and there.kind == "cave" and there.kind_ru == "пещера" and there.inside
    assert there.anchor == "NW_XARDAS_BANDITS_04"  # its own waypoint nearest its middle (the way through too)
    assert places.ways_in_ru(there) == "вход — на ступенях"
    assert there.entrances[0].outside == "NW_XARDAS_STAIRS_04" and there.entrances[0].pos == (0, 0, 0)
    assert places.distance(there, (-500.0, 0.0, 0.0)) == 500  # to its way in, not its middle


async def test_the_model_confirms_the_kinds_and_a_kind_it_makes_up_is_not_taken():
    groups = group(cave_and_steps())
    surveys = survey(groups)
    answer = [{"id": "XARDAS_STAIRS", "kind": "field"}, {"id": "XARDAS_BANDITS", "kind": "dungeon"}]
    llm = FakeBackend(script=[FakeStep(json.dumps(answer))], delay=0)
    got = await kinds(groups, {}, surveys, llm)
    assert got == {"XARDAS_STAIRS": "field", "XARDAS_BANDITS": "cave"}  # the code's guess for the made-up one
    prompt = llm.calls[0][1][0].content
    assert "внутри: 100% точек; секторы: " + CAVE_ROOM in prompt and "догадка: cave" in prompt


def test_where_a_place_is_from_the_hero_and_the_nearest_of_each_kind():
    data = {
        "version": 2,
        "world": "NEWWORLD.ZEN",
        "places": [
            *[
                {"id": f"SPOT{i}", "ru": f"место {i}", "where_ru": f"у места {i}", "en": "spot",
                 "where_en": "at a spot", "center": [100 * i, 0, 0], "kind": "other"}
                for i in range(1, 8)
            ],
            {"id": "CAVE", "ru": "пещера бандитов", "where_ru": "в пещере бандитов", "en": "cave",
             "where_en": "in the cave", "center": [0, 0, 3000], "kind": "cave", "inside": True,
             "entrances": [{"in": "C1", "out": "S1", "pos": [0, 0, 2000], "side": "SPOT1"}]},
        ],
        "points": {**{f"S{i}": i - 1 for i in range(1, 8)}, "C1": 7},
    }  # fmt: skip
    places = PlaceBook(data)
    shown = places.around((0.0, 0.0, 0.0), 8000, 6)
    assert [p.id for p, _ in shown] == ["SPOT1", "SPOT2", "SPOT3", "SPOT4", "SPOT5", "SPOT6", "CAVE"]
    cave = places.get("CAVE")
    assert cave is not None
    # the hero faces +x (his right is -z, as the world is left-handed): the cave's way in at +z is on his left
    assert side_of((0, 0, 0), (1, 0), (0, -1), (0, 0, 2000)) == "left"
    assert side_of((0, 0, 0), (1, 0), (0, -1), (3000, 0, 100)) == "ahead"
    assert side_of((0, 0, 0), (1, 0), (0, -1), (-3000, 0, 100)) == "behind"
    line = places.line_ru(cave, 2000, (0, 0, 0), (1, 0), (0, -1))
    assert line == "CAVE — пещера бандитов: пещера, 20 м, слева от героя; вход — у места 1"
    assert places.line_ru(cave, 2000) == "CAVE — пещера бандитов: пещера, 20 м; вход — у места 1"

#!/usr/bin/env python3
"""Check a real full GameSession archive and its fresh-process, pre-step sleeping corpse."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import zipfile

from ragdoll_compare import finite_vector, unique_object, reject_constant, distance


def read_json(path):
    return json.loads(path.read_text(), object_pairs_hook=unique_object,
                      parse_constant=reject_constant)


def snapshot(path, operation):
    data = read_json(path)
    if data.get("schema_version") != 1 or data.get("operation") != operation:
        raise ValueError("snapshot schema or operation differs: " + str(path))
    for field in ("world", "actor", "symbol"):
        if not isinstance(data[field], str) or not data[field]:
            raise ValueError("missing snapshot identity: " + field)
    for field in ("actor_id", "handle_id", "t_game_ms", "part_count"):
        if type(data[field]) is not int or data[field] < 0:
            raise ValueError("invalid snapshot counter: " + field)
    if type(data["hp"]) is not int:
        raise ValueError("invalid HP")
    for field in ("feature_enabled", "dead", "unconscious", "recovering", "sleeping"):
        if type(data[field]) is not bool:
            raise ValueError("invalid snapshot flag: " + field)
    if operation == "initial":
        if type(data["initial_dt"]) is not int or data["initial_dt"] != 0:
            raise ValueError("initial snapshot was not taken before positive simulation dt")
    elif data["initial_dt"] is not None:
        raise ValueError("ordinary snapshot falsely claims initial-frame timing")
    finite_vector(data["position_cm"], 3, "NPC position")
    finite_vector(data["object_transform"], 16, "object matrix")
    inventory = data["inventory"]
    if not isinstance(inventory, list):
        raise ValueError("actual inventory counts are absent")
    identities = []
    for item in inventory:
        if type(item["class_id"]) is not int or item["class_id"] < 0 or type(item["count"]) is not int or item["count"] <= 0 or not isinstance(item["class_name"], str) or not item["class_name"]:
            raise ValueError("invalid actual inventory identity/count")
        identities.append(item["class_id"])
    if identities != sorted(set(identities)):
        raise ValueError("inventory counts are not uniquely ordered by class")
    bones = data["world_bones"]
    if not isinstance(bones, list) or not bones:
        raise ValueError("full world bone coverage is empty")
    names = []
    for bone in bones:
        if not isinstance(bone["bone"], str) or not bone["bone"]:
            raise ValueError("missing bone name")
        names.append(bone["bone"])
        finite_vector(bone["position_cm"], 3, "world bone")
    if len(set(names)) != len(names):
        raise ValueError("duplicate world bone")
    body = data["snapshot"]
    if not isinstance(body, dict) or type(body["version"]) is not int or body["version"] != 3:
        raise ValueError("actual native snapshot is absent or has invalid version")
    if not isinstance(body["skeleton"], str) or not body["skeleton"]:
        raise ValueError("native skeleton is absent")
    finite_vector(body["root"], 16, "native root")
    parts = body["parts"]
    if not isinstance(parts, list) or len(parts) != data["part_count"] or len(parts) <= 10:
        raise ValueError("required complete physical body is absent")
    indices = []
    for part in parts:
        index = part["bone"]
        if type(index) is not int or index < 0 or index >= len(bones):
            raise ValueError("native part refers to an absent visual bone")
        indices.append(index)
        finite_vector(part["transform"], 16, "part matrix")
        finite_vector(part["reference_transform"], 16, "initial affine bone reference")
        finite_vector(part["center_of_mass_meters"], 3, "exact native COM")
        finite_vector(part["rotationxyzw"], 4, "exact native quaternion")
        finite_vector(part["linear_velocity_meters"], 3, "exact native linear velocity")
        if abs(sum(value*value for value in part["rotationxyzw"])-1) > .001:
            raise ValueError("native quaternion is not a proper unit rotation")
        finite_vector(part["linear_velocity_cm_s"], 3, "linear velocity")
        finite_vector(part["angular_velocity_rad_s"], 3, "angular velocity")
        if type(part["sleeping"]) is not bool or part["sleeping"] is not True:
            raise ValueError("sleeping full-session fixture contains an awake part")
        if distance(part["transform"][12:15], bones[index]["position_cm"]) > .1:
            raise ValueError("saved final world bone does not follow its physical frame")
    if len(set(indices)) != len(indices):
        raise ValueError("duplicate native part")
    if body["legacy_ancestors"] is not False or not isinstance(body["ancestors"], list):
        raise ValueError("fresh full-session fixture lost its historic ancestor topology")
    ancestors = []
    for ancestor in body["ancestors"]:
        index, anchor = ancestor["bone"], ancestor["anchor_bone"]
        if type(index) is not int or type(anchor) is not int or index < 0 or index >= len(bones) or index in indices or anchor not in indices:
            raise ValueError("invalid historic ancestor/native anchor")
        ancestors.append(index)
        finite_vector(ancestor["reference_transform"], 16, "historic ancestor frame")
    if len(ancestors) != len(set(ancestors)):
        raise ValueError("duplicate historic ancestor")
    if not data["feature_enabled"] or not data["dead"] or data["unconscious"] or data["recovering"] or data["hp"] != 0 or not data["sleeping"]:
        raise ValueError("full-session fixture is not an enabled, sleeping corpse")
    if distance(body["root"][12:15], data["position_cm"]) > .1:
        raise ValueError("corpse focus/loot position does not follow the native root")
    return data


def compare(expected, actual, clock_equal, minimum_advance_ms=1000):
    for field in ("world", "actor_id", "actor", "symbol", "handle_id", "feature_enabled",
                  "dead", "unconscious", "recovering", "hp", "sleeping", "part_count"):
        if actual[field] != expected[field]:
            raise ValueError("loaded identity/state differs: " + field)
    if actual["inventory"] != expected["inventory"]:
        raise ValueError("loaded actual inventory item classes/counts differ")
    if clock_equal and actual["t_game_ms"] != expected["t_game_ms"]:
        raise ValueError("loaded simulation clock differs before the first physical step")
    if not clock_equal and actual["t_game_ms"] < expected["t_game_ms"] + minimum_advance_ms:
        raise ValueError("sleeping continuation does not cover its declared normal-step interval")
    deltas = {"position_cm": 0.0, "basis": 0.0, "linear_velocity_cm_s": 0.0,
              "angular_velocity_rad_s": 0.0, "world_bones_cm": 0.0}
    def matrix(left, right):
        deltas["position_cm"] = max(deltas["position_cm"], distance(left[12:15], right[12:15]))
        deltas["basis"] = max(deltas["basis"], max(abs(left[i]-right[i]) for i in range(16) if i not in (12, 13, 14)))
    deltas["position_cm"] = distance(expected["position_cm"], actual["position_cm"])
    matrix(expected["object_transform"], actual["object_transform"])
    a, e = actual["snapshot"], expected["snapshot"]
    if a["version"] != e["version"] or a["skeleton"] != e["skeleton"]:
        raise ValueError("loaded native snapshot identity differs")
    matrix(e["root"], a["root"])
    for left, right in zip(e["parts"], a["parts"]):
        if left["bone"] != right["bone"] or left["sleeping"] != right["sleeping"]:
            raise ValueError("loaded native part mapping/sleep differs")
        matrix(left["transform"], right["transform"])
        matrix(left["reference_transform"], right["reference_transform"])
        for field in ("center_of_mass_meters", "rotationxyzw", "linear_velocity_meters"):
            if left[field] != right[field]:
                raise ValueError("loaded exact native precision differs: " + field)
        for key in ("linear_velocity_cm_s", "angular_velocity_rad_s"):
            deltas[key] = max(deltas[key], distance(left[key], right[key]))
    if a["legacy_ancestors"] != e["legacy_ancestors"] or a["ancestors"] != e["ancestors"]:
        raise ValueError("loaded historic ancestor topology or immutable birth frames differ")
    if len(expected["world_bones"]) != len(actual["world_bones"]):
        raise ValueError("loaded final bone coverage differs")
    for left, right in zip(expected["world_bones"], actual["world_bones"]):
        if left["bone"] != right["bone"]:
            raise ValueError("loaded final bone ordering differs")
        deltas["world_bones_cm"] = max(deltas["world_bones_cm"], distance(left["position_cm"], right["position_cm"]))
    limits = {"position_cm": .001, "basis": .00001, "linear_velocity_cm_s": .001,
              "angular_velocity_rad_s": .00001, "world_bones_cm": .001}
    if any(deltas[field] > limits[field] for field in limits):
        raise ValueError("full-session pose/velocity delta exceeds declared bounds: " + repr(deltas))
    return deltas


def complete_process(path):
    if "done" not in (path / "bench.log").read_text().splitlines():
        raise ValueError("actual game process did not finish its scenario: " + str(path))
    events = [json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant)
              for line in (path / "events.jsonl").read_text().splitlines()]
    if any(event.get("kind") == "ragdoll_error" for event in events):
        raise ValueError("actual game process reported a control error: " + str(path))


def check_archive(path, saved):
    if saved["archive"] != path.name or type(saved["archive_bytes"]) is not int or saved["archive_bytes"] != path.stat().st_size:
        raise ValueError("full save filename/size differs from the actual saver")
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or archive.testzip() is not None:
            raise ValueError("full save has duplicate/corrupt ZIP entries")
        prefix = "worlds/" + saved["world"]
        npc = prefix + "/npc/" + str(saved["actor_id"])
        required = {"header", "preview.jpg", "game/session", "game/camera", "game/quests",
                    "game/state", "game/director", "game/bennet", prefix + "/version", prefix + "/world",
                    npc + "/data", npc + "/visual", npc + "/ragdoll", npc + "/ragdoll-recovery"}
        if not required.issubset(names):
            raise ValueError("archive does not contain the complete GameSession/world/NPC entries: " + repr(required-set(names)))
        if any(archive.getinfo(name).file_size == 0 for name in required):
            raise ValueError("required full GameSession archive entry is empty")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("saved", "initial", "settled", "save-file", "process-one", "process-two", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    result = {"schema_version": 1, "pass": False,
              "scope": "full GameSession save and fresh-process load; initial snapshot at dt=0, then normal sleeping continuation",
              "limits": "position/bones/linear <=0.001 cm (linear cm/s), basis/angular <=0.00001 (angular rad/s)"}
    try:
        complete_process(args.process_one)
        complete_process(args.process_two)
        saved = snapshot(args.saved, "save")
        initial = snapshot(args.initial, "initial")
        settled = snapshot(args.settled, "snapshot")
        result["save_sha256"] = check_archive(args.save_file, saved)
        result["actor_id"] = saved["actor_id"]
        result["initial_deltas"] = compare(saved, initial, True)
        result["continuation_deltas"] = compare(saved, settled, False)
        result["part_count"] = saved["part_count"]
        result["bone_count"] = len(saved["world_bones"])
        result["pass"] = True
    except (ValueError, KeyError, TypeError, OSError, zipfile.BadZipFile, OverflowError) as error:
        result["error"] = str(error)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("PASS" if result["pass"] else "FAIL")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Select the reflection records changed since the last completed review."""
import json
import os
from pathlib import Path


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def scope(value):
    return sorted(set(value.split()))


def main():
    home = Path(os.environ.get("PASEO_HOME", Path.home() / ".paseo"))
    runs = home / "plugin-data/lerenn/paseo-playbooks/runs"
    current = read_json(runs / (os.environ["PLAYBOOK_RUN_ID"] + ".json"))
    selected = scope(os.environ["PLAYBOOK_INPUT_WORKSPACE_IDS"])
    projects = read_json(home / "projects/workspaces.json")
    ids = {ws["workspaceId"] for ws in projects if ws["projectId"] == current["projectId"]}
    if selected != ["all"]:
        if not selected or not set(selected) <= ids:
            raise ValueError("Select workspace IDs belonging to this project")
        ids = set(selected)

    since = ""
    reviewed_at = {}
    for path in runs.glob("*.json"):
        previous = read_json(path)
        if (previous["projectId"] == current["projectId"]
                and previous["playbook"]["id"] == current["playbook"]["id"]
                and previous["status"] == "completed"
                and scope(previous["inputs"]["workspace_ids"]) == selected
                and previous["createdAt"] < current["createdAt"]):
            since = max(since, previous["createdAt"])
            outputs = previous["steps"]["review"].get("outputs", {})
            reviewed = outputs.get("reviewed_proposals", [])
            if isinstance(reviewed, str):
                reviewed = json.loads(reviewed)
            for record_id in reviewed:
                reviewed_at[record_id] = max(reviewed_at.get(record_id, ""), previous["createdAt"])

    registry = home / "plugin-data/paseo-reflect/workspaces"
    if not registry.is_dir():
        raise FileNotFoundError(registry)
    sources = []
    lessons = 0
    pending_ids = []
    for workspace_id in sorted(ids):
        directory = registry / workspace_id
        if not directory.is_dir():
            continue
        sources.append(workspace_id)
        for name in ("lessons.jsonl", "proposals.jsonl"):
            path = directory / name
            if not path.exists():
                continue
            for line in path.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                record = json.loads(line)
                updated = record["updatedAt"]
                if updated > current["createdAt"]:
                    continue
                if name == "lessons.jsonl":
                    if record["enabled"] and updated > since:
                        lessons += 1
                elif record["status"] == "pending" and updated > reviewed_at.get(record["id"], ""):
                    pending_ids.append(record["id"])

    window = dict(root=str(registry), workspaces=sources, after=since or None,
                  through=current["createdAt"], pending_ids=pending_ids)
    print(json.dumps({
        "summary": f"{lessons} new or updated lessons and {len(pending_ids)} pending proposals in {len(sources)} workspaces",
        "outcome": "ready" if lessons or pending_ids else "unchanged",
        "outputs": {"window": json.dumps(window)},
    }))


if __name__ == "__main__":
    main()

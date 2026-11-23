"""Interactive server console for the current game session."""

from __future__ import annotations

import asyncio
import json
import logging
import shlex
import sys
from typing import Any

from . import programs
from .crimes import COMMUNITIES
from .game import GameError, GameLink, GameServer
from .session import ServerState

log = logging.getLogger("gothic_ai")

HELP = """commands (npc defaults to the last attached one):
  attach <npc> | detach [npc] | snap [npc] | ping
  goto <waypoint> | come | turn | ani <name> | wait <ms> | stop | routine
  schedule [npc]    the npc's daily routine, its variants and current script state
  talk <text>       say it as the hero, looking at the npc (talk.py decides whom it is said to)
  additem <item> [n] put an item into the npc's inventory (game needs -devmode)
  memory [npc]      the current save branch and what the npc remembers
  brain             what the npc planner is doing (tiers, requests, decisions)
  save <slot> | load <slot>   save_slot_<slot>.sav (game needs -devmode)
  graph             story graph changes on this save branch (docs/story-graph.md)
  graph hide|show <node>      switch a dialog or a choice off / on again
  graph cond <node> <json>    add a condition (story graph expression) to a dialog; "-" drops them
  graph add <json>            add a dialog: {"id", "npc", "description", "lines", "effects", ...}
  graph remove <id>           remove an added dialog
  graph program <node> [condition] [<json>|-]   the program of a dialog's information (or condition) or of a
                    choice; with steps, the director's program in its place; "-" brings the scripts' one back
  quest <instance> <n> <item> <n> <title>   the npc gives the hero a quest: kill n creatures
                    (they appear 30 m away), the reward is an item it has
  quests            the director's quests: paid, lied about, found out
  director          quests left without an ending and the director's repairs of them
  director look     look for such quests now and repair them
  occasions         the hero's place, queued and recent occasions for the director
  questlog          the scripts' open quests and recent state changes
  crimes            the current branch's crimes, witnesses, encounter reactions and communities
  lines             the director's plot lines, their state and remaining budget
  pitch             an occasion here and now, subject to the director's budget
  feats             the chronicler's feats on the current save branch
  fates             this fate and the other fates, grouped by the save fork
  claims            quests of the scripts the hero said were done: believed, lies, found out
  know [npc] [query]  what the npc knows of the world now (M6), and what recall finds for the query
  see [npc]         what the npc perceives now: in Russian for its words, in English for its body
  seen [npc] [query]  what the npc saw itself: the hero's sightings, the people it knows (and recall)
  witness [npc]     whom the npc may ask about the hero, its questions under way and what it was told
  trust [npc]       what the npc knows of the hero's word: lies, rumors, claims that were true
  guard [npc]       whether the npc may leave its spot (plot, post), its concerns about the hero
  rumors            rumors of the hero's lies and who knows them, by retellings
  ties [npc]        whom the npc meets by its routine (and when) and has dealings with
  gossip            the latest scene of two talking about the hero near him: its lines, how it ended
  day [npc]         the npc's last menus of its next hour (the old plan, do) and the trip it is on
  amends            the director's storylets of amends for the hero's lies, and where they stand
  author [npc]      the director makes up a quest the npc gives the hero (docs/director.md); none: the quests
  chain <npc> [new] the director makes a linear chain of quests and the characters it needs; new: someone new
  cast              the director's own characters on this branch (docs/director.md, stage 2)
  triggers          the director's triggers in the game: counts and conditions (stage 3, -devmode)
  character <place> the director makes up a character for the place (places.py id), and a quest he gives
  deal              offers open to the hero, and what the game's deal window shows (needs -devmode)
  deal <npc gives> / <hero gives>   the npc offers a deal, e.g. deal ITPO_HEALTH_01:2 / ITMI_GOLD:30
  deal accept       press Y on the deal window as the player does (game needs -devmode)
  dialogs           what the npc offers in the dialog menu now (game needs -devmode)
  choose <node>     play a dialog of the npc as if chosen in the menu (game needs -devmode)
  eval <json>       compute a story graph expression in the game, self = the npc (game needs -devmode)
  open [npc] | menu | pick <text>   talk to the npc as with the action key; what the dialog window offers
                    now; pick its entry as the player does (game needs -devmode)
  debug <cmd> [k=v ...]  a test helper of the game, as the scenarios use it (-devmode): player_goto
                    waypoint=..., teleport near=... distance=..., kill instance=... near=..., time ...
  quit"""



class Console:
    """Line commands from stdin, executed against the current game link."""

    def __init__(self, server: GameServer, state: ServerState, npc: str) -> None:
        self.server = server
        self.state = state
        self.npc = npc

    async def run(self) -> None:
        reader = asyncio.StreamReader()
        loop = asyncio.get_running_loop()
        await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
        print(HELP, flush=True)
        while line := (await reader.readline()).decode():
            args = shlex.split(line)
            if not args:
                continue
            if args[0] in ("quit", "exit"):
                return
            link = self.server.link
            if link is None:
                print("game is not connected", flush=True)
                continue
            try:
                await self.execute(link, args)
            except (GameError, TimeoutError, KeyError) as err:
                print(f"error: {err}", flush=True)
            except Exception as err:  # a typo in a command must not end the server
                log.exception("console: %s", line.strip())
                print(f"error: {type(err).__name__}: {err}", flush=True)

    async def execute(self, link: GameLink, args: list[str]) -> None:
        cmd, rest = args[0], args[1:]
        npc = self.npc
        match cmd:
            case "attach":
                self.npc = (rest[0] if rest else npc).upper()
                await link.attach(self.npc)
            case "detach":
                await link.detach(rest[0] if rest else npc)
            case "snap":
                print((await link.snapshot(rest[0] if rest else npc, fresh=True)).model_dump_json(indent=2))
            case "schedule":
                info = await link.routine(rest[0] if rest else npc)
                print(f"now: {info.current}; variants: {', '.join(info.routines) or '-'}")
                for e in info.entries:
                    print(f"  {e.start}-{e.end} {e.state} at {e.waypoint}")
            case "ping":
                print(f"pong in {await link.ping() * 1000:.1f} ms")
            case "open":
                await link.start_talk(rest[0] if rest else npc)
                print("ok", flush=True)
            case "menu":
                for entry in await link.dialog_menu():
                    print(f"  {entry}", flush=True)
                print("(menu end)", flush=True)
            case "pick" if rest:
                await link.pick_in_menu(" ".join(rest))
                print("ok", flush=True)
            case "debug" if rest:
                params: dict[str, Any] = {}
                for arg in rest[1:]:
                    key, _, value = arg.partition("=")
                    params[key] = int(value) if value.lstrip("-").isdigit() else value
                await link.debug(rest[0], **params)  # type: ignore[arg-type]
                print("ok", flush=True)
            case "goto" if rest:
                print(await link.act(npc, "go_to_waypoint", waypoint=rest[0]))
            case "come":
                print(await link.act(npc, "go_to_npc", npc="PLAYER"))
            case "turn":
                print(await link.act(npc, "turn_to", npc="PLAYER"))
            case "ani" if rest:
                print(await link.act(npc, "play_ani", name=rest[0]))
            case "wait" if rest:
                print(await link.act(npc, "wait", ms=int(rest[0])))
            case "stop":
                print(await link.act(npc, "stop"))
            case "routine":
                print(await link.act(npc, "return_to_routine"))
            case "additem" if rest:
                count = int(rest[1]) if len(rest) > 1 else 1
                print(await link.act(npc, "debug_add_item", item=rest[0].upper(), count=count))
            case "memory" if self.state.dialogue is not None:
                d = self.state.dialogue
                branch = d.branch or "-"
                print(f"branch {branch}, head {d.memory.head(branch)}")
                mem = d.memory.load(branch, (rest[0] if rest else npc).upper())
                if mem.summary:
                    print(f"summary: {mem.summary}")
                for e in mem.entries:
                    print(f"  {e.seq:4d} {e.role:9s} {e.content}")
            case "save" | "load" if rest:
                await link.debug(cmd, int(rest[0]))
            case "brain" if self.state.brain is not None:
                print(self.state.brain.snapshot())
                for d in list(self.state.brain.applier.recent)[-10:]:
                    print(f"  {d.npc} {d.question}={d.choice} ({d.confidence:.2f}) {','.join(d.stimuli)}")
            case "occasions":
                if (occasions := self.state.occasions) is None:
                    print("occasions: off")
                else:
                    place = occasions.current
                    print(
                        f"hero: {place.ru if place else 'settling / unknown'} ({occasions.waypoint or '-'})"
                    )
                    for label, facts in (("queue", occasions.pending), ("recent", occasions.recent)):
                        print(f"{label}: {len(facts)}")
                        for fact in facts:
                            print("  " + json.dumps(fact, ensure_ascii=False))
            case "questlog":
                if (questlog := self.state.questlog) is None:
                    print("questlog: off")
                else:
                    for var, status in questlog.states.items():
                        if status == "running":
                            print(f"{var}: {questlog.graph.quests[var].title}; {status}")
                    print("recent:")
                    for fact in questlog.recent:
                        print("  " + json.dumps(fact, ensure_ascii=False))
            case "crimes":
                if (crimes := self.state.crimes) is None:
                    print("crimes: off")
                else:
                    for fact in crimes.chronicle.facts("crime"):
                        print(json.dumps(fact, ensure_ascii=False))
                        for reaction in crimes.chronicle.facts("crime_reaction"):
                            if reaction.get("crime") == fact["id"]:
                                print("  " + json.dumps(reaction, ensure_ascii=False))
                        for reaction in crimes.chronicle.facts("crime_meet"):
                            if reaction.get("crime") == fact["id"]:
                                print("  " + json.dumps(reaction, ensure_ascii=False))
                    for reaction in crimes.chronicle.facts("crime_reaction"):
                        if reaction.get("verdict") == "scripts" and not crimes.crime(
                            reaction.get("crime", "")
                        ):
                            print("  " + json.dumps(reaction, ensure_ascii=False))
                    print("communities:")
                    for community in COMMUNITIES:
                        print(f"  {community}: {', '.join(crimes.lawkeepers(community)) or '-'}")
                    if self.state.crime_law is not None:
                        await self.state.crime_law.tick()
                        print(self.state.crime_law.law.summary())
            case "lines":
                if (showrunner := self.state.showrunner) is None:
                    print("lines: off")
                else:
                    for line in showrunner.lines:
                        status = "open" if showrunner.is_open(line) else "closed"
                        print(
                            f"{line['kind']} {line.get('giver', '-')}: {line.get('premise', '')}; "
                            f"{', '.join(line.get('quests', []))}; {status}"
                        )
                    at = await link.story_state(vars=["KAPITEL"])
                    chapter = int(at.get("vars", {}).get("KAPITEL") or 1)
                    print(showrunner.budget(chapter, at).text())
            case "pitch":
                if (showrunner := self.state.showrunner) is None:
                    print("pitch: off")
                else:
                    fact = await showrunner.pitch()
                    print(json.dumps(fact, ensure_ascii=False) if fact else "(no occasion now)")
            case "feats":
                if self.state.chronicle is not None:
                    epilogues = {f["feat"]: f for f in self.state.chronicle.facts("feat_epilogue")}
                    shown = {f["feat"] for f in self.state.chronicle.facts("feat_epilogue_shown")}
                    for feat in self.state.chronicle.facts("feat"):
                        print(
                            f"{feat['id']}: {feat['title']}; {feat['text']}; "
                            f"evidence={feat['evidence']}"
                        )
                        if epilogue := epilogues.get(feat["id"]):
                            pending = " (не показан)" if feat["id"] not in shown else ""
                            print(f"  {epilogue['text']}{pending}")
            case "fates":
                data = self.state.fates.read() if self.state.fates is not None else None
                print("Fate:")
                if data is not None:
                    for feat in data.fate:
                        print(f"  {feat.id}: {feat.title}; {feat.text}")
                        if feat.epilogue:
                            print(f"    {feat.epilogue}")
                    for fork in data.others:
                        print(f"Other fate: chapter {fork.chapter}, day {fork.day}")
                        for feat in fork.feats:
                            print(f"  {feat.id}: {feat.title}; {feat.text}")
                            if feat.epilogue:
                                print(f"    {feat.epilogue}")
                        for end in fork.ends:
                            print(f"  {end.kind}: {end.title}; {end.text}")
            case "graph" if self.state.story is not None:
                await self.graph(rest)
            case "quest" if len(rest) >= 5 and self.state.quests is not None:
                await self.quest(link, npc, rest)
            case "quests" if self.state.quests is not None:
                board = self.state.quests
                for q in board.quests.values():
                    state = "found out" if q.found_out else "lied" if q.lie else "paid" if q.paid else "open"
                    print(f"{q.id} {q.giver}: {q.title}; {q.reward()}; {state}; done {board.done(q)}")
            case "director" if self.state.director is not None:
                await self.director(rest)
            case "claims" if self.state.vanilla is not None:
                for t in self.state.vanilla.told.values():
                    state = "found out" if t.found_out else "lie" if t.lie else "true"
                    print(f"{t.quest} {t.npc}: {t.title}; {t.reward}; {state} ({t.kind or '-'})")
            case "see" if self.state.perception is not None:
                who = (rest[0] if rest else npc).upper()
                view = await self.state.perception.fresh(who)
                if view is None:
                    print("(no view)")
                else:
                    sight = self.state.perception.sight(who, link.snapshots.get(who))
                    assert sight is not None
                    print("\n".join(line for line in (sight.place, sight.hero, sight.people) if line))
                    print("\n".join(self.state.perception.english(who)))
            case "seen" if self.state.sightings is not None:
                who = (rest[0] if rest else npc).upper()
                print(self.state.sightings.situation(who) or "(saw nothing)")
                if len(rest) > 1:
                    print(self.state.sightings.recall(who, " ".join(rest[1:])) or "(recall: nothing)")
            case "witness" if self.state.witnesses is not None:
                who = (rest[0] if rest else npc).upper()
                await self.state.witnesses.prepare(who)
                print(self.state.witnesses.situation(who) or "(nobody to ask, nothing told)")
            case "know" if self.state.knowledge is not None:
                who = (rest[0] if rest else npc).upper()
                await self.state.knowledge.prepare(who)
                print(self.state.knowledge.situation(who))
                if len(rest) > 1:
                    print(self.state.knowledge.recall(who, " ".join(rest[1:])) or "(recall: nothing)")
            case "guard" if self.state.guard is not None:
                who = (rest[0] if rest else npc).upper()
                guard = self.state.guard
                state = (
                    self.state.talk.people[who].state
                    if self.state.talk and who in self.state.talk.people
                    else ""
                )
                body = guard.body(who, state) or "may walk"
                print(f"plot {guard.plot(who)}, post {who in guard.posts}, body: {body}")
                if self.state.concerns is not None:
                    print("concerns:", [c.key for c in self.state.concerns.of(who)] or "none")
                print("reads where it stands:", guard.placed().get(who, []) or "no")
            case "trust" if self.state.trust is not None:
                print(self.state.trust.situation(rest[0] if rest else npc) or "(nothing)")
            case "amends" if self.state.amends is not None:
                for s in self.state.amends.storylets.values():
                    closed = ", ".join(s.close)
                    print(f"{s.id} {s.name}: {s.stage}; closed {closed}; {s.gold} gold to {s.to_name}")
            case "author" if self.state.author is not None:
                if rest:
                    q = await self.state.author.quest(rest[0])
                    print("(no quest: see the log)" if q is None else f"{q.id} «{q.title}»: {q.asked}")
                for old in self.state.author.old.values():
                    print(f"{old.id} {old.name}: «{old.title}» {old.stage}; {old.item_name} {old.where}")
                book = self.state.author.book
                for q in book.all().values():
                    p = book.progress(q.id)
                    print(
                        f"{q.id} {q.name(q.giver)}: «{q.title}» {p.state}, stage {p.stage}/{len(q.stages)}; "
                        f"{q.reward()}"
                    )
            case "chain" if self.state.chains is not None and rest:
                quests = await self.state.chains.chain(rest[0], new_people="new" in rest[1:])
                if quests is None:
                    print("(no chain: see the log)")
                else:
                    for q in quests:
                        print(f"{q.id} «{q.title}»: {q.asked}; after {q.after or '-'}")
            case "triggers":
                for t in await link.triggers():
                    holds = {True: "holds", False: "does not hold", None: "-"}[t.get("holds")]
                    print(
                        f"{t['id']} on {t['on']} {t['npc'] or ''} {t['node'] or ''}: {t['count']}×, {holds}"
                    )
            case "cast" if self.state.cast is not None:
                for c in self.state.cast.characters():
                    print(f"{c.npc} «{c.name}» like {c.like} at {c.at}")
            case "character" if self.state.casting is not None and rest:
                # a new character for the place, and a quest of the author's for him to give
                spec = await self.state.casting.character(rest[0])
                print(
                    "(no character: see the log)"
                    if spec is None
                    else f"{spec.npc} «{spec.name}» at {spec.at}"
                )
                if spec is not None and self.state.author is not None:
                    q = await self.state.author.quest(spec.npc)
                    print("(no quest: see the log)" if q is None else f"{q.id} «{q.title}»: {q.asked}")
            case "deal" if self.state.deals is not None:
                await self.deal(link, npc, rest)
            case "rumors" if self.state.rumors is not None:
                print("\n".join(self.state.rumors.describe()) or "(none)")
            case "ties" if self.state.rumors is not None and self.state.rumors.social is not None:
                print("\n".join(self.state.rumors.social.describe(rest[0] if rest else npc)) or "(none)")
            case "gossip" if self.state.gossip is not None:
                gossip = self.state.gossip
                s = gossip.scene or gossip.last
                if s is None:
                    print(f"(no scene yet; {gossip.scenes} begun)")
                else:
                    print(f"{s.id} {s.a_name} -> {s.b_name}: {s.state}{f' ({s.why})' if s.why else ''}")
                    names = {s.a: s.a_name, s.b: s.b_name}
                    said = [x.text for x in s.play.said()] if s.play is not None else []
                    for n, text, _ in s.lines:  # + said, - written and not heard
                        print(f"  {'+' if text in said else '-'} {names[n]}: {text}")
                    for e in s.errors:
                        print(f"  rejected: {e}")
            case "day" if self.state.day is not None:
                self.day((rest[0] if rest else npc).upper())
            case "dialogs":
                print(" ".join(await link.offered_dialogs(rest[0] if rest else npc)) or "(none)")
            case "choose" if rest:
                await link.choose_dialog(rest[1] if len(rest) > 1 else npc, rest[0])
            case "eval" if rest:
                print(json.dumps(await link.evaluate(json.loads(rest[0]), rest[1] if len(rest) > 1 else npc)))
            case "talk" if rest and self.state.talk is not None:
                self.state.talk.hear(" ".join(rest), npc, {"via": "console"})
            case _:
                print(HELP)

    def day(self, npc: str) -> None:
        """day: the npc's last menus of its next hour, as the planner asked them, and the trip it is on."""
        desk = self.state.day
        assert desk is not None
        menu = desk.menus.get(npc)
        if menu is None:
            print("(not asked for its next hour yet)")
        elif menu.stay is not None:
            print(f"not asked: {menu.stay}")
        else:
            for key, text in menu.plan[0].items():
                print(f"  plan {key}: {text}")
            for o in menu.do.values():
                print(f"  do {o.key} [{o.id}]: {o.text}")
        if (trip := desk.walking.get(npc)) is not None:
            print(f"on its way: {trip.id}")

    async def deal(self, link: GameLink, npc: str, rest: list[str]) -> None:
        """deal: the open offers; deal accept: Y on the window; deal <npc gives> / <hero gives>: an offer."""
        deals = self.state.deals
        assert deals is not None
        if not rest:
            for d in deals.open.values():
                print(f"{d.id} v{d.version} {d.name}: {d.state}; {d.terms()}")
            print(f"window: {json.dumps(await link.deal_window(), ensure_ascii=False)}")
            return
        if rest == ["accept"]:
            print(await link.press_deal())
            return
        text = " ".join(rest)
        mine, _, theirs = text.partition("/")

        def gives(part: str) -> list[dict[str, object]]:
            out = []
            for word in part.split():
                item, _, count = word.partition(":")
                out.append({"item": item, "count": int(count or 1)})
            return out

        snap = await link.snapshot(npc, fresh=True)
        name = snap.npc.name or npc
        outcome = await deals.propose(
            npc, name, {"npc_gives": gives(mine), "hero_gives": gives(theirs)}, snap
        )
        print(f"{outcome.status}: {outcome.output}")

    async def quest(self, link: GameLink, npc: str, rest: list[str]) -> None:
        """quest <instance> <count> <reward item> <reward count> <title...>: the npc gives a quest."""
        assert self.state.quests is not None
        instance, count, item, pay = rest[0], int(rest[1]), rest[2].upper(), int(rest[3])
        snap = await link.snapshot(npc, fresh=True)
        reward = next((i.name for i in snap.npc.inventory if i.item == item), item)
        title = " ".join(rest[4:])
        q = await self.state.quests.give(npc, snap.npc.name, title, instance, count, item, reward, pay)
        print(f"{q.id}: {q.title} ({q.count} {q.instance} as {q.tag}), reward {q.reward()}")

    async def director(self, rest: list[str]) -> None:
        director = self.state.director
        assert director is not None
        if rest == ["look"]:
            print("repaired:", " ".join(await director.look()) or "nothing to repair")
            return
        story = await director.story()
        print("without an ending:", " ".join(story.dead_ends()) or "none")
        for r in director.repairs:
            print(f"  {r.quest}: {r.kind} {r.npc} {r.node}".rstrip())
        if director.given_up:
            print("no repair found:", " ".join(sorted(director.given_up)))

    async def graph(self, rest: list[str]) -> None:
        story = self.state.story
        assert story is not None
        match rest:
            case []:
                for op in story.ops():
                    print(json.dumps(op, ensure_ascii=False))
                return
            case ["hide", node]:
                op: dict[str, Any] = {"op": "disable", "node": node.upper()}
            case ["show", node]:
                op = {"op": "enable", "node": node.upper()}
            case ["cond", node, "-"]:
                op = {"op": "condition", "node": node.upper(), "expr": None}
            case ["cond", node, expr]:
                op = {"op": "condition", "node": node.upper(), "expr": json.loads(expr)}
            case ["add", node]:
                op = {"op": "add_node", "node": json.loads(node)}
            case ["remove", node]:
                op = {"op": "remove_node", "node": node.upper()}
            case ["program", node, *more] if len(more) <= 2:
                part = "condition" if more[:1] == ["condition"] else "information"
                more = more[1:] if more[:1] == ["condition"] else more
                if not more:
                    p = await story.link.story_program(node, part)
                    print(f"{p.get('fn')}{' (the director’s)' if p.get('patched') else ''}:")
                    print(programs.render(p.get("steps")))
                    return
                steps = None if more[0] == "-" else json.loads(more[0])
                op = {"op": "program", "node": node.upper(), "part": part, "steps": steps}
            case _:
                print(HELP)
                return
        await story.apply(op)
        print("ok")

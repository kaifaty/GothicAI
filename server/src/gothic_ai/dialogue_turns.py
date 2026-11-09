"""Conversational turns, streamed replies and tools."""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Callable
from dataclasses import replace
from typing import TYPE_CHECKING

from . import executor, npc_tools
from .dialogue_constants import (
    FIGHTS_HERO,
    PARALLEL_ACT_HINT,
    PREPARE_S,
    QUICK_HINT,
    REACT_HINT,
    SOUND_SAID,
    STEP_RETRIES,
)
from .dialogue_state import Conversation, StaleTurn, TurnState
from .dialogue_text import MoodTag, SentenceSplitter, game_time, to_game_text
from .embeddings import EmbedError
from .game import GameError
from .llm import Item, LLMError, Message, Tool, ToolCall, ToolChoice, ToolResult
from .protocol import ChatTextCmd, Snapshot
from .quest_tools import AMEND
from .trace import CallTrace, TurnTrace
from .turn import MAX_STEPS, ToolOutcome, go_on, refusal_note, tool_choice, tool_output

if TYPE_CHECKING:
    from .perception import Sight


log = logging.getLogger("gothic_ai.dialogue")


class DialogueTurnsMixin:
    async def _turn(
        self,
        conv: Conversation,
        trigger: Message,
        kind: str,
        relevant: Callable[[], bool] | None = None,
        *,
        overlay: bool = False,
        tools: list[Tool] | None = None,
        body: str | None = None,
        to: str | None = None,
        act: bool = False,
    ) -> None:
        """A turn on the conversation's floor (group.py): it thinks once those before it have their text
        ready; with no voice left for it (three to a phrase of the hero's) it says nothing."""
        if self.floor is not None:
            try:
                seated = await self.floor.queue(conv.npc, kind)
            except asyncio.CancelledError:
                conv.cut = None  # cut off before it began: nothing said, nothing to remember
                raise
            if not seated:
                log.info("%s: no voice left in the conversation, its %s turn is dropped", conv.npc, kind)
                return
        try:
            await self._speak(
                conv, trigger, kind, relevant, overlay=overlay, tools=tools, body=body, to=to, act=act
            )
        except Exception as err:
            # a bug, not the model: logged now (conv.reply keeps the task), and the chat does not keep
            # "<npc> думает..." for good
            log.exception("%s: the %s turn failed", conv.npc, kind)
            await self._status(conv, "error", f"сбой сервера ({type(err).__name__})")
        finally:
            if self.floor is not None:
                self.floor.done(conv.npc)

    async def _speak(
        self,
        conv: Conversation,
        trigger: Message,
        kind: str,
        relevant: Callable[[], bool] | None = None,
        *,
        overlay: bool = False,
        tools: list[Tool] | None = None,
        body: str | None = None,
        to: str | None = None,
        act: bool = False,
    ) -> None:
        """One turn of the conversational model: up to MAX_STEPS calls (see turn.py). Text streams to the
        subtitles and the voice as it comes; tool calls are handled after their step. `body`: what the
        npc's body does meanwhile (the behavioral model's choice). In a fight the quick model says the
        line first and the conversational model's steps only act (`_quick_line`, `_act_on_line`)."""
        conv.trigger = trigger
        await self._status(conv, "thinking")
        quick = kind != "mercy" and self._fighting(conv)  # surrender is the conversational model's decision
        parallel = quick and kind == "player" and self.fights_hero(conv.npc)
        # the hero's phrase (not on the move): the quick model's first words, the answer goes on from them
        react = kind == "player" and not overlay and not quick and self.quick is not None
        trace = TurnTrace(
            self.trace,
            npc=conv.npc,
            branch=conv.branch,
            trigger={"kind": kind, "text": trigger.content},
            instructions=conv.persona.instructions,
            tools=[],
            snapshot=None,
            llm=self.llm.name,
            model=getattr(self.llm, "model", None),
        )
        if quick:
            self._yield_to(conv.npc)
        first: asyncio.Task[str] | None = None
        full: asyncio.Task[tuple[str, Snapshot | None, Sight | None]] | None = None
        quick_line: asyncio.Task[bool] | None = None
        try:
            # the quick model's words do not wait for the extensions: they ready the whole situation meanwhile
            situation_text, snap, sight = await self._situation(conv, body, brief=quick or react)
            trace.record["snapshot"] = snap.model_dump(mode="json") if snap is not None else None
            if quick or react:
                full = asyncio.create_task(self._situation(conv, body))
            if kind == "player":  # its first reaction at once: words (outside a fight) and a gesture
                hint = Message("developer", f"{situation_text}\n{REACT_HINT}")
                words = [*conv.history, hint, trigger] if react else None
                first = asyncio.create_task(self._first_reaction(conv, trigger.content, words, trace))
            sound = await first if first is not None else ""
            if react and full is not None:  # the answer: the conversational model's, on the whole situation
                situation_text, _, sight = await full
        except (asyncio.CancelledError, Exception) as err:
            for task in (first, full):
                if task is not None:
                    task.cancel()
            if first is not None:
                await asyncio.gather(first, return_exceptions=True)
            trace.close("cancelled" if isinstance(err, asyncio.CancelledError) else "error")
            raise
        if sound:
            situation_text += SOUND_SAID.format(sound)
        if quick:
            situation_text += "\n" + QUICK_HINT
        request: list[Item] = [*conv.history, Message("developer", situation_text), trigger]
        now = time.monotonic()
        lines = (*conv.earlier_lines, *conv.last_lines)
        conv.earlier_lines = [line for line in lines if self._to_hear(conv, line, now)]
        conv.last_lines = []
        state = TurnState(
            relevant=relevant,
            overlay=overlay,
            tools=self._offered(conv.npc, sight) if tools is None else tools,
            sight=sight,
            dynamic_tools=tools is None,
            reply=f"R{next(self._reply_ids)}",
            # voiced, the first line is synthesized before it is heard: cut it at the first clause
            splitter=SentenceSplitter(first_clause=self.tts is not None),
            started=time.monotonic(),
            filler=sound,
            to=to.upper() if to else None,
            act=act and bool(tools),
        )
        conv.reply_id = state.reply
        trace.record["tools"] = [t.name for t in state.tools]
        speaker = self.llm
        try:
            handed_off = False
            if quick and full is not None:
                ready = asyncio.get_running_loop().create_future()
                quick_line = asyncio.create_task(self._quick_line(conv, state, request, trace, ready=ready))
                handed_off = await ready  # the situation must include the surrender probe's outcome
            if handed_off:
                assert quick_line is not None
                if await quick_line:
                    assert self.quick is not None
                    speaker = self.quick
                status = "completed"
            elif parallel:
                assert full is not None and quick_line is not None
                status = await self._act_on_line(conv, state, trigger, full, sound, trace,
                                                 quick_line=quick_line)
                if await quick_line:
                    assert self.quick is not None
                    speaker = self.quick
            elif quick_line is not None and await quick_line:
                assert full is not None
                assert self.quick is not None
                speaker = self.quick
                status = await self._act_on_line(conv, state, trigger, full, sound, trace)
            else:
                if quick and full is not None:  # the quick model said nothing: the whole situation, as usual
                    situation_text, _, state.sight = await full
                    if self.hero_fight_held(conv.npc):
                        situation_text, _, state.sight = await self._situation(conv, body)
                    if state.dynamic_tools:
                        state.tools = self._offered(conv.npc, state.sight)
                    if sound:
                        situation_text += SOUND_SAID.format(sound)
                    request = [*conv.history, Message("developer", situation_text), trigger]
                status = await self._steps(conv, state, request, trace)
            if state.text:
                await self.link.send(
                    ChatTextCmd(npc=conv.npc, reply=state.reply, text=to_game_text(state.text), final=True)
                )
        except LLMError as err:
            log.error("%s: %s", conv.npc, err)
            self.journal.add("llm_error", npc=conv.npc, name=conv.persona.name, detail=str(err))
            if state.text:  # a turn that failed before a word was said is not remembered
                self._commit(conv, trigger, kind, state)
            trace.close("llm_error")
            await self._status(conv, "error", str(err))
            return
        except StaleTurn:
            # what the turn was about is over: words now would be out of place
            self._commit(conv, trigger, kind, state)
            trace.close("stale")
            await self._status(conv, "idle")
            return
        except asyncio.CancelledError:
            if first is not None:
                first.cancel()  # its gesture too: the hero speaks again
            if conv.cut is not None:  # the hero interrupted: only what was said counts
                said, conv.cut = conv.cut, None
                self._commit(conv, trigger, kind, state, said)
                self._note(conv, "Герой перебил тебя" + ("" if said else ", ты не успел ничего сказать"))
                trace.close("interrupted")
                raise
            self._commit(conv, trigger, kind, state)  # the npc remembers what it already said
            trace.close("cancelled")
            raise
        except Exception:
            if state.text:  # what the hero heard is remembered; _turn says the rest
                self._commit(conv, trigger, kind, state)
            trace.close("error")
            raise
        finally:
            conv.acting = False
            if quick_line is not None:
                quick_line.cancel()
                await asyncio.gather(quick_line, return_exceptions=True)
            if full is not None:
                full.cancel()  # done by now, unless the turn ended before the line

        self._commit(conv, trigger, kind, state)
        if kind == "player" and state.to is None and not state.end_requested and not state.follow_set:
            self._repair_follow_agreement(conv, trigger.content, state.text)
        trace.close(status)
        total = time.monotonic() - state.started
        if state.text:
            self.journal.add(
                "npc",
                npc=conv.npc,
                name=conv.persona.name,
                text=to_game_text(state.text),
                llm=speaker.name,
                first_token_s=round(state.first_delta or 0.0, 2),
                total_s=round(total, 2),
            )
        log.info(
            "%s replied via %s in %d step(s): first token %.2fs, first line %.2fs, total %.2fs",
            conv.npc,
            speaker.name,
            len(trace.steps),
            state.first_delta or 0.0,
            state.first_line or 0.0,
            total,
        )
        await self._status(conv, "idle")
        if state.end_requested:
            task = asyncio.create_task(self._end_after_speech(conv, list(conv.last_lines)))
            conv.actions.add(task)
            task.add_done_callback(conv.actions.discard)

    async def _steps(
        self,
        conv: Conversation,
        state: TurnState,
        request: list[Item],
        trace: TurnTrace,
        *,
        speak: bool = True,
        quick_line: asyncio.Task[bool] | None = None,
    ) -> str:
        """The conversational model's steps of a turn; tool calls are handled after each (see turn.py).
        `speak` False: the quick model said the npc's line, these steps only act on it, their words unsaid.
        The turn's status: "completed", or "step_cap" when the model wanted to go on still."""
        added: list[Item] = list(request)
        retries = STEP_RETRIES
        first_said = to_game_text(state.filler)
        for step in range(MAX_STEPS):
            choice: ToolChoice | None = "required" if state.act and step == 0 else tool_choice(step)
            st = trace.step(added, choice, silent=not speak)
            while True:
                said = state.text.strip()
                try:
                    st.started_s = round(time.time() - trace.started, 3)
                    done = await self._step(conv, state, request, choice, st, speak=speak)
                    break
                except LLMError as err:
                    # asked again only if nothing of this step was said: else the npc would repeat it
                    if not err.retryable or retries <= 0 or (speak and state.text.strip() != said):
                        raise
                    retries -= 1
                    log.warning("%s: %s; asking the model again", conv.npc, err)
                    trace.event("retry", error=str(err))
            if quick_line is not None:
                if await quick_line:
                    conv.acting = True
                    await self._status(conv, "idle")
                    if not state.overlay:  # tool words are overlays; let an ordinary line finish first
                        if conv.voice is not None:
                            await conv.voice.join()
                        while self.audible(conv.npc):  # noqa: ASYNC110 - playback ends by clock, not event
                            await asyncio.sleep(0.05)
                else:
                    # Gemma said nothing: voice Sol's buffer, or ask once for speech without tools.
                    speak = True
                    st.silent = False
                    trace.event("quick_fallback", step=trace.steps.index(st))
                    if done.text:
                        state.first_delta = time.monotonic() - state.started
                        pieces = state.spoken(
                            state.moods.feed(done.text),
                            {t.name for t in self._request_tools(state.tools)}, conv.npc,
                        )
                        await self._feed_lines(conv, state, pieces)
                        await self._say_rest(conv, state)
                    else:
                        request = [
                            replace(item, content=item.content.removesuffix("\n" + PARALLEL_ACT_HINT))
                            if isinstance(item, Message) and item.role == "developer" else item
                            for item in request
                        ]
                        spoken = trace.step(request, "none")
                        spoken.started_s = round(time.time() - trace.started, 3)
                        await self._step(conv, state, request, "none", spoken)
                quick_line = None
            if state.relevant is not None and not state.relevant() and not (speak and state.text.strip()):
                raise StaleTurn  # the incident is over: nothing said yet, or only late acts are left
            calls = [i for i in done.items if isinstance(i, ToolCall)]
            outcomes = []
            for call in calls:
                before = [t.name for t in state.tools]
                trace.event("tool_start", step=trace.steps.index(st), name=call.name, call_id=call.call_id)
                outcomes.append(await self._tool(conv, call, state))
                after = [t.name for t in state.tools]
                if before != after:
                    trace.event("tools_changed", step=step, before=before, after=after)
            for call, outcome in zip(calls, outcomes, strict=True):
                st.calls.append(
                    CallTrace(call.name, call.arguments, call.call_id, outcome.output, outcome.status)
                )
                note = refusal_note(call.name, outcome.output) if outcome.status == "rejected" else ""
                if note and note not in state.refused:
                    state.refused.append(note)
            if not go_on(outcomes, bool(state.text.strip())):
                return "completed"
            if step == MAX_STEPS - 1:
                return "step_cap"
            spoken = [self._lines[line][0] for line in conv.last_lines]
            said = " ".join(s for s in [first_said, *spoken] if s) if speak else ""
            results = [
                ToolResult(c.call_id, tool_output(o, said))
                for c, o in zip(calls, outcomes, strict=True)
            ]
            added = [*done.items, *results]
            request += added
        return "completed"

    async def _quick_line(
        self, conv: Conversation, state: TurnState, request: list[Item], trace: TurnTrace,
        *, ready: asyncio.Future[bool] | None = None,
    ) -> bool:
        """The npc's line in a fight, from the quick model. False if it said nothing (it failed or went
        quiet first): the conversational model speaks then, as in any other turn."""
        assert self.quick is not None
        player = conv.trigger is not None and conv.trigger.role == "user"
        slot = self._quick_slot(conv.npc) if player and self.surrender_hold is not None else None
        if slot is not None:
            self._quick_busy[slot] += 1  # keep the speaker's prefix while the other slot classifies
        handed_off = False
        try:
            try:
                if slot is not None and self.surrender_hold is not None:
                    handed_off = bool(await self.surrender_hold(conv, request))
            finally:
                if ready is not None and not ready.done():
                    ready.set_result(handed_off)
            st = trace.step(request, None, llm=getattr(self.quick, "model", None) or self.quick.name)
            try:
                await self._step(conv, state, request, None, st, quick=True)
            except LLMError as err:
                log.warning("%s: the quick model failed: %s", conv.npc, err)
                trace.event("quick_failed", error=str(err))
        finally:
            if slot is not None:
                self._quick_busy[slot] -= 1
                self._quick_free.set()
        return bool(state.text.strip())

    async def _act_on_line(
        self,
        conv: Conversation,
        state: TurnState,
        trigger: Message,
        full: asyncio.Task[tuple[str, Snapshot | None, Sight | None]],
        sound: str,
        trace: TurnTrace,
        *,
        quick_line: asyncio.Task[bool] | None = None,
    ) -> str:
        """After the quick model's line: the conversational model, on the whole situation, decides what the
        npc does about it (a bandit that shouted "Держи его!" attacks). Its words are not said; a failure
        only loses the acts, the line is said already."""
        if quick_line is None:
            conv.acting = True  # the hero may speak again: a new turn, not an interruption
            await self._status(conv, "idle")
        situation_text, _, state.sight = await full
        if self.hero_fight_held(conv.npc):
            situation_text, _, state.sight = await self._situation(conv, None)
        if state.dynamic_tools:
            state.tools = self._offered(conv.npc, state.sight)
        if quick_line is None:
            said = " ".join(s for s in (sound, state.text.strip()) if s)
            situation_text += (
                f"\nТы уже сказал вслух: «{said}». Новых слов не нужно: реши, что ты делаешь, и сделай это "
                "инструментами, если нужно."
            )
        else:
            situation_text += "\n" + PARALLEL_ACT_HINT
        request: list[Item] = [*conv.history, Message("developer", situation_text), trigger]
        try:
            return await self._steps(conv, state, request, trace, speak=False, quick_line=quick_line)
        except LLMError as err:
            if quick_line is not None and not await quick_line:
                raise  # neither model supplied a line
            log.warning("%s: no acts on its line: %s", conv.npc, err)
            self.journal.add("llm_error", npc=conv.npc, name=conv.persona.name, detail=str(err))
            trace.event("act_failed", error=str(err))
            return "completed"

    async def _situation(
        self, conv: Conversation, body: str | None, *, brief: bool = False
    ) -> tuple[str, Snapshot | None, Sight | None]:
        """The situation of a turn, from what the game and the extensions say now. `brief`: for the quick
        model's line in a fight, what the npc perceives and does, the extensions not asked; the events that
        came meanwhile are kept for the whole situation that follows."""
        if conv.new_meeting:
            conv.new_meeting = False
            if conv.memory.entries or conv.memory.summary:
                when = game_time(self.link.snapshots.get(conv.npc))
                self._note(conv, f"Новая встреча с героем{f', {when}' if when else ''}")
        if not brief:
            for ext in self.extensions:
                try:
                    await asyncio.wait_for(ext.prepare(conv.npc), PREPARE_S)
                except (GameError, TimeoutError) as err:
                    log.warning("%s: %s not ready: %s", conv.npc, type(ext).__name__, err)
                except Exception:
                    log.exception("%s: %s prepare failed", conv.npc, type(ext).__name__)
        snap = self.link.snapshots.get(conv.npc)
        sight = self.sight(conv.npc, snap) if self.sight is not None else None
        situation_text = npc_tools.render_situation(snap, conv.intent, sight, brief=brief)
        if brief and self.balance is not None and (balance := self.balance(conv.npc)):
            situation_text += "\n" + balance
        if not brief:
            for ext in self.extensions:
                try:
                    extra = ext.situation(conv.npc)
                except Exception:  # one extension's bug must not silence the npc: it answers without it
                    log.exception("%s: %s situation failed", conv.npc, type(ext).__name__)
                    continue
                if extra:
                    situation_text += "\n" + extra
        if not brief and self.fights_hero(conv.npc):
            situation_text += "\n" + FIGHTS_HERO
        if body:
            situation_text += f"\nТвоё тело сейчас: {body} — это уже происходит, не противоречь этому."
        if conv.pending:  # events that came during the last turn
            situation_text += "\nТолько что: " + "; ".join(c.strip("[]") for c in conv.pending) + "."
            if not brief:
                conv.pending = []
        return situation_text, snap, sight

    async def _say_rest(self, conv: Conversation, state: TurnState) -> None:
        """What is left of the step's text once its stream is over: the last sentence."""
        pieces = state.spoken(
            state.moods.flush(),
            {t.name for t in self._request_tools([*npc_tools.TOOLS, *state.tools])},
            conv.npc,
            flush=True,
        )
        await self._feed_lines(conv, state, pieces, flush=True)

    async def _feed_lines(
        self, conv: Conversation, state: TurnState, pieces: list[str | MoodTag], *, flush: bool = False
    ) -> None:
        for line in state.lines(pieces, flush=flush):
            await self._line(conv, state, line)

    async def _line(self, conv: Conversation, state: TurnState, line: str) -> None:
        if state.first_line is None:
            state.first_line = time.monotonic() - state.started
        line = state.strip_line(line)
        if not line:
            return
        # «Браго, …»: from this line on it speaks to him (the two look at each other), and he may answer
        if self.floor is not None and (who := self.floor.called(conv.npc, line)) is not None:
            state.to = who
        await self._say(conv, line, state.reply, state.overlay, state.mood, to=state.to)

    async def _tool(self, conv: Conversation, call: ToolCall, state: TurnState) -> ToolOutcome:
        """Handle a call of the model. Checks run here, against what the npc has and sees: a rejection
        goes back to the model at once. Accepted commitments are carried out after the npc's line."""
        snap = self.link.snapshots.get(conv.npc)
        if all(t.name != call.name for t in state.tools):  # in the request, but not offered in this turn
            return ToolOutcome(f"Отклонено: сейчас {call.name} тебе недоступен.", "rejected")
        match call.name:
            case npc_tools.END_CONVERSATION:
                state.end_requested = True
                return ToolOutcome("Разговор закончится, когда ты договоришь.", "accepted")
            case npc_tools.SET_INTENT:
                outcome = self._set_intent(conv, call, snap, state=state)
                if outcome.status == "accepted" and conv.intent.goal in executor.COMPANION_GOALS:
                    state.follow_set = True
                return outcome
            case npc_tools.RECALL:
                if state.recalled:
                    return ToolOutcome("Больше ничего не вспоминается.", "ignored")
                state.recalled = True
                return ToolOutcome(await self._recall(conv, str(call.arguments.get("query", ""))), "read")
            case npc_tools.GIVE_ITEM:
                args, reason = executor.check_give(call.arguments, snap)
                if args is None:
                    return ToolOutcome(f"Отклонено: {reason}", "rejected")
                key = (args["item"], state.reply)
                if key in state.given:  # the same item again in this turn: already on its way
                    return ToolOutcome("Этот предмет ты уже отдаёшь.", "accepted")
                state.given.add(key)
                self._journal_call(conv, call.name, args)
                give = ToolCall(call.name, args, call.call_id)
                self._after_speech(conv, lambda: self._give(conv, give, snap))
                return ToolOutcome("Принято: отдашь после реплики.", "accepted")
            case npc_tools.TAKE_ITEM:
                sight = state.sight
                item = str(call.arguments.get("item", "")).strip().upper()
                thing = sight.things.get(item) if sight is not None else None
                if thing is None:
                    seen = ", ".join(sight.things) if sight is not None and sight.things else "ничего"
                    why = f"Отклонено: на земле нет {item or '?'}; ты видишь там: {seen}."
                    return ToolOutcome(why, "rejected")
                if item in state.taken:
                    return ToolOutcome("Его ты уже поднимаешь.", "accepted")
                state.taken.add(item)
                self._journal_call(conv, call.name, {"item": item})
                take = ToolCall(call.name, {"item": item, "name": thing.name}, call.call_id)
                self._after_speech(conv, lambda: self._take(conv, take, snap))
                return ToolOutcome("Принято: поднимешь после реплики.", "accepted")
        for ext in self.extensions:
            if (outcome := await ext.handle(self, conv, call, snap)) is not None:
                if call.name == "surrender" and outcome.status == "accepted":
                    state.tools = [tool for tool in state.tools if tool.name != "surrender"]
                if call.name == AMEND and outcome.status == "accepted" and state.dynamic_tools:
                    from .vanilla import ACCEPT, ACCEPT_NOW

                    before = {t.name for t in state.tools}
                    state.tools = self._offered(conv.npc, state.sight)
                    if ACCEPT not in before and any(t.name == ACCEPT for t in state.tools):
                        outcome = replace(
                            outcome, output=f"{outcome.output}\n{ACCEPT_NOW}", continue_turn=True
                        )
                return outcome
        return ToolOutcome(f"Отклонено: нет такого инструмента {call.name}.", "rejected")

    def _request_tools(self, offered: list[Tool]) -> list[Tool]:
        """The tools of a request: every one a turn may offer, the same list each time, so that the start
        of an npc's requests (and the provider's prompt cache) does not change with what is offered; only
        `offered` may be called (`allowed`)."""
        tools = [*self.tools, *(t for ext in self.extensions for t in ext.all_tools)]
        unique = {}
        for tool in [*tools, *offered]:
            unique.setdefault(tool.name, tool)
        return list(unique.values())

    def _offered(self, npc: str, sight: Sight | None = None) -> list[Tool]:
        """The tools for a turn with the hero: the npc's own and those of the extensions."""
        tools = [*self.tools, *(t for ext in self.extensions for t in ext.tools(npc))]
        if sight is None and self.sight is not None:
            sight = self.sight(npc, None)
        if sight is None or not sight.things:  # nothing lying about that it sees: nothing to pick up
            tools = [t for t in tools if t.name != npc_tools.TAKE_ITEM]
        unique = {}
        for tool in tools:
            unique.setdefault(tool.name, tool)
        return list(unique.values())

    async def _recall(self, conv: Conversation, query: str) -> str:
        """Older messages than the npc has in mind (folded into the summary, or beyond what was loaded), and
        what extensions that answer recall know of it (knowledge.py: the npc's dialogs in full)."""
        first = conv.memory.entries[0].seq if conv.memory.entries else conv.memory.summary_seq + 1
        before = min(conv.memory.summary_seq, first - 1)
        path, reason = "lexical", "embeddings disabled or unconfigured"
        if self.memory.embeddings is not None:
            try:
                found = await self.memory.semantic_search(conv.branch, conv.npc, query, before)
                path, reason = "semantic", "none"
            except EmbedError as exc:
                reason = str(exc).replace("\n", " ")
                found = self.memory.search(conv.branch, conv.npc, query, before)
        else:
            found = self.memory.search(conv.branch, conv.npc, query, before)
        parts = []
        if found:
            speaker = {"user": "Герой", "assistant": "Ты", "developer": ""}
            lines = []
            for r in found:
                who = speaker.get(r.role, "")
                lines.append(f"[{r.game_time or '?'}] {who + ': ' if who else ''}{r.content}")
            parts.append("Ты вспоминаешь:\n" + "\n".join(lines))
        for ext in self.extensions:
            recall = getattr(ext, "recall", None)
            if recall is not None and (known := recall(conv.npc, query)):
                parts.append(known)
        log.info(
            "%s recalls %r: path=%s reason=%s, %d message(s), %d part(s)",
            conv.npc, query, path, reason, len(found), len(parts),
        )
        return "\n\n".join(parts) or "Ничего такого не вспоминается."

    async def _give(self, conv: Conversation, call: ToolCall, snap: Snapshot | None) -> tuple[str, bool]:
        if not await self._hold(conv, "give"):
            return "Тело не смогло отдать предмет: ты занят", False
        try:
            outcome = await npc_tools.execute(self.link, conv.npc, call, snap)
        finally:
            self._free(conv, "give")
        return outcome.note, outcome.ok

    async def _take(self, conv: Conversation, call: ToolCall, snap: Snapshot | None) -> tuple[str, bool]:
        """Pick a thing up off the ground. Walking up to it ends a companion's following: it goes with the
        hero again after."""
        if not await self._hold(conv, "take"):
            return "Тело не смогло поднять предмет: ты занят", False
        try:
            outcome = await npc_tools.execute(self.link, conv.npc, call, snap)
            if conv.intent.goal in executor.COMPANION_GOALS:
                await self._body(conv, [("follow", {"npc": "PLAYER"})])
        finally:
            self._free(conv, "take")
        return outcome.note, outcome.ok

"""Knowledge about the witness annotates evidence without deciding its truth."""

import pytest

from gothic_ai.witnesses import Candidate
from tests.fake_game import FakeGame, connected
from tests.test_cover import SAID, VOUCH, CoverWorld
from tests.test_perception import HILDA, LOBART, MALETH


@pytest.mark.parametrize("heard", [False, True])
@pytest.mark.parametrize("covered", [False, True])
async def test_known_cover_annotates_candidates_and_testimony_without_changing_evidence(heard, covered):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.saw()
        if covered:
            w.covered(day=1, vouch=VOUCH)
        testimony = await w.testify()
        w.witnesses._near[LOBART] = [Candidate(HILDA, "Хильда", "near", 900, True)]
        before = w.witnesses.situation(LOBART) or ""
        evidence = w.witnesses.told(LOBART, "С1")
        assert evidence is not None
        assert "(Хильда" not in before
        if heard:
            w.chronicle.record({
                "fact": "rumor", "kind": "cover", "subject": HILDA, "subject_name": "Хильда",
                "about": MALETH, "about_name": "Малет", "from": MALETH, "from_name": "Малет",
                "to": LOBART, "quest": "cover-other", "hops": 1, "day": 1,
                "title": "Хильда покрывала героя и солгала человеку по имени Малет",
            })
        else:
            w.chronicle.record({
                "fact": "exposed", "npc": LOBART, "name": "Лобарт", "liar": HILDA,
                "subject_name": "Хильда", "testimony": testimony.id, "by": "caught-elsewhere",
                "day": 1, "at": w.witnesses.now,
            })
        situation = w.witnesses.situation(LOBART) or ""
        warning = w.witnesses.people_knowledge.warning(LOBART, HILDA)
        assert warning and "Хильда" in warning
        candidates, rows = situation.split("Что тебе рассказали", 1)
        assert warning in candidates
        assert warning in rows
        assert w.witnesses.told(LOBART, "С1") == evidence
        assert evidence.said == SAID
        assert w.witnesses.people_knowledge.warning(LOBART, MALETH) is None

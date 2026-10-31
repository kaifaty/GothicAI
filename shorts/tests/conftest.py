"""Shared fixtures."""

import pytest

from gothic_shorts import delivery


@pytest.fixture(autouse=True)
def delivery_passes(request, monkeypatch):
    """Publication runs the delivery check (gothic_shorts.delivery), which has its own tests in test_delivery.py.
    Elsewhere the synthetic 16x16 videos would fail it, so it passes, recording what it saw."""
    seen = []
    if request.module.__name__.endswith("test_delivery"):
        return seen
    monkeypatch.setattr(delivery, "check_zones", lambda ws, plan, report=None: seen.append(("zones", plan["lang"]))
                        or report or delivery.Report())
    monkeypatch.setattr(delivery, "check_file", lambda video, report=None: seen.append(("file", video.name))
                        or report or delivery.Report())
    return seen

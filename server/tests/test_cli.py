"""CLI defaults start the director only for ordinary play."""

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest

from gothic_ai import cli


@pytest.mark.parametrize(("flags", "websocket"), [([], True), (["--no-websocket"], False)])
def test_serve_arguments_select_responses_transport(monkeypatch, flags, websocket):
    monkeypatch.setenv("OPENAI_API_KEY", "e2e-placeholder")

    async def serve(args):
        backend = cli._backend(args)
        try:
            assert backend._websocket is websocket
        finally:
            await backend.aclose()
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--llm", "openai", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize(
    ("flags", "running"),
    [
        ([], True),
        (["--no-showrunner"], False),
        (["--scenario", "m0"], False),
        (["--scenario", "showrunner"], False),
        (["--scenario", "showrunner_live"], False),
    ],
)
def test_serve_arguments_control_showrunner_task(monkeypatch, flags, running):
    showrunner = SimpleNamespace(run=AsyncMock())

    async def serve(args):
        task = cli._start_showrunner(args, showrunner)
        assert isinstance(task, asyncio.Task) is running
        if task is not None:
            await task
        assert showrunner.run.await_count == int(running)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


def test_showrunner_opt_in_flag_is_removed(monkeypatch, capsys):
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--showrunner"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
    assert "unrecognized arguments: --showrunner" in capsys.readouterr().err


@pytest.mark.parametrize("flags", [[], ["--scenario", "questlog"], ["--no-questlog"]])
def test_questlog_is_enabled_by_default_including_scenarios(monkeypatch, flags):
    async def serve(args):
        assert args.no_questlog == ("--no-questlog" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("flags", [[], ["--scenario", "crimes"], ["--no-crimes"]])
def test_crimes_is_enabled_by_default_including_scenarios(monkeypatch, flags):
    async def serve(args):
        assert args.no_crimes == ("--no-crimes" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize(
    ("flags", "env_url", "enabled", "expected_url", "created"),
    [
        ([], None, True, None, False),
        ([], "tcp://policy:47835", True, "tcp://policy:47835", True),
        (["--no-neural-body"], "tcp://policy:47835", False, "tcp://policy:47835", False),
        (["--neural-body", "--body-url", "tcp://explicit:47835"], None, True, "tcp://explicit:47835", True),
        (["--body-url", "tcp://explicit:47835"], "tcp://policy:47835", True, "tcp://explicit:47835", True),
        (["--body-url", ""], "tcp://policy:47835", True, "", False),
    ],
)
def test_serve_arguments_create_bodypilot_only_with_flag_and_url(
    monkeypatch, flags, env_url, enabled, expected_url, created
):
    if env_url is None:
        monkeypatch.delenv("GOTHIC_BODY_URL", raising=False)
    else:
        monkeypatch.setenv("GOTHIC_BODY_URL", env_url)
    factory = Mock()
    monkeypatch.setattr(cli, "BodyPilot", factory)
    link = SimpleNamespace(body_watch=AsyncMock(), body_acquire=AsyncMock())
    talk = SimpleNamespace(orders=SimpleNamespace(companion=Mock()), attached=set(), partner=None)

    async def serve(args):
        assert args.neural_body is enabled
        assert args.body_url == expected_url
        pilot = cli._start_bodypilot(args, link, talk)
        if created:
            assert pilot is factory.return_value
            assert factory.call_args.args == (link, expected_url)
            assert factory.call_args.kwargs["companion"] is talk.orders.companion
        else:
            assert pilot is None
            factory.assert_not_called()
        link.body_watch.assert_not_called()
        link.body_acquire.assert_not_called()
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


def test_bodypilot_excludes_current_conversation_and_attached_npcs(monkeypatch):
    factory = Mock()
    monkeypatch.setattr(cli, "BodyPilot", factory)
    talk = SimpleNamespace(orders=SimpleNamespace(companion=Mock()), attached=set(), partner=None)
    cli._start_bodypilot(SimpleNamespace(neural_body=True, body_url="tcp://policy:47835"), Mock(), talk)
    unavailable = factory.call_args.kwargs["unavailable"]
    assert not unavailable("npc")
    talk.partner = SimpleNamespace(npc="NPC")
    assert unavailable("npc")
    talk.partner = None
    talk.attached.add("NPC")
    assert unavailable("npc")
    talk.attached.clear()
    assert not unavailable("NPC")

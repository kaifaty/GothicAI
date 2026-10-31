import hashlib
import json
from pathlib import Path

import httpx
import pytest

from gothic_launcher import checks
from gothic_launcher.settings import Config, Secrets, Settings

NO_CREDITS = "You have no credits remaining. Please add credits to continue using the service."


def _ready_base(tmp_path, monkeypatch, text="Привет, герой!", explicit=False):
    game = tmp_path / "game-settings"
    (game / "system").mkdir(parents=True)
    (game / "system/Gothic.ini").write_text("[GAME]\n")
    directory = tmp_path / "base"
    directory.mkdir()
    library = json.dumps({
        "format": 1, "kind": "cutscene_library", "blocks": [{
            "object_id": 0, "class": "zCCSBlock", "name": "TEST", "block": {
                "object_id": 1, "class": "zCCSAtomicBlock", "message": {
                    "object_id": 2, "class": "oCMsgConversation",
                    "name": "DIA_TEST_14_00.WAV", "text": text.encode("cp1251").decode("latin1"),
                },
            },
        }],
    }).encode()
    record = hashlib.sha256(library).hexdigest()
    files = {
        f"outputunits/{record}.json": library,
        "outputunits/outputunits.json": json.dumps({
            "format": "gothic-outputunits", "version": 1,
            "server": {"ou_py": 0}, "sources": [{"record": record}], "records": {record: record},
        }).encode(),
        "unread-payload.bin": b"payload",
    }
    for name, raw in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    manifest = json.dumps({
        "format": "gothic-base-files", "version": 1,
        "files": [{"path": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
                  for name, raw in files.items()],
    }).encode()
    (directory / "base-files.json").write_bytes(manifest)
    (directory / "base.json").write_text(json.dumps({
        "format": "gothic-base", "version": 2, "fingerprint": "a" * 64,
        "manifest": {"path": "base-files.json", "size": len(manifest),
                     "sha256": hashlib.sha256(manifest).hexdigest()},
    }))
    cache = tmp_path / "cache"
    pointer = cache / "current" / f"{hashlib.sha256(str(game.resolve()).encode()).hexdigest()}.json"
    pointer.parent.mkdir(parents=True)
    pointer.write_text(json.dumps({
        "version": 1, "gothic": str(game.resolve()),
        "base": {"path": str(directory.resolve()), "fingerprint": "a" * 64},
    }))
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(cache))
    monkeypatch.setenv("OPENGOTHIC_BASE", str(directory) if explicit else "")
    return game, directory, pointer


def test_base_helper_matches_server_contract():
    root = Path(__file__).resolve().parents[3]
    assert (root / "server/src/gothic_ai/gamedata/base.py").read_bytes() == (
        root / "launcher/core/src/gothic_launcher/base.py"
    ).read_bytes()


@pytest.mark.parametrize("explicit", [False, True])
@pytest.mark.parametrize("text,russian", [("Привет, герой!", True), ("Hello, hero!", False)])
def test_ready_base_without_installation_checks_ou_and_never_opens_game_files(
    tmp_path, monkeypatch, explicit, text, russian,
):
    game, directory, _ = _ready_base(tmp_path, monkeypatch, text, explicit)
    original = Path.open

    def guarded(path, *args, **kwargs):
        assert not path.is_relative_to(game)
        assert path != directory / "unread-payload.bin"
        return original(path, *args, **kwargs)

    monkeypatch.setattr(Path, "open", guarded)
    items = checks.game_folder(game)
    assert [item.id for item in items] == ["base", "russian"]
    assert items[0].ok and items[0].title == "База игры готова" and items[0].detail == "a" * 64
    assert items[1].ok is russian


def test_installation_without_base_promises_first_launch_import(tmp_path, monkeypatch):
    monkeypatch.setenv("OPENGOTHIC_BASE", "")
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(tmp_path / "cache"))
    game = tmp_path / "game"
    (game / "Data").mkdir(parents=True)
    (game / "Data/Worlds.vdf").touch()
    (game / "Data/Addon.vdf").touch()
    ou = game / checks.OU_PATH
    ou.parent.mkdir(parents=True)
    ou.write_bytes("Привет, герой!".encode("cp1251"))
    items = checks.game_folder(game)
    assert all(item.ok for item in items)
    assert items[0].title == "Лаунчер подготовит базу перед запуском (несколько минут)"


def test_missing_installation_or_foreign_pointer_requests_one_time_installation(tmp_path, monkeypatch):
    game, _, pointer = _ready_base(tmp_path, monkeypatch)
    current = json.loads(pointer.read_text())
    current["gothic"] = str(tmp_path / "other-game")
    pointer.write_text(json.dumps(current))
    [foreign] = checks.game_folder(game)
    assert not foreign.ok and "foreign" in foreign.detail
    pointer.unlink()
    [missing] = checks.game_folder(game)
    assert not missing.ok and "Укажите папку установленной Gothic II один раз" in missing.detail


def test_check_command_checks_an_explicit_directory_without_config(tmp_path, monkeypatch, capsys):
    from gothic_launcher import cli

    game, _, _ = _ready_base(tmp_path, monkeypatch)
    monkeypatch.setattr(cli.sys, "argv", ["gothic-launcher", "check", str(game)])
    with pytest.raises(SystemExit) as error:
        cli.main()
    assert error.value.code == 0
    assert [item["id"] for item in json.loads(capsys.readouterr().out)] == ["base", "russian"]


class Inworld:
    """Answers as the real one does: the key first, then the credits, then the voice. Records requests."""

    def __init__(self) -> None:
        self.key = "good-key"
        self.credits = True
        self.stream: tuple[int, dict] | None = None  # an answer the check does not know
        self.requests: list[httpx.Request] = []

    def __call__(self, request: httpx.Request) -> httpx.Response:
        self.requests.append(request)
        if request.headers.get("Authorization") != f"Basic {self.key}":
            return httpx.Response(401, json={"code": 16, "message": "Invalid authorization credentials"})
        if request.url.path == "/tts/v1/voices":
            return httpx.Response(200, json={"voices": [{"voiceId": "Dmitry"}]})
        assert request.url.path == "/tts/v1/voice:stream"
        if self.stream is not None:
            return httpx.Response(self.stream[0], json=self.stream[1])
        if not self.credits:
            return httpx.Response(402, json={"error": {"code": 7, "message": NO_CREDITS, "details": []}})
        voice = json.loads(request.content)["voiceId"]
        return httpx.Response(404, json={"code": 5, "message": f"Unknown voice: {voice}"})


@pytest.fixture
def api() -> Inworld:
    return Inworld()


@pytest.fixture(params=["voice", "speech"])
def part(request) -> str:
    return request.param


def config(tmp_path, part: str, key: str | None = "good-key") -> Config:
    settings = Settings()
    settings.voice.provider, settings.voice.fallback = "chatgpt", "none"
    settings.tts.provider, settings.tts.fallback = "openai", "none"
    selected = settings.voice if part == "voice" else settings.tts
    selected.provider = "inworld"
    return Config(settings, Secrets(inworld_api_key=key), tmp_path)


async def provider(api: Inworld, cfg: Config, part: str) -> dict:
    found = await checks.providers(cfg, transport=httpx.MockTransport(api))
    [inworld] = found[part]["providers"]
    return inworld


def inworld_requests(api: Inworld) -> list[httpx.Request]:
    return [r for r in api.requests if r.url.host == "api.inworld.ai"]


async def test_inworld_with_credits_is_available(api, tmp_path, part):
    assert await provider(api, config(tmp_path, part), part) == {
        "id": "inworld",
        "ok": True,
        "title": "Inworld",
        "detail": "Доступен",
    }
    [request] = inworld_requests(api)  # one line for a voice that does not exist, nothing else
    assert request.method == "POST"
    assert json.loads(request.content)["voiceId"] == checks.INWORLD_CHECK["voiceId"]


async def test_inworld_without_credits_says_so(api, tmp_path, part):
    """The free voice list still answers 200 on an empty account; the check line gets 402."""
    api.credits = False
    found = await provider(api, config(tmp_path, part), part)
    assert not found["ok"]
    assert found["detail"].startswith("Нет кредитов")
    assert [r.method for r in inworld_requests(api)] == ["POST"]  # not saved by the voice list


async def test_inworld_wrong_key(api, tmp_path, part):
    found = await provider(api, config(tmp_path, part, key="old-key"), part)
    assert (found["ok"], found["detail"]) == (False, "Ключ не подходит")
    assert [r.method for r in inworld_requests(api)] == ["POST"]


async def test_inworld_unknown_answer_falls_back_to_the_voice_list(api, tmp_path, part):
    api.stream = (400, {"code": 3, "message": "something new"})
    found = await provider(api, config(tmp_path, part), part)
    assert (found["ok"], found["detail"]) == (True, "Доступен")
    assert [(r.method, r.url.path) for r in inworld_requests(api)] == [
        ("POST", "/tts/v1/voice:stream"),
        ("GET", "/tts/v1/voices"),
    ]


async def test_inworld_down(api, tmp_path, part):
    api.stream = (503, {"message": "unavailable"})
    found = await provider(api, config(tmp_path, part), part)
    assert (found["ok"], found["detail"]) == (False, "Ответил 503")
    assert len(inworld_requests(api)) == 1


async def test_inworld_without_key_asks_nothing(api, tmp_path, part):
    found = await provider(api, config(tmp_path, part, key=None), part)
    assert (found["ok"], found["detail"]) == (False, "Ключ не задан")
    assert inworld_requests(api) == []


async def test_voxtral_checks_health_at_the_configured_address(tmp_path):
    cfg = Config.load(tmp_path)
    cfg.set("voice.provider", "voxtral")
    cfg.set("voice.voxtral_url", "http://gpu:47830/")
    cfg.set("voice.fallback", "none")
    cfg.set("tts.provider", "openai")
    cfg.set("tts.fallback", "none")
    requests = []

    def health(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"ok": True})

    found = await checks.providers(cfg, transport=httpx.MockTransport(health))
    assert found["voice"]["providers"] == [
        {"id": "voxtral", "ok": True, "title": "Voxtral на своём сервере", "detail": "Доступен"}
    ]
    [request] = requests
    assert request.method == "GET"
    assert str(request.url) == "http://gpu:47830/health"

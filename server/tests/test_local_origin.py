import asyncio
import json
import logging

import httpx
import pytest
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import InvalidStatus

from gothic_ai import cli
from gothic_ai.game import GameServer
from gothic_ai.tts import TtsClient
from gothic_ai.voice import AsrClient


@pytest.fixture
async def game_server():
    server = GameServer(port=0)
    task = asyncio.create_task(server.serve_forever())
    try:
        await asyncio.wait_for(server.started.wait(), 5)
        yield server
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


@pytest.mark.parametrize("origin", [
    None, "ws://127.0.0.1:{port}", "ws://localhost:{port}", "ws://[::1]:{port}",
    "ws://127.0.0.1", "ws://localhost", "ws://[::1]",
])
async def test_local_clients_connect(game_server, origin):
    port = game_server.port
    async with connect(f"ws://127.0.0.1:{port}", origin=origin.format(port=port) if origin else None):
        link = await asyncio.wait_for(game_server.wait_link(), 5)
        assert not link.closed.is_set()


@pytest.mark.parametrize("origin", [
    "https://evil.example", "null", "http://localhost:3000", "http://127.0.0.1:3000",
    "http://localhost", "ws://evil.example:47817", "ws://127.0.0.1.evil.example:1",
])
async def test_other_origins_get_http_403(game_server, origin):
    with pytest.raises(InvalidStatus) as exc:
        async with connect(f"ws://127.0.0.1:{game_server.port}", origin=origin):
            pytest.fail("Unexpected WebSocket connection")
    assert exc.value.response.status_code == 403
    assert game_server.link is None


@pytest.mark.parametrize("origin", [None, "https://evil.example"])
async def test_health_is_unchanged(game_server, origin):
    async with httpx.AsyncClient() as http:
        response = await http.get(
            f"http://127.0.0.1:{game_server.port}/health", headers={"Origin": origin} if origin else {}
        )
    assert response.status_code == 200
    assert response.headers["content-type"] == "application/json"
    assert response.json() == {"ok": True, "service": "gothic-ai", "game": False}


@pytest.mark.parametrize("service", ["asr", "tts"])
async def test_voice_clients_do_not_send_origin(service):
    headers = []

    async def handler(ws):
        headers.append(ws.request.headers)
        async for raw in ws:
            if isinstance(raw, bytes):
                continue
            msg = json.loads(raw)
            if service == "asr" and msg["type"] == "end":
                await ws.send(json.dumps({"type": "final", "text": "test", "provider": "fake"}))
            elif service == "tts":
                await ws.send(json.dumps({"type": "start", "id": msg["id"], "sample_rate": 16000}))
                await ws.send(json.dumps({"type": "done", "id": msg["id"]}))

    async with serve(handler, "127.0.0.1", 0) as server:
        url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}"
        client = AsrClient(url) if service == "asr" else TtsClient(url)
        try:
            if service == "asr":
                await client.recognize(b"\0\0", 16000)
            else:
                await client.speak("test", "test", "test voice")
        finally:
            await client.aclose()
    assert len(headers) == 1
    assert "Origin" not in headers[0]


def test_verbose_keeps_websocket_headers_out_of_logs(monkeypatch, caplog):
    caplog.set_level(logging.DEBUG)
    caplog.set_level(logging.NOTSET, logger="websockets")
    caplog.set_level(logging.NOTSET, logger="websockets.client")
    monkeypatch.setattr("sys.argv", ["gothic-ai", "--verbose", "voices", "TEST_NPC"])
    monkeypatch.setattr(cli, "voices", lambda args: 0)
    with pytest.raises(SystemExit) as exc:
        cli.main()
    assert exc.value.code == 0
    for name in ("websockets", "websockets.client"):
        assert logging.getLogger(name).getEffectiveLevel() >= logging.WARNING

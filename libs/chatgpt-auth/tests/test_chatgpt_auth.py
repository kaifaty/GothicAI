import json
import stat
import sys
import time
from urllib.parse import parse_qs, urlsplit

import httpx
import pytest

from chatgpt_auth import ISSUER, RESOURCE, AuthError, ChatGPTAuth, Credentials, TokenStore, login

DISCOVERY = {
    "issuer": ISSUER,
    "authorization_endpoint": f"{ISSUER}/oauth/authorize",
    "token_endpoint": f"{ISSUER}/oauth/token",
}


class FakeAuthServer:
    def __init__(self) -> None:
        self.token_requests: list[dict[str, str]] = []
        self.refresh_count = 0

    def handler(self, request: httpx.Request) -> httpx.Response:
        if request.url.path == "/.well-known/openid-configuration":
            return httpx.Response(200, json=DISCOVERY)
        if request.url.path == "/oauth/token":
            form = {k: v[0] for k, v in parse_qs(request.content.decode()).items()}
            self.token_requests.append(form)
            self.refresh_count += form["grant_type"] == "refresh_token"
            return httpx.Response(
                200,
                json={
                    "token_type": "Bearer",
                    "access_token": f"access-{len(self.token_requests)}",
                    "refresh_token": f"refresh-{len(self.token_requests)}",
                    "expires_in": 3600,
                    "scope": "openid offline_access chatgpt.tokens.use.direct",
                },
            )
        return httpx.Response(404)

    @property
    def transport(self) -> httpx.MockTransport:
        return httpx.MockTransport(self.handler)


async def test_login_completes_pkce_flow_via_loopback(tmp_path):
    auth = FakeAuthServer()
    store = TokenStore(tmp_path / "chatgpt.json")
    opened: list[str] = []

    async def browser(url: str) -> None:
        # what the user's browser does after consent: redirect to our loopback with code + issued client id
        opened.append(url)
        q = {k: v[0] for k, v in parse_qs(urlsplit(url).query).items()}
        async with httpx.AsyncClient() as http:
            await http.get(
                q["redirect_uri"], params={"code": "c0de", "state": q["state"], "client_id": "app_123"}
            )

    import asyncio

    creds = await login(
        store, port=0, open_browser=lambda url: asyncio.ensure_future(browser(url)), transport=auth.transport
    )

    q = {k: v[0] for k, v in parse_qs(urlsplit(opened[0]).query).items()}
    assert q["client_id"] == "dynamic_agent_client"
    assert q["code_challenge_method"] == "S256" and q["resource"] == RESOURCE
    assert "chatgpt.tokens.use.direct" in q["scope"]
    exchange = auth.token_requests[0]
    assert exchange["grant_type"] == "authorization_code"
    assert exchange["client_id"] == "app_123" and exchange["code"] == "c0de" and exchange["code_verifier"]
    assert creds.client_id == "app_123" and creds.access_token == "access-1"
    if sys.platform != "win32":  # Windows has no modes: the user profile's permissions apply
        assert stat.S_IMODE(store.path.stat().st_mode) == 0o600


async def test_valid_token_is_returned_without_network(tmp_path):
    store = TokenStore(tmp_path / "chatgpt.json")
    store.save(Credentials("app_1", "fresh", time.time() + 3600, "r"))
    auth = ChatGPTAuth(store, transport=httpx.MockTransport(lambda r: pytest.fail("no network expected")))
    assert await auth() == "fresh"


async def test_expired_token_is_refreshed_once_and_rotation_persisted(tmp_path):
    import asyncio

    server = FakeAuthServer()
    store = TokenStore(tmp_path / "chatgpt.json")
    store.save(Credentials("app_1", "old", time.time() - 10, "refresh-0"))
    auth = ChatGPTAuth(store, transport=server.transport)

    tokens = await asyncio.gather(auth(), auth(), auth())

    assert server.refresh_count == 1  # concurrent callers share one refresh
    assert set(tokens) == {"access-1"}
    saved = json.loads(store.path.read_text())
    assert saved["refresh_token"] == "refresh-1"
    assert server.token_requests[0]["refresh_token"] == "refresh-0"


async def test_not_signed_in(tmp_path):
    with pytest.raises(AuthError, match="login chatgpt"):
        await ChatGPTAuth(TokenStore(tmp_path / "none.json"))()


async def test_processes_sharing_a_store_refresh_once(tmp_path):
    """Two token providers on one store (server + ASR service) must not both spend the refresh token."""
    import asyncio

    server = FakeAuthServer()
    path = tmp_path / "chatgpt.json"
    TokenStore(path).save(Credentials("app_1", "old", time.time() - 10, "refresh-0"))
    first = ChatGPTAuth(TokenStore(path), transport=server.transport)
    second = ChatGPTAuth(TokenStore(path), transport=server.transport)

    tokens = await asyncio.gather(first(), second())

    assert server.refresh_count == 1
    assert set(tokens) == {"access-1"}

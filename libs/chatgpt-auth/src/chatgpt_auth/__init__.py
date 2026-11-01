"""Sign in with ChatGPT: use a ChatGPT plan from this local app (OAuth 2 + PKCE, loopback redirect).

Protocol as documented for open-source/local apps (issuer auth.openai.com, dynamic client registration
via `client_id=dynamic_agent_client`, scope `chatgpt.tokens.use.direct`, resource api.openai.com/v1).
Access tokens live ~1 h; refresh tokens rotate, so refreshes are serialized and persisted immediately.
Several processes may share one token store (the AI server and the ASR service): a refresh happens under
an exclusive file lock and re-reads the store first, so a rotated refresh token is never used twice.
The lock is flock on Unix and a byte-range lock (msvcrt.locking) on Windows; both belong to the open file
and go away with it or with the process. Tokens are stored in a 0600 file (on Windows in the user profile,
whose permissions keep it private) and never logged.
"""

from __future__ import annotations

import asyncio
import base64
import errno
import hashlib
import json
import logging
import os
import secrets
import sys
import time
import webbrowser
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit

import httpx

log = logging.getLogger(__name__)

ISSUER = "https://auth.openai.com"
RESOURCE = "https://api.openai.com/v1"
SCOPES = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
CALLBACK_PATH = "/auth/callback"
APP_NAME = "Gothic AI"
DEFAULT_PORT = 47818
DEFAULT_STORE = Path.home() / ".config" / "gothic-ai" / "chatgpt.json"
REFRESH_MARGIN_S = 120
LOGIN_HINT = "run `gothic-ai login chatgpt`"
LOCK_POLL_S = 0.05  # Windows has no blocking lock without a 10 s limit: poll, as flock waits
REPLACE_TRIES = 50  # Windows: up to ~1 s for a reader to close the store before it can be replaced
# what msvcrt.locking raises while another handle holds the bytes
_LOCKED = {errno.EACCES, getattr(errno, "EDEADLOCK", errno.EDEADLK)}


class AuthError(RuntimeError):
    """Not signed in, the session expired or the sign-in failed. Retrying does not help."""

    retryable = False


@dataclass
class Credentials:
    client_id: str
    access_token: str
    expires_at: float
    refresh_token: str | None = None
    scopes: list[str] = field(default_factory=list)
    email: str | None = None


class TokenStore:
    def __init__(self, path: Path = DEFAULT_STORE) -> None:
        self.path = path

    def load(self) -> Credentials | None:
        try:
            return Credentials(**json.loads(self.path.read_text()))
        except FileNotFoundError:
            return None

    def save(self, creds: Credentials) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        tmp = self.path.with_suffix(".tmp")
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(asdict(creds), f)
        _replace(tmp, self.path)

    def delete(self) -> None:
        self.path.unlink(missing_ok=True)

    def lock(self) -> _FileLock:
        """Exclusive lock shared by every process using this store (held while refreshing)."""
        return _FileLock(self.path.with_suffix(".lock"))


def _replace(tmp: Path, path: Path) -> None:
    """Windows refuses to replace a file that another process has open (a reader of the store, for a
    moment): wait for it rather than lose a rotated refresh token. Elsewhere a refusal is final."""
    for attempt in range(REPLACE_TRIES):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if sys.platform != "win32" or attempt == REPLACE_TRIES - 1:
                raise
            time.sleep(0.02)


class _FileLock:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._fd: int | None = None

    def acquire(self) -> None:
        """Blocking until the holder releases it or exits: call it from a worker thread."""
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            if sys.platform == "win32":
                _lock_windows(fd)
            else:
                import fcntl

                fcntl.flock(fd, fcntl.LOCK_EX)
        except BaseException:
            os.close(fd)
            raise
        self._fd = fd

    def release(self) -> None:
        if self._fd is not None:
            if sys.platform == "win32":
                import msvcrt

                os.lseek(self._fd, 0, os.SEEK_SET)
                msvcrt.locking(self._fd, msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self._fd, fcntl.LOCK_UN)
            os.close(self._fd)
            self._fd = None


def _lock_windows(fd: int) -> None:
    """The first byte of the lock file, exclusively. LK_LOCK gives up after 10 s, so this polls the
    non-blocking form instead: it waits as long as flock would, and other errors still raise."""
    import msvcrt

    while True:
        os.lseek(fd, 0, os.SEEK_SET)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            return
        except OSError as err:
            if err.errno not in _LOCKED:
                raise
        time.sleep(LOCK_POLL_S)


async def _discovery(http: httpx.AsyncClient) -> dict[str, str]:
    resp = await http.get(f"{ISSUER}/.well-known/openid-configuration")
    resp.raise_for_status()
    data = resp.json()
    if data.get("issuer") != ISSUER:
        raise AuthError("ChatGPT sign-in configuration could not be verified")
    for key in ("authorization_endpoint", "token_endpoint"):
        if not str(data.get(key, "")).startswith(ISSUER + "/"):
            raise AuthError("ChatGPT sign-in configuration could not be verified")
    return data


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def _id_token_email(id_token: str | None) -> str | None:
    # Display only; the token came straight from the token endpoint over TLS.
    if not id_token or id_token.count(".") != 2:
        return None
    payload = id_token.split(".")[1]
    try:
        claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
    except ValueError:
        return None
    return claims.get("email")


def _credentials(data: dict, client_id: str, previous: Credentials | None = None) -> Credentials:
    if str(data.get("token_type", "")).lower() != "bearer" or not data.get("access_token"):
        raise AuthError("ChatGPT returned incomplete credentials, sign in again")
    scopes = str(data.get("scope") or " ".join(previous.scopes if previous else [])).split()
    return Credentials(
        client_id=client_id,
        access_token=data["access_token"],
        expires_at=time.time() + float(data.get("expires_in", 3600)),
        refresh_token=data.get("refresh_token") or (previous.refresh_token if previous else None),
        scopes=scopes,
        email=_id_token_email(data.get("id_token")) or (previous.email if previous else None),
    )


class _Callback:
    """Minimal loopback HTTP listener for the OAuth redirect."""

    def __init__(self, state: str) -> None:
        self.state = state
        self.result: asyncio.Future[dict[str, str]] = asyncio.get_running_loop().create_future()

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request_line = (await reader.readline()).decode("latin-1")
            while (await reader.readline()) not in (b"\r\n", b"\n", b""):
                pass
            method, target, *_ = request_line.split(" ")
            url = urlsplit(target)
            params = {k: v[0] for k, v in parse_qs(url.query).items() if len(v) == 1}
            if method != "GET" or url.path != CALLBACK_PATH or params.get("state") != self.state:
                await self._reply(writer, 404, "Not found")
                return
            await self._reply(writer, 200, "Вход выполнен. Можно закрыть вкладку и вернуться в Gothic AI.")
            if not self.result.done():
                self.result.set_result(params)
        finally:
            writer.close()

    @staticmethod
    async def _reply(writer: asyncio.StreamWriter, status: int, text: str) -> None:
        body = f"<!doctype html><meta charset=utf-8><title>Gothic AI</title><p>{text}</p>".encode()
        head = (
            f"HTTP/1.1 {status} OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            f"Content-Length: {len(body)}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
        )
        writer.write(head.encode() + body)
        await writer.drain()


async def login(
    store: TokenStore,
    *,
    port: int = DEFAULT_PORT,
    open_browser: Callable[[str], object] = webbrowser.open,
    timeout: float = 300,
    transport: httpx.AsyncBaseTransport | None = None,
) -> Credentials:
    """Interactive sign-in in the user's browser. Reuses the registered client id on later sign-ins."""
    previous = store.load()
    state, nonce, verifier = (secrets.token_urlsafe(32) for _ in range(3))
    callback = _Callback(state)
    server = await asyncio.start_server(callback.handle, "127.0.0.1", port)
    redirect_uri = f"http://127.0.0.1:{server.sockets[0].getsockname()[1]}{CALLBACK_PATH}"
    try:
        async with httpx.AsyncClient(timeout=30, follow_redirects=False, transport=transport) as http:
            provider = await _discovery(http)
            params = {
                "client_id": previous.client_id if previous else "dynamic_agent_client",
                "response_type": "code",
                "redirect_uri": redirect_uri,
                "scope": SCOPES,
                "resource": RESOURCE,
                "state": state,
                "nonce": nonce,
                "code_challenge_method": "S256",
                "code_challenge": _b64url(hashlib.sha256(verifier.encode()).digest()),
            }
            if previous is None:
                params["agent_name_hint"] = APP_NAME
            open_browser(f"{provider['authorization_endpoint']}?{urlencode(params)}")
            log.info("waiting for ChatGPT sign-in in the browser...")
            result = await asyncio.wait_for(callback.result, timeout)

            if "error" in result:
                raise AuthError(f"ChatGPT sign-in failed: {result['error']}")
            client_id = result.get("client_id") or (previous.client_id if previous else None)
            if not result.get("code") or not client_id or client_id == "dynamic_agent_client":
                raise AuthError("ChatGPT did not complete app registration, try again")

            resp = await http.post(
                provider["token_endpoint"],
                data={
                    "grant_type": "authorization_code",
                    "client_id": client_id,
                    "code": result["code"],
                    "code_verifier": verifier,
                    "redirect_uri": redirect_uri,
                    "resource": RESOURCE,
                },
                headers={"accept": "application/json"},
            )
            if resp.status_code != 200:
                raise AuthError(f"ChatGPT token exchange failed: HTTP {resp.status_code}")
            creds = _credentials(resp.json(), client_id)
            store.save(creds)
            return creds
    finally:
        server.close()
        await server.wait_closed()


class ChatGPTAuth:
    """Token provider: `await auth()` returns a valid plan access token, refreshing it when needed."""

    def __init__(
        self, store: TokenStore | None = None, *, transport: httpx.AsyncBaseTransport | None = None
    ) -> None:
        self.store = store or TokenStore()
        self._transport = transport
        self._lock = asyncio.Lock()

    async def __call__(self) -> str:
        async with self._lock:
            creds = self._valid()
            if creds is not None:
                return creds.access_token
            lock = self.store.lock()
            await asyncio.to_thread(lock.acquire)
            try:
                # another process may have refreshed while we waited for the lock
                creds = self._valid()
                if creds is not None:
                    return creds.access_token
                creds = self.store.load()
                if creds is None or not creds.refresh_token:
                    raise AuthError(f"ChatGPT session expired: {LOGIN_HINT}")
                return (await self._refresh(creds)).access_token
            finally:
                lock.release()

    def _valid(self) -> Credentials | None:
        creds = self.store.load()
        if creds is None:
            raise AuthError(f"not signed in to ChatGPT: {LOGIN_HINT}")
        return creds if creds.expires_at - REFRESH_MARGIN_S > time.time() else None

    async def _refresh(self, creds: Credentials) -> Credentials:
        async with httpx.AsyncClient(timeout=30, follow_redirects=False, transport=self._transport) as http:
            provider = await _discovery(http)
            resp = await http.post(
                provider["token_endpoint"],
                data={
                    "grant_type": "refresh_token",
                    "client_id": creds.client_id,
                    "refresh_token": creds.refresh_token,
                    "resource": RESOURCE,
                },
                headers={"accept": "application/json"},
            )
        if resp.status_code != 200:
            raise AuthError(f"ChatGPT token refresh failed (HTTP {resp.status_code}): {LOGIN_HINT}")
        fresh = _credentials(resp.json(), creds.client_id, previous=creds)
        # the old refresh token is consumed now: persist before anything else can fail
        self.store.save(fresh)
        log.info("ChatGPT access token refreshed")
        return fresh

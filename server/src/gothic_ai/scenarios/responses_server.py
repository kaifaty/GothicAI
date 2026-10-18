"""An in-process Responses endpoint for the websocket scenario; no model or credentials involved.

websockets rejects HTTP requests with bodies before process_request runs. A small HTTP ingress
therefore serves POSTs as SSE and forwards upgrades to a private websockets listener on loopback.
Both transports use the same base URL and answer callback, as the real backend expects.
"""

import asyncio
import json
from collections.abc import Callable
from contextlib import suppress
from typing import Self

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

Answer = Callable[[dict, int], list[dict]]


def reply(response_id: str, text: str, *, call: dict | None = None) -> list[dict]:
    """Responses events for a short reply, an optional complete function_call item, or both."""
    events: list[dict] = []
    items: list[dict] = []
    if text:
        events.append(
            {"type": "response.output_text.delta", "delta": text, "item_id": "m", "output_index": 0}
        )
        items.append(
            {
                "type": "message",
                "id": "m",
                "role": "assistant",
                "status": "completed",
                "content": [{"type": "output_text", "text": text, "annotations": []}],
            }
        )
    if call is not None:
        items.append(call)
    events.extend(
        {"type": "response.output_item.done", "output_index": index, "item": item}
        for index, item in enumerate(items)
    )
    events.append(
        {
            "type": "response.completed",
            "response": {
                "id": response_id,
                "status": "completed",
                "output": items,
                "usage": {
                    "input_tokens": 10,
                    "input_tokens_details": {"cached_tokens": 0},
                    "output_tokens": 5,
                    "output_tokens_details": {"reasoning_tokens": 0},
                    "total_tokens": 15,
                },
            },
        }
    )
    return events


class ResponsesServer:
    """Keep real HTTP/WS requests and answer each with answer(request, unique_response_number)."""

    def __init__(self, answer: Answer | None = None) -> None:
        self.answer = answer or (lambda request, n: reply(f"resp_{n}", f"Ответ {n}."))
        self.base_url = ""
        self.connections: list[list[dict]] = []
        self.http_requests: list[dict] = []
        self.handshakes = 0
        self.refused = 0
        self.refuse_websocket = False
        self.count = 0
        self._http: asyncio.Server | None = None
        self._ws: Server | None = None
        self._ws_port = 0
        self._tasks: set[asyncio.Task] = set()
        self._writers: set[asyncio.StreamWriter] = set()

    @property
    def requests(self) -> list[dict]:
        return [request for connection in self.connections for request in connection]

    async def __aenter__(self) -> Self:
        self._ws = await serve(self._handle_ws, "127.0.0.1", 0, close_timeout=1)
        self._ws_port = self._ws.sockets[0].getsockname()[1]
        try:
            self._http = await asyncio.start_server(self._handle_http, "127.0.0.1", 0)
        except BaseException:
            await self.aclose()
            raise
        port = self._http.sockets[0].getsockname()[1]
        self.base_url = f"http://127.0.0.1:{port}/v1"
        return self

    async def __aexit__(self, *exc) -> None:
        await self.aclose()

    async def aclose(self) -> None:
        if self._http is not None:
            self._http.close()
            await self._http.wait_closed()
        if self._ws is not None:
            self._ws.close()
        for writer in list(self._writers):
            writer.close()
        tasks = list(self._tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if self._ws is not None:
            await self._ws.wait_closed()

    def _answer(self, request: dict) -> list[dict]:
        self.count += 1
        return self.answer(request, self.count)

    async def _handle_ws(self, ws: ServerConnection) -> None:
        got: list[dict] = []
        self.connections.append(got)
        with suppress(ConnectionClosed):
            async for raw in ws:
                request = json.loads(raw)
                got.append(request)
                for event in self._answer(request):
                    await ws.send(json.dumps(event, ensure_ascii=False))

    async def _handle_http(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        task = asyncio.current_task()
        assert task is not None
        self._tasks.add(task)
        self._writers.add(writer)
        try:
            async with asyncio.timeout(10):
                header = await reader.readuntil(b"\r\n\r\n")
                first, *lines = header.decode("iso-8859-1").split("\r\n")
                method, path, _ = first.split(" ", 2)
                headers = dict(line.split(":", 1) for line in lines if line)
                headers = {name.lower(): value.strip() for name, value in headers.items()}
                if path != "/v1/responses":
                    await self._respond(writer, "404 Not Found", b"Unknown endpoint")
                    return
                if method == "GET" and headers.get("upgrade", "").lower() == "websocket":
                    self.handshakes += 1
                    if self.refuse_websocket:
                        self.refused += 1
                        await self._respond(writer, "403 Forbidden", b"WebSocket disabled for this check")
                        return
                elif method == "POST":
                    size = int(headers.get("content-length", "0"))
                    request = json.loads(await reader.readexactly(size))
                    self.http_requests.append(request)
                    body = "".join(
                        f"event: {event['type']}\ndata: {json.dumps(event, ensure_ascii=False)}\n\n"
                        for event in self._answer(request)
                    ).encode()
                    await self._respond(writer, "200 OK", body, content_type="text/event-stream")
                    return
                else:
                    await self._respond(writer, "405 Method Not Allowed", b"Expected POST or WebSocket")
                    return
            # WebSocket lifetimes outlast the timeout for parsing an HTTP request.
            upstream_reader, upstream_writer = await asyncio.open_connection("127.0.0.1", self._ws_port)
            self._writers.add(upstream_writer)
            try:
                upstream_writer.write(header)
                await upstream_writer.drain()
                await self._relay(reader, writer, upstream_reader, upstream_writer)
            finally:
                upstream_writer.close()
                with suppress(ConnectionError):
                    await upstream_writer.wait_closed()
                self._writers.discard(upstream_writer)
        except (ConnectionError, asyncio.IncompleteReadError, TimeoutError):
            pass
        finally:
            writer.close()
            with suppress(ConnectionError):
                await writer.wait_closed()
            self._writers.discard(writer)
            self._tasks.discard(task)

    @staticmethod
    async def _respond(
        writer: asyncio.StreamWriter, status: str, body: bytes, *, content_type: str = "text/plain"
    ) -> None:
        writer.write(
            (
                f"HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\n"
                f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
            ).encode()
            + body
        )
        await writer.drain()

    @staticmethod
    async def _relay(
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        upstream_reader: asyncio.StreamReader,
        upstream_writer: asyncio.StreamWriter,
    ) -> None:
        async def copy(source: asyncio.StreamReader, destination: asyncio.StreamWriter) -> None:
            while data := await source.read(65536):
                destination.write(data)
                await destination.drain()

        tasks = [
            asyncio.create_task(copy(reader, upstream_writer)),
            asyncio.create_task(copy(upstream_reader, writer)),
        ]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

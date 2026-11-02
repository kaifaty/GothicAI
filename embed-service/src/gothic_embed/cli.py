"""Command-line entry point for the embedding service."""

import argparse
import asyncio
import logging
import os

from aiohttp import web

from .service import EmbedService, create_app

log = logging.getLogger("gothic_embed")


async def serve(args: argparse.Namespace, service: EmbedService) -> None:
    runner = web.AppRunner(create_app(service))
    try:
        await runner.setup()
        await web.TCPSite(runner, args.host, args.port).start()
        await service.load()
        log.info(
            "listening on %s:%s model=%s dim=%s dtype=%s",
            args.host, args.port, service.model, service.dim, service.dtype,
        )
        await asyncio.Event().wait()
    finally:
        await runner.cleanup()


def main() -> None:
    parser = argparse.ArgumentParser(prog="gothic-embed")
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("serve", help="run the embedding HTTP service")
    command.add_argument("--host", default=os.environ.get("EMBED_HOST", "127.0.0.1"))
    command.add_argument("--port", type=int, default=int(os.environ.get("EMBED_PORT", "47837")))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        service = EmbedService(
            model=os.environ.get("EMBED_MODEL", "ai-forever/FRIDA"),
            dtype=os.environ.get("EMBED_DTYPE", "bfloat16"),
            device=os.environ.get("EMBED_DEVICE", "cuda"),
        )
    except ValueError as exc:
        parser.error(str(exc))
    try:
        asyncio.run(serve(args, service))
    except KeyboardInterrupt:
        pass

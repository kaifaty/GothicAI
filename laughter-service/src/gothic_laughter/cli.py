"""Command-line entry point for the laughter service."""

import argparse
import asyncio
import logging
import os

from aiohttp import web

from .service import LaughterService, create_app

log = logging.getLogger("gothic_laughter")


async def serve(args: argparse.Namespace, service: LaughterService) -> None:
    runner = web.AppRunner(create_app(service))
    try:
        await runner.setup()
        await web.TCPSite(runner, args.host, args.port).start()
        await service.load()
        log.info(
            "listening on %s:%s device=%s provider=%s",
            args.host, args.port, service.device, service.provider,
        )
        await asyncio.Event().wait()
    finally:
        await runner.cleanup()


def main() -> None:
    parser = argparse.ArgumentParser(prog="gothic-laughter")
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("serve", help="run the laughter HTTP service")
    command.add_argument("--host", default=os.environ.get("LAUGHTER_HOST", "127.0.0.1"))
    command.add_argument("--port", type=int, default=int(os.environ.get("LAUGHTER_PORT", "47838")))
    command.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    command.add_argument("--threads", type=int, default=4, help="CPU inference threads")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        service = LaughterService(device=args.device, threads=args.threads)
    except ValueError as exc:
        parser.error(str(exc))
    try:
        asyncio.run(serve(args, service))
    except KeyboardInterrupt:
        pass

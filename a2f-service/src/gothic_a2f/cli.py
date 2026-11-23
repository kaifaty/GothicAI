"""Command-line entry point of gothic-a2f."""

import argparse
import asyncio
import logging
import os
import time
from pathlib import Path

from aiohttp import web

from .engine import NATIVE_LIB, Engine
from .service import FaceService, create_app

log = logging.getLogger("gothic_a2f")
DEFAULT_MODEL_DIR = "~/gothic-ai/a2f/model/v3.0"


async def serve(args: argparse.Namespace, service: FaceService, started: float) -> None:
    runner = web.AppRunner(create_app(service))
    try:
        await runner.setup()
        await web.TCPSite(runner, args.host, args.port).start()
        await service.load()
        log.info(
            "listening on %s:%s model=%s face=%s solver=%s shapes=%d ready in %.1f s",
            args.host, args.port, args.model_dir, args.face, args.solver, len(service.engine.shapes),
            time.perf_counter() - started,
        )
        await asyncio.Event().wait()
    finally:
        await runner.cleanup()


def main() -> None:
    env = os.environ.get
    parser = argparse.ArgumentParser(prog="gothic-a2f")
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("serve", help="run the face track HTTP service")
    command.add_argument("--host", default=env("A2F_HOST", "127.0.0.1"))
    command.add_argument("--port", type=int, default=int(env("A2F_PORT", "47839")))
    command.add_argument("--model-dir", default=env("A2F_MODEL_DIR", DEFAULT_MODEL_DIR),
                         help="directory of the diffusion model.json with a built network.trt")
    command.add_argument("--model-name", default=env("A2F_MODEL_NAME", "v3.0"), help="the model in /health")
    command.add_argument("--face", default=env("A2F_FACE", "Mark"), help="identity of the model")
    command.add_argument("--solver", choices=("gpu", "cpu"), default=env("A2F_SOLVER", "cpu"),
                         help="cpu: exact BVLS, the weights of a2f-weights; "
                         "gpu: the SDK's 4-step ADMM, faster, other weights")
    command.add_argument("--lib", default=env("A2F_LIB", str(NATIVE_LIB)), help="the built liba2f_engine.so")
    args = parser.parse_args()
    started = time.perf_counter()
    model_dir, lib = Path(args.model_dir).expanduser(), Path(args.lib).expanduser()
    service = FaceService(
        lambda: Engine(model_dir, args.face, gpu_solver=args.solver == "gpu", lib=lib),
        model=args.model_name, face=args.face,
    )
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        asyncio.run(serve(args, service, started))
    except KeyboardInterrupt:
        pass

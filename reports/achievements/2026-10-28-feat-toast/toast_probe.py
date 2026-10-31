"""Stand-in AI server for a look at the feat toast: once the world is loaded it sends three toasts.

uv run --with websockets python toast_probe.py PORT ICON_DIR
"""

import asyncio
import json
import sys
import time

import websockets

PORT, ICONS = int(sys.argv[1]), sys.argv[2]
TOASTS = [
    ("Волки, которых не было", "Сказал Лотару, что перебил стаю, и никто не узнал правду.", "deceit"),
    ("Сдался и выжил", "Встал на колени перед бандитами и ушёл живым.", "survival"),
    ("Вернул чужое", "Нашёл украденное у Торбена и вернул без награды.", "oath"),
]


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


async def handler(ws):
    sent = False
    async for raw in ws:
        msg = json.loads(raw)
        kind = msg.get("type")
        if kind not in ("perception", "npc_roster", "story_world", "body_observations", "npc_view"):
            log("<-", kind)
        if kind in ("world_loaded", "session") and not sent:
            sent = True
            asyncio.create_task(send(ws))


async def send(ws):
    await asyncio.sleep(12)
    for i, (title, text, cat) in enumerate(TOASTS * 20):
        await ws.send(json.dumps({"type": "feat_toast", "feat": f"probe{i}", "title": title, "text": text,
                                  "icon": f"{ICONS}/{cat}/icon.png"}, ensure_ascii=False))
        log("-> toast", title)
        await asyncio.sleep(9)
    await asyncio.sleep(3600)


async def main():
    async with websockets.serve(handler, "127.0.0.1", PORT, max_size=None):
        log("listening", PORT)
        await asyncio.Future()


asyncio.run(main())

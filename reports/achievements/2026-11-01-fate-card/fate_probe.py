"""Stand-in AI server for a look at the final card: answers fate_end with a sample card (FATE_FEATS=0..3).

uv run --with websockets python fate_probe.py PORT CATEGORY_DIR FEATS_DIR
"""

import asyncio
import json
import os
import sys
import time

import websockets

PORT, CATS, FEATS = int(sys.argv[1]), sys.argv[2], sys.argv[3]


def entry(i, title, text, cat, chapter, day, picture=None, epilogue=""):
    own = os.path.join(FEATS, picture) if picture else ""
    engraving = os.path.join(own, "engraving.jpg") if own else os.path.join(CATS, cat, "engraving.jpg")
    icon = os.path.join(own, "icon.png") if own else os.path.join(CATS, cat, "icon.png")
    if not os.path.isfile(icon):
        icon = os.path.join(CATS, cat, "icon.png")
    return {"id": f"p{i}", "title": title, "text": text, "category": cat, "chapter": chapter, "day": day,
            "engraving": engraving if os.path.isfile(engraving) else "", "icon": icon, "epilogue": epilogue}


PICS = sorted(os.listdir(FEATS)) if os.path.isdir(FEATS) else []
FATE = [
    entry(1, "Волки, которых не было", "Сказал Лотару, что перебил стаю у фермы, и взял за это золото.", "deceit", 1, 2,
          epilogue="Позже Лотар сам увидел живых волков у фермы и узнал об обмане."),
    entry(2, "Сдался и выжил", "Встал на колени перед бандитами у моста и ушёл живым.", "survival", 1, 3,
          PICS[0] if PICS else None),
    entry(3, "Вернул чужое", "Нашёл украденный кошель Торбена и вернул без награды.", "oath", 2, 6),
]
OTHERS = [
    {"chapter": 2, "day": 5, "feats": [
        entry(4, "Смерть землепашца", "Убил Лобарта, жителя окрестностей, в башне Ксардаса.", "blood", 2, 5,
              PICS[1] if len(PICS) > 1 else None),
        entry(5, "Пощадил вора", "Отпустил Вино, когда тот сдался с краденой репой.", "mercy", 2, 5),
    ]},
    {"chapter": 1, "day": 1, "feats": [
        entry(6, "Ложная клятва", "Поклялся Хильде вернуть сковороду и продал её в городе.", "word", 1, 1),
    ]},
]


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


async def handler(ws):
    async for raw in ws:
        msg = json.loads(raw)
        if msg.get("type") == "chronicle_get":
            await ws.send(json.dumps({"type": "chronicle", "req": msg.get("req", ""), "fate": FATE, "others": OTHERS},
                                     ensure_ascii=False))
            log("-> chronicle", msg.get("req"))
        elif msg.get("type") == "fate_end":
            n = int(os.environ.get("FATE_FEATS", "3"))
            await asyncio.sleep(float(os.environ.get("FATE_DELAY", "3")))
            await ws.send(json.dumps({"type": "fate_card", "req": msg.get("req", ""), "off": False,
                                      "title": "Лжец с добрым сердцем",
                                      "text": "Он обманывал ради золота, но вернул чужое и щадил тех, кто сдавался; "
                                              "Хоринис запомнит его и тем, и другим.",
                                      "feats": FATE[:n], "chapter": 2, "day": 6, "count": 7}, ensure_ascii=False))
            log("-> fate_card", msg.get("kind"), msg.get("killer"), msg.get("name"))
        elif msg.get("type") in ("hello", "session", "world_loaded"):
            log("<-", msg.get("type"))


async def main():
    async with websockets.serve(handler, "127.0.0.1", PORT, max_size=None):
        log("listening", PORT)
        await asyncio.Future()


asyncio.run(main())

"""Stand-in AI server for a look at the ends of other fates in the chronicle tab (protocol v57).

uv run --with websockets python chronicle_probe.py PORT CATEGORY_DIR FEATS_DIR
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
    ], "ends": [
        {"kind": "death", "title": "Кровь на мельнице", "name": "Бандит", "chapter": 2, "day": 6,
         "text": "Герой пролил кровь невинного в башне Ксардаса и пощадил вора, но сам пал от бандитского "
                 "клинка у мельницы, так и не узнав, чего стоила его милость."},
    ]},
    {"chapter": 1, "day": 1, "feats": [
        entry(6, "Ложная клятва", "Поклялся Хильде вернуть сковороду и продал её в городе.", "word", 1, 1),
    ], "ends": [
        {"kind": "wrath", "title": "Вечная служба", "name": "", "chapter": 1, "day": 1,
         "text": "Ксардас не стерпел дерзости, и герой стал его слугой."},
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
        elif msg.get("type") in ("hello", "session", "world_loaded"):
            log("<-", msg.get("type"))


async def main():
    async with websockets.serve(handler, "127.0.0.1", PORT, max_size=None):
        log("listening", PORT)
        await asyncio.Future()


asyncio.run(main())

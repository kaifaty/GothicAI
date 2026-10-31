#!/usr/bin/env python3
"""Generate or edit an image through the OpenAI Images API with an explicit model.

The second image source of the remaster pipeline (docs/remaster-pipeline.md): unlike
Codex's built-in image_gen, the model is chosen and recorded. The key is read from the
`OPEN_AI` (or `OPENAI_API_KEY`) environment variable or the repository's `.env`; it is
never printed. Every result is stored like an image_gen source: an immutable PNG plus JSON
with the exact prompt, model, size, quality, input images, token usage, cost estimate and
C2PA, under `~/.local/share/gothic-ai/content-sources/<namespace>/<stem>-attempt-N.png`.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys

import remaster

# USD per 1M tokens (developers.openai.com/api/docs/pricing, 2026-10-21): text in, image in, image out.
PRICES = {
    'gpt-image-2.5-sunburst': (5.0, 8.0, 30.0),
    'gpt-image-2.5-flare': (5.0, 8.0, 30.0),
    'gpt-image-2': (5.0, 8.0, 30.0),
    'gpt-image-1.5': (5.0, 8.0, 32.0),
}


def api_key() -> str:
    for name in ('OPEN_AI', 'OPENAI_API_KEY'):
        if os.environ.get(name):
            return os.environ[name].strip()
    for env in (remaster.ROOT / '.env', remaster.ROOT.parents[2] / '.env'):
        if env.is_file():
            for line in env.read_text().splitlines():
                if line.startswith(('OPEN_AI=', 'OPENAI_API_KEY=')):
                    return line.split('=', 1)[1].strip().strip('"').strip("'")
    raise ValueError('no OpenAI API key: set OPEN_AI or OPENAI_API_KEY, or add it to .env')


def cost(model: str, usage: dict | None) -> float | None:
    if not usage or model not in PRICES:
        return None
    text_in, image_in, image_out = PRICES[model]
    details = usage.get('input_tokens_details') or {}
    text = details.get('text_tokens', usage.get('input_tokens', 0) - details.get('image_tokens', 0))
    image = details.get('image_tokens', 0)
    out = usage.get('output_tokens', 0)
    return round((text * text_in + image * image_in + out * image_out) / 1e6, 4)


def request(args) -> tuple[bytes, dict]:
    import httpx
    headers = {'Authorization': f'Bearer {api_key()}'}
    prompt = Path(args.prompt_file).read_text().strip()
    fields = {'model': args.model, 'prompt': prompt, 'size': args.size, 'quality': args.quality, 'n': '1'}
    with httpx.Client(timeout=600) as http:
        if args.image:
            files = [('image[]', (Path(p).name, Path(p).read_bytes(), 'image/png')) for p in args.image]
            response = http.post('https://api.openai.com/v1/images/edits', headers=headers, data=fields, files=files)
        else:
            body = dict(fields, n=1)
            response = http.post('https://api.openai.com/v1/images/generations', headers=headers, json=body)
    if response.status_code != 200:
        raise ValueError(f'OpenAI images API {response.status_code}: {response.text[:400]}')
    payload = response.json()
    return base64.b64decode(payload['data'][0]['b64_json']), dict(usage=payload.get('usage'), prompt=prompt,
        revised_prompt=payload['data'][0].get('revised_prompt'))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--namespace', required=True, help='folder under content-sources')
    parser.add_argument('--stem', required=True, help='e.g. concept or NW_CITY_HAFEN_GIEBEL_01')
    parser.add_argument('--texture', help='texture name when the image is a texture source')
    parser.add_argument('--model', required=True)
    parser.add_argument('--prompt-file', required=True)
    parser.add_argument('--image', action='append', help='input image for an edit (repeatable); never an original Gothic texture or screenshot')
    parser.add_argument('--size', default='1024x1024')
    parser.add_argument('--quality', default='high', choices=('low', 'medium', 'high', 'auto'))
    parser.add_argument('--reason')
    args = parser.parse_args(argv)
    try:
        folder = remaster.sources_root() / args.namespace
        folder.mkdir(parents=True, exist_ok=True)
        numbers = [int(m['n']) for p in folder.glob(f'{args.stem}-attempt-*.png')
                   if (m := remaster.ATTEMPT.fullmatch(p.stem))]
        attempt = max(numbers, default=0) + 1
        start = datetime.now(timezone.utc)
        raw, info = request(args)
        end = datetime.now(timezone.utc)
        target = folder / f'{args.stem}-attempt-{attempt}.png'
        target.write_bytes(raw)
        meta = dict(texture=args.texture, role='texture' if args.texture else 'concept', attempt=attempt,
            prompt=info['prompt'], utc=start.strftime('%Y-%m-%dT%H:%M:%SZ'), utc_end=end.strftime('%Y-%m-%dT%H:%M:%SZ'),
            seconds=round((end - start).total_seconds()),
            generator=f'OpenAI Images API, model {args.model} (explicit), quality {args.quality}, size {args.size}',
            model=args.model, size=args.size, quality=args.quality,
            inputs=[dict(file=str(p), sha256=remaster.sha256(Path(p))) for p in (args.image or [])],
            usage=info['usage'], cost_usd=cost(args.model, info['usage']), revised_prompt=info['revised_prompt'],
            reason=args.reason, c2pa=remaster.c2pa_agent(target), source_sha256=remaster.sha256(target))
        target.with_suffix('.json').write_text(json.dumps(meta, ensure_ascii=False, indent=2) + '\n')
        for path in (target, target.with_suffix('.json')):
            path.chmod(0o444)
        print(json.dumps(dict(file=str(target), model=args.model, seconds=meta['seconds'], cost_usd=meta['cost_usd'],
                              c2pa=meta['c2pa'], usage=meta['usage']), ensure_ascii=False))
    except (OSError, ValueError, KeyError) as error:
        print(f'image_api: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

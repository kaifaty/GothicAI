#!/usr/bin/env bash
# Starts the quick model of fights. Listens on GEMMA_HOST only (a VPN address, set in
# ~/gothic-ai/gothic-ai.env by scripts/deploy-gpu.sh from GOTHIC_QUICK_LLM_URL), like the other services.
# - the alias is the model name the AI server asks for (gothic_ai.llm.QUICK_MODEL);
# - three slots of 16384 tokens (-c is split between them): up to three npc's around the hero stay warmed at
#   once (2026-10-23, +0.8 GB); the rest are kept in RAM (--cache-ram, 8 GB by default);
# - a fourth slot holds only the surrender classifier's prompt (gothic_ai.dialogue.CLASSIFIER_SLOT), and idle
#   slots are not copied to RAM on every new task (--no-cache-idle-slots; a slot is still saved when its prompt
#   is replaced): together the probe took 73-87 ms instead of 0.46-0.64 s, up to 2 s in a fight (2026-10-28);
# - 16384 tokens of context per slot: a line's prompt is 2-5k, a long talk's grows past 6k (6144 overflowed, 2026-10-22);
#   only the global layers' KV cache grows with it, q8_0 keeps the GPU's use at ~8.3 GB with three slots;
# - no thinking: the template's <|think|> is left out and the reasoning budget is 0.
set -euo pipefail
cd "$(dirname "$0")"
exec llama.cpp/build/bin/llama-server -m models/gemma-4-12b-it-qat-q4_0.gguf --alias gemma-4-12b \
  --host "${GEMMA_HOST:-127.0.0.1}" --port "${GEMMA_PORT:-47834}" \
  -ngl 99 -c 65536 -np 4 -fa on -ctk q8_0 -ctv q8_0 --no-cache-idle-slots \
  --jinja --reasoning-budget 0 --chat-template-kwargs '{"enable_thinking":false}' "$@"

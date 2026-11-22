"""Kimodo's text encoder server on local copies of LLM2Vec and its Llama 3 base.

    ~/gothic-ai/kimodo/.venv/bin/python kimodo_textencoder_local.py   # run_kimodo.sh starts it

Kimodo's preset names the Hugging Face repos: the LLM2Vec MNTP adapter, whose base is the gated
meta-llama/Meta-Llama-3-8B-Instruct, and the supervised adapter. Here they come from
$KIMODO_MODELS (default ~/gothic-ai/kimodo/models): Meta-Llama-3-8B-Instruct/ (an ungated copy whose
safetensors have the same SHA-256 as Meta's), llm2vec-mntp/ and llm2vec-supervised/. The MNTP
adapter is pointed at the local base. Its config keeps the name meta-llama/Meta-Llama-3-8B-Instruct:
LLM2Vec wraps the prompt in the Llama 3 chat template only under that name, as in Kimodo's training.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

from kimodo.scripts import run_text_encoder_server as server

LLAMA3 = "meta-llama/Meta-Llama-3-8B-Instruct"


def main():
    models = Path(os.environ.get("KIMODO_MODELS", Path.home() / "gothic-ai/kimodo/models"))
    base, mntp, supervised = models / "Meta-Llama-3-8B-Instruct", models / "llm2vec-mntp", models / "llm2vec-supervised"
    for d in (base, mntp, supervised):
        if not d.is_dir():
            sys.exit(f"missing {d}")
    name = json.loads((mntp / "config.json").read_text()).get("_name_or_path")
    if name != LLAMA3:
        sys.exit(f"{mntp}/config.json names {name!r}, LLM2Vec needs {LLAMA3!r} for the Llama 3 prompt template")
    adapter = mntp / "adapter_config.json"
    config = json.loads(adapter.read_text())
    if config.get("base_model_name_or_path") != str(base):
        config["base_model_name_or_path"] = str(base)
        adapter.write_text(json.dumps(config, indent=2))
    kwargs = server.TEXT_ENCODER_PRESETS["llm2vec"]["kwargs"]
    kwargs["base_model_name_or_path"] = str(mntp)
    kwargs["peft_model_name_or_path"] = str(supervised)
    server.main()


if __name__ == "__main__":
    main()

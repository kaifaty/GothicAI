"""Model loading and retrieval prefixes, matching scripts/recall_eval.py."""

import numpy as np

PREFIXES = {
    "intfloat/multilingual-e5-large-instruct": (
        "Instruct: Given a question, retrieve conversation lines that answer it\nQuery: ", ""
    ),
    "intfloat/multilingual-e5": ("query: ", "passage: "),
    "ai-forever/FRIDA": ("search_query: ", "search_document: "),
    "deepvk/USER2": ("search_query: ", "search_document: "),
}


def validate_settings(model: str, dtype: str) -> None:
    if model.startswith("ai-forever/FRIDA") and dtype == "float16":
        raise ValueError("FRIDA does not support float16 reliably; use bfloat16 or float32")
    if dtype not in ("bfloat16", "float32", "float16"):
        raise ValueError("EMBED_DTYPE must be bfloat16, float32 or float16")


def load_encoder(model: str, dtype: str, device: str):
    # Imported only on the GPU host; HTTP tests do not need torch or model weights.
    import torch
    from sentence_transformers import SentenceTransformer

    return SentenceTransformer(model, device=device, model_kwargs={"torch_dtype": getattr(torch, dtype)})


def encode(encoder, model: str, texts: list[str], kind: str, dim: int) -> np.ndarray:
    prefixes = next((value for name, value in PREFIXES.items() if model.startswith(name)), None)
    if prefixes is None and "query" in (encoder.prompts or {}):
        method = encoder.encode_query if kind == "query" else encoder.encode_document
        vectors = method(texts, normalize_embeddings=True)
    else:
        prefix = (prefixes or ("", ""))[0 if kind == "query" else 1]
        vectors = encoder.encode([prefix + text for text in texts], normalize_embeddings=True)
    vectors = np.asarray(vectors, dtype=np.float32)
    if vectors.shape != (len(texts), dim):
        raise ValueError(f"Encoder returned shape {vectors.shape}, expected {(len(texts), dim)}")
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    if not np.isfinite(vectors).all() or not np.isfinite(norms).all() or np.any(norms == 0):
        raise ValueError("Encoder returned non-finite or zero vectors")
    # Normalize in float32 even if model inference uses bf16; the wire format is always little-endian.
    return np.asarray(vectors / norms, dtype="<f4", order="C")

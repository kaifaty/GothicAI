"""Pinned downloads for setup; this module never imports torch or uses the GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

from huggingface_hub import HfApi, hf_hub_download, snapshot_download

REVISIONS = {
    "trellis2": ("microsoft/TRELLIS.2-4B", "af44b45f2e35a493886929c6d786e563ec68364d"),
    "trellis-v1": ("microsoft/TRELLIS-image-large", "25e0d31ffbebe4b5a97464dd851910efc3002d96"),
    "dinov3": ("facebook/dinov3-vitl16-pretrain-lvd1689m", "ea8dc2863c51be0a264bab82070e3e8836b02d51"),
}
DINO2_URL = "https://dl.fbaipublicfiles.com/dinov2/dinov2_vitl14/dinov2_vitl14_reg4_pretrain.pth"
DINO2_SHA256 = "36e4deffbaef061a2576705b0c36f93621e2ae20bf6274694821b0b492551b51"


def download_public(root):
    # Download only the decoders/flows used by mesh inference, never GS/RF/rembg.
    for key in ("trellis-v1", "trellis2"):
        repo, revision = REVISIONS[key]
        config_path = hf_hub_download(repo, "pipeline.json", revision=revision, token=False,
            cache_dir=root / "hf/hub")
        config = json.loads(Path(config_path).read_text())
        models = config["args"]["models"]
        if key == "trellis-v1":
            models.pop("slat_decoder_gs")
            models.pop("slat_decoder_rf")
        allowed = ["README.md"]
        for checkpoint in models.values():
            if checkpoint.startswith("ckpts/"):
                allowed.extend([checkpoint + ".json", checkpoint + ".safetensors"])
        snapshot_download(repo, revision=revision, token=False,
            local_dir=root / "models" / key, allow_patterns=allowed, max_workers=2)
        # Restore the filtered object after snapshot_download writes upstream config.
        config["args"].pop("rembg_model", None)
        for name, checkpoint in models.items():
            if checkpoint.startswith("microsoft/TRELLIS-image-large/"):
                checkpoint = checkpoint.removeprefix("microsoft/TRELLIS-image-large/")
                models[name] = str(root / "models/trellis-v1" / checkpoint)
            else:
                models[name] = str(root / "models" / key / checkpoint)
        if key == "trellis2":
            config["args"]["image_cond_model"]["args"]["model_name"] = str(root / "models/dinov3")
            config["args"]["low_vram"] = True
        (root / "models" / key / "pipeline.json").write_text(json.dumps(config, indent=2) + "\n")
        print(f"Downloaded {repo}@{revision}", flush=True)
    target = root / "models/dinov2/dinov2_vitl14_reg4_pretrain.pth"
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        temporary = target.with_suffix(".part")
        urllib.request.urlretrieve(DINO2_URL, temporary)
        temporary.replace(target)
    digest = hashlib.sha256()
    with target.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != DINO2_SHA256:
        raise ValueError("DINOv2 checkpoint SHA256 does not match the pinned weights")
    print(f"DINOv2 SHA256 {digest.hexdigest()}", flush=True)


def dino_token():
    token = (Path.home() / "gothic-ai/materials/hf/token").read_text().strip()
    if not token or any(character.isspace() for character in token):
        raise ValueError("DINOv3 credential file is empty or malformed")
    return token


def download_dinov3(root):
    # The credential is read only for these explicitly authenticated calls.
    token = dino_token()
    print(HfApi().whoami(token=token)["name"], flush=True)
    repo, revision = REVISIONS["dinov3"]
    snapshot_download(repo, revision=revision, token=token,
        local_dir=root / "models/dinov3",
        allow_patterns=["config.json", "model.safetensors", "preprocessor_config.json", "LICENSE.md"],
        max_workers=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["public", "dinov3"])
    parser.add_argument("--root", type=Path, default=Path.home() / "gothic-ai/mesh3d")
    args = parser.parse_args()
    {"public": download_public, "dinov3": download_dinov3}[args.command](args.root.resolve())

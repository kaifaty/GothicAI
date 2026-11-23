# Runs LAM Audio2Expression once over every clip: load time, per-clip time, CSV in the a2f-weights layout.
import glob, json, os, sys, time
import numpy as np, torch
from engines.defaults import default_config_parser, default_setup
from engines.infer import INFER
from models.utils import ARKitBlendShape

A2F = ("eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
       "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight "
       "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft "
       "mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft "
       "mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft "
       "mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft "
       "browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight "
       "noseSneerLeft noseSneerRight tongueOut").split()
clips, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
t0 = time.time()
cfg = default_config_parser("configs/lam_audio2exp_config_streaming.py",
                            {"save_path": out + "/exp", "weight": "pretrained_models/lam_audio2exp_streaming.tar", "ex_vol": False})
cfg = default_setup(cfg)
infer = INFER.build(dict(type=cfg.infer.type, cfg=cfg))
torch.cuda.synchronize()
print("load %.2f s" % (time.time() - t0), flush=True)
for wav in sorted(glob.glob(clips + "/*.wav")):
    n = os.path.basename(wav)[:-4]
    cfg.audio_input = wav
    cfg.save_json_path = "%s/%s.json" % (out, n)
    t = time.time(); infer.infer(); torch.cuda.synchronize(); dt = time.time() - t
    d = json.load(open(cfg.save_json_path))
    names = d.get("names") or ARKitBlendShape
    frames = d["frames"]
    rows = []
    for i, f in enumerate(frames):
        w = dict(zip(names, f["weights"])) if isinstance(f, dict) else dict(zip(names, f))
        rows.append([round(i * 16000 / 30)] + [w.get(k, 0.0) for k in A2F])
    np.savetxt("%s/%s.lam.csv" % (out, n), np.array(rows), delimiter=",", fmt="%.4f")
    print("%s: %.2f s for %d frames, peak alloc %.2f GB" % (n, dt, len(rows), torch.cuda.max_memory_allocated() / 1e9), flush=True)

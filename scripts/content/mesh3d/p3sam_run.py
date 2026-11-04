"""P3-SAM auto masks on every mesh of IN, with a small prompt batch (8 GB of the 3090 is free beside the services).

On dev-ubuntu, in ~/gothic-ai/parts (p3sam-venv, Hunyuan3D-Part clone; parts-pilot.md): python p3sam_run.py IN OUT 4
Writes OUT/<mesh>/parts.npz (face_ids, the cleaned vertices and faces) for parts_pilot.py score."""
import os, sys, time, torch, trimesh
demo = os.path.expanduser("~/gothic-ai/parts/Hunyuan3D-Part/P3-SAM/demo")
sys.path.insert(0, demo); os.chdir(demo)
import auto_mask as am
src, dst, bs = sys.argv[1], sys.argv[2], int(sys.argv[3])
model = am.AutoMask(os.path.expanduser("~/gothic-ai/parts/weights/p3sam/p3sam.safetensors"))
for name in sorted(os.listdir(src)):
    out = os.path.join(dst, name[:-4]); os.makedirs(out, exist_ok=True)
    mesh = trimesh.load(os.path.join(src, name), force="mesh")
    am.set_seed(42); torch.cuda.reset_peak_memory_stats(); t = time.time()
    aabb, face_ids, cleaned = model.predict_aabb(mesh, save_path=out, point_num=100000, prompt_num=400, threshold=0.95, post_process=0,
                       save_mid_res=0, show_info=0, seed=42, is_parallel=0, clean_mesh_flag=1, prompt_bs=bs)
    import numpy as np
    np.savez(os.path.join(out, "parts.npz"), face_ids=np.asarray(face_ids), vertices=np.asarray(cleaned.vertices), faces=np.asarray(cleaned.faces))
    print("DONE", name, round(time.time() - t, 1), "s, peak", round(torch.cuda.max_memory_allocated() / 2**30, 2), "GiB", flush=True)

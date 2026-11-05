# LAM on CPU: patch .cuda() away and time the six clips with 4 threads.
import sys, time, torch
torch.set_num_threads(4)
torch.Tensor.cuda = lambda self, *a, **k: self
torch.nn.Module.cuda = lambda self, *a, **k: self
torch.cuda.synchronize = lambda *a, **k: None
torch.cuda.max_memory_allocated = lambda *a, **k: 0
_load = torch.load
torch.load = lambda *a, **k: _load(*a, **{**k, "map_location": "cpu"})
sys.argv = ["lam_run_clips.py"] + sys.argv[1:]
exec(open("lam_run_clips.py").read())

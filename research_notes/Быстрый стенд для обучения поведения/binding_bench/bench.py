import ctypes, timeit, statistics, sys, numpy as np, os
sys.path.insert(0, os.getcwd())
import b_pb, b_nb
from cffi import FFI
lib = ctypes.CDLL('./libcore.dylib')
lib.add1.argtypes=[ctypes.c_int]; lib.add1.restype=ctypes.c_int
lib.fill.argtypes=[ctypes.c_void_p, ctypes.c_int]; lib.fill.restype=None
ffi = FFI(); ffi.cdef("int add1(int); void fill(float*,int);")
C = ffi.dlopen('./libcore.dylib')
# cffi API mode
ffi2 = FFI(); ffi2.cdef("int add1(int); void fill(float*,int);")
ffi2.set_source('_cffi_api', 'int add1(int); void fill(float*,int);', extra_objects=['core.o'])
ffi2.compile(tmpdir='.', verbose=False)
import _cffi_api
A = _cffi_api.lib
def pyadd1(x): return x+1
N=1000
buf = np.zeros(N, dtype=np.float32)
bufp = buf.ctypes.data
bufc = ffi.cast("float*", bufp)
def t(fn, number=500000, repeat=7):
    r=[min(timeit.repeat(fn, number=number, repeat=1)) / number * 1e9 for _ in range(repeat)]
    return statistics.median(r)
print("ns per call (median of 7), Apple M1 Pro, Python", sys.version.split()[0])
rows = [
 ("python function add1", lambda: pyadd1(1)),
 ("ctypes add1(int)", lambda: lib.add1(1)),
 ("cffi ABI add1(int)", lambda: C.add1(1)),
 ("cffi API add1(int)", lambda: A.add1(1)),
 ("pybind11 add1(int)", lambda: b_pb.add1(1)),
 ("nanobind add1(int)", lambda: b_nb.add1(1)),
]
for n,f in rows: print(f"{n:34s} {t(f):8.1f}")
print("--- passing a numpy array (1 element fill work trivial: n=1)")
small = np.zeros(1, dtype=np.float32)
rows2 = [
 ("ctypes fill(ptr from .ctypes.data) n=1", lambda: lib.fill(small.ctypes.data, 1)),
 ("ctypes fill(cached ptr int) n=1", lambda: lib.fill(small.ctypes.data, 1)),
 ("cffi API fill(cast ptr) n=1", lambda: A.fill(ffi.cast('float*', small.ctypes.data), 1)),
 ("pybind11 fill(ndarray) n=1", lambda: b_pb.fill(small)),
 ("nanobind fill(ndarray) n=1", lambda: b_nb.fill(small)),
 ("pybind11 fill_ptr(int) n=1", lambda: b_pb.fill_ptr(small.ctypes.data, 1)),
 ("nanobind fill_ptr(int) n=1", lambda: b_nb.fill_ptr(small.ctypes.data, 1)),
]
sp = small.ctypes.data
rows2[1] = ("ctypes fill(cached ptr int) n=1", lambda: lib.fill(sp, 1))
for n,f in rows2: print(f"{n:42s} {t(f, 200000):8.1f}")
print("--- one call doing work for N=1000 floats (per call, then per element)")
for n,f in [("ctypes fill cached ptr N=1000", lambda: lib.fill(bufp, N)),("nanobind fill ndarray N=1000", lambda: b_nb.fill(buf)),("pybind11 fill ndarray N=1000", lambda: b_pb.fill(buf))]:
    v=t(f,200000); print(f"{n:42s} {v:8.1f} ns/call  {v/N:6.3f} ns/elem")
print("--- numpy view over C buffer creation cost")
import ctypes as ct
arr_t = (ct.c_float*N)
cbuf = arr_t()
print("np.frombuffer(ctypes array)       %.1f" % t(lambda: np.frombuffer(cbuf, dtype=np.float32), 200000))
print("np.ctypeslib.as_array(ptr,shape)  %.1f" % t(lambda: np.ctypeslib.as_array(ct.cast(bufp, ct.POINTER(ct.c_float)), shape=(N,)), 50000))
print("buf[:] view slice                 %.1f" % t(lambda: buf[10:20], 500000))

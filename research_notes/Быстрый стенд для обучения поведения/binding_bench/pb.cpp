#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
namespace py=pybind11;
extern "C" { int add1(int); void fill(float*,int); }
PYBIND11_MODULE(b_pb, m){
  m.def("add1", [](int x){ return add1(x); });
  m.def("fill", [](py::array_t<float, py::array::c_style> a){ fill(a.mutable_data(), (int)a.size()); });
  m.def("fill_ptr", [](uintptr_t p, int n){ fill((float*)p, n); });
}

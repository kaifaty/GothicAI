#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
namespace nb=nanobind;
extern "C" { int add1(int); void fill(float*,int); }
NB_MODULE(b_nb, m){
  m.def("add1", [](int x){ return add1(x); });
  m.def("fill", [](nb::ndarray<float, nb::c_contig> a){ fill(a.data(), (int)a.size()); });
  m.def("fill_ptr", [](uintptr_t p, int n){ fill((float*)p, n); });
}

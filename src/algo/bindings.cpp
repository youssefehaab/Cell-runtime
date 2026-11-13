#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "algo/aabb.hpp"
#include "algo/pallet_packer.hpp"

namespace py = pybind11;
using namespace cell::algo;

PYBIND11_MODULE(cell_algo, m) {
  m.doc() = "Palletising algorithms from cell-runtime: greedy pallet packing and AABB checks";

  py::class_<Aabb>(m, "Aabb")
      .def(py::init([](double min_x, double min_y, double min_z, double max_x, double max_y,
                       double max_z) { return Aabb{min_x, min_y, min_z, max_x, max_y, max_z}; }),
           py::arg("min_x"), py::arg("min_y"), py::arg("min_z"), py::arg("max_x"), py::arg("max_y"),
           py::arg("max_z"))
      .def_readwrite("min_x", &Aabb::min_x)
      .def_readwrite("min_y", &Aabb::min_y)
      .def_readwrite("min_z", &Aabb::min_z)
      .def_readwrite("max_x", &Aabb::max_x)
      .def_readwrite("max_y", &Aabb::max_y)
      .def_readwrite("max_z", &Aabb::max_z)
      .def("volume", &Aabb::volume);
  m.def("intersects", &intersects, "True if the two boxes overlap (touching is not overlap)");
  m.def("contains", &contains, py::arg("outer"), py::arg("inner"));

  py::class_<Box>(m, "Box")
      .def(py::init([](int id, double length, double width, double height, double weight,
                       double max_load, bool can_rotate) {
             return Box{id, length, width, height, weight, max_load, can_rotate};
           }),
           py::arg("id"), py::arg("length"), py::arg("width"), py::arg("height"), py::arg("weight"),
           py::arg("max_load"), py::arg("can_rotate") = true)
      .def_readwrite("id", &Box::id)
      .def_readwrite("length", &Box::length)
      .def_readwrite("width", &Box::width)
      .def_readwrite("height", &Box::height)
      .def_readwrite("weight", &Box::weight)
      .def_readwrite("max_load", &Box::max_load)
      .def_readwrite("can_rotate", &Box::can_rotate);

  py::class_<Pallet>(m, "Pallet")
      .def(py::init([](double length, double width, double max_height, double max_weight) {
             return Pallet{length, width, max_height, max_weight};
           }),
           py::arg("length") = 1.2, py::arg("width") = 0.8, py::arg("max_height") = 1.5,
           py::arg("max_weight") = 1000.0)
      .def_readwrite("length", &Pallet::length)
      .def_readwrite("width", &Pallet::width)
      .def_readwrite("max_height", &Pallet::max_height)
      .def_readwrite("max_weight", &Pallet::max_weight);

  py::class_<PackOptions>(m, "PackOptions")
      .def(py::init([](double min_support) { return PackOptions{min_support}; }),
           py::arg("min_support") = 0.75)
      .def_readwrite("min_support", &PackOptions::min_support);

  py::class_<Placement>(m, "Placement")
      .def_readonly("id", &Placement::id)
      .def_readonly("x", &Placement::x)
      .def_readonly("y", &Placement::y)
      .def_readonly("z", &Placement::z)
      .def_readonly("length", &Placement::length)
      .def_readonly("width", &Placement::width)
      .def_readonly("height", &Placement::height)
      .def_readonly("weight", &Placement::weight)
      .def_readonly("rotated", &Placement::rotated)
      .def("bounds", &Placement::bounds);

  py::class_<PackResult>(m, "PackResult")
      .def_readonly("placed", &PackResult::placed)
      .def_readonly("unplaced", &PackResult::unplaced)
      .def_readonly("fill_ratio", &PackResult::fill_ratio)
      .def_readonly("total_weight", &PackResult::total_weight)
      .def_readonly("load_height", &PackResult::load_height);

  m.def("pack", &pack, py::arg("pallet"), py::arg("boxes"), py::arg("options") = PackOptions{},
        "Greedy pallet packing; returns a PackResult");
  m.def("validate", &validate, py::arg("pallet"), py::arg("boxes"), py::arg("result"),
        py::arg("options") = PackOptions{},
        "Empty string if the result satisfies every constraint, else the first violation");
}

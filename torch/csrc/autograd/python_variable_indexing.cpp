#include "torch/csrc/autograd/python_variable_indexing.h"

#include "torch/csrc/DynamicTypes.h"
#include "torch/csrc/Exceptions.h"
#include "torch/csrc/THP_export.h"
#include "torch/csrc/autograd/python_variable.h"
#include "torch/csrc/autograd/utils/wrap_outputs.h"
#include "torch/csrc/utils/python_compat.h"
#include "torch/csrc/utils/python_numbers.h"

#include <vector>

using namespace at;
using namespace torch::autograd::utils;

THP_API PyObject* THPLongTensorClass;
#ifdef WITH_CUDA
THP_API PyObject* THCPLongTensorClass;
#endif

extern bool THPModule_isTensor(PyObject *obj);

namespace torch { namespace autograd {

Py_ssize_t THPVariable_length(PyObject* self) {
  HANDLE_TH_ERRORS
  auto& self_ = reinterpret_cast<THPVariable*>(self)->cdata;
  if (self_.dim() == 0) {
    return 0;
  }
  return (Py_ssize_t)self_.size(0);
  END_HANDLE_TH_ERRORS_RET(-1)
}


// We allow indexing by integers, slices, ellipsis, None, Variables,
// and tuples of those types. We also handle bools as if they were a
// Variable[ByteTensor].

static int64_t count_specified_dimensions(PyObject* index) {
  // Count the number of indexed dimensions (everything but ellipsis and None)
  int64_t count = 0;
  auto size = PyTuple_GET_SIZE(index);
  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject* obj = PyTuple_GET_ITEM(index, i);
    if (THPVariable_Check(obj)) {
      auto& var = reinterpret_cast<THPVariable*>(obj)->cdata;
      if (var.type().scalarType() == kByte) {
        count += var.dim();
      }
    } else if (obj != Py_None && obj != Py_Ellipsis) {
      count++;
    }
  }
  return count;
}

[[noreturn]]
static void invalid_index(PyObject* obj) {
  throw IndexError(
    "only integers, slices (`:`), ellipsis (`...`), None and long or byte "
    "Variables are valid indices (got %s)", Py_TYPE(obj)->tp_name);
}

static Variable applySlice(const Variable& self, int64_t dim, PyObject* slice, bool ensure_view=false) {
  Py_ssize_t start, stop, step, slicelength;
  auto length = self.size(dim);
  if (THPUtils_parseSlice(slice, length, &start, &stop, &step, &slicelength) < 0) {
    throw python_error();
  }
  if (step == 0) {
    throw IndexError("step cannot be zero");
  }
  if (step < 0) {
    // TODO: implement negative step
    throw IndexError("negative step not yet supported");
  }
  if (!ensure_view && start == 0 && stop == length && step == 1) {
    return self;
  }
  return self.slice(start, stop, step, dim);
}

static Variable applySelect(const Variable& self, int64_t dim, int64_t index) {
  int64_t size = self.size(dim);
  if (index < -size || index >= size) {
    throw IndexError("index %lld is out of bounds for dimension %lld with size %lld",
      index, dim, size);
  }
  if (index < 0) {
    index += size;
  }
  return self.select(dim, index);
}

static Variable sequenceToVariable(const Type& type, PyObject* seq) {
  PyObject* ctor = THPLongTensorClass;
#ifdef WITH_CUDA
  if (type.is_cuda()) ctor = THCPLongTensorClass;
#endif
  auto obj = THPObjectPtr(PyObject_CallFunctionObjArgs(ctor, seq, NULL));
  if (!obj) throw python_error();
  return make_variable(createTensor(obj.get()));
}

static Variable valueToTensor(const Type & type, PyObject* value) {
  if (THPVariable_Check(value)) {
    return reinterpret_cast<THPVariable*>(value)->cdata;
  }
  if (THPUtils_checkLong(value)) {
    return type.scalarTensor(Scalar(THPUtils_unpackLong(value)));
  }
  if (PyFloat_Check(value)) {
    return type.scalarTensor(Scalar(THPUtils_unpackDouble(value)));
  }
  throw TypeError("can't assign a %s to a %s", Py_TYPE(value)->tp_name, type.toString());
}

static Variable applySlicing(const Variable& self, PyObject* index, variable_list& outIndices) {
  int64_t size = PyTuple_GET_SIZE(index);
  int64_t dim = 0;

  auto handle_var = [&](const Variable& var) {
    // TODO: check scalarType
    outIndices.resize(dim + 1);
    outIndices[dim] = var;
    dim++;
  };

  Variable result = self;
  for (int64_t i = 0; i < size; i++) {
    PyObject* obj = PyTuple_GET_ITEM(index, i);
    if (THPUtils_checkLong(obj)) {
      result = applySelect(result, dim, THPUtils_unpackLong(obj));
    } else if (PySlice_Check(obj)) {
      result = applySlice(result, dim, obj);
      dim++;
    } else if (obj == Py_Ellipsis) {
      dim += self.dim() - count_specified_dimensions(index);
    } else if (obj == Py_None) {
      result = result.unsqueeze(dim);
      dim++;
    } else if (THPVariable_Check(obj)) {
      handle_var(reinterpret_cast<THPVariable*>(obj)->cdata);
    } else if (THPModule_isTensor(obj)) {
      handle_var(make_variable(createTensor(obj)));
    } else if (PySequence_Check(obj)) {
      handle_var(sequenceToVariable(self.type(), obj));
    } else {
      invalid_index(obj);
    }
  }
  return result;
}

static std::vector<Tensor> asTensorList(const variable_list& v) {
  return std::vector<Tensor>(v.begin(), v.end());
}

static Variable dispatch_index(const Variable& self, const variable_list& indices) {
  AutoNoGIL no_gil;
  AutoGPU auto_gpu(self);
  return self.index(asTensorList(indices));
}

static Variable dispatch_index_put_(Variable& self, const variable_list& indices, const Variable& value) {
  AutoNoGIL no_gil;
  AutoGPU auto_gpu(self);
  return self.index_put_(asTensorList(indices), value);
}

static bool treatSequenceAsTuple(PyObject* index) {
  if (PyTuple_Check(index)) {
    return true;
  }
  if (!PySequence_Check(index)) {
    return false;
  }
  // This uses a heuristics from NumPy for determining whether to treat
  // non-tuple sequences as if they were a tuple. From the NumPy code comments:
  //
  // "At this point, we're left with a non-tuple, non-array, sequence:
  //  typically, a list. We use some somewhat-arbitrary heuristics from here
  //  onwards to decided whether to treat that list as a single index, or a
  //  list of indices. Backwards compatibility only takes effect for short
  //  sequences - otherwise we treat it like any other scalar."
  auto n = PySequence_Size(index);
  if (n < 0) {
    // Negative size indicates a Python error in the PySequence_Size call.
    PyErr_Clear();
    return false;
  }
  if (n >= 32) {
    return false;
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    auto obj = THPObjectPtr{PySequence_GetItem(index, i)};
    if (!obj.get()) {
      PyErr_Clear();
      return false;
    }
    if (THPVariable_Check(obj.get()) || PySequence_Check(obj.get()) || PySlice_Check(obj.get())) {
      return true;
    }
    if (obj.get() == Py_Ellipsis || obj.get() == Py_None) {
      return true;
    }
  }
  return false;
}

static THPObjectPtr wrapTuple(PyObject* index) {
  THPObjectPtr res;
  if (treatSequenceAsTuple(index)) {
    res = PySequence_Tuple(index);
  } else {
    res = PyTuple_Pack(1, index);
  }
  if (!res) throw python_error();
  return res;
}

PyObject* THPVariable_getitem(PyObject* self, PyObject* index) {
  HANDLE_TH_ERRORS
  auto& self_ = reinterpret_cast<THPVariable*>(self)->cdata;

  // handle simple types: integers, slices, ellipsis
  if (index == Py_None) {
    return wrap(self_.unsqueeze(0));
  } else if (index == Py_Ellipsis) {
    return wrap(at::alias(self_));
  } else if (THPUtils_checkLong(index)) {
    return wrap(applySelect(self_, 0, THPUtils_unpackLong(index)));
  } else if (PyBool_Check(index)) {
    if (index == Py_True) {
      return wrap(self_.unsqueeze(0));
    } else {
      return wrap(self_.type().tensor({0}));
    }
  } else if (PySlice_Check(index)) {
    return wrap(applySlice(self_, 0, index, true));
  }

  // wrap index in a tuple if it's not already one
  THPObjectPtr holder = wrapTuple(index);

  variable_list variableIndices;
  Variable sliced = applySlicing(self_, holder.get(), variableIndices);
  if (variableIndices.empty()) {
    if (sliced.get() == self_.get()) {
      // ensure we return a shallow copy for things like x[...]
      sliced = at::alias(sliced);
    }
    return wrap(sliced);
  }

  // indexing by tensors ("advanced" indexing)
  return wrap(dispatch_index(sliced, variableIndices));
  Py_RETURN_NONE;
  END_HANDLE_TH_ERRORS
}

int THPVariable_setitem(PyObject* self, PyObject* index, PyObject* py_value) {
  HANDLE_TH_ERRORS
  auto& self_ = reinterpret_cast<THPVariable*>(self)->cdata;
  auto value = valueToTensor(self_.type(), py_value);

  // handle simple types: integers, slices, ellipsis, bool
  if (index == Py_False) {
    return 0;
  } else if (index == Py_None || index == Py_Ellipsis || index == Py_True) {
    self_.copy_(value);
    return 0;
  } else if (THPUtils_checkLong(index)) {
    applySelect(self_, 0, THPUtils_unpackLong(index)).copy_(value);
    return 0;
  } else if (PySlice_Check(index)) {
    applySlice(self_, 0, index).copy_(value);
    return 0;
  }

  // wrap index in a tuple if it's not already one
  THPObjectPtr holder = wrapTuple(index);

  variable_list variableIndices;
  Variable sliced = applySlicing(self_, holder.get(), variableIndices);
  if (variableIndices.empty()) {
    sliced.copy_(value);
    return 0;
  }

  // indexing by tensors ("advanced" indexing)
  dispatch_index_put_(sliced, variableIndices, value);
  return 0;
  END_HANDLE_TH_ERRORS_RET(-1)
}

}} // namespace torch::autograd

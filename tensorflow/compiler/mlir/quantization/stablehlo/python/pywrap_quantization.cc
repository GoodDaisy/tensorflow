/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <string>
#include <unordered_set>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "pybind11/cast.h"  // from @pybind11
#include "pybind11/detail/common.h"  // from @pybind11
#include "pybind11/pybind11.h"  // from @pybind11
#include "pybind11/pytypes.h"  // from @pybind11
#include "pybind11/stl.h"  // from @pybind11  // IWYU pragma: keep
#include "pybind11_abseil/absl_casters.h"  // from @pybind11_abseil   // IWYU pragma: keep
#include "pybind11_abseil/import_status_module.h"  // from @pybind11_abseil
#include "pybind11_abseil/status_casters.h"  // from @pybind11_abseil  // IWYU pragma: keep
#include "tensorflow/compiler/mlir/quantization/stablehlo/cc/calibration/assign_ids.h"
#include "tensorflow/compiler/mlir/quantization/stablehlo/cc/calibration/statistics.h"
#include "tensorflow/compiler/mlir/quantization/stablehlo/cc/debugger.h"
#include "tensorflow/compiler/mlir/quantization/stablehlo/cc/io.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/exported_model.pb.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/python/py_function_lib.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/python/quantize_model.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/python/type_casters.h"  // IWYU pragma: keep
#include "tensorflow/compiler/mlir/quantization/tensorflow/quantization_options.pb.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"

namespace py = pybind11;

namespace {

using ::stablehlo::quantization::AddCalibrationStatistics;
using ::stablehlo::quantization::AssignIdsToCustomAggregatorOps;
using ::stablehlo::quantization::EnableDebugging;
using ::stablehlo::quantization::io::CreateTmpDir;
using ::tensorflow::SignatureDef;
using ::tensorflow::quantization::ExportedModel;
using ::tensorflow::quantization::PyFunctionLibrary;
using ::tensorflow::quantization::QuantizationOptions;
using ::tensorflow::quantization::RepresentativeDatasetFile;

}  // namespace

PYBIND11_MODULE(pywrap_quantization, m) {
  // Supports absl::Status type conversions.
  pybind11::google::ImportStatusModule();

  m.doc() = "StableHLO Quantization APIs.";

  m.def(
      // If the function signature changes, likely its corresponding .pyi type
      // hinting should also change.
      // LINT.IfChange
      "static_range_ptq",
      [](const absl::string_view src_saved_model_path,
         const absl::string_view dst_saved_model_path,
         const QuantizationOptions& quantization_options,
         const std::vector<std::string>& signature_keys,
         const absl::flat_hash_map<std::string, SignatureDef>&
             signature_def_map,
         const absl::flat_hash_map<std::string, std::string>& function_aliases,
         const PyFunctionLibrary& py_function_library,
         const absl::flat_hash_map<std::string, RepresentativeDatasetFile>&
             representative_dataset_file_map_serialized) -> absl::Status {
        // LINT.ThenChange(pywrap_quantization.pyi:static_range_ptq)
        std::unordered_set<std::string> tags;
        tags.insert(quantization_options.tags().begin(),
                    quantization_options.tags().end());

        absl::StatusOr<ExportedModel> exported_model =
            QuantizePtqModelPreCalibration(src_saved_model_path, signature_keys,
                                           tags, quantization_options,
                                           function_aliases);
        if (!exported_model.ok()) return exported_model.status();

        AssignIdsToCustomAggregatorOps(*exported_model->mutable_graph_def());

        const absl::StatusOr<std::string> precalibrated_saved_model_dir =
            CreateTmpDir();
        if (!precalibrated_saved_model_dir.ok()) {
          throw py::value_error(absl::StrFormat(
              "Failed to create tmp dir for precalibrated saved model: %s",
              precalibrated_saved_model_dir.status().ToString()));
        }

        py_function_library.SaveExportedModel(
            *precalibrated_saved_model_dir, *exported_model,
            src_saved_model_path, tags, signature_def_map);

        py_function_library.RunCalibration(
            *precalibrated_saved_model_dir, signature_keys, tags,
            quantization_options.calibration_options(),
            quantization_options.force_graph_mode_calibration(),
            representative_dataset_file_map_serialized);

        if (absl::Status status = AddCalibrationStatistics(
                *exported_model->mutable_graph_def(),
                quantization_options.calibration_options(),
                py_function_library);
            !status.ok()) {
          LOG(WARNING) << "Some CustomAggregator ops do not have min or max "
                          "values. Parts of the graph are not quantized. "
                       << status;
        }

        if (quantization_options.has_debugger_options()) {
          EnableDebugging(*exported_model,
                          quantization_options.debugger_options(),
                          py_function_library, src_saved_model_path, tags,
                          signature_def_map);
        }

        const absl::StatusOr<std::string> calibrated_saved_model_path =
            CreateTmpDir();
        if (!calibrated_saved_model_path.ok()) {
          throw py::value_error(absl::StrFormat(
              "Failed to create tmp dir for calibrated saved model: %s",
              calibrated_saved_model_path.status().ToString()));
        }

        py_function_library.SaveExportedModel(
            *calibrated_saved_model_path, *exported_model, src_saved_model_path,
            tags, signature_def_map);

        const absl::flat_hash_map<std::string, std::string>
            function_aliases_after_calibration(
                exported_model->function_aliases().begin(),
                exported_model->function_aliases().end());

        const absl::StatusOr<ExportedModel> post_calibrated_exported_model =
            QuantizePtqModelPostCalibration(
                *calibrated_saved_model_path, signature_keys, tags,
                quantization_options, function_aliases_after_calibration);
        if (!post_calibrated_exported_model.ok()) {
          return post_calibrated_exported_model.status();
        }

        // Remove the `tpu` tag from the debug quantized saved model as it is
        // for CPU. Note the 'tpu' value should be the same as `TPU` defined in
        // tensorflow/python/saved_model/tag_constants.py.
        if (quantization_options.has_debugger_options()) {
          tags.erase("tpu");
        }
        py_function_library.SaveExportedModel(
            dst_saved_model_path, *post_calibrated_exported_model,
            *calibrated_saved_model_path, tags, signature_def_map);

        return absl::OkStatus();
      },
      R"pbdoc(
      Runs static-range post-training quantization (PTQ) on a SavedModel at
      `src_saved_model_path` and saves the resulting model to
      `dst_saved_model_path`.

      The user should pass a serialized `QuantizationOptions` for the
      `quantization_options_serialized` argument, and a signature key ->
      serialized `SignatureDef` mapping for the `signature_def_map_serialized`
      argument.

      `function_aliases` maps actual function names to the function aliases, as
      defined by the `MetaGraphDef::MetaInfoDef::function_aliases` from the
      input SavedModel.

      `representative_dataset_file_map_serialized` is a signature key ->
      `RepresentativeDatasetFile` (serialized) mapping for running the
      calibration step. Each dataset file stores the representative dataset for
      the function matching the signature key.

      Raises `StatusNotOk` exception if when the run was unsuccessful.
      )pbdoc",
      py::arg("saved_model_path"), py::arg("dst_saved_model_path"),
      py::arg("quantization_options_serialized"), py::kw_only(),
      py::arg("signature_keys"), py::arg("signature_def_map_serialized"),
      py::arg("function_aliases"), py::arg("py_function_library"),
      py::arg("representative_dataset_file_map_serialized"));
}

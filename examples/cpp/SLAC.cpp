// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------
#include "open3d/Open3D.h"
#include "open3d/t/pipelines/slac/ControlGrid.h"

using namespace open3d;
using namespace open3d::core;

void PrintHelp() {
    using namespace open3d;

    PrintOpen3DVersion();
    // clang-format off
    utility::LogInfo("Usage:");
    utility::LogInfo(">    SLAC [dataset_folder] [options]");
    utility::LogInfo("--method [default: rigid, optional: slac]");
    utility::LogInfo("--voxel_size [default: 0.05]");
    utility::LogInfo("--weight [default: 1]");
    utility::LogInfo("--distance_threshold [default: 0.07]");
    utility::LogInfo("--fitness_threshold [default: 0.3]");
    utility::LogInfo("--iterations [default: 5]");
    utility::LogInfo("--device [default: CPU:0]");
    utility::LogInfo("--debug");
    utility::LogInfo("--debug_node [default: 0]");
    // clang-format on
    utility::LogInfo("");
}

int main(int argc, char** argv) {
    if (argc == 1 || utility::ProgramOptionExists(argc, argv, "--help") ||
        argc < 2) {
        PrintHelp();
        return 1;
    }

    // Expect reconstruction system has finished running
    std::string dataset_folder = std::string(argv[1]);
    std::string fragment_folder = dataset_folder + "/fragments";
    std::string scene_folder = dataset_folder + "/scene";
    std::string slac_folder = dataset_folder + "/slac";

    std::vector<std::string> fragment_fnames;
    utility::filesystem::ListFilesInDirectoryWithExtension(
            fragment_folder, "ply", fragment_fnames);
    if (fragment_fnames.size() == 0) {
        utility::LogError(
                "No fragment found in {}, please make sure the "
                "reconstruction_system has "
                "finished running on the dataset.",
                fragment_folder);
    }
    std::sort(fragment_fnames.begin(), fragment_fnames.end());

    std::string pose_graph_fname =
            scene_folder + "/refined_registration_optimized.json";
    auto pose_graph = io::CreatePoseGraphFromFile(pose_graph_fname);
    if (pose_graph == nullptr) {
        utility::LogError(
                "{} not found, please make sure the reconstruction_system has "
                "finished running on the dataset.",
                pose_graph_fname);
    }

    // Parameters
    auto params = t::pipelines::slac::SLACOptimizerParams();
    params.slac_folder_ = slac_folder;
    params.voxel_size_ =
            utility::GetProgramOptionAsDouble(argc, argv, "--voxel_size", 0.05);
    params.regularizer_weight_ =
            utility::GetProgramOptionAsDouble(argc, argv, "--weight", 1);
    params.distance_threshold_ = utility::GetProgramOptionAsDouble(
            argc, argv, "--distance_threshold", 0.07);
    params.fitness_threshold_ = utility::GetProgramOptionAsDouble(
            argc, argv, "--fitness_threshold", 0.3);
    params.max_iterations_ =
            utility::GetProgramOptionAsInt(argc, argv, "--iterations", 5);
    params.device_ =
            utility::GetProgramOptionAsString(argc, argv, "--device", "CPU:0");

    // Debug
    auto debug_option = t::pipelines::slac::SLACDebugOption();
    debug_option.debug_ = utility::ProgramOptionExists(argc, argv, "--debug");
    debug_option.debug_start_node_idx_ =
            utility::GetProgramOptionAsInt(argc, argv, "--debug_node", 0);

    std::string method =
            utility::GetProgramOptionAsString(argc, argv, "--method", "slac");

    // Run the system
    pipelines::registration::PoseGraph pose_graph_updated;
    if ("rigid" == method) {
        pose_graph_updated = t::pipelines::slac::RunRigidOptimizerForFragments(
                fragment_fnames, *pose_graph, params, debug_option);
    } else if ("slac" == method) {
        t::pipelines::slac::ControlGrid control_grid;

        std::tie(pose_graph_updated, control_grid) =
                t::pipelines::slac::RunSLACOptimizerForFragments(
                        fragment_fnames, *pose_graph, params, debug_option);

        // Write control grids
        auto hashmap = control_grid.GetHashmap();
        core::Tensor active_addrs;
        hashmap->GetActiveIndices(active_addrs);
        active_addrs = active_addrs.To(core::Dtype::Int64);
        hashmap->GetKeyTensor()
                .IndexGet({active_addrs})
                .Save(params.GetSubfolderName() + "/ctr_grid_keys.npy");
        hashmap->GetValueTensor()
                .IndexGet({active_addrs})
                .Save(params.GetSubfolderName() + "/ctr_grid_values.npy");
    }

    // Write pose graph
    io::WritePoseGraph(params.GetSubfolderName() + "/optimized_posegraph_" +
                               method + ".json",
                       pose_graph_updated);

    // Write trajectory for SLACIntegrate
    camera::PinholeCameraTrajectory trajectory;
    for (size_t i = 0; i < pose_graph_updated.nodes_.size(); ++i) {
        auto fragment_pose_graph = io::CreatePoseGraphFromFile(fmt::format(
                "{}/fragment_optimized_{:03d}.json", fragment_folder, i));
        for (auto node : fragment_pose_graph->nodes_) {
            auto pose = pose_graph_updated.nodes_[i].pose_ * node.pose_;
            camera::PinholeCameraParameters param;
            param.extrinsic_ = pose.inverse().eval();
            trajectory.parameters_.push_back(param);
        }
    }
    io::WritePinholeCameraTrajectory(params.GetSubfolderName() +
                                             "/optimized_trajectory_" + method +
                                             ".log",
                                     trajectory);

    return 0;
}

/*******************************************************************************
 * Copyright 2017-2020 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include <iomanip>

#include "absl/synchronization/mutex.h"
#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/graph/graph.h"

#include "logging/ngraph_log.h"
#include "logging/tf_graph_writer.h"
#include "ngraph_bridge/ngraph_api.h"
#include "ngraph_bridge/ngraph_assign_clusters.h"
#include "ngraph_bridge/ngraph_cluster_manager.h"
#include "ngraph_bridge/ngraph_deassign_clusters.h"
#include "ngraph_bridge/ngraph_encapsulate_clusters.h"
#include "ngraph_bridge/ngraph_mark_for_clustering.h"
#include "ngraph_bridge/ngraph_utils.h"

#include "ocm/include/ocm_nodes_checker.h"

using namespace std;

namespace tensorflow {
namespace ngraph_bridge {

class NGraphRewritePass : public GraphOptimizationPass {
 public:
  virtual Status Run(const GraphOptimizationPassOptions& options) = 0;

 protected:
  // Returns a fresh "serial number" to avoid filename collisions in the graph
  // dumps.
  static int FreshIndex() {
    mutex_lock l(s_serial_counter_mutex);
    return s_serial_counter++;
  }

  static int s_serial_counter GUARDED_BY(s_serial_counter_mutex);
  static mutex s_serial_counter_mutex;
};

int NGraphRewritePass::s_serial_counter = 0;
mutex NGraphRewritePass::s_serial_counter_mutex;

//
// Pass that rewrites the graph for nGraph operation.
//
// The pass has several phases, each executed in the below sequence:
//
//   1. Marking [ngraph_mark_for_clustering.cc]
//   2. Cluster Assignment [ngraph_assign_clusters.cc]
//   3. Cluster Deassignment [ngraph_deassign_clusters.cc]
//   4. Cluster Encapsulation [ngraph_encapsulate_clusters.cc]
//
// Between phases, graph dumps (in both .dot and .pbtxt format) may be
// requested by setting the following environment variables:
//
//   NGRAPH_TF_DUMP_UNMARKED_GRAPHS=1      dumps graphs before phase 1
//   NGRAPH_TF_DUMP_MARKED_GRAPHS=1        dumps graphs after phase 1
//   NGRAPH_TF_DUMP_CLUSTERED_GRAPHS=1     dumps graphs after phase 2
//   NGRAPH_TF_DUMP_DECLUSTERED_GRAPHS=1   dumps graphs after phase 3
//   NGRAPH_TF_DUMP_ENCAPSULATED_GRAPHS=1  dumps graphs after phase 4
//   NGRAPH_TF_DUMP_GRAPHS=1               all of the above
//
class NGraphEncapsulationPass : public NGraphRewritePass {
 public:
  Status Run(const GraphOptimizationPassOptions& options) override {
    // If we don't get a main graph, log that fact and bail.
    if (options.graph == nullptr) {
      NGRAPH_VLOG(0) << "NGraphEncapsulationPass: options.graph == nullptr";
      return Status::OK();
    }

    // For filename generation purposes, grab a fresh index. This is just an
    // arbitrary integer to avoid filename collisions resulting from subsequent
    // runs of this pass.
    int idx = FreshIndex();

    // If requested, dump unmarked graphs.
    if (DumpUnmarkedGraphs()) {
      DumpGraphs(options, idx, "unmarked", "Unmarked Graph");
    }

    // If ngraph is disabled via ngraph_bridge api or NGRAPH_TF_DISABLE is set
    // we will not do anything; all subsequent
    // passes become a no-op.
    bool ngraph_not_enabled =
        (!config::IsEnabled()) || (std::getenv("NGRAPH_TF_DISABLE") != nullptr);
    bool already_processed = IsProcessedByNgraphPass(options.graph->get());
    if (!already_processed && ngraph_not_enabled) {
      NGRAPH_VLOG(0) << "NGraph is available but disabled.";
    }
    if (ngraph_not_enabled || already_processed) {
      NGRAPH_VLOG(1) << std::string("Rewrite pass will not run because ") +
                            (already_processed ? "graph is already preprocessed"
                                               : "ngraph is disabled");
      NGraphClusterManager::EvictAllClusters();
      return Status::OK();
    }

    // Now Process the Graph

    // 1. Mark for clustering then, if requested, dump the graphs.
    std::set<string> skip_these_nodes = {};
    // TF_RETURN_IF_ERROR(
    // MarkForClustering(options.graph->get(), skip_these_nodes));

    // OCM bypassing the MarkForClustering function call
    const char* device_id = std::getenv("NGRAPH_TF_BACKEND");
    if (device_id == nullptr) {
      device_id = "CPU";
    }
    std::string ov_version = "2021_1";
    ocm::Framework_Names fName = ocm::Framework_Names::TF;
    ocm::FrameworkNodesChecker FC(fName, device_id, ov_version,
                                  options.graph->get());
    std::vector<void*> nodes_list = FC.MarkSupportedNodes();

    // cast back the nodes in the TF format and mark the nodes for clustering
    // (moved out from MarkForClustering function)
    const std::map<std::string, SetAttributesFunction>& set_attributes_map =
        GetAttributeSetters();
    for (auto void_node : nodes_list) {
      // TODO(amprocte): move attr name to a constant
      tensorflow::Node* node = (tensorflow::Node*)void_node;
      node->AddAttr("_ngraph_marked_for_clustering", true);
      auto it = set_attributes_map.find(node->type_string());
      if (it != set_attributes_map.end()) {
        it->second(node);
      }
    }

    if (DumpMarkedGraphs()) {
      DumpGraphs(options, idx, "marked", "Graph Marked for Clustering");
    }

    // 2. Assign clusters then, if requested, dump the graphs.
    TF_RETURN_IF_ERROR(AssignClusters(options.graph->get()));
    if (DumpClusteredGraphs()) {
      DumpGraphs(options, idx, "clustered", "Graph with Clusters Assigned");
    }

    // 3. Deassign trivial clusters then, if requested, dump the graphs.
    TF_RETURN_IF_ERROR(DeassignClusters(options.graph->get(), device_id));
    if (DumpDeclusteredGraphs()) {
      DumpGraphs(options, idx, "declustered",
                 "Graph with Trivial Clusters De-Assigned");
    }

    // 4. Encapsulate clusters then, if requested, dump the graphs.
    std::unordered_map<std::string, std::string> config_map;
    auto status = EncapsulateClusters(options.graph->get(), idx, config_map);
    if (status != Status::OK()) {
      return status;
    }

    if (DumpEncapsulatedGraphs()) {
      DumpGraphs(options, idx, "encapsulated",
                 "Graph with Clusters Encapsulated");
    }
    return Status::OK();
  }
};

}  // namespace ngraph_bridge

REGISTER_OPTIMIZATION(OptimizationPassRegistry::POST_REWRITE_FOR_EXEC, 0,
                      ngraph_bridge::NGraphEncapsulationPass);
}  // namespace tensorflow

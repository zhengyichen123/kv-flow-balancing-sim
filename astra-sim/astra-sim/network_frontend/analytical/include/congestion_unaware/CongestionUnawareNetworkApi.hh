/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/CommonNetworkApi.hh"
#include <astra-network-analytical/common/Type.h>
#include <astra-network-analytical/congestion_unaware/Topology.h>
#include <vector>
#include <map>

using namespace AstraSim;
using namespace AstraSimAnalytical;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalCongestionUnaware;

namespace AstraSimAnalyticalCongestionUnaware {

/**
 * CongestionUnawareNetworkApi is a AstraNetworkAPI
 * implemented for congestion_unaware analytical network backend.
 */
class CongestionUnawareNetworkApi final : public CommonNetworkApi {
  public:
    /**
     * Set the topology to be used.
     *
     * @param topology_ptr pointer to the to
     */
    static void set_topology(std::shared_ptr<Topology> topology_ptr) noexcept;
    static void set_collective_topology(ComType collective,
                                       std::shared_ptr<Topology> topology_ptr);

    /**
     * Constructor.
     *
     * @param rank id of the API
     */
    explicit CongestionUnawareNetworkApi(int rank) noexcept;

    /**
     * Implement sim_send of AstraNetworkAPI.
     */
    int sim_send(void* buffer,
                 uint64_t count,
                 int type,
                 int dst,
                 int tag,
                 sim_request* request,
                 void (*msg_handler)(void* fun_arg),
                 void* fun_arg) override;

  private:
    /// topology
    static std::shared_ptr<Topology> topology;
    static std::map<ComType, std::shared_ptr<Topology>> collective_topologies;
};

}  // namespace AstraSimAnalyticalCongestionUnaware

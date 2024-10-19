#ifndef CONFIG_UTIL_H
#define CONFIG_UTIL_H

#include "lib/transport/address.h"
#include "process_config.h"

#include <utility>
#include <vector>

// Helpers for defining nng endpoints!
// Need to define pairs for each set of processes that communicate
// TODO add shards
/*
 *
 * Connections
 *      1. clientBase + (clientId  * (numProxies + numReplicas) + replicaId <==> replicaBase + clientId
 *      2. clientBase + (clientId  * (numProxies + numReplicas) + nReplicas + proxyId <==> proxyForwardBase + clientId
 *      3. proxyForwardBase + nClients + receiverId <==> receiverBase + proxyId
 *      4. proxyMeasurmentBase + receiverId <==> receiverBase + proxyId + numProxies
 *      5a. Each replica/receiver own address (i.e. for local experiments)
 *              receiverBase + numProxies * 2 <==> replicaBase + numClients
 *      5b. Each replica/receiver own machine
 *              (127.0.0.1) receiverBase + numProxies * 2 <==> (127.0.0.2) replicaBase + numClients
 *      6. Replica/replica communication
 *
 * Note(s): base addresses need to be sufficiently apart to prevent overlap
 * For 1. each client gets its own port range to communicate with proxies and replicas on, so we can
 * have multiple clients on a single machine.
 */

std::vector<std::pair<Address, Address>> getClientAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getProxyAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getReceiverAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getReplicaAddrs(ProcessConfig config, uint32_t id);

#endif
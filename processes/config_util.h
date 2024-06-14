#ifndef CONFIG_UTIL_H
#define CONFIG_UTIL_H

#include "process_config.h"
#include "lib/transport/address.h"

#include <utility>
#include <vector>


// Helpers for defining nng endpoints!
// Need to define pairs for each set of processes that communicate
// TODO add shards
/*
 * 
 * Connections
 *      1. clientBase + replicaId <==> replicaBase + clientId 
 *      2. clientBase + nReplicas + proxyId <==> proxyForwardBase + clientId 
 *      3. proxyForwardBase + nClients + receiverId <==> receiverBase + proxyId
 *      4. proxyMeasurmentBase + receiverId <==> receiverBase + proxyId + numProxies
 *      5a. Each replica/receiver own address (i.e. loopback for local exp.)
 *              receiverBase + numProxies * 2 <==> replicaBase + numClients
 *      5b. Each replica/receiver own machine
 *              (127.0.0.1) receiverBase + numProxies * 2 <==> (127.0.0.2) replicaBase + numClients
 * 
 */


std::vector<std::pair<Address, Address>> getClientAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getProxyAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getReceiverAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getReplicaAddrs(ProcessConfig config, uint32_t id);

#endif
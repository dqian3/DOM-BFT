#ifndef CONFIG_UTIL_H
#define CONFIG_UTIL_H

#include "lib/transport/address.h"
#include "process_config.h"

#include <utility>
#include <vector>

// Helpers for defining nng endpoints!
// Need to define pairs for each set of processes that communicate
/*
 * Connections
 *      1. Client/replica communication
 *             clientBase + (clientId  * (nProxies + nReplicas)) + replicaId <==> replicaBase + clientId
 *
 *      2. Client/proxy communication
 *             clientBase + (clientId  * (nProxies + nReplicas)) + nReplicas + proxyId <==> proxyForwardBase + clientId
 *
 *      3. Proxy/replica communication
 *             (proxyBase + nClients) + replicaId <==> (replicaBase + nClients) + proxyId
 *
 *      4. Replica/replica communication
 *             (Replica A)                                           (Replica B)
 *             (replicaBase + nClients + nProxies) + replicaIdB <==> (replicaBase + nClients + nProxies) + replicaIdA
 *
 * Note(s): base addresses need to be sufficiently apart to prevent overlap
 * For 1. each client gets its own port range to communicate with proxies and replicas on, so we can
 * have multiple clients on a single machine.
 */

std::vector<std::pair<Address, Address>> getClientAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getProxyAddrs(ProcessConfig config, uint32_t id);
std::vector<std::pair<Address, Address>> getReplicaAddrs(ProcessConfig config, uint32_t id);

#endif
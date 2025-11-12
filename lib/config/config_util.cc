#include "config_util.h"

#include <glog/logging.h>

using namespace std;

void addAddrPairs(
    vector<pair<Address, Address>> &pairs, const std::string &myIp, uint32_t myBasePort,
    const std::vector<std::string> theirIps, int theirPort
)
{
    for (uint32_t i = 0; i < theirIps.size(); i++) {
        pairs.push_back({Address(myIp, myBasePort + i), Address(theirIps[i], theirPort)});
    }
}

void addAddrPairsToClient(
    vector<pair<Address, Address>> &pairs, const std::string &myIp, uint32_t myBasePort,
    const std::vector<std::string> theirIps, int theirPort, int portRangeWidth
)
{
    // Adds the address pairs where the "their" side uses a port offset by
    // portRangeWidth for each client
    for (uint32_t i = 0; i < theirIps.size(); i++) {
        pairs.push_back({Address(myIp, myBasePort + i), Address(theirIps[i], theirPort + i * portRangeWidth)});
    }
}

vector<pair<Address, Address>> getClientAddrs(ProcessConfig config, uint32_t id)
{
    // TODO modify config to specify that these are base ports, and not
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());
    uint32_t clientBase = config.clientPort + id * portRangeWidth;

    // 1. Client/replica:
    //        clientBase + (clientId  * (numProxies + numReplicas)) + replicaId <==> replicaBase + clientId
    std::string clientIp = config.clientIps[id];
    uint32_t replicaPort = config.replicaPort + id;
    addAddrPairs(ret, clientIp, clientBase, config.replicaIps, replicaPort);

    // 2. Client/proxy:
    //        clientBase + (clientId  * (numProxies + numReplicas)) + nReplicas + proxyId <==> proxyForwardBase +
    //        clientId
    clientBase += config.replicaIps.size();
    uint32_t proxyPort = config.proxyForwardPort + id;
    addAddrPairs(ret, clientIp, clientBase, config.proxyIps, proxyPort);

    return ret;
}

vector<pair<Address, Address>> getProxyAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    // 2. Client/proxy:
    //        clientBase + (clientId  * (nProxies + nReplicas) + nReplicas + proxyId <==> proxyForwardBase + clientId
    std::string proxyIp = config.proxyIps[id];
    uint32_t proxyBase = config.proxyForwardPort;
    uint32_t clientPort = config.clientPort + config.replicaIps.size() + id;
    addAddrPairsToClient(ret, proxyIp, proxyBase, config.clientIps, clientPort, portRangeWidth);

    // 3. Proxy/replica:
    //        (proxyBase + nClients) + replicaId <==> (replicaBase + nClients) + proxyId
    proxyBase += config.clientIps.size();
    uint32_t replicaPort = config.replicaPort + config.clientIps.size() + id;
    addAddrPairs(ret, proxyIp, proxyBase, config.replicaIps, replicaPort);

    return ret;
}

vector<pair<Address, Address>> getReplicaAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    // 2. Client/replica:
    //        clientBase + (clientId  * (nProxies + nReplicas)) + replicaId <==> replicaBase + clientId
    std::string replicaIp = config.replicaIps[id];
    uint32_t replicaBase = config.replicaPort;
    uint32_t clientPort = config.clientPort + id;
    addAddrPairsToClient(ret, replicaIp, replicaBase, config.clientIps, clientPort, portRangeWidth);

    //  3. Proxy/replica communication
    //         (proxyBase + nClients) + replicaId <==> (replicaBase + nClients) + proxyId
    replicaBase = config.replicaPort + config.clientIps.size();
    uint32_t proxyPort = config.proxyForwardPort + config.clientIps.size() + id;
    addAddrPairs(ret, replicaIp, replicaBase, config.proxyIps, proxyPort);

    //  4. Replica/replica communication
    //         (Replica A)                                           (Replica B)
    //         (replicaBase + nClients + nProxies) + replicaIdB <==> (replicaBase + nClients + nProxies) + replicaIdA
    replicaBase = config.replicaPort + config.clientIps.size() + config.proxyIps.size();
    // Each replica connects to other replicas at (replicaBase + replicaId)
    for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
        ret.push_back({Address(replicaIp, replicaBase + i), Address(config.replicaIps[i], replicaBase + id)});
    }

    return ret;
}

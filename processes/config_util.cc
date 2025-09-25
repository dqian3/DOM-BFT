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

    // 1. clientBase + (clientId  * (numProxies + numReplicas) + replicaId <==> replicaBase + clientId
    std::string clientIp = config.clientIps[id];
    uint32_t replicaPort = config.replicaPort + id;
    addAddrPairs(ret, clientIp, clientBase, config.replicaIps, replicaPort);

    // 2. clientBase + (clientId  * (numProxies + numReplicas) + nReplicas + proxyId <==> proxyForwardBase + clientId
    clientBase += config.replicaIps.size();
    uint32_t proxyPort = config.proxyForwardPort + id;
    addAddrPairs(ret, clientIp, clientBase, config.proxyIps, proxyPort);

    return ret;
}

vector<pair<Address, Address>> getProxyAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    // 2. clientBase + (clientId  * (numProxies + numReplicas) + nReplicas + proxyId <==> proxyForwardBase + clientId
    std::string proxyIp = config.proxyIps[id];
    uint32_t proxyBase = config.proxyForwardPort;
    uint32_t clientPort = config.clientPort + id;
    addAddrPairsToClient(ret, proxyIp, proxyBase, config.clientIps, clientPort, portRangeWidth);

    // 3. proxyForwardBase + nClients + receiverId <==> receiverBase + proxyId
    proxyBase += config.clientIps.size();
    uint32_t receiverPort = config.replicaPort + id;
    addAddrPairs(ret, proxyIp, proxyBase, config.replicaIps, receiverPort);

    // 4. proxyMeasurmentBase + receiverId <==> receiverBase + proxyId + numProxies
    proxyBase = config.proxyMeasurementPort;
    receiverPort = config.replicaPort + config.proxyIps.size() + id;
    addAddrPairs(ret, proxyIp, proxyBase, config.replicaIps, receiverPort);

    return ret;
}

vector<pair<Address, Address>> getReplicaAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    // 1. clientBase + (clientId  * (numProxies + numReplicas) + replicaId <==> replicaBase + clientId
    std::string replicaIp = config.replicaIps[id];
    uint32_t replicaBase = config.replicaPort;
    uint32_t clientPort = config.clientPort + id;
    addAddrPairsToClient(ret, replicaIp, replicaBase, config.clientIps, clientPort, portRangeWidth);

    // 5a. Each replica/receiver own address (i.e. loopback for local exp.)
    //      receiverBase + numProxies * 2 <==> replicaBase + numClients
    // 5b. Each replica/receiver own machine
    //      (127.0.0.1) receiverBase + numProxies * 2 <==> (127.0.0.2) replicaBase + numClients

    uint32_t replicaPort = config.replicaPort + config.clientIps.size();
    uint32_t receiverPort = config.replicaPort + 2 * config.proxyIps.size();

    // For unified process: replica handles both replica and receiver functionality

    replicaBase = config.replicaPort + config.clientIps.size();
    // Each replica just uses (base + i) to connect with replica i
    for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
        ret.push_back({Address(replicaIp, replicaBase + i), Address(config.replicaIps[i], replicaBase + id)});
    }

    return ret;
}

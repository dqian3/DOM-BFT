#include "config_manager.h"
#include <glog/logging.h>
#include <sstream>

namespace dombft {

// Static member initialization
std::unique_ptr<ConfigManager> ConfigManager::instance_ = nullptr;

void ConfigManager::initialize(const ProcessConfig &config)
{
    if (instance_) {
        LOG(WARNING) << "ConfigManager already initialized. Replacing existing instance.";
    }

    instance_ = std::unique_ptr<ConfigManager>(new ConfigManager());
    instance_->config_ = config;
    instance_->calculateDerivedValues();

    LOG(INFO) << "ConfigManager initialized: " << instance_->getConfigurationSummary();
}

ConfigManager &ConfigManager::getInstance()
{
    if (!instance_) {
        throw std::runtime_error("ConfigManager not initialized. Call initialize() first.");
    }
    return *instance_;
}

bool ConfigManager::isInitialized() { return instance_ != nullptr; }

void ConfigManager::reset() { instance_.reset(); }

void ConfigManager::calculateDerivedValues()
{
    numReplicas_ = config_.replicaIps.size();
    numClients_ = config_.clientIps.size();
    numProxies_ = config_.proxyIps.size();

    // Calculate BFT parameters based on resiliency configuration
    // Check if resiliencyParams contains explicit f and e values
    auto fIt = config_.resiliencyParams.find("f");
    auto eIt = config_.resiliencyParams.find("e");

    if (fIt != config_.resiliencyParams.end()) {
        // Use explicit f value from configuration
        f_ = fIt->second;

        // Calculate quorum sizes based on f
        if (eIt != config_.resiliencyParams.end() && eIt->second > 0) {
            // Byzantine faults with equivocation: n = 5f + 1, quorum = 4f + 1
            quorumSize_ = 4 * f_ + 1;
            superQuorumSize_ = 4 * f_ + 1;
        } else {
            // Standard Byzantine faults: n = 3f + 1, quorum = 2f + 1
            quorumSize_ = 2 * f_ + 1;
            superQuorumSize_ = 3 * f_ + 1;
        }
    } else {
        // Fall back to legacy string-based resiliency calculation
        // This supports backward compatibility with existing configurations
        if (numReplicas_ % 5 == 1) {
            // Assume 5f+1 configuration
            f_ = (numReplicas_ - 1) / 5;
            quorumSize_ = 4 * f_ + 1;
            superQuorumSize_ = 4 * f_ + 1;
        } else {
            // Assume 3f+1 configuration
            f_ = (numReplicas_ - 1) / 3;
            quorumSize_ = 2 * f_ + 1;
            superQuorumSize_ = 3 * f_ + 1;
        }
    }

    VLOG(1) << "Calculated BFT parameters: f=" << f_
            << ", quorumSize=" << quorumSize_
            << ", superQuorumSize=" << superQuorumSize_
            << ", numReplicas=" << numReplicas_;
}

bool ConfigManager::validateConfiguration() const
{
    // Validate replica count matches BFT requirements
    uint32_t expectedReplicas;
    if (superQuorumSize_ == 4 * f_ + 1) {
        expectedReplicas = 5 * f_ + 1;
    } else {
        expectedReplicas = 3 * f_ + 1;
    }

    if (numReplicas_ != expectedReplicas) {
        LOG(ERROR) << "Invalid replica count: expected " << expectedReplicas << " for f=" << f_ << ", got "
                   << numReplicas_;
        return false;
    }

    // Validate quorum sizes
    if (quorumSize_ > numReplicas_) {
        LOG(ERROR) << "Quorum size (" << quorumSize_ << ") exceeds replica count (" << numReplicas_ << ")";
        return false;
    }

    if (superQuorumSize_ > numReplicas_) {
        LOG(ERROR) << "Super quorum size (" << superQuorumSize_ << ") exceeds replica count (" << numReplicas_ << ")";
        return false;
    }

    // Validate that we have sufficient replicas for the fault tolerance
    if (numReplicas_ < 2 * f_ + 1) {
        LOG(ERROR) << "Insufficient replicas (" << numReplicas_ << ") for fault tolerance f=" << f_;
        return false;
    }

    // Validate network configuration
    if (config_.replicaIps.size() != numReplicas_) {
        LOG(ERROR) << "Replica IP count mismatch";
        return false;
    }

    if (config_.clientIps.size() != numClients_) {
        LOG(ERROR) << "Client IP count mismatch";
        return false;
    }

    if (config_.proxyIps.size() != numProxies_) {
        LOG(ERROR) << "Proxy IP count mismatch";
        return false;
    }

    return true;
}

std::string ConfigManager::getConfigurationSummary() const
{
    std::ostringstream oss;
    oss << "BFT Config: f=" << f_ << ", quorum=" << quorumSize_ << ", superQuorum=" << superQuorumSize_
        << ", replicas=" << numReplicas_ << ", clients=" << numClients_ << ", proxies=" << numProxies_
        << ", transport=" << config_.transport << ", app=" << config_.appStr;
    return oss.str();
}

}   // namespace dombft
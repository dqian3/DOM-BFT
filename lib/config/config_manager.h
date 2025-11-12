#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "process_config.h"
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace dombft {

/**
 * Singleton class for centralized configuration management.
 * Provides global access to configuration parameters and derived BFT values.
 */
class ConfigManager {
private:
    static std::unique_ptr<ConfigManager> instance_;
    ProcessConfig config_;

    // Derived BFT parameters
    uint32_t f_;
    uint32_t e_;

    uint32_t quorumSize_;
    uint32_t superQuorumSize_;
    uint32_t numReplicas_;
    uint32_t numClients_;
    uint32_t numProxies_;

    // Private constructor for singleton
    ConfigManager() = default;

    void calculateDerivedValues();

public:
    // Delete copy constructor and assignment operator
    ConfigManager(const ConfigManager &) = delete;
    ConfigManager &operator=(const ConfigManager &) = delete;

    /**
     * Initialize the ConfigManager singleton with configuration data.
     * Must be called before getInstance().
     * @param config The ProcessConfig to use
     */
    static void initialize(const ProcessConfig &config);

    /**
     * Get the singleton instance.
     * @return Reference to the ConfigManager instance
     * @throws std::runtime_error if not initialized
     */
    static ConfigManager &getInstance();

    /**
     * Check if the ConfigManager has been initialized.
     * @return true if initialized, false otherwise
     */
    static bool isInitialized();

    /**
     * Reset the singleton (mainly for testing).
     */
    static void reset();

    // BFT parameter accessors
    uint32_t getF() const { return f_; }
    uint32_t getQuorumSize() const { return quorumSize_; }
    uint32_t getSuperQuorumSize() const { return superQuorumSize_; }
    uint32_t getNumReplicas() const { return numReplicas_; }
    uint32_t getNumClients() const { return numClients_; }
    uint32_t getNumProxies() const { return numProxies_; }

    // Configuration accessors
    const ProcessConfig &getConfig() const { return config_; }

    // Network configuration accessors
    const std::vector<std::string> &getClientIps() const { return config_.clientIps; }
    const std::vector<std::string> &getReplicaIps() const { return config_.replicaIps; }
    const std::vector<std::string> &getProxyIps() const { return config_.proxyIps; }

    int getClientPort() const { return config_.clientPort; }
    int getReplicaPort() const { return config_.replicaPort; }
    int getProxyForwardPort() const { return config_.proxyForwardPort; }

    // Validation methods
    std::string getConfigurationSummary() const;
};

}   // namespace dombft

#endif   // CONFIG_MANAGER_H
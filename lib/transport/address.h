#ifndef ADDRESS_H
#define ADDRESS_H
#include <arpa/inet.h>
#include <boost/functional/hash.hpp>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

#include <iostream>   // Include this for the ostream

/**
 * Encapsualted IP addresses
 *
 * This class could be used to handle general addresses, but is only used
 * for this purpose now. See MessageHandler for details, essentially, we
 * could also implement other addresses for different kinds of endpoints.
 *
 * TODO Move?
 */

class Address {
public:
    std::string ip_;
    int port_;
    struct sockaddr_in addr_;

    Address();
    Address(const Address &other)
        : ip_(other.ip_)
        , port_(other.port_)
    {
        memcpy(&addr_, &(other.addr_), sizeof(struct sockaddr_in));
    }
    Address(struct sockaddr_in addr);
    Address(const std::string &ip, const int port);
    ~Address();

    std::string ip() const;
    int port() const;

    bool operator==(const Address &other) const;

    // Friend declaration for the << operator
    friend std::ostream &operator<<(std::ostream &os, const Address &address);
};

// Define hash function so we can key a unordered_map by Address
// Thank you chatgpt for making this less of a headache
template <> struct std::hash<Address> {
    std::size_t operator()(const Address &k) const
    {
        return boost::hash<pair<std::string, int>>()(make_pair(k.ip_, k.port_));
    }
};

#endif
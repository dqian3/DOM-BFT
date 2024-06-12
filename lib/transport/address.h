#ifndef ADDRESS_H
#define ADDRESS_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <utility>
#include <boost/functional/hash.hpp>


/**
 * Encapsualted IP addresses
 *
 * This class could be used to handle general addresses, but is only used
 * for this purpose now. See MessageHandler for details, essentially, we
 * could also implement other addresses for different kinds of endpoints.
 */

class Address
{
public:
    std::string ip_;
    int port_;
    struct sockaddr_in addr_;

    Address();
    Address(const Address &addr)
        : ip_(addr.ip_), port_(addr.port_)
    {
        memcpy(&addr_, &(addr.addr_), sizeof(struct sockaddr_in));
    }
    Address(const std::string &ip, const int port);
    ~Address();

    std::string GetIPAsString();
    int GetPortAsInt();

    bool operator==(const Address& other) const;
};

// Define hash function so we can key a unordered_map by Address
// Thank you chatgpt for making this less of a headache
template <>
struct std::hash<Address>
{
    std::size_t operator()(const Address &k) const
    {
        boost::hash<pair<std::string, int>>()(make_pair(k.ip_, k.port_));
    }
};

#endif
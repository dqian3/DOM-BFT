#include "lib/signature_provider.h"
#include <glog/logging.h>
#include <filesystem>

#include <openssl/pem.h>

#define MAX_SIG_LEN 256

SignatureProvider::SignatureProvider()
    : privKey_(nullptr) {}

SignatureProvider::~SignatureProvider() {}

bool SignatureProvider::loadPrivateKey(const std::string &privateKeyPath)
{
    BIO *bo = BIO_new_file(privateKeyPath.c_str(), "r");
    PEM_read_bio_PrivateKey(bo, &privKey_, 0, 0);
    BIO_free(bo);

    if (privKey_ == NULL)
    {
        LOG(ERROR) << "Unable to load client private key!";
        return false;
    }
    return true;
}

// Assumes keys are in directory keyDir with names ending in an _n.pub, where n is the id number
bool SignatureProvider::loadPublicKeys(const std::string &keyType, const std::string &keyDir)
{
    // Ohh some C++17 stuff, hopefully this is reasonable
    const std::filesystem::path keysPath(keyDir);

    try
    {
        for (auto const &dirEntry : std::filesystem::directory_iterator(keysPath))
        {
            if (!dirEntry.is_regular_file())
                continue;
            if (dirEntry.path().extension() != ".pub")
                continue;


            EVP_PKEY *pubKey = nullptr;
            BIO *bo = BIO_new_file(dirEntry.path().c_str(), "r");
            PEM_read_bio_PUBKEY(bo, &pubKey, 0, 0);
            BIO_free(bo);

            if (pubKey == nullptr)
            {
                LOG(ERROR) << "Unable to load public key from " << dirEntry.path()
                           << " continuing...";
            }

            // Find id by just looking for the suffix _<n>.pub
            std::string stem = dirEntry.path().stem();
            // Assumes id is end of name.
            int id = std::stoi(stem.substr(stem.find_first_of("0123456789")));
            pubKeys_[keyType][id] = pubKey;
        }
    }
    catch (const std::exception &e)
    {
        LOG(ERROR) << e.what();
        return false;
    }
    

    LOG(INFO) << "Loaded " << pubKeys_[keyType].size() << " keys for "
              << keyType << " from '" << keyDir << "'";
}

int SignatureProvider::appendSignature(MessageHeader *hdr, uint32_t bufLen)
{
    if (privKey_ == nullptr)
    {
        LOG(ERROR) << "Private key not initialized";
        return -1;
    }

    size_t sigLen;

    if (hdr->msgLen + sizeof(MessageHeader) > bufLen)
    {
        LOG(ERROR) << "Error signing message, inital message size "
                   << hdr->msgLen + sizeof(MessageHeader)
                   << " exceeds given buffer capacity "
                   << bufLen;

        return -1;
    }

    byte *data = (byte *)(hdr + 1);
    byte *sig = data + hdr->msgLen;

    // Write signature after msg
    EVP_MD_CTX *mdctx = NULL;
    if (!(mdctx = EVP_MD_CTX_create()))
        return -1;
    // Use SHA256 as digest to sign
    if (1 != EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, privKey_))
        return -1;
    if (1 != EVP_DigestSignUpdate(mdctx, data, hdr->msgLen))
        return -1;

    if (1 != EVP_DigestSignFinal(mdctx, NULL, &sigLen))
    {
        LOG(ERROR) << "Failed to calculate signature length!\n";
        return -1;
    }
    hdr->sigLen = sigLen;

    if (hdr->msgLen + hdr->sigLen + sizeof(MessageHeader) > bufLen)
    {
        LOG(ERROR) << "Error signing message, not enough room in buffer for signature!";
        return -1;
    }

    if (1 != EVP_DigestSignFinal(mdctx, sig, &sigLen))
    {
        LOG(ERROR) << "Failed to sign message!\n";
        return -1;
    }

    return 0;
}

bool SignatureProvider::verify(MessageHeader *hdr, byte *body, const std::string &pubKeyId)
{
    EVP_MD_CTX *mdctx = NULL;

    byte *data = (byte *)(hdr + 1);

    /* Create the Message Digest Context */
    if (!(mdctx = EVP_MD_CTX_create()))
    {
        LOG(ERROR) << "Error creating OpenSSL Context";
        return false;
    }

    if (1 != EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubKeys_[pubKeyId]))
    {
        LOG(ERROR) << "Error initializing digest context";
        return false;
    }

    /* Initialize `key` with a public key */
    if (1 != EVP_DigestVerifyUpdate(mdctx, data, hdr->msgLen))
    {
        LOG(ERROR) << "Error EVP_DigestVerifyUpdate";
        return false;
    }

    if (1 == EVP_DigestVerifyFinal(mdctx, data + hdr->msgLen, hdr->sigLen))
    {
        return true;
    }
    else
    {
        LOG(ERROR) << "signature did not verify :(";
        return false;
    }
}
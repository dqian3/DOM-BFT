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
        LOG(ERROR) << "Unable to load private key!";
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

            // Find id by just looking for the prefix_<n>.pub
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

    return true;
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

    // These are ust taken from examples here:
    // https://www.openssl.org/docs/man3.0/man7/Ed25519.html
    // https://wiki.openssl.org/index.php/EVP_Signing_and_Verifying
    if (EVP_PKEY_id(privKey_) == EVP_PKEY_ED25519)
    {
        if (1 != EVP_DigestSignInit(mdctx, NULL, NULL, NULL, privKey_))
            return -1;
        if (1 != EVP_DigestSign(mdctx, NULL, &sigLen, data, hdr->msgLen))
            return -1;

        if (hdr->msgLen + hdr->sigLen + sizeof(MessageHeader) > bufLen)
        {
            LOG(ERROR) << "Error signing message, not enough room in buffer for signature!";
            return -1;
        }

        hdr->sigLen = sigLen;

        if (1 != EVP_DigestSign(mdctx, sig, &sigLen, data, hdr->msgLen))
            return -1;
    }
    else
    {
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
    }


    return 0;
}

std::string SignatureProvider::getSignature(MessageHeader *hdr, byte *body)
{
    byte *data = (byte *)(hdr + 1);
    return std::string((char *)data + hdr->msgLen, hdr->sigLen);
}

bool SignatureProvider::verify(byte *data, uint32_t dataLen, byte *sig, uint32_t sigLen, const std::string &pubKeyType, int pubKeyId)
{
    if (!pubKeys_[pubKeyType].count(pubKeyId))
    {
        LOG(ERROR) << "Public key of type " << pubKeyType << " and id " << pubKeyId << " not found!";
        return false;
    }

    EVP_PKEY *key = pubKeys_[pubKeyType][pubKeyId];
    EVP_MD_CTX *mdctx = NULL;

    /* Create the Message Digest Context */
    if (!(mdctx = EVP_MD_CTX_create()))
    {
        LOG(ERROR) << "Error creating OpenSSL Context";
        return false;
    }

    if (EVP_PKEY_id(privKey_) == EVP_PKEY_ED25519)
    {
        if (1 != EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, key))
        {
            LOG(ERROR) << "Error initializing digest context";
            return false;
        }

        if (1 == EVP_DigestVerify(mdctx, sig, sigLen, data, dataLen))
        {
            return true;
        }
        else
        {
            LOG(ERROR) << "signature did not verify :(";
            return false;
        }
    }
    else
    {
        if (1 != EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, key))
        {
            LOG(ERROR) << "Error initializing digest context";
            return false;
        }

        /* Initialize `key` with a public key */
        if (1 != EVP_DigestVerifyUpdate(mdctx, data, dataLen))
        {
            LOG(ERROR) << "Error EVP_DigestVerifyUpdate";
            return false;
        }

        if (1 == EVP_DigestVerifyFinal(mdctx, sig, sigLen))
        {
            return true;
        }
        else
        {
            LOG(ERROR) << "signature did not verify :(";
            return false;
        }
    }
}

bool SignatureProvider::verify(MessageHeader *hdr, byte *body, const std::string &pubKeyType, int pubKeyId)
{
    byte *data = (byte *)(hdr + 1);
    return verify(data, hdr->msgLen, data + hdr->msgLen, hdr->sigLen, pubKeyType, pubKeyId);
}
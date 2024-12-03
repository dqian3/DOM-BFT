#include "lib/signature_provider.h"
#include <filesystem>
#include <glog/logging.h>

#include <cryptopp/cryptlib.h>
#include <cryptopp/files.h>
#include <cryptopp/osrng.h>

#define MAX_SIG_LEN 256

using namespace CryptoPP;

SignatureProvider::SignatureProvider() {}

SignatureProvider::~SignatureProvider() {}

bool SignatureProvider::loadPrivateKey(const std::string &privateKeyPath)
{
    // Load private key from DER file
    FileSource fs(privateKeyPath.c_str(), true);
    privKey_.Load(fs);

    return true;
}

// Assumes keys are in directory keyDir with names ending in an _n.pub, where n is the id number
bool SignatureProvider::loadPublicKeys(const std::string &keyType, const std::string &keyDir)
{
    // Ohh some C++17 stuff, hopefully this is reasonable
    const std::filesystem::path keysPath(keyDir);

    for (auto const &dirEntry : std::filesystem::directory_iterator(keysPath)) {

        try {
            if (!dirEntry.is_regular_file())
                continue;
            if (dirEntry.path().extension() != ".pub")
                continue;

            ed25519PublicKey pubKey;
            {
                FileSource fs(dirEntry.path().c_str(), true);
                pubKey.Load(fs);
            }

            // Find id by just looking for the prefix_<n>.pub
            std::string stem = dirEntry.path().stem();
            // Assumes id is end of name.
            int id = std::stoi(stem.substr(stem.find_first_of("0123456789")));
            pubKeys_[keyType][id] = pubKey;

        } catch (const CryptoPP::Exception &e) {
            LOG(ERROR) << "While loading " << dirEntry << ", encountered crypto++ exception: " << e.what();
            return false;
        } catch (const std::exception &e) {
            LOG(ERROR) << e.what();
            return false;
        }
    }

    LOG(INFO) << "Loaded " << pubKeys_[keyType].size() << " keys for " << keyType << " from '" << keyDir << "'";

    return true;
}

// TODO don't know if this actually needs to be thread local
thread_local AutoSeededRandomPool rng_;

bool SignatureProvider::appendSignature(MessageHeader *hdr, uint32_t bufLen)
{
#if SKIP_CRYPTO
    return true;
#endif

    size_t sigLen;

    if (hdr->msgLen + sizeof(MessageHeader) > bufLen) {
        LOG(ERROR) << "Error signing message, inital message size " << hdr->msgLen + sizeof(MessageHeader)
                   << " exceeds given buffer capacity " << bufLen;

        return false;
    }

    byte *data = (byte *) (hdr + 1);
    byte *sig = data + hdr->msgLen;

    ed25519Signer signer(privKey_);

    if (sizeof(MessageHeader) + hdr->msgLen + signer.MaxSignatureLength() > bufLen) {
        LOG(ERROR) << "Error signing message, signing would exceed given buffer capacity " << bufLen;
        return false;
    }

    hdr->sigLen = signer.SignMessage(rng_, data, hdr->msgLen, sig);

    return true;
}

std::vector<byte> SignatureProvider::getSignature(MessageHeader *hdr)
{
    byte *data = (byte *) (hdr + 1);
    return std::vector<byte>(data + hdr->msgLen, data + hdr->sigLen);
}

bool SignatureProvider::verify(
    byte *data, uint32_t dataLen, byte *sig, uint32_t sigLen, const std::string &pubKeyType, int pubKeyId
)
{
#if SKIP_CRYPTO
    return true;
#endif

    if (!pubKeys_.count(pubKeyType) || !pubKeys_.at(pubKeyType).count(pubKeyId)) {
        LOG(ERROR) << "Public key of type " << pubKeyType << " and id " << pubKeyId << " not found!";
        return false;
    }

    ed25519Verifier verifier(pubKeys_.at(pubKeyType).at(pubKeyId));
    return verifier.VerifyMessage(data, dataLen, sig, sigLen);
}

bool SignatureProvider::verify(MessageHeader *hdr, const std::string &pubKeyType, int pubKeyId)
{
    byte *data = (byte *) (hdr + 1);
    return verify(data, hdr->msgLen, data + hdr->msgLen, hdr->sigLen, pubKeyType, pubKeyId);
}
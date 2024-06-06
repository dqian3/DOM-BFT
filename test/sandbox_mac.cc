#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <string.h>



int main(int argc, char *argv[])
{
    EVP_MD_CTX *mdctx = NULL;
    unsigned char pkey[64];
    RAND_bytes(pkey, 64);
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, nullptr, pkey, 64);

    char msg[] = "Test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test test";
    unsigned char *sig = NULL;
    size_t slen = 0;

    int ret;

    if(key == NULL)
        goto err;
    
    printf("Loaded Key\n");

    for (int i = 0; i < 10000; i++) {
        // msg[0] = i;
        /* Create the Message Digest Context */
        if(!(mdctx = EVP_MD_CTX_create())) goto err;
        printf("EVP Create\n");

            /* Initialise the DigestSign operation - SHA-256 has been selected as the message digest function in this example */
        if(1 != EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, key)) goto err;
        printf("Digest Sign Init Complete\n");

        /* Call update with the message */
        if(1 != EVP_DigestSignUpdate(mdctx, msg, strlen(msg))) goto err;
        
        printf("Digest Sign Update Complete\n");

        /* Finalise the DigestSign operation */
        /* First call EVP_DigestSignFinal with a NULL sig parameter to obtain the length of the
        * signature. Length is returned in slen */
        if(1 != EVP_DigestSignFinal(mdctx, NULL, &slen)) goto err;

        printf("Digest Sign Initial Size %ld\n", slen);

        /* Allocate memory for the signature based on size in slen */
        if(!(sig = (unsigned char *)OPENSSL_malloc(sizeof(unsigned char) * (slen)))) goto err;
        /* Obtain the signature */
        if(1 != EVP_DigestSignFinal(mdctx, sig, &slen)) goto err;
        printf("Digest Sign Final Size %ld\n", slen);
    }

    /* Success */
    ret = 1;
    
    err:
    if(ret != 1)
    {
        printf("Error\n");
    }
    
    /* Clean up */
    if(sig && !ret) OPENSSL_free(sig);
    if(mdctx) EVP_MD_CTX_destroy(mdctx);
    
}
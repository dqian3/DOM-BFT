# include <openssl/evp.h>
# include <openssl/pem.h>

#include <stdio.h>
#include <string.h>



int main(int argc, char *argv[])
{
    EVP_MD_CTX *mdctx = NULL;
    int ret = 0;
    size_t slen = 0;
    
    unsigned char *sig = NULL;
    char msg[] = "Test test test";

    // Read files
    BIO* bo = BIO_new_file(argv[1], "r");
    EVP_PKEY* key = NULL;
    PEM_read_bio_PrivateKey( bo, &key, 0, 0 );

    if(key == NULL)
        goto err;
    
    printf("Loaded Key\n");

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
/*
 * Copyright 2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/**
 * @file 
 * This implements the externally-visible functions
 * for handling Encrypted ClientHello (ECHO)
 */

#ifndef OPENSSL_NO_ECHO

# include <openssl/ssl.h>
# include <openssl/echo.h>
# include "ssl_local.h"
# include "echo_local.h"

/*
 * Yes, global vars! 
 * For decoding input strings with public keys (aka ECHOConfig) we'll accept
 * semi-colon separated lists of strings via the API just in case that makes
 * sense.
 */

/* asci hex is easy:-) either case allowed*/
const char *AH_alphabet="0123456789ABCDEFabcdef;";
/* we actually add a semi-colon here as we accept multiple semi-colon separated values */
const char *B64_alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=;";
/* telltale for HTTPSSVC - TODO: finalise when possible */
const char *httpssvc_telltale="echoconfig=";

/*
 * Ancilliary functions
 */

/**
 * Try figure out ECHOConfig encodng
 *
 * @param eklen is the length of rrval
 * @param rrval is encoded thing
 * @param guessedfmt is our returned guess at the format
 * @return 1 for success, 0 for error
 */
static int echo_guess_fmt(size_t eklen, 
                    char *rrval,
                    int *guessedfmt)
{
    if (!guessedfmt || eklen <=0 || !rrval) {
        return(0);
    }

    /*
     * Try from most constrained to least in that order
     */
    if (strstr(rrval,httpssvc_telltale)) {
        *guessedfmt=ECHO_RRFMT_HTTPSSVC;
    } else if (eklen<=strspn(rrval,AH_alphabet)) {
        *guessedfmt=ECHO_RRFMT_ASCIIHEX;
    } else if (eklen<=strspn(rrval,B64_alphabet)) {
        *guessedfmt=ECHO_RRFMT_B64TXT;
    } else {
        // fallback - try binary
        *guessedfmt=ECHO_RRFMT_BIN;
    }
    return(1);
} 


/**
 * @brief Decode from TXT RR to binary buffer
 *
 * This is like ct_base64_decode from crypto/ct/ct_b64.c
 * but a) isn't static and b) is extended to allow a set of 
 * semi-colon separated strings as the input to handle
 * multivalued RRs.
 *
 * Decodes the base64 string |in| into |out|.
 * A new string will be malloc'd and assigned to |out|. This will be owned by
 * the caller. Do not provide a pre-allocated string in |out|.
 * The input is modified if multivalued (NULL bytes are added in 
 * place of semi-colon separators.
 *
 * @param in is the base64 encoded string
 * @param out is the binary equivalent
 * @return is the number of octets in |out| if successful, <=0 for failure
 */
static int echo_base64_decode(char *in, unsigned char **out)
{
    const char* sepstr=";";
    size_t inlen = strlen(in);
    int i=0;
    int outlen=0;
    unsigned char *outbuf = NULL;
    int overallfraglen=0;

    if (out == NULL) {
        return 0;
    }
    if (inlen == 0) {
        *out = NULL;
        return 0;
    }

    /*
     * overestimate of space but easier than base64 finding padding right now
     */
    outbuf = OPENSSL_malloc(inlen);
    if (outbuf == NULL) {
        goto err;
    }

    char *inp=in;
    unsigned char *outp=outbuf;

    while (overallfraglen<inlen) {

        /* find length of 1st b64 string */
        int ofraglen=0;
        int thisfraglen=strcspn(inp,sepstr);
        inp[thisfraglen]='\0';
        overallfraglen+=(thisfraglen+1);

        ofraglen = EVP_DecodeBlock(outp, (unsigned char *)inp, thisfraglen);
        if (ofraglen < 0) {
            goto err;
        }

        /* Subtract padding bytes from |outlen|.  Any more than 2 is malformed. */
        i = 0;
        while (inp[thisfraglen-i-1] == '=') {
            if (++i > 2) {
                goto err;
            }
        }
        outp+=(ofraglen-i);
        outlen+=(ofraglen-i);
        inp+=(thisfraglen+1);

    }

    *out = outbuf;
    return outlen;
err:
    OPENSSL_free(outbuf);
    return -1;
}


/**
 * @brief Free an ECHOConfig structure's internals
 * @param tbf is the thing to be free'd
 */
void ECHOConfig_free(ECHOConfig *tbf)
{
    if (!tbf) return;
    if (tbf->public_name) OPENSSL_free(tbf->public_name);
    if (tbf->pub) OPENSSL_free(tbf->pub);
    if (tbf->ciphersuites) OPENSSL_free(tbf->ciphersuites);
    if (tbf->exttypes) OPENSSL_free(tbf->exttypes);
    if (tbf->extlens) OPENSSL_free(tbf->extlens);
    int i=0;
    for (i=0;i!=tbf->nexts;i++) {
        if (tbf->exts[i]) OPENSSL_free(tbf->exts[i]);
    }
    if (tbf->exts) OPENSSL_free(tbf->exts);
    memset(tbf,0,sizeof(ECHOConfig));
    return;
}

/**
 * @brief Free an ECHOConfigs structure's internals
 * @param tbf is the thing to be free'd
 */
void ECHOConfigs_free(ECHOConfigs *tbf)
{
    if (!tbf) return;
    if (tbf->encoded) OPENSSL_free(tbf->encoded);
    int i;
    for (i=0;i!=tbf->nrecs;i++) {
        ECHOConfig_free(&tbf->recs[i]);
    }
    if (tbf->recs) OPENSSL_free(tbf->recs);
    memset(tbf,0,sizeof(ECHOConfigs));
    return;
}

/**
 * @brief free an SSL_ECHO
 *
 * Free everything within an SSL_ECHO. Note that the
 * caller has to free the top level SSL_ECHO, IOW the
 * pattern here is: 
 *      SSL_ECHO_free(tbf);
 *      OPENSSL_free(tbf);
 *
 * @param tbf is a ptr to an SSL_ECHO structure
 */
void SSL_ECHO_free(SSL_ECHO *tbf)
{
    if (!tbf) return;
    if (tbf->cfg) {
        ECHOConfigs_free(tbf->cfg);
        OPENSSL_free(tbf->cfg);
    }
    /*
     * More TODO
     */
    return;
}

/**
 * @brief Decode the first ECHOConfigs from a binary buffer (and say how may octets not consumed)
 *
 * @param con is the SSL connection
 * @param binbuf is the buffer with the encoding
 * @param binblen is the length of binbunf
 * @param leftover is the number of unused octets from the input
 * @return NULL on error, or a pointer to an ECHOConfigs structure 
 */
static ECHOConfigs *ECHOConfigs_from_binary(SSL *con, unsigned char *binbuf, size_t binblen, int *leftover)
{
    ECHOConfigs *er=NULL; ///< ECHOConfigs record
    ECHOConfig  *te=NULL; ///< Array of ECHOConfig to be embedded in that
    int rind=0; ///< record index

    /* sanity check: version + checksum + KeyShareEntry have to be there - min len >= 10 */
    if (binblen < ECHO_MIN_ECHOCONFIG_LEN) {
        goto err;
    }
    if (leftover==NULL) {
        goto err;
    }
    if (binbuf==NULL) {
        goto err;
    }

    PACKET pkt={binbuf,binblen};

    /* 
     * Overall length of this ECHOConfigs (olen) still could be
     * less than the input buffer length, (binblen) if the caller has been
     * given a catenated set of binary buffers, which could happen
     * and which we will support
     */
    unsigned int olen;
    if (!PACKET_get_net_2(&pkt,&olen)) {
        goto err;
    }
    if (olen < ECHO_MIN_ECHOCONFIG_LEN) {
        goto err;
    }

    int not_to_consume=binblen-olen;

    while (PACKET_remaining(&pkt)>not_to_consume) {

        te=OPENSSL_realloc(te,(rind+1)*sizeof(ECHOConfig));
        if (!te) {
            goto err;
        }
        ECHOConfig *ec=&te[rind];
        memset(ec,0,sizeof(ECHOConfig));

        /*
         * Version
         */
        if (!PACKET_get_net_2(&pkt,&ec->version)) {
            goto err;
        }

        /*
         * check version and fail early if failing 
         */
        switch (ec->version) {
            case ECHO_DRAFT_06_VERSION:
                break;
            default:
                goto err;
        }

        /* 
         * read public_name 
         */
        PACKET public_name_pkt;
        if (!PACKET_get_length_prefixed_2(&pkt, &public_name_pkt)) {
            goto err;
        }
        ec->public_name_len=PACKET_remaining(&public_name_pkt);
        if (ec->public_name_len<=1||ec->public_name_len>TLSEXT_MAXLEN_host_name) {
            goto err;
        }
        ec->public_name=OPENSSL_malloc(ec->public_name_len+1);
        if (ec->public_name==NULL) {
            goto err;
        }
        PACKET_copy_bytes(&public_name_pkt,ec->public_name,ec->public_name_len);
        ec->public_name[ec->public_name_len]='\0';

        /* 
         * read HPKE public key - just a blob
         */
        PACKET pub_pkt;
        if (!PACKET_get_length_prefixed_2(&pkt, &pub_pkt)) {
            goto err;
        }
        ec->pub_len=PACKET_remaining(&pub_pkt);
        ec->pub=OPENSSL_malloc(ec->pub_len);
        if (ec->pub==NULL) {
            goto err;
        }
        PACKET_copy_bytes(&pub_pkt,ec->pub,ec->pub_len);

        /*
         * Kem ID
         */
        if (!PACKET_get_net_2(&pkt,&ec->kem_id)) {
            goto err;
        }
	
	    /*
	     * List of ciphersuites - 2 byte len + 2 bytes per ciphersuite
	     * Code here inspired by ssl/ssl_lib.c:bytes_to_cipher_list
	     */
	    PACKET cipher_suites;
	    if (!PACKET_get_length_prefixed_2(&pkt, &cipher_suites)) {
	        goto err;
	    }
	    int suiteoctets=PACKET_remaining(&cipher_suites);
	    if (suiteoctets<=0 || (suiteoctets % 1)) {
	        goto err;
	    }
	    ec->nsuites=suiteoctets/2;
	    ec->ciphersuites=OPENSSL_malloc(ec->nsuites*sizeof(unsigned int));
	    if (ec->ciphersuites==NULL) {
	        goto err;
	    }
        unsigned char cipher[TLS_CIPHER_LEN];
        int ci=0;
        while (PACKET_copy_bytes(&cipher_suites, cipher, TLS_CIPHER_LEN)) {
            ec->ciphersuites[ci++]=cipher[0]*256+cipher[1];
        }
        if (PACKET_remaining(&cipher_suites) > 0) {
            goto err;
        }

        /*
         * Maximum name length
         */
        if (!PACKET_get_net_2(&pkt,&ec->maximum_name_length)) {
            goto err;
        }

        /*
         * Extensions: we'll just store 'em for now and try parse any
         * we understand a little later
         */
        PACKET exts;
        if (!PACKET_get_length_prefixed_2(&pkt, &exts)) {
            goto err;
        }
        while (PACKET_remaining(&exts) > 0) {
            ec->nexts+=1;
            /*
             * a two-octet length prefixed list of:
             * two octet extension type
             * two octet extension length
             * length octets
             */
            unsigned int exttype=0;
            if (!PACKET_get_net_2(&exts,&exttype)) {
                goto err;
            }
            unsigned int extlen=0;
            if (extlen>=ECHO_MAX_RRVALUE_LEN) {
                goto err;
            }
            if (!PACKET_get_net_2(&exts,&extlen)) {
                goto err;
            }
            unsigned char *extval=NULL;
            if (extlen != 0 ) {
                extval=(unsigned char*)OPENSSL_malloc(extlen);
                if (extval==NULL) {
                    goto err;
                }
                if (!PACKET_copy_bytes(&exts,extval,extlen)) {
                    OPENSSL_free(extval);
                    goto err;
                }
            }
            /* assign fields to lists, have to realloc */
            unsigned int *tip=(unsigned int*)OPENSSL_realloc(ec->exttypes,ec->nexts*sizeof(ec->exttypes[0]));
            if (tip==NULL) {
                if (extval!=NULL) OPENSSL_free(extval);
                goto err;
            }
            ec->exttypes=tip;
            ec->exttypes[ec->nexts-1]=exttype;
            unsigned int *lip=(unsigned int*)OPENSSL_realloc(ec->extlens,ec->nexts*sizeof(ec->extlens[0]));
            if (lip==NULL) {
                if (extval!=NULL) OPENSSL_free(extval);
                goto err;
            }
            ec->extlens=lip;
            ec->extlens[ec->nexts-1]=extlen;
            unsigned char **vip=(unsigned char**)OPENSSL_realloc(ec->exts,ec->nexts*sizeof(unsigned char*));
            if (vip==NULL) {
                if (extval!=NULL) OPENSSL_free(extval);
                goto err;
            }
            ec->exts=vip;
            ec->exts[ec->nexts-1]=extval;
        }
	
        rind++;
    }

    int lleftover=PACKET_remaining(&pkt);
    if (lleftover<0 || lleftover>binblen) {
        goto err;
    }

    /*
     * Success - make up return value
     */
    *leftover=lleftover;
    er=(ECHOConfigs*)OPENSSL_malloc(sizeof(ECHOConfigs));
    if (er==NULL) {
        goto err;
    }
    memset(er,0,sizeof(ECHOConfigs));
    er->nrecs=rind;
    er->recs=te;
    er->encoded_len=olen+2;
    er->encoded=binbuf;
    return er;

err:
    if (er) {
        ECHOConfigs_free(er);
        OPENSSL_free(er);
        er=NULL;
    }
    if (te) {
        OPENSSL_free(te); 
        te=NULL;
    }
    return NULL;
}

/**
 * @brief Decode and check the value retieved from DNS (binary, base64 or ascii-hex encoded)
 *
 * The esnnikeys value here may be the catenation of multiple encoded ECHOKeys RR values 
 * (or TXT values for draft-02), we'll internally try decode and handle those and (later)
 * use whichever is relevant/best. The fmt parameter can be e.g. ECHO_RRFMT_ASCII_HEX
 *
 * @param con is the SSL connection 
 * @param eklen is the length of the binary, base64 or ascii-hex encoded value from DNS
 * @param ekval is the binary, base64 or ascii-hex encoded value from DNS
 * @param num_echos says how many SSL_ECHO structures are in the returned array
 * @return is 1 for success, error otherwise
 */
int SSL_echo_add(
        SSL *con, 
        int ekfmt, 
        size_t eklen, 
        char *ekval, 
        int *num_echos)
{
    /*
     * Sanity checks on inputs
     */
    int detfmt=ECHO_RRFMT_GUESS;
    int rv=0;
    if (eklen==0 || !ekval || !num_echos || !con) {
        SSLerr(SSL_F_SSL_ECHO_ADD, SSL_R_BAD_VALUE);
        return(0);
    }
    if (eklen>=ECHO_MAX_RRVALUE_LEN) {
        SSLerr(SSL_F_SSL_ECHO_ADD, SSL_R_BAD_VALUE);
        return(0);
    }
    switch (ekfmt) {
        case ECHO_RRFMT_GUESS:
            rv=echo_guess_fmt(eklen,ekval,&detfmt);
            if (rv==0)  {
                SSLerr(SSL_F_SSL_ECHO_ADD, SSL_R_BAD_VALUE);
                return(rv);
            }
            break;
        case ECHO_RRFMT_HTTPSSVC:
        case ECHO_RRFMT_ASCIIHEX:
        case ECHO_RRFMT_B64TXT:
            detfmt=ekfmt;
            break;
        default:
            return(0);
    }

    /*
     * Do the various decodes
     */
    unsigned char *outbuf = NULL;   /* a binary representation of a sequence of ECHOConfigs */
    size_t declen=0;                /* length of the above */

    char *ekcpy=ekval;
    if (detfmt==ECHO_RRFMT_HTTPSSVC) {
        ekcpy=strstr(ekval,httpssvc_telltale);
        if (ekcpy==NULL) {
            SSLerr(SSL_F_SSL_ECHO_ADD, SSL_R_BAD_VALUE);
            return(rv);
        }
    }

    if (detfmt==ECHO_RRFMT_B64TXT) {
        /* need an int to get -1 return for failure case */
        int tdeclen = echo_base64_decode(ekcpy, &outbuf);
        if (tdeclen < 0) {
            SSLerr(SSL_F_SSL_ECHO_ADD, SSL_R_BAD_VALUE);
            goto err;
        }
        declen=tdeclen;
    }

    if (detfmt==ECHO_RRFMT_ASCIIHEX) {
        /* Yay AH */
        int adr=hpke_ah_decode(eklen,ekcpy,&declen,&outbuf);
        if (adr==0) {
            goto err;
        }
    }
    if (detfmt==ECHO_RRFMT_BIN) {
        /* just copy over the input to where we'd expect it */
        declen=eklen;
        outbuf=OPENSSL_malloc(declen);
        if (outbuf==NULL){
            goto err;
        }
        memcpy(outbuf,ekcpy,declen);
    }

    /*
     * Now try decode each binary encoding if we can
     */
    int done=0;
    unsigned char *outp=outbuf;
    int oleftover=declen;
    int nlens=0;
    SSL_ECHO *retechos=NULL;
    SSL_ECHO *newecho=NULL;
    while (!done) {
        nlens+=1;
        SSL_ECHO *ts=OPENSSL_realloc(retechos,nlens*sizeof(SSL_ECHO));
        if (!ts) {
            goto err;
        }
        retechos=ts;
        newecho=&retechos[nlens-1];
        memset(newecho,0,sizeof(SSL_ECHO));
    
        int leftover=oleftover;
        ECHOConfigs *er=ECHOConfigs_from_binary(con,outp,oleftover,&leftover);
        if (er==NULL) {
            goto err;
        }
        newecho->cfg=er;
        if (leftover<=0) {
           done=1;
        }
        oleftover=leftover;
        outp+=er->encoded_len;
    }
    con->nechos=nlens;
    con->echo=retechos;
    
    *num_echos=nlens;

    return(1);

err:
    if (outbuf!=NULL) {
        OPENSSL_free(outbuf);
    }
    SSLerr(SSL_F_SSL_ECHO_ADD, SSL_R_BAD_VALUE);
    return(0);
}

/**
 * @brief Decode and check the value retieved from DNS (binary, base64 or ascii-hex encoded)
 *
 * The esnnikeys value here may be the catenation of multiple encoded ECHOKeys RR values 
 * (or TXT values for draft-02), we'll internally try decode and handle those and (later)
 * use whichever is relevant/best. The fmt parameter can be e.g. ECHO_RRFMT_ASCII_HEX
 *
 * @param ctx is the parent SSL_CTX
 * @param eklen is the length of the binary, base64 or ascii-hex encoded value from DNS
 * @param echokeys is the binary, base64 or ascii-hex encoded value from DNS
 * @param num_echos says how many SSL_ECHO structures are in the returned array
 * @return is 1 for success, error otherwise
 */
int SSL_CTX_echo_add(SSL_CTX *ctx, const short ekfmt, const size_t eklen, const char *echokeys, int *num_echos)
{
    return 1;
}

/**
 * @brief Turn on SNI encryption for an (upcoming) TLS session
 * 
 * @param s is the SSL context
 * @param hidden_name is the hidden service name
 * @param public_name is the cleartext SNI name to use
 * @return 1 for success, error otherwise
 * 
 */
int SSL_echo_server_name(SSL *s, const char *hidden_name, const char *public_name)
{
    return 1;
}

/**
 * @brief Turn on ALPN encryption for an (upcoming) TLS session
 * 
 * @param s is the SSL context
 * @param hidden_alpns is the hidden service alpns
 * @param public_alpns is the cleartext SNI alpns to use
 * @return 1 for success, error otherwise
 * 
 */
int SSL_echo_alpns(SSL *s, const char *hidden_alpns, const char *public_alpns)
{
    return 1;
}

/**
 * @brief query the content of an SSL_ECHO structure
 *
 * This function allows the application to examine some internals
 * of an SSL_ECHO structure so that it can then down-select some
 * options. In particular, the caller can see the public_name and
 * IP address related information associated with each ECHOKeys
 * RR value (after decoding and initial checking within the
 * library), and can then choose which of the RR value options
 * the application would prefer to use.
 *
 * @param in is the SSL session
 * @param out is the returned externally visible detailed form of the SSL_ECHO structure
 * @param nindices is an output saying how many indices are in the ECHO_DIFF structure 
 * @return 1 for success, error otherwise
 */
int SSL_echo_query(SSL *in, ECHO_DIFF **out, int *nindices)
{
    return 1;
}

/** 
 * @brief free up memory for an ECHO_DIFF
 *
 * @param in is the structure to free up
 * @param size says how many indices are in in
 */
void SSL_ECHO_DIFF_free(ECHO_DIFF *in, int size)
{
    return;
}

/**
 * @brief utility fnc for application that wants to print an ECHO_DIFF
 *
 * @param out is the BIO to use (e.g. stdout/whatever)
 * @param se is a pointer to an ECHO_DIFF struture
 * @param count is the number of elements in se
 * @return 1 for success, error othewise
 */
int SSL_ECHO_DIFF_print(BIO* out, ECHO_DIFF *se, int count)
{
    return 1;
}

/**
 * @brief down-select to use of one option with an SSL_ECHO
 *
 * This allows the caller to select one of the RR values 
 * within an SSL_ECHO for later use.
 *
 * @param in is an SSL structure with possibly multiple RR values
 * @param index is the index value from an ECHO_DIFF produced from the 'in'
 * @return 1 for success, error otherwise
 */
int SSL_echo_reduce(SSL *in, int index)
{
    return 1;
}

/**
 * Report on the number of ECHO key RRs currently loaded
 *
 * @param s is the SSL server context
 * @param numkeys returns the number currently loaded
 * @return 1 for success, other otherwise
 */
int SSL_CTX_echo_server_key_status(SSL_CTX *s, int *numkeys)
{
    return 1;
}

/**
 * Zap the set of stored ECHO Keys to allow a re-load without hogging memory
 *
 * Supply a zero or negative age to delete all keys. Providing age=3600 will
 * keep keys loaded in the last hour.
 *
 * @param s is the SSL server context
 * @param age don't flush keys loaded in the last age seconds
 * @return 1 for success, other otherwise
 */
int SSL_CTX_echo_server_flush_keys(SSL_CTX *s, int age)
{
    return 1;
}

/**
 * Turn on ECHO server-side
 *
 * When this works, the server will decrypt any ECHO seen in ClientHellos and
 * subsequently treat those as if they had been send in cleartext SNI.
 *
 * @param s is the SSL server context
 * @param con is the SSL connection (can be NULL)
 * @param echokeyfile has the relevant (X25519) private key in PEM format, or both keys
 * @param echopubfile has the relevant (binary encoded, not base64) ECHOKeys structure, or is NULL
 * @return 1 for success, other otherwise
 */
int SSL_CTX_echo_server_enable(SSL_CTX *s, const char *echokeyfile, const char *echopubfile)
{
    return 1;
}

/** 
 * Print the content of an SSL_ECHO
 *
 * @param out is the BIO to use (e.g. stdout/whatever)
 * @param con is an SSL session strucutre
 * @param selector allows for picking all (ECHO_SELECT_ALL==-1) or just one of the RR values in orig
 * @return 1 for success, anything else for failure
 * 
 */
int SSL_echo_print(BIO* out, SSL *con, int selector)
{
    return 1;
}

/**
 * @brief API to allow calling code know ECHO outcome, post-handshake
 *
 * This is intended to be called by applications after the TLS handshake
 * is complete. This works for both client and server. The caller does
 * not have to (and shouldn't) free the hidden or clear_sni strings.
 * TODO: Those are pointers into the SSL struct though so maybe better
 * to allocate fresh ones.
 *
 * Note that the PR we sent to curl will include a check that this
 * function exists (something like "AC_CHECK_FUNCS( SSL_get_echo_status )"
 * so don't change this name without co-ordinating with that.
 * The curl PR: https://github.com/curl/curl/pull/4011
 *
 * @param s The SSL context (if that's the right term)
 * @param hidden will be set to the address of the hidden service
 * @param clear_sni will be set to the address of the hidden service
 * @return 1 for success, other otherwise
 */
int SSL_echo_get_status(SSL *s, char **hidden, char **clear_sni)
{
    return 1;
}

#endif

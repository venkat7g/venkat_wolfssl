/* asn.c
 *
 * Copyright (C) 2006-2021 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

/*
 * DESCRIPTION
 * This library provides the interface to Abstract Syntax Notation One (ASN.1)
 * objects.
 * ASN.1 is a standard interface description language for defining data
 * structures that can be serialized and deserialized in a cross-platform way.
 *
 * Encoding of ASN.1 is either using Basic Encoding Rules (BER) or
 * Distinguished Encoding Rules (DER). DER has only one possible encoding for a
 * ASN.1 description and the data.
 * Encode using DER and decode BER or DER.
 *
 * Provides routines to convert BER into DER. Replaces indefinite length
 * encoded items with explicit lengths.
 */
#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>

/*
ASN Options:
 * NO_ASN_TIME: Disables time parts of the ASN code for systems without an RTC
    or wishing to save space.
 * IGNORE_NAME_CONSTRAINTS: Skip ASN name checks.
 * ASN_DUMP_OID: Allows dump of OID information for debugging.
 * RSA_DECODE_EXTRA: Decodes extra information in RSA public key.
 * WOLFSSL_CERT_GEN: Cert generation. Saves extra certificate info in GetName.
 * WOLFSSL_NO_ASN_STRICT: Disable strict RFC compliance checks to
    restore 3.13.0 behavior.
 * WOLFSSL_NO_OCSP_OPTIONAL_CERTS: Skip optional OCSP certs (responder issuer
    must still be trusted)
 * WOLFSSL_NO_TRUSTED_CERTS_VERIFY: Workaround for situation where entire cert
    chain is not loaded. This only matches on subject and public key and
    does not perform a PKI validation, so it is not a secure solution.
    Only enabled for OCSP.
 * WOLFSSL_NO_OCSP_ISSUER_CHECK: Can be defined for backwards compatibility to
    disable checking of OCSP subject hash with issuer hash.
 * WOLFSSL_SMALL_CERT_VERIFY: Verify the certificate signature without using
    DecodedCert. Doubles up on some code but allows smaller dynamic memory
    usage.
 * WOLFSSL_NO_OCSP_DATE_CHECK: Disable date checks for OCSP responses. This
    may be required when the system's real-time clock is not very accurate.
    It is recommended to enforce the nonce check instead if possible.
 * WOLFSSL_FORCE_OCSP_NONCE_CHECK: Require nonces to be available in OCSP
    responses. The nonces are optional and may not be supported by all
    responders. If it can be ensured that the used responder sends nonces this
    option may improve security.
 * WOLFSSL_ASN_TEMPLATE: Encoding and decoding using a template.
 * WOLFSSL_DEBUG_ASN_TEMPLATE: Enables debugging output when using ASN.1
    templates.
 * WOLFSSL_ASN_TEMPLATE_TYPE_CHECK: Use ASN functions to better test compiler
    type issues for testing
 * CRLDP_VALIDATE_DATA: For ASN template only, validates the reason data
*/

#ifndef NO_ASN

#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/md2.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/pwdbased.h>
#include <wolfssl/wolfcrypt/des3.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/rc2.h>
#include <wolfssl/wolfcrypt/wc_encrypt.h>
#include <wolfssl/wolfcrypt/logging.h>

#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/hash.h>
#ifdef NO_INLINE
    #include <wolfssl/wolfcrypt/misc.h>
#else
    #define WOLFSSL_MISC_INCLUDED
    #include <wolfcrypt/src/misc.c>
#endif

#ifndef NO_RC4
    #include <wolfssl/wolfcrypt/arc4.h>
#endif

#ifdef HAVE_NTRU
    #include "libntruencrypt/ntru_crypto.h"
#endif

#if defined(WOLFSSL_SHA512) || defined(WOLFSSL_SHA384)
    #include <wolfssl/wolfcrypt/sha512.h>
#endif

#ifndef NO_SHA256
    #include <wolfssl/wolfcrypt/sha256.h>
#endif

#ifdef HAVE_ECC
    #include <wolfssl/wolfcrypt/ecc.h>
#endif

#ifdef HAVE_ED25519
    #include <wolfssl/wolfcrypt/ed25519.h>
#endif
#ifdef HAVE_CURVE25519
    #include <wolfssl/wolfcrypt/curve25519.h>
#endif

#ifdef HAVE_ED448
    #include <wolfssl/wolfcrypt/ed448.h>
#endif
#ifdef HAVE_CURVE448
    #include <wolfssl/wolfcrypt/curve448.h>
#endif

#ifdef WOLFSSL_QNX_CAAM
	#include <wolfssl/wolfcrypt/port/caam/wolfcaam.h>
#endif

#ifndef NO_RSA
    #include <wolfssl/wolfcrypt/rsa.h>
#if defined(WOLFSSL_XILINX_CRYPT) || defined(WOLFSSL_CRYPTOCELL)
extern int wc_InitRsaHw(RsaKey* key);
#endif
#endif

#ifndef NO_DSA
    #include <wolfssl/wolfcrypt/dsa.h>
#else
    typedef void* DsaKey;
#endif

#ifdef WOLF_CRYPTO_CB
    #include <wolfssl/wolfcrypt/cryptocb.h>
#endif

#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
    #include <wolfssl/internal.h>
    #include <wolfssl/openssl/objects.h>
#endif

#if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
        !defined(WOLFCRYPT_ONLY)
    #define WOLFSSL_X509_NAME_AVAILABLE
#endif

#ifdef _MSC_VER
    /* 4996 warning to use MS extensions e.g., strcpy_s instead of XSTRNCPY */
    #pragma warning(disable: 4996)
#endif

#define ERROR_OUT(err, eLabel) { ret = (err); goto eLabel; }

#if !defined(NO_SKID) && (!defined(HAVE_FIPS) || !defined(HAVE_FIPS_VERSION))
    #if !defined(HAVE_SELFTEST) || (defined(HAVE_SELFTEST) && \
                                   (!defined(HAVE_SELFTEST_VERSION) || \
                                    HAVE_SELFTEST_VERSION < 2))
    #ifndef WOLFSSL_AES_KEY_SIZE_ENUM
    #define WOLFSSL_AES_KEY_SIZE_ENUM
    enum Asn_Misc {
        AES_IV_SIZE         = 16,
        AES_128_KEY_SIZE    = 16,
        AES_192_KEY_SIZE    = 24,
        AES_256_KEY_SIZE    = 32
    };
    #endif
    #endif /* HAVE_SELFTEST */
#endif
#ifdef WOLFSSL_RENESAS_TSIP_TLS
void tsip_inform_key_position(const word32 key_n_start,
                const word32 key_n_len, const word32 key_e_start,
                const word32 key_e_len);
int tsip_tls_CertVerify(const byte *cert, word32 certSz,
                        const byte *signature, word32 sigSz,
                        word32 key_n_start, word32 key_n_len,
                        word32 key_e_start, word32 key_e_len,
                        byte *tsip_encRsaKeyIdx);
#endif

/* Calculates the minimum number of bytes required to encode the value.
 *
 * @param [in] value  Value to be encoded.
 * @return  Number of bytes to encode value.
 */
static word32 BytePrecision(word32 value)
{
    word32 i;
    for (i = (word32)sizeof(value); i; --i)
        if (value >> ((i - 1) * WOLFSSL_BIT_SIZE))
            break;

    return i;
}

/* DER encodes the length value in output buffer.
 *
 *    0 ->  2^7-1: <len byte>.
 *  2^7 ->       : <0x80 + #bytes> <len big-endian bytes>
 *
 * @param [in]      length  Value to encode.
 * @param [in, out] output  Buffer to encode into.
 * @return  Number of bytes used in encoding.
 */
WOLFSSL_LOCAL word32 SetASNLength(word32 length, byte* output)
{
    word32 i = 0, j;

    if (length < ASN_LONG_LENGTH)
        output[i++] = (byte)length;
    else {
        output[i++] = (byte)(BytePrecision(length) | ASN_LONG_LENGTH);

        for (j = BytePrecision(length); j; --j) {
            output[i] = (byte)(length >> ((j - 1) * WOLFSSL_BIT_SIZE));
            i++;
        }
    }

    return i;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* Calculate the size of a DER encoded length value.
 *
 *    0 ->  2^7-1: <length byte>.
 *  2^7 ->       : <0x80 + #bytes> <big-endian length bytes>
 *
 * @param [in] length  Value to encode.
 * @return  Number of bytes required to encode.
 */
static word32 SizeASNLength(word32 length)
{
    return 1 + ((length >= ASN_LONG_LENGTH) ? BytePrecision(length) : 0);
}

/* Calculate the size of a DER encoded header.
 *
 * Header = Tag | Encoded length
 *
 * @param [in] length  Length value to encode.
 * @return  Number of bytes required to encode a DER header.
 */
#define SizeASNHeader(length) \
    (1 + SizeASNLength(length))
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
#ifdef WOLFSSL_SMALL_STACK
    /* Declare the variable that is the dynamic data for decoding BER data.
     *
     * @param [in] name  Variable name to declare.
     * @param [in] cnt   Number of elements required.
     */
    #define DECL_ASNGETDATA(name, cnt)                                         \
        ASNGetData* name = NULL;

    /* Allocates the dynamic BER decoding data.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define ALLOC_ASNGETDATA(name, cnt, err, heap)                             \
    do {                                                                       \
        if (err == 0) {                                                        \
            name = (ASNGetData*)XMALLOC(sizeof(ASNGetData) * cnt, heap,        \
                                        DYNAMIC_TYPE_TMP_BUFFER);              \
            if (name == NULL) {                                                \
                err = MEMORY_E;                                                \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    while (0)

    /* Allocates the dynamic BER decoding data and clears the memory.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define CALLOC_ASNGETDATA(name, cnt, err, heap)                            \
    do {                                                                       \
        ALLOC_ASNGETDATA(name, cnt, err, heap);                                \
        if (err == 0) {                                                        \
            XMEMSET(name, 0, sizeof(ASNGetData) * cnt);                        \
        }                                                                      \
    }                                                                          \
    while (0)

    /* Disposes of the dynamic BER decoding data.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define FREE_ASNGETDATA(name, heap)                                        \
    do {                                                                       \
        if (name != NULL) {                                                    \
            XFREE(name, heap, DYNAMIC_TYPE_TMP_BUFFER);                        \
        }                                                                      \
    }                                                                          \
    while (0)

    /* Declare the variable that is the dynamic data for encoding DER data.
     *
     * @param [in] name  Variable name to declare.
     * @param [in] cnt   Number of elements required.
     */
    #define DECL_ASNSETDATA(name, cnt)                                         \
        ASNSetData* name = NULL;

    /* Allocates the dynamic DER encoding data.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define ALLOC_ASNSETDATA(name, cnt, err, heap)                             \
    do {                                                                       \
        if (err == 0) {                                                        \
            name = (ASNSetData*)XMALLOC(sizeof(ASNGetData) * cnt, heap,        \
                                    DYNAMIC_TYPE_TMP_BUFFER);                  \
            if (name == NULL) {                                                \
                err = MEMORY_E;                                                \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    while (0)

    /* Allocates the dynamic DER encoding data and clears the memory.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define CALLOC_ASNSETDATA(name, cnt, err, heap)                            \
    do {                                                                       \
        ALLOC_ASNSETDATA(name, cnt, err, heap);                                \
        if (err == 0) {                                                        \
            XMEMSET(name, 0, sizeof(ASNSetData) * cnt);                        \
        }                                                                      \
    }                                                                          \
    while (0)

    /* Disposes of the dynamic DER encoding data.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define FREE_ASNSETDATA(name, heap)                                        \
    do {                                                                       \
        if (name != NULL) {                                                    \
            XFREE(name, heap, DYNAMIC_TYPE_TMP_BUFFER);                        \
        }                                                                      \
    }                                                                          \
    while (0)
#else
    /* Declare the variable that is the dynamic data for decoding BER data.
     *
     * @param [in] name  Variable name to declare.
     * @param [in] cnt   Number of elements required.
     */
    #define DECL_ASNGETDATA(name, cnt)                  \
        ASNGetData name[cnt];

    /* No implementation as declartion is static.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define ALLOC_ASNGETDATA(name, cnt, err, heap)

    /* Clears the memory of the dynamic BER encoding data.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define CALLOC_ASNGETDATA(name, cnt, err, heap)     \
        XMEMSET(name, 0, sizeof(name));

    /* No implementation as declartion is static.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define FREE_ASNGETDATA(name, heap)

    /* Declare the variable that is the dynamic data for encoding DER data.
     *
     * @param [in] name  Variable name to declare.
     * @param [in] cnt   Number of elements required.
     */
    #define DECL_ASNSETDATA(name, cnt)                  \
        ASNSetData name[cnt];

    /* No implementation as declartion is static.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define ALLOC_ASNSETDATA(name, cnt, err, heap)

    /* Clears the memory of the dynamic BER encoding data.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      cnt   Number of elements required.
     * @param [in, out] err   Error variable.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define CALLOC_ASNSETDATA(name, cnt, err, heap)     \
        XMEMSET(name, 0, sizeof(name));

    /* No implementation as declartion is static.
     *
     * @param [in]      name  Variable name to declare.
     * @param [in]      heap  Dynamic memory allocation hint.
     */
    #define FREE_ASNSETDATA(name, heap)
#endif


#ifdef DEBUG_WOLFSSL
    /* Enable this when debugging the parsing or creation of ASN.1 data. */
    #if 0
        #define WOLFSSL_DEBUG_ASN_TEMPLATE
    #endif
#endif

#ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
/* String representations of tags. */
static const char* tagString[4][32] = {
    /* Universal */
    {
        "EOC",
        "BOOLEAN",
        "INTEGER",
        "BIT STRING",
        "OCTET STRING",
        "NULL",
        "OBJECT ID",
        "ObjectDescriptor",
        "INSTANCE OF",
        "REAL",
        "ENUMERATED",
        "EMBEDDED PDV",
        "UT8String",
        "RELATIVE-OID",
        "(0x0e) 14",
        "(0x0f) 15",
        "SEQUENCE",
        "SET",
        "NumericString",
        "PrintableString",
        "T61String",
        "VideotexString",
        "IA5String",
        "UTCTime",
        "GeneralizedTime",
        "GraphicString",
        "ISO646String",
        "GeneralString",
        "UniversalString",
        "CHARACTER STRING",
        "BMPString",
        "(0x1f) 31",
    },
    /* Application */
    {
         "[A 0]",  "[A 1]",  "[A 2]",  "[A 3]",
         "[A 4]",  "[A 5]",  "[A 6]",  "[A 7]",
         "[A 8]",  "[A 9]", "[A 10]", "[A 11]",
        "[A 12]", "[A 13]", "[A 14]", "[A 15]",
        "[A 16]", "[A 17]", "[A 18]", "[A 19]",
        "[A 20]", "[A 21]", "[A 22]", "[A 23]",
        "[A 24]", "[A 25]", "[A 26]", "[A 27]",
        "[A 28]", "[A 20]", "[A 30]", "[A 31]"
    },
    /* Context-Specific */
    {
         "[0]",  "[1]",  "[2]",  "[3]",  "[4]",  "[5]",  "[6]",  "[7]",
         "[8]",  "[9]", "[10]", "[11]", "[12]", "[13]", "[14]", "[15]",
        "[16]", "[17]", "[18]", "[19]", "[20]", "[21]", "[22]", "[23]",
        "[24]", "[25]", "[26]", "[27]", "[28]", "[20]", "[30]", "[31]"
    },
    /* Private */
    {
         "[P 0]",  "[P 1]",  "[P 2]",  "[P 3]",
         "[P 4]",  "[P 5]",  "[P 6]",  "[P 7]",
         "[P 8]",  "[P 9]", "[P 10]", "[P 11]",
        "[P 12]", "[P 13]", "[P 14]", "[P 15]",
        "[P 16]", "[P 17]", "[P 18]", "[P 19]",
        "[P 20]", "[P 21]", "[P 22]", "[P 23]",
        "[P 24]", "[P 25]", "[P 26]", "[P 27]",
        "[P 28]", "[P 20]", "[P 30]", "[P 31]"
    }
};

/* Converts a tag byte to string.
 *
 * @param [in] tag  BER tag value to interpret.
 * @return  String corresponding to tag.
 */
static const char* TagString(byte tag)
{
    return tagString[tag >> 6][tag & ASN_TYPE_MASK];
}

#include <stdarg.h>

/* Log a message that has the printf format string.
 *
 * @param [in] <va_args>  printf style arguments.
 */
#define WOLFSSL_MSG_VSNPRINTF(...)                    \
    do {                                              \
      char line[81];                                  \
      snprintf(line, sizeof(line) - 1, __VA_ARGS__);  \
      line[sizeof(line) - 1] = '\0';                  \
      WOLFSSL_MSG(line);                              \
    }                                                 \
    while (0)
#endif

/* Returns whether ASN.1 item is an integer and the Most-Significant Bit is set.
 *
 * @param [in] asn    ASN.1 items to encode.
 * @param [in] data   Data to place in each item. Lengths set were not known.
 * @param [in] i      Index of item to check.
 * @return  1 when ASN.1 item is an integer and MSB is 1.
 * @erturn  0 otherwise.
 */
#define ASNIntMSBSet(asn, data, i)                    \
    ((asn[i].tag == ASN_INTEGER) &&                   \
      (data[i].data.buffer.data != NULL &&            \
      (data[i].data.buffer.data[0] & 0x80) == 0x80))


/* Calculate the size of a DER encoded number.
 *
 * @param [in] n     Number to be encoded.
 * @param [in] bits  Maximum number of bits to encode.
 * @param [in] tag   BER tag e.g. INTEGER, BIT_STRING, etc.
 * @return  Number of bytes to the ASN.1 item.
 */
static word32 SizeASN_Num(word32 n, int bits, byte tag)
{
    int    j;
    word32 len;

    len = 1 + 1 + bits / 8;
    /* Discover actual size by checking for high zeros. */
    for (j = bits - 8; j > 0; j -= 8) {
        if (n >> j)
            break;
        len--;
    }
    if (tag == ASN_BIT_STRING)
        len++;
    else if ((tag == ASN_INTEGER) && (((n >> j) & 0x80) == 0x80))
        len++;

    return len;
}

/* Calculate the size of the data in the constructed item based on the
 * length of the ASN.1 items below.
 *
 * @param [in]      asn    ASN.1 items to encode.
 * @param [in, out] data   Data to place in each item. Lengths set were not
 *                         known.
 * @param [in]      idx    Index of item working on.
 */
static void SizeASN_CalcDataLength(const ASNItem* asn, ASNSetData *data,
                                   int idx, int max)
{
    int j;

    data[idx].data.buffer.length = 0;
    /* Sum the item length of all items underneath. */
    for (j = idx + 1; j < max; j++) {
        /* Stop looking if the next ASN.1 is same level or higher. */
        if (asn[j].depth <= asn[idx].depth)
            break;
        /* Only add in length if it is one level below. */
        if (asn[j].depth - 1 == asn[idx].depth) {
            data[idx].data.buffer.length += data[j].length;
            /* The length of a header only item doesn't include the data unless
             * a replacement buffer is supplied.
             */
            if (asn[j].headerOnly && data[j].dataType !=
                                                 ASN_DATA_TYPE_REPLACE_BUFFER) {
                data[idx].data.buffer.length += data[j].data.buffer.length;
            }
        }
    }
}

/* Calculate the size of the DER encoding.
 *
 * Call SetASN_Items() to write encoding to a buffer.
 *
 * @param [in]      asn    ASN.1 items to encode.
 * @param [in, out] data   Data to place in each item. Lengths set were not
 *                         known.
 * @param [in]      count  Count of items to encode.
 * @param [out]     encSz  Length of the DER encoding.
 * @return  0 on success.
 * @return  BAD_STATE_E when the data type is not supported.
 */
int SizeASN_Items(const ASNItem* asn, ASNSetData *data, int count, int* encSz)
{
    int    i;
    word32 sz = 0;
    word32 len;
    word32 dataLen;
    int    length;

#ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
    WOLFSSL_ENTER("SizeASN_Items");
#endif

    for (i = count - 1; i >= 0; i--) {
        /* Skip this ASN.1 item when encoding. */
        if (data[i].noOut) {
            /* Set the offset to the current size - used in writing DER. */
            data[i].offset = sz;
            continue;
        }

        len = 0;
        switch (data[i].dataType) {
            /* Calculate the size of the number of different sizes. */
            case ASN_DATA_TYPE_WORD8:
                len = SizeASN_Num(data[i].data.u8, 8, asn[i].tag);
                break;
            case ASN_DATA_TYPE_WORD16:
                len = SizeASN_Num(data[i].data.u16, 16, asn[i].tag);
                break;
        #ifdef WOLFSSL_ASN_TEMPLATE_NEED_SET_INT32
            /* Not used yet! */
            case ASN_DATA_TYPE_WORD32:
                len = SizeASN_Num(data[i].data.u32, 32, asn[i].tag);
                break;
        #endif

            case ASN_DATA_TYPE_MP:
                /* Calculate the size of the MP integer data. */
                length = mp_unsigned_bin_size(data[i].data.mp);
                length += mp_leading_bit(data[i].data.mp) ? 1 : 0;
                len = SizeASNHeader(length) + length;
                break;

            case ASN_DATA_TYPE_REPLACE_BUFFER:
                /* Buffer is put in directly - use the length. */
                len = data[i].data.buffer.length;
                break;

            case ASN_DATA_TYPE_NONE:
                /* Calculate the size based on the data to be included.
                 * Mostly used for constructed items.
                 */
                if (asn[i].headerOnly) {
                    /* Calculate data length from items below. */
                    SizeASN_CalcDataLength(asn, data, i, count);
                }
                if (asn[i].tag == ASN_BOOLEAN) {
                    dataLen = 1;
                }
                else {
                    dataLen = data[i].data.buffer.length;
                }
                /* BIT_STRING and INTEGER have one byte prepended. */
                if ((asn[i].tag == ASN_BIT_STRING) ||
                                                   ASNIntMSBSet(asn, data, i)) {
                    dataLen++;
                    /* ASN.1 items are below and cannot include extra byte. */
                    if (asn[i].headerOnly) {
                        len++;
                    }
                }
                /* Add in the size of tag and length. */
                len += SizeASNHeader(dataLen);
                /* Include data in length if not header only. */
                if (!asn[i].headerOnly) {
                    len += dataLen;
                }
                break;

        #ifdef DEBUG_WOLFSSL
            default:
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("%2d: %d", i, data[i].dataType);
                WOLFSSL_MSG("Bad data type");
            #endif
                return BAD_STATE_E;
        #endif
        }

        /* Set the total length of the item. */
        data[i].length = len;
        /* Add length to total size. */
        sz += len;
        /* Set the offset to the current size - used in writing DER. */
        data[i].offset = sz;

    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        WOLFSSL_MSG_VSNPRINTF("%2d: %4d %4d %c %*s %-16s", i,
                data[i].offset, data[i].length, asn[i].constructed ? '+' : ' ',
                asn[i].depth, "", TagString(asn[i].tag));
    #endif
    }

    *encSz = sz;
    return 0;
}

/* Create the DER encoding of a number.
 *
 * Assumes that the out buffer is large enough for encoding.
 *
 * @param [in] n     Number to be encoded.
 * @param [in] bits  Maximum number of bits to encode.
 * @param [in] tag   DER tag e.g. INTEGER, BIT_STRING, etc.
 */
static void SetASN_Num(word32 n, int bits, byte* out, byte tag)
{
    int    j;
    word32 idx;
    byte   len;

    /* Encoding: Tag (1 byte) | Length (1 byte) | Data (number) */

    /* Data will start at index 2 unless BIT_STRING or INTEGER */
    idx = 2;

    /* Set the length of the number based on maximum bit length. */
    len = bits / 8;
    /* Discover actual size by checking for leading zero bytes. */
    for (j = bits - 8; j > 0; j -= 8) {
        if ((n >> j) != 0) {
            break;
        }
        len--;
    }
    /* Keep j, index of first non-zero byte, for writing out. */

    /* A BIT_STRING has the number of unused bits in last byte prepended to
     * data.
     */
    if (tag == ASN_BIT_STRING) {
        byte unusedBits = 0;
        byte lastByte = n >> j;

        /* Quick check last bit. */
        if ((lastByte & 0x01) == 0x00) {
            unusedBits++;
            /* Check each bit for first least significant bit set. */
            while (((lastByte >> unusedBits) & 0x01) == 0x00)
                unusedBits++;
        }
        /* Add unused bits byte. */
        len++;
        out[idx++] = unusedBits;
    }

    /* An INTEGER has a prepended byte if MSB of number is 1 - makes encoded
     * value positive. */
    if ((tag == ASN_INTEGER) && (((n >> j) & 0x80) == 0x80)) {
        len++;
        out[idx++] = 0;
    }

    /* Go back and put in length. */
    out[1] = len;
    /* Place in the required bytes of the number. */
    for (; j >= 0; j -= 8)
        out[idx++] = n >> j;
}

/* Creates the DER encoding of the ASN.1 items.
 *
 * Assumes the output buffer is large enough to hold encoding.
 * Must call SizeASN_Items() to determine size of encoding and offsets.
 *
 * @param [in]      asn     ASN.1 items to encode.
 * @param [in]      data    Data to place in each item.
 * @param [in]      count   Count of items to encode.
 * @param [in, out] output  Buffer to write encoding into.
 * @return  Size of the DER encoding in bytes.
 */
int SetASN_Items(const ASNItem* asn, ASNSetData *data, int count, byte* output)
{
    int    i;
    int    length;
    int    err;
    word32 sz;
    word32 idx;
    byte*  out;

#ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
    WOLFSSL_ENTER("SetASN_Items");
#endif

    /* Offset of first item is the total length.
     * SizeASN_Items() calculated this. */
    sz = data[0].offset;

    /* Write out each item. */
    for (i = 0; i < count; i++) {
        /* Skip items not writing out. */
        if (data[i].noOut)
            continue;

        /* Start position to write item based on reverse offsets. */
        out = output + sz - data[i].offset;
        /* Index from start of item out. */
        idx = 0;

        if (data[i].dataType != ASN_DATA_TYPE_REPLACE_BUFFER) {
            /* Put in the tag - not dumping in DER from buffer. */
            out[idx++] = asn[i].tag |
                         (asn[i].constructed ? ASN_CONSTRUCTED : 0);
        }

    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        WOLFSSL_MSG_VSNPRINTF("%2d: %4d %4d %c %*s %-16s", i,
                sz - data[i].offset,
                data[i].length, asn[i].constructed ? '+' : ' ', asn[i].depth,
                "", TagString(asn[i].tag));
    #endif

        switch (data[i].dataType) {
            /* Write out the length and data of a number. */
            case ASN_DATA_TYPE_WORD8:
                SetASN_Num(data[i].data.u8, 8, out, asn[i].tag);
                break;
            case ASN_DATA_TYPE_WORD16:
                SetASN_Num(data[i].data.u16, 16, out, asn[i].tag);
                break;
        #ifdef WOLFSSL_ASN_TEMPLATE_NEED_SET_INT32
            /* Not used yet! */
            case ASN_DATA_TYPE_WORD32:
                SetASN_Num(data[i].data.u32, 32, out, asn[i].tag);
                break;
        #endif

            /* Write out the length and data of a multi-precision number. */
            case ASN_DATA_TYPE_MP:
                /* Get length in bytes. */
                length = mp_unsigned_bin_size(data[i].data.mp);
                /* Add one for leading zero to make encoding a positive num. */
                length += mp_leading_bit(data[i].data.mp) ? 1 : 0;
                /* Write out length. */
                idx += SetASNLength(length, out + idx);
                /* Write out leading zero to make positive. */
                if (mp_leading_bit(data[i].data.mp)) {
                    out[idx++] = 0;
                }
                /* Encode number in big-endian byte array. */
                err = mp_to_unsigned_bin(data[i].data.mp, out + idx);
                if (err != MP_OKAY) {
                    WOLFSSL_MSG("SetASN_Items: Failed to write mp_int");
                    return MP_TO_E;
                }
                break;

            case ASN_DATA_TYPE_REPLACE_BUFFER:
                if (data[i].data.buffer.data == NULL) {
                    /* Return pointer for caller to use. */
                    data[i].data.buffer.data = out + idx;
                }
                else {
                    /* Dump in the DER encoded data. */
                    XMEMCPY(out + idx, data[i].data.buffer.data,
                            data[i].data.buffer.length);
                }
                break;

            case ASN_DATA_TYPE_NONE:
                if (asn[i].tag == ASN_BOOLEAN) {
                    /* Always one byte of data. */
                    out[idx++] = 1;
                    /* TRUE = 0xff, FALSE = 0x00 */
                    out[idx] = data[i].data.u8 ? -1 : 0;
                }
                else if (asn[i].tag == ASN_TAG_NULL) {
                    /* NULL tag is always a zero length item. */
                    out[idx] = 0;
                }
                else {
                    word32 dataLen = data[i].data.buffer.length;
                    /* Add one to data length for BIT_STRING unused bits and
                     * INTEGER leading zero to make positive.
                     */
                    if ((asn[i].tag == ASN_BIT_STRING) ||
                                                   ASNIntMSBSet(asn, data, i)) {
                        dataLen++;
                    }
                    /* Write out length. */
                    idx += SetASNLength(dataLen, out + idx);
                    if ((asn[i].tag == ASN_BIT_STRING) ||
                                                   ASNIntMSBSet(asn, data, i)) {
                       /* Write out leading byte. BIT_STRING has no unused bits
                        * - use number data types if needed. */
                        out[idx++] = 0x00;
                    }
                    /* Record pointer for caller if data not supplied. */
                    if (data[i].data.buffer.data == NULL) {
                        data[i].data.buffer.data = out + idx;
                    }
                    /* Copy supplied data if not putting out header only. */
                    else if (!asn[i].headerOnly) {
                        /* Allow data to come from output buffer. */
                        XMEMMOVE(out + idx, data[i].data.buffer.data,
                                 data[i].data.buffer.length);
                    }
                }
                break;

        #ifdef DEBUG_WOLFSSL
            default:
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Bad data type: %d", data[i].dataType);
            #endif
                return BAD_STATE_E;
        #endif
        }
    }

    return sz;
}


static int GetOID(const byte* input, word32* inOutIdx, word32* oid,
                  word32 oidType, int length);

/* Maximum supported depth in ASN.1 description. */
#define GET_ASN_MAX_DEPTH          7
/* Maximum number of checked numbered choices. Only one of the items with the
 * number is allowed.
 */
#define GET_ASN_MAX_CHOICES        2

/* Use existing function to decode BER length encoding. */
#define GetASN_Length GetLength_ex

/* Check an INTEGER's first byte - must be a positive number.
 *
 * @param [in] input    BER encoded data.
 * @param [in] idx      Index of BIT_STRING data.
 * @param [in] length   Length of input data.
 * @param [in] positve  Indicates number must be positive.
 * @return  0 on success.
 * @return  ASN_PARSE_E when 0 is not required but seen.
 * @return  ASN_EXPECT_0_E when 0 is required and not seen.
 */
static int GetASN_Integer(const byte* input, word32 idx, int length,
                          int positive)
{
    if (input[idx] == 0) {
        /* Check leading zero byte required. */
        if ((length > 1) && ((input[idx + 1] & 0x80) == 0)) {
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            WOLFSSL_MSG("Zero not required on INTEGER");
        #endif
            return ASN_PARSE_E;
        }
    }
    /* Check whether a leading zero byte was required. */
    else if (positive && (input[idx] & 0x80)) {
    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        WOLFSSL_MSG("INTEGER is negative");
    #endif
        return ASN_EXPECT_0_E;
    }

    return 0;
}

/* Check a BIT_STRING's first byte - unused bits.
 *
 * @param [in] input   BER encoded data.
 * @param [in] idx     Index of BIT_STRING data.
 * @param [in] length  Length of input data.
 * @return  0 on success.
 * @return  ASN_PARSE_E when unused bits is invalid.
 */
static int GetASN_BitString(const byte* input, word32 idx, int length)
{
    /* Ensure unused bits value is valid range. */
    if (input[idx] > 7) {
    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        WOLFSSL_MSG_VSNPRINTF("BIT STRING unused bits too big: %d > 7",
                input[idx]);
    #endif
        return ASN_PARSE_E;
    }
    /* Ensure unused bits are zero. */
    if ((byte)(input[idx + length - 1] << (8 - input[idx])) != 0) {
    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        WOLFSSL_MSG_VSNPRINTF("BIT STRING unused bits used: %d %02x",
                input[idx], input[idx + length - 1]);
    #endif
        return ASN_PARSE_E;
    }

    return 0;
}

/* Get the ASN.1 items from the BER encoding.
 *
 * @param [in] asn         ASN.1 item expected.
 * @param [in] data        Data array to place found item into.
 * @param [in] input       BER encoded data.
 * @param [in] idx         Starting index of item data.
 * @param [in] len         Length of input buffer upto end of this item's data.
 * @param [in] zeroPadded  INTEGER was zero padded to make positive.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data is invalid.
 * @return  ASN_EXPECT_0_E when NULL tagged item has a non-zero length.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 * @return  BAD_STATE_E when the data type is not supported.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
static int GetASN_StoreData(const ASNItem* asn, ASNGetData* data,
                            const byte* input, word32 idx, int len,
                            int zeroPadded)
{
    int i;
    int err;

    /* Parse data based on data type to extract. */
    switch (data->dataType) {
        /* Parse a data into a number of specified bits. */
        case ASN_DATA_TYPE_WORD8:
            /* Check data is small enough to fit. */
            if (len != 1) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Expecting one byte: %d", len);
            #endif
                return ASN_PARSE_E;
            }
            /* Fill number with all of data. */
            *data->data.u8 = input[idx];
            break;
        case ASN_DATA_TYPE_WORD16:
            /* Check data is small enough to fit. */
            if (len == 0 || len > 2) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Expecting 1 or 2 bytes: %d", len);
            #endif
                return ASN_PARSE_E;
            }
            /* Fill number with all of data. */
            *data->data.u16 = 0;
            for (i = 0; i < len; i++) {
                *data->data.u16 <<= 8;
                *data->data.u16 |= input[idx + i] ;
            }
            break;
        case ASN_DATA_TYPE_WORD32:
            /* Check data is small enough to fit. */
            if (len == 0 || len > 4) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Expecting 1 to 4 bytes: %d", len);
            #endif
                return ASN_PARSE_E;
            }
            /* Fill number with all of data. */
            *data->data.u32 = 0;
            for (i = 0; i < len; i++) {
                *data->data.u32 <<= 8;
                *data->data.u32 |= input[idx + i] ;
            }
            break;

        case ASN_DATA_TYPE_BUFFER:
            /* Check buffer is big enough to hold data. */
            if (len > (int)*data->data.buffer.length) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Buffer too small for data: %d %d", len,
                        *data->data.buffer.length);
            #endif
                return ASN_PARSE_E;
            }
            /* Copy in data and record actual length seen. */
            XMEMCPY(data->data.buffer.data, input + idx, len);
            *data->data.buffer.length = len;
            break;

        case ASN_DATA_TYPE_EXP_BUFFER:
            /* Check data is same size expected. */
            if (len != (int)data->data.ref.length) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Data not expected length: %d %d", len,
                        data->data.ref.length);
            #endif
                return ASN_PARSE_E;
            }
            /* Check data is same as expected. */
            if (XMEMCMP(data->data.ref.data, input + idx, len) != 0) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG("Data not as expected");
            #endif
                return ASN_PARSE_E;
            }
            break;

        case ASN_DATA_TYPE_MP:
        case ASN_DATA_TYPE_MP_POS_NEG:
            /* Initialize mp_int and read in big-endian byte array. */
            if (mp_init(data->data.mp) != MP_OKAY) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Failed to init mp: %p", data->data.mp);
            #endif
                return MP_INIT_E;
            }
            err = mp_read_unsigned_bin(data->data.mp, (byte*)input + idx, len);
            if (err != 0) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Failed to read mp: %d", err);
            #endif
                mp_clear(data->data.mp);
                return ASN_GETINT_E;
            }
        #ifdef HAVE_WOLF_BIGINT
            err = wc_bigint_from_unsigned_bin(&data->data.mp->raw, input + idx,
                    len);
            if (err != 0) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Failed to create bigint: %d", err);
            #endif
                mp_clear(data->data.mp);
                return ASN_GETINT_E;
            }
        #endif /* HAVE_WOLF_BIGINT */
            /* Don't always read as positive. */
            if ((data->dataType == ASN_DATA_TYPE_MP_POS_NEG) && (!zeroPadded) &&
                (input[idx] & 0x80)) {
                data->data.mp->sign = MP_NEG;
            }
            break;

        case ASN_DATA_TYPE_CHOICE:
            /* Check if tag matched any of the choices specified. */
            for (i = 0; data->data.choice[i] != 0; i++)
                if (data->data.choice[i] == data->tag)
                    break;
            if (data->data.choice[i] == 0) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG("Tag didn't match a choice");
            #endif
                return ASN_PARSE_E;
            }

            /* Store data pointer and length for caller. */
            data->data.ref.data = input + idx;
            data->data.ref.length = len;
            break;

        case ASN_DATA_TYPE_NONE:
            /* Default behaviour based on tag. */
            if (asn->tag == ASN_BOOLEAN) {
                /* BOOLEAN has only one byte of data in BER. */
                if (len != 1) {
                #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                    WOLFSSL_MSG_VSNPRINTF("BOOLEAN length too long: %d", len);
                #endif
                    return ASN_PARSE_E;
                }
                /* Store C boolean value. */
                *data->data.u8 = (input[idx] != 0);
                break;
            }
            if (asn->tag == ASN_TAG_NULL) {
                /* NULL has no data in BER. */
                if (len != 0) {
                #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                    WOLFSSL_MSG_VSNPRINTF("NULL length too long: %d", len);
                #endif
                    return ASN_EXPECT_0_E;
                }
                data->data.ref.data = input + idx;
                break;
            }
            if (asn->tag == ASN_OBJECT_ID) {
                word32 oidIdx = 0;
                /* Store OID data pointer and length */
                data->data.oid.data = input + idx;
                data->data.oid.length = len;
                /* Get the OID sum. */
                err = GetOID(input + idx, &oidIdx, &data->data.oid.sum,
                        data->data.oid.type, len);
                if (err < 0) {
                #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                    WOLFSSL_MSG_VSNPRINTF("OID check failed: %d", err);
                #endif
                    return err;
                }
                break;
            }

            /* Otherwise store data pointer and length. */
            data->data.ref.data = input + idx;
            data->data.ref.length = len;
            break;

    #ifdef DEBUG_WOLFSSL
        default:
            /* Bad ASN data type. */
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            WOLFSSL_MSG_VSNPRINTF("Bad data type: %d", data->dataType);
        #endif
            return BAD_STATE_E;
    #endif
    }

    return 0;
}

/* Get the ASN.1 items from the BER encoding.
 *
 * @param [in]      asn       ASN.1 items expected.
 * @param [in]      data      Data array to place found items into.
 * @param [in]      count     Count of items to parse.
 * @param [in]      complete  Whether the whole buffer is to be used up.
 * @param [in]      input     BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of data.
 *                            On out, end of parsed data.
 * @param [in]      length    Length of input buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 * @return  BAD_STATE_E when the data type is not supported.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int GetASN_Items(const ASNItem* asn, ASNGetData *data, int count, int complete,
                 const byte* input, word32* inOutIdx, word32 length)
{
    int    i;
    int    j;
    int    err;
    int    len;
    /* Current index into buffer. */
    word32 idx = *inOutIdx;
    /* Initialize the end index at each depth to be the length. */
    word32 endIdx[GET_ASN_MAX_DEPTH] = { length, length, length, length, length,
                                         length, length };
    /* Set choices to -1 to indicate they haven't been seen or found. */
    char   choiceMet[GET_ASN_MAX_CHOICES] = { -1, -1 };
    /* Not matching a choice right now. */
    int    choice = 0;
    /* Current depth of ASN.1 item. */
    int    depth;
    /* Minimum depth value seen. */
    int    minDepth;
    /* Integer had a zero prepended. */
    int    zeroPadded;

#ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
    WOLFSSL_ENTER("GetASN_Items");
#endif

    /* Start depth at first items depth. */
    minDepth = depth = asn[0].depth;
    /* Check every ASN.1 item. */
    for (i = 0; i < count; i++) {
        /* Store offset of ASN.1 item. */
        data[i].offset = idx;
        /* Length of data in ASN.1 item starts empty. */
        data[i].length = 0;
        /* Get current item depth. */
        depth = asn[i].depth;
    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        if (depth > GET_ASN_MAX_DEPTH) {
            WOLFSSL_MSG("Depth in template too large");
            return ASN_PARSE_E;
        }
    #endif
        /* Keep track of minimum depth. */
        if (depth < minDepth) {
            minDepth = depth;
        }

        /* Reset choice if different from previous. */
        if (choice > 0 && asn[i].optional != choice) {
            choice = 0;
        }
        /* Check if first of numbered choice. */
        if (choice == 0 && asn[i].optional > 1) {
            choice = asn[i].optional;
            if (choiceMet[choice - 2] == -1) {
                /* Choice seen but not found a match yet. */
                choiceMet[choice - 2] = 0;
            }
        }

        /* Check for end of data or not a choice and tag not matching. */
        if (idx == endIdx[depth] || (data[i].dataType != ASN_DATA_TYPE_CHOICE &&
                              (input[idx] & ~ASN_CONSTRUCTED) != asn[i].tag)) {
            if (asn[i].optional) {
                /* Skip over ASN.1 items underneath this optional item. */
                for (j = i + 1; j < count; j++) {
                    if (asn[i].depth >= asn[j].depth)
                        break;
                    data[j].offset = idx;
                    data[j].length = 0;
                }
                i = j - 1;
                continue;
            }

            /* Check for end of data. */
            if (idx == length) {
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF(
                    "%2d: %4d %4d %c %*s %-16s%*s  (index past end)",
                    i, data[i].offset, data[i].length,
                    asn[i].constructed ? '+' : ' ', asn[i].depth, "",
                    TagString(asn[i].tag), 6 - asn[i].depth, "");
                WOLFSSL_MSG_VSNPRINTF("Index past end of data: %d %d", idx,
                        length);
        #endif
                return BUFFER_E;
            }
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            /* Show expected versus found. */
            WOLFSSL_MSG_VSNPRINTF(
                "%2d: %4d %4d %c %*s %-16s%*s  Tag=0x%02x (%s)",
                i, data[i].offset, data[i].length,
                asn[i].constructed ? '+' : ' ', asn[i].depth, "",
                TagString(asn[i].tag), 6 - asn[i].depth, "",
                input[idx], TagString(input[idx]));
        #endif
            /* Check for end of data at this depth. */
            if (idx == endIdx[depth]) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF("Index past outer item: %d %d", idx,
                        endIdx[depth]);
            #endif
                return ASN_PARSE_E;
            }

            /* Expecting an OBJECT_ID */
            if (asn[i].tag == ASN_OBJECT_ID) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG("Expecting OBJECT ID");
            #endif
                return ASN_OBJECT_ID_E;
            }
            /* Expecting a BIT_STRING */
            if (asn[i].tag == ASN_BIT_STRING) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG("Expecting BIT STRING");
            #endif
                return ASN_BITSTR_E;
            }
            /* Not the expected tag. */
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            WOLFSSL_MSG("Bad tag");
        #endif
            return ASN_PARSE_E;
        }

        /* Store found tag in data. */
        data[i].tag = input[idx];
        if (data[i].dataType != ASN_DATA_TYPE_CHOICE) {
            int constructed = (input[idx] & ASN_CONSTRUCTED) == ASN_CONSTRUCTED;
            /* Check constructed match expected for non-choice ASN.1 item. */
            if (asn[i].constructed != constructed) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF(
                        "%2d: %4d %4d %c %*s %-16s%*s  Tag=0x%02x (%s)",
                        i, data[i].offset, data[i].length,
                        asn[i].constructed ? '+' : ' ', asn[i].depth, "",
                        TagString(asn[i].tag), 6 - asn[i].depth, "",
                        input[idx], TagString(input[idx]));
                if (!constructed) {
                    WOLFSSL_MSG("Not constructed");
                }
                else {
                    WOLFSSL_MSG("Not expected to be constructed");
                }
            #endif
                return ASN_PARSE_E;
            }
        }
        /* Move index to start of length. */
        idx++;
        /* Get the encoded length. */
        if ((err = GetASN_Length(input, &idx, &len, endIdx[depth], 1)) < 0) {
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            WOLFSSL_MSG_VSNPRINTF("%2d: idx=%d len=%d end=%d", i, idx, len,
                    endIdx[depth]);
        #endif
            return ASN_PARSE_E;
        }
        /* Store length of data. */
        data[i].length = len;
        /* Note the max length of items under this one. */
        endIdx[depth + 1] = idx + len;
        if (choice > 1) {
            /* Note we found a number choice. */
            choiceMet[choice - 2] = 1;
        }

    #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
        WOLFSSL_MSG_VSNPRINTF("%2d: %4d %4d %c %*s %-16s", i,
                data[i].offset, data[i].length, asn[i].constructed ? '+' : ' ',
                asn[i].depth, "", TagString(data[i].tag));
    #endif

        /* Assume no zero padding on INTEGER. */
        zeroPadded = 0;
        /* Check data types that prepended a byte. */
        if (asn[i].tag == ASN_INTEGER) {
            /* Check validity of first byte. */
            err = GetASN_Integer(input, idx, len,
                    data[i].dataType == ASN_DATA_TYPE_MP);
            if (err != 0)
                return err;
            if (len > 1 && input[idx] == 0) {
                zeroPadded = 1;
                /* Move over prepended byte. */
                idx++;
                len--;
            }
        }
        else if (asn[i].tag == ASN_BIT_STRING) {
            /* Check prepended byte is correct. */
            err = GetASN_BitString(input, idx, len);
            if (err != 0)
                return err;
            /* Move over prepended byte. */
            idx++;
            len--;
        }

        /* Don't parse data if only header required. */
        if (asn[i].headerOnly) {
            /* Store reference to data and length. */
            data[i].data.ref.data = input + idx;
            data[i].data.ref.length = len;
            continue;
        }

        /* Store the data at idx in the ASN data item. */
        err = GetASN_StoreData(&asn[i], &data[i], input, idx, len, zeroPadded);
        if (err != 0) {
            return err;
        }

        /* Move index to next item. */
        idx += len;

        /* When matched numbered choice ... */
        if (asn[i].optional > 1) {
            /* Skip over other ASN.1 items of the same number. */
            for (j = i + 1; j < count; j++) {
                if (asn[j].depth <= asn[i].depth &&
                                           asn[j].optional != asn[i].optional) {
                   break;
                }
            }
            i = j - 1;
        }
    }

    if (complete) {
        /* When expecting ASN.1 items to completely use data, check we did. */
        for (j = depth; j > minDepth; j--) {
            if (idx < endIdx[j]) {
            #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
                WOLFSSL_MSG_VSNPRINTF(
                    "More data in constructed item at depth: %d", j - 1);
            #endif
                return ASN_PARSE_E;
            }
        }
    }

    /* Check all choices where met - found an item for them. */
    for (j = 0; j < GET_ASN_MAX_CHOICES; j++) {
        if (choiceMet[j] == 0) {
        #ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            WOLFSSL_MSG_VSNPRINTF("No choice seen: %d", j + 2);
        #endif
            return ASN_PARSE_E;
        }
    }

    /* Return index after ASN.1 data has been parsed. */
    *inOutIdx = idx;

    return 0;
}

#ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
/* Calculate the size of the DER encoding.
 *
 * Call SetASN_Items() to write encoding to a buffer.
 *
 * @param [in]      asn    ASN.1 items to encode.
 * @param [in, out] data   Data to place in each item. Lengths set were not
 *                         known.
 * @param [in]      count  Count of items to encode.
 * @param [out]     len    Length of the DER encoding.
 * @return  Size of the DER encoding in bytes.
 */
static int SizeASN_ItemsDebug(const char* name, const ASNItem* asn,
    ASNSetData *data, int count, int* encSz)
{
    WOLFSSL_MSG_VSNPRINTF("TEMPLATE: %s", name);
    return SizeASN_Items(asn, data, count, encSz);
}

/* Creates the DER encoding of the ASN.1 items.
 *
 * Assumes the output buffer is large enough to hold encoding.
 * Must call SizeASN_Items() to determine size of encoding and offsets.
 *
 * Displays the template name first.
 *
 * @param [in]      name    Name of ASN.1 template.
 * @param [in]      asn     ASN.1 items to encode.
 * @param [in]      data    Data to place in each item.
 * @param [in]      count   Count of items to encode.
 * @param [in, out] output  Buffer to write encoding into.
 * @return  Size of the DER encoding in bytes.
 */
static int SetASN_ItemsDebug(const char* name, const ASNItem* asn,
    ASNSetData *data, int count, byte* output)
{
    WOLFSSL_MSG_VSNPRINTF("TEMPLATE: %s", name);
    return SetASN_Items(asn, data, count, output);
}

/* Get the ASN.1 items from the BER encoding.
 *
 * Displays the template name first.
 *
 * @param [in]      name      Name of ASN.1 template.
 * @param [in]      asn       ASN.1 items expected.
 * @param [in]      data      Data array to place found items into.
 * @param [in]      count     Count of items to parse.
 * @param [in]      complete  Whether the whole buffer is to be used up.
 * @param [in]      input     BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of data.
 *                            On out, end of parsed data.
 * @param [in]      maxIdx    Maximum index of input data.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 * is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 * @return  BAD_STATE_E when the data type is not supported.
 */
static int GetASN_ItemsDebug(const char* name, const ASNItem* asn,
    ASNGetData *data, int count, int complete, const byte* input,
    word32* inOutIdx, word32 maxIdx)
{
    WOLFSSL_MSG_VSNPRINTF("TEMPLATE: %s", name);
    return GetASN_Items(asn, data, count, complete, input, inOutIdx, maxIdx);
}

/* Calculate the size of the DER encoding.
 *
 * Call SetASN_Items() to write encoding to a buffer.
 *
 * @param [in]      asn    ASN.1 items to encode.
 * @param [in, out] data   Data to place in each item. Lengths set were not
 *                         known.
 * @param [in]      count  Count of items to encode.
 * @param [out]     len    Length of the DER encoding.
 * @return  Size of the DER encoding in bytes.
 */
#define SizeASN_Items(asn, data, count, encSz)  \
    SizeASN_ItemsDebug(#asn, asn, data, count, encSz)

/* Creates the DER encoding of the ASN.1 items.
 *
 * Assumes the output buffer is large enough to hold encoding.
 * Must call SizeASN_Items() to determine size of encoding and offsets.
 *
 * Displays the template name first.
 *
 * @param [in]      name    Name of ASN.1 template.
 * @param [in]      asn     ASN.1 items to encode.
 * @param [in]      data    Data to place in each item.
 * @param [in]      count   Count of items to encode.
 * @param [in, out] output  Buffer to write encoding into.
 * @return  Size of the DER encoding in bytes.
 */
#define SetASN_Items(asn, data, count, output)  \
    SetASN_ItemsDebug(#asn, asn, data, count, output)

/* Get the ASN.1 items from the BER encoding.
 *
 * Displays the template name first.
 *
 * @param [in]      name      Name of ASN.1 template.
 * @param [in]      asn       ASN.1 items expected.
 * @param [in]      data      Data array to place found items into.
 * @param [in]      count     Count of items to parse.
 * @param [in]      complete  Whether the whole buffer is to be used up.
 * @param [in]      input     BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of data.
 *                            On out, end of parsed data.
 * @param [in]      maxIdx    Maximum index of input data.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 * is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 * @return  BAD_STATE_E when the data type is not supported.
 */
#define GetASN_Items(asn, data, count, complete, input, inOutIdx, maxIdx)  \
    GetASN_ItemsDebug(#asn, asn, data, count, complete, input, inOutIdx, maxIdx)
#endif /* WOLFSSL_DEBUG_ASN_TEMPLATE */

/* Decode a BER encoded constructed sequence.
 *
 * @param [in]       input     Buffer of BER encoded data.
 * @param [in, out]  inOutIdx  On in, index to start decoding from.
 *                             On out, index of next encoded byte.
 * @param [out]      len       Length of data under SEQUENCE.
 * @param [in]       maxIdx    Maximim index of data. Index of byte after SEQ.
 * @param [in]       complete  All data used with SEQUENCE and data under.
 * @return  0 on success.
 * @return  BUFFER_E when not enough data to complete decode.
 * @return  ASN_PARSE when decoding failed.
 */
static int GetASN_Sequence(const byte* input, word32* inOutIdx, int* len,
                           word32 maxIdx, int complete)
{
    int ret = 0;
    word32 idx = *inOutIdx;

    /* Check buffer big enough for tag. */
    if (idx + 1 > maxIdx) {
        ret = BUFFER_E;
    }
    /* Check it is a constructed SEQUENCE. */
    if ((ret == 0) && (input[idx++] != (ASN_SEQUENCE | ASN_CONSTRUCTED))) {
        ret = ASN_PARSE_E;
    }
    /* Get the length. */
    if ((ret == 0) && (GetASN_Length(input, &idx, len, maxIdx, 1) < 0)) {
        ret = ASN_PARSE_E;
    }
    /* Check all data used if complete set. */
    if ((ret == 0) && complete && (idx + *len != maxIdx)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* Return index of next byte of encoded data. */
        *inOutIdx = idx;
    }

    return ret;
}


#ifdef WOLFSSL_ASN_TEMPLATE_TYPE_CHECK
/* Setup ASN data item to get an 8-bit number.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Pointer to an 8-bit variable.
 */
void GetASN_Int8Bit(ASNGetData *dataASN, byte* num)
{
    dataASN->dataType = ASN_DATA_TYPE_WORD8;
    dataASN->data.u8  = num;
}

/* Setup ASN data item to get a 16-bit number.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Pointer to a 16-bit variable.
 */
void GetASN_Int16Bit(ASNGetData *dataASN, word16* num)
{
    dataASN->dataType = ASN_DATA_TYPE_WORD16;
    dataASN->data.u16 = num;
}

/* Setup ASN data item to get a 32-bit number.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Pointer to a 32-bit variable.
 */
void GetASN_Int32Bit(ASNGetData *dataASN, word32* num)
{
    dataASN->dataType = ASN_DATA_TYPE_WORD32;
    dataASN->data.u32 = num;
}

/* Setup ASN data item to get data into a buffer of a specific length.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] data     Buffer to hold data.
 * @param [in] length   Length of buffer in bytes.
 */
void GetASN_Buffer(ASNGetData *dataASN, byte* data, word32* length)
{
    dataASN->dataType           = ASN_DATA_TYPE_BUFFER;
    dataASN->data.buffer.data   = data;
    dataASN->data.buffer.length = length;
}

/* Setup ASN data item to check parsed data against expected buffer.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] data     Buffer containing expected data.
 * @param [in] length   Length of buffer in bytes.
 */
void GetASN_ExpBuffer(ASNGetData *dataASN, const byte* data, word32 length)
{
    dataASN->dataType        = ASN_DATA_TYPE_EXP_BUFFER;
    dataASN->data.ref.data   = data;
    dataASN->data.ref.length = length;
}

/* Setup ASN data item to get a number into an mp_int.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Multi-precision number object.
 */
void GetASN_MP(ASNGetData *dataASN, mp_int* num)
{
    dataASN->dataType = ASN_DATA_TYPE_MP;
    dataASN->data.mp  = num;
}

/* Setup ASN data item to get a positive or negative number into an mp_int.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Multi-precision number object.
 */
void GetASN_MP_PosNeg(ASNGetData *dataASN, mp_int* num)
{
    dataASN->dataType = ASN_DATA_TYPE_MP_POS_NEG;
    dataASN->data.mp  = num;
}

/* Setup ASN data item to be a choice of tags.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] options  0 terminated list of tags that are valid.
 */
void GetASN_Choice(ASNGetData *dataASN, const byte* options)
{
    dataASN->dataType    = ASN_DATA_TYPE_CHOICE;
    dataASN->data.choice = options;
}

/* Setup ASN data item to get a boolean value.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Pointer to an 8-bit variable.
 */
void GetASN_Boolean(ASNGetData *dataASN, byte* num)
{
    dataASN->dataType    = ASN_DATA_TYPE_NONE;
    dataASN->data.choice = num;
}

/* Setup ASN data item to be a an OID of a specific type.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] oidType  Type of OID to expect.
 */
void GetASN_OID(ASNGetData *dataASN, int oidType)
{
    dataASN->data.oid.type = oidType;
}

/* Get the data and length from an ASN data item.
 *
 * @param [in]  dataASN  Dynamic ASN data item.
 * @param [out] data     Pointer to data of item.
 * @param [out] length   Length of buffer in bytes.
 */
void GetASN_GetConstRef(ASNGetData * dataASN, const byte** data, word32* length)
{
    *data   = dataASN->data.ref.data;
    *length = dataASN->data.ref.length;
}

/* Get the data and length from an ASN data item.
 *
 * @param [in]  dataASN  Dynamic ASN data item.
 * @param [out] data     Pointer to data of item.
 * @param [out] length   Length of buffer in bytes.
 */
void GetASN_GetRef(ASNGetData * dataASN, byte** data, word32* length)
{
    *data   = (byte*)dataASN->data.ref.data;
    *length =        dataASN->data.ref.length;
}

/* Get the data and length from an ASN data item that is an OID.
 *
 * @param [in]  dataASN  Dynamic ASN data item.
 * @param [out] data     Pointer to .
 * @param [out] length   Length of buffer in bytes.
 */
void GetASN_OIDData(ASNGetData * dataASN, byte** data, word32* length)
{
    *data   = (byte*)dataASN->data.oid.data;
    *length =        dataASN->data.oid.length;
}

/* Setup an ASN data item to set a boolean.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] val      Boolean value.
 */
void SetASN_Boolean(ASNSetData *dataASN, byte val)
{
    dataASN->dataType = ASN_DATA_TYPE_NONE;
    dataASN->data.u8  = val;
}

/* Setup an ASN data item to set an 8-bit number.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      8-bit number to set.
 */
void SetASN_Int8Bit(ASNSetData *dataASN, byte num)
{
    dataASN->dataType = ASN_DATA_TYPE_WORD8;
    dataASN->data.u8  = num;
}

/* Setup an ASN data item to set a 16-bit number.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      16-bit number to set.
 */
void SetASN_Int16Bit(ASNSetData *dataASN, word16 num)
{
    dataASN->dataType = ASN_DATA_TYPE_WORD16;
    dataASN->data.u16 = num;
}

/* Setup an ASN data item to set the data in a buffer.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] data     Buffer containing data to set.
 * @param [in] length   Length of data in buffer in bytes.
 */
void SetASN_Buffer(ASNSetData *dataASN, const byte* data, word32 length)
{
    dataASN->data.buffer.data   = data;
    dataASN->data.buffer.length = length;
}

/* Setup an ASN data item to set the DER encode data in a buffer.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] data     Buffer containing BER encoded data to set.
 * @param [in] length   Length of data in buffer in bytes.
 */
void SetASN_ReplaceBuffer(ASNSetData *dataASN, const byte* data, word32 length)
{
    dataASN->dataType           = ASN_DATA_TYPE_REPLACE_BUFFER;
    dataASN->data.buffer.data   = data;
    dataASN->data.buffer.length = length;
}

/* Setup an ASN data item to set an multi-precision number.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] num      Multi-precision number.
 */
void SetASN_MP(ASNSetData *dataASN, mp_int* num)
{
    dataASN->dataType = ASN_DATA_TYPE_MP;
    dataASN->data.mp  = num;
}

/* Setup an ASN data item to set an OID based on id and type.
 *
 * oid and oidType pair are unique.
 *
 * @param [in] dataASN  Dynamic ASN data item.
 * @param [in] oid      OID identifier.
 * @param [in] oidType  Type of OID.
 */
void SetASN_OID(ASNSetData *dataASN, int oid, int oidType)
{
    dataASN->data.buffer.data = OidFromId(oid, oidType,
                                                  &dataASN->data.buffer.length);
}
#endif /* WOLFSSL_ASN_TEMPLATE_TYPE_CHECK */

#ifdef CRLDP_VALIDATE_DATA
/* Get the data of the BIT_STRING as a 16-bit number.
 *
 * @param [in]  dataASN  Dynamic ASN data item.
 * @param [out] val      ASN.1 item's data as a 16-bit number.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BITSTRING value is more than 2 bytes.
 * @return  ASN_PARSE_E when unused bits of BITSTRING is invalid.
 */
static int GetASN_BitString_Int16Bit(ASNGetData* dataASN, word16* val)
{
    int ret;
    int i;
    const byte* input = dataASN->data.ref.data;
    int length = dataASN->data.ref.length;

    /* Validate the BIT_STRING data. */
    ret = GetASN_BitString(input, 0, length);
    if (ret == 0) {
        /* Skip unused bits byte. */
        input++;
        length--;

        /* Check the data is usable. */
        if (length == 0 || length > 2) {
#ifdef WOLFSSL_DEBUG_ASN_TEMPLATE
            WOLFSSL_MSG_VSNPRINTF("Expecting 1 or 2 bytes: %d", length);
#endif
            ret = ASN_PARSE_E;
        }
    }
    if (ret == 0) {
        /* Fill 16-bit var with all the data. */
        *val = 0;
        for (i = 0; i < length; i++) {
            *val <<= 8;
            *val |= input[i];
        }
    }
    return ret;
}
#endif /* CRLDP_VALIDATE_DATA */

#endif /* WOFLSSL_ASN_TEMPLATE */


/* Decode the BER/DER length field.
 *
 * @param [in]      input     BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of length.
 *                            On out, end of parsed length.
 * @param [out]     len       Length value decoded.
 * @param [in]      maxIdx    Maximum index of input data.
 * @return  0 on success.
 * @return  ASN_PARSE_E if the encoding is invalid.
 * @return  BUFFER_E when not enough data to complete decode.
 */
int GetLength(const byte* input, word32* inOutIdx, int* len, word32 maxIdx)
{
    return GetLength_ex(input, inOutIdx, len, maxIdx, 1);
}


/* Decode the BER/DER length field and check the length is valid on request.
 *
 * BER/DER has Type-Length-Value triplets.
 * When requested will check that the Length decoded, indicating the number
 * of bytes in the Value, is available in the buffer after the Length bytes.
 *
 * Only supporting a length upto INT_MAX.
 *
 * @param [in]      input     BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of length.
 *                            On out, end of parsed length.
 * @param [out]     len       Length value decoded.
 * @param [in]      maxIdx    Maximum index of input data.
 * @param [in]      check     Whether to check the buffer has at least the
 *                            decoded length of bytes remaining.
 * @return  0 on success.
 * @return  ASN_PARSE_E if the encoding is invalid.
 * @return  BUFFER_E when not enough data to complete decode.
 */
int GetLength_ex(const byte* input, word32* inOutIdx, int* len, word32 maxIdx,
                 int check)
{
    int     length = 0;
    word32  idx = *inOutIdx;
    byte    b;

    /* Ensure zero return length on error. */
    *len = 0;

    /* Check there is at least on byte avaialble containing length information.
     */
    if ((idx + 1) > maxIdx) {
        WOLFSSL_MSG("GetLength - bad index on input");
        return BUFFER_E;
    }

    /* Get the first length byte. */
    b = input[idx++];
    /* Check if the first byte indicates the count of bytes. */
    if (b >= ASN_LONG_LENGTH) {
        /* Bottom 7 bits are the number of bytes to calculate length with.
         * Note: 0 indicates indefinte length encoding *not* 0 bytes of length.
         */
        word32 bytes = b & 0x7F;

        /* Check the number of bytes required are available. */
        if ((idx + bytes) > maxIdx) {
            WOLFSSL_MSG("GetLength - bad long length");
            return BUFFER_E;
        }

        /* Only support up to the number of bytes that fit into return var. */
        if (bytes > sizeof(length)) {
            return ASN_PARSE_E;
        }
        /* Big-endian encoding of number. */
        while (bytes--) {
            b = input[idx++];
            length = (length << 8) | b;
        }
        /* Negative value indicates we overflowed the signed int. */
        if (length < 0) {
            return ASN_PARSE_E;
        }
    }
    else {
        /* Length in first byte. */
        length = b;
    }

    /* When request, check the buffer has at least length bytes left. */
    if (check && ((idx + length) > maxIdx)) {
        WOLFSSL_MSG("GetLength - value exceeds buffer length");
        return BUFFER_E;
    }

    /* Return index after length encoding. */
    *inOutIdx = idx;
    /* Return length if valid. */
    if (length > 0) {
        *len = length;
    }

    /* Return length calculated or error code. */
    return length;
}


/* Gets the tag of next BER/DER encoded item.
 *
 * Checks there is enough data in the buffer for the tag byte.
 *
 * @param [in]      input     BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of tag.
 *                            On out, end of parsed tag.
 * @param [out]     tag       Tag value found.
 * @param [in]      maxIdx    Maximum index of input data.
 *
 * return  0 on success
 * return  BAD_FUNC_ARG when tag, inOutIdx or input is NULL.
 * return  BUFFER_E when not enough space in buffer for tag.
 */
int GetASNTag(const byte* input, word32* inOutIdx, byte* tag, word32 maxIdx)
{
    int ret = 0;
    word32 idx = 0;

    /* Check validity of parameters. */
    if ((tag == NULL) || (inOutIdx == NULL) || (input == NULL)) {
        ret = BAD_FUNC_ARG;
    }
    if (ret == 0) {
        /* Get index and ensure space for tag. */
        idx = *inOutIdx;
        if (idx + ASN_TAG_SZ > maxIdx) {
            WOLFSSL_MSG("Buffer too small for ASN tag");
            ret = BUFFER_E;
        }
    }
    if (ret == 0) {
        /* Return the tag and the index after tag. */
        *tag = input[idx];
        *inOutIdx = idx + ASN_TAG_SZ;
    }
    /* Return error code. */
    return ret;
}


/* Decode the DER/BER header (Type-Length) and check the length when requested.
 *
 * BER/DER has Type-Length-Value triplets.
 * Check that the tag/type is the required value.
 * When requested will check that the Length decoded, indicating the number
 * of bytes in the Value, is available in the buffer after the Length bytes.
 *
 * Only supporting a length upto INT_MAX.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in]      tag       ASN.1 tag value expected in header.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @param [in]      check     Whether to check the buffer has at least the
 *                            decoded length of bytes remaining.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the expected tag is not found or length is invalid.
 */
static int GetASNHeader_ex(const byte* input, byte tag, word32* inOutIdx,
                           int* len, word32 maxIdx, int check)
{
    int    ret = 0;
    word32 idx = *inOutIdx;
    byte   tagFound;
    int    length = 0;

    /* Get tag/type. */
    if (GetASNTag(input, &idx, &tagFound, maxIdx) != 0) {
        ret = ASN_PARSE_E;
    }
    /* Ensure tag is the expected value. */
    if ((ret == 0) && (tagFound != tag)) {
        ret = ASN_PARSE_E;
    }
    /* Get the encoded length. */
    if ((ret == 0) && (GetLength_ex(input, &idx, &length, maxIdx, check) < 0)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* Return the length of data and index after header. */
        *len      = length;
        *inOutIdx = idx;
        ret = length;
    }
    /* Return number of data bytes or error code. */
    return ret;
}


/* Decode the DER/BER header (Type-Length) and check the length.
 *
 * BER/DER has Type-Length-Value triplets.
 * Check that the tag/type is the required value.
 * Checks that the Length decoded, indicating the number of bytes in the Value,
 * is available in the buffer after the Length bytes.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in]      tag       ASN.1 tag value expected in header.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the expected tag is not found or length is invalid.
 */
static int GetASNHeader(const byte* input, byte tag, word32* inOutIdx, int* len,
                        word32 maxIdx)
{
    return GetASNHeader_ex(input, tag, inOutIdx, len, maxIdx, 1);
}

#ifndef WOLFSSL_ASN_TEMPLATE
static int GetHeader(const byte* input, byte* tag, word32* inOutIdx, int* len,
                     word32 maxIdx, int check)
{
    word32 idx = *inOutIdx;
    int    length;

    if ((idx + 1) > maxIdx)
        return BUFFER_E;

    *tag = input[idx++];

    if (GetLength_ex(input, &idx, &length, maxIdx, check) < 0)
        return ASN_PARSE_E;

    *len      = length;
    *inOutIdx = idx;
    return length;
}
#endif

/* Decode the header of a BER/DER encoded SEQUENCE.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the tag is not a SEQUENCE or length is invalid.
 */
int GetSequence(const byte* input, word32* inOutIdx, int* len,
                           word32 maxIdx)
{
    return GetASNHeader(input, ASN_SEQUENCE | ASN_CONSTRUCTED, inOutIdx, len,
                        maxIdx);
}

/* Decode the header of a BER/DER encoded SEQUENCE.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @param [in]      check     Whether to check the buffer has at least the
 *                            decoded length of bytes remaining.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the tag is not a SEQUENCE or length is invalid.
 */
int GetSequence_ex(const byte* input, word32* inOutIdx, int* len,
                           word32 maxIdx, int check)
{
    return GetASNHeader_ex(input, ASN_SEQUENCE | ASN_CONSTRUCTED, inOutIdx, len,
                        maxIdx, check);
}

/* Decode the header of a BER/DER encoded SET.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the tag is not a SET or length is invalid.
 */
int GetSet(const byte* input, word32* inOutIdx, int* len,
                        word32 maxIdx)
{
    return GetASNHeader(input, ASN_SET | ASN_CONSTRUCTED, inOutIdx, len,
                        maxIdx);
}

/* Decode the header of a BER/DER encoded SET.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @param [in]      check     Whether to check the buffer has at least the
 *                            decoded length of bytes remaining.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the tag is not a SET or length is invalid.
 */
int GetSet_ex(const byte* input, word32* inOutIdx, int* len,
                        word32 maxIdx, int check)
{
    return GetASNHeader_ex(input, ASN_SET | ASN_CONSTRUCTED, inOutIdx, len,
                        maxIdx, check);
}

#if !defined(WOLFSSL_ASN_TEMPLATE) || defined(HAVE_OCSP)
/* Decode the BER/DER encoded NULL.
 *
 * No data in a NULL ASN.1 item.
 * Ensure that the all fields are as expected and move index past the element.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of NULL item.
 *                            On out, end of parsed NULL item.
 * @param [in]      maxIdx    Length of data in buffer.
 * @return  0 on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_TAG_NULL_E when the NULL tag is not found.
 * @return  ASN_EXPECT_0_E when the length is not zero.
 */
static int GetASNNull(const byte* input, word32* inOutIdx, word32 maxIdx)
{
    int ret = 0;
    word32 idx = *inOutIdx;

    /* Check buffer has enough data for a NULL item. */
    if ((idx + 2) > maxIdx) {
        ret = BUFFER_E;
    }
    /* Check the tag is NULL. */
    if ((ret == 0) && (input[idx++] != ASN_TAG_NULL)) {
        ret = ASN_TAG_NULL_E;
    }
    /* Check the length is zero. */
    if ((ret == 0) && (input[idx++] != 0)) {
        ret = ASN_EXPECT_0_E;
    }
    if (ret == 0) {
        /* Return the index after NULL tag. */
        *inOutIdx = idx;
    }
    /* Return error code. */
    return ret;
}
#endif

#ifndef WOLFSSL_ASN_TEMPLATE
/* Set the DER/BER encoding of the ASN.1 NULL element.
 *
 * output  Buffer to write into.
 * returns the number of bytes added to the buffer.
 */
static int SetASNNull(byte* output)
{
    output[0] = ASN_TAG_NULL;
    output[1] = 0;

    return 2;
}
#endif

#ifndef NO_CERTS
#ifndef WOLFSSL_ASN_TEMPLATE
/* Get the DER/BER encoding of an ASN.1 BOOLEAN.
 *
 * input     Buffer holding DER/BER encoded data.
 * inOutIdx  Current index into buffer to parse.
 * maxIdx    Length of data in buffer.
 * returns BUFFER_E when there is not enough data to parse.
 *         ASN_PARSE_E when the BOOLEAN tag is not found or length is not 1.
 *         Otherwise, 0 to indicate the value was false and 1 to indicate true.
 */
static int GetBoolean(const byte* input, word32* inOutIdx, word32 maxIdx)
{
    word32 idx = *inOutIdx;
    byte   b;

    if ((idx + 3) > maxIdx)
        return BUFFER_E;

    b = input[idx++];
    if (b != ASN_BOOLEAN)
        return ASN_PARSE_E;

    if (input[idx++] != 1)
        return ASN_PARSE_E;

    b = input[idx++] != 0;

    *inOutIdx = idx;
    return b;
}
#endif
#endif /* !NO_CERTS*/


/* Decode the header of a BER/DER encoded OCTET STRING.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @return  Number of bytes in the ASN.1 data on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the tag is not a OCTET STRING or length is invalid.
 */
int GetOctetString(const byte* input, word32* inOutIdx, int* len, word32 maxIdx)
{
    return GetASNHeader(input, ASN_OCTET_STRING, inOutIdx, len, maxIdx);
}

#ifndef WOLFSSL_ASN_TEMPLATE
/* Get the DER/BER encoding of an ASN.1 INTEGER header.
 *
 * Removes the leading zero byte when found.
 *
 * input     Buffer holding DER/BER encoded data.
 * inOutIdx  Current index into buffer to parse.
 * len       The number of bytes in the ASN.1 data (excluding any leading zero).
 * maxIdx    Length of data in buffer.
 * returns BUFFER_E when there is not enough data to parse.
 *         ASN_PARSE_E when the INTEGER tag is not found, length is invalid,
 *         or invalid use of or missing leading zero.
 *         Otherwise, 0 to indicate success.
 */
static int GetASNInt(const byte* input, word32* inOutIdx, int* len,
                     word32 maxIdx)
{
    int    ret;

    ret = GetASNHeader(input, ASN_INTEGER, inOutIdx, len, maxIdx);
    if (ret < 0)
        return ret;

    if (*len > 0) {

#ifndef WOLFSSL_ASN_INT_LEAD_0_ANY
        /* check for invalid padding on negative integer.
         * c.f. X.690 (ISO/IEC 8825-2:2003 (E)) 10.4.6; RFC 5280 4.1
         */
        if (*len > 1) {
            if ((input[*inOutIdx] == 0xff) && (input[*inOutIdx + 1] & 0x80))
                return ASN_PARSE_E;
        }
#endif

        /* remove leading zero, unless there is only one 0x00 byte */
        if ((input[*inOutIdx] == 0x00) && (*len > 1)) {
            (*inOutIdx)++;
            (*len)--;

#ifndef WOLFSSL_ASN_INT_LEAD_0_ANY
            if (*len > 0 && (input[*inOutIdx] & 0x80) == 0)
                return ASN_PARSE_E;
#endif
        }
    }

    return 0;
}

#ifndef NO_CERTS
/* Get the DER/BER encoding of an ASN.1 INTEGER that has a value of no more than
 * 7 bits.
 *
 * input     Buffer holding DER/BER encoded data.
 * inOutIdx  Current index into buffer to parse.
 * maxIdx    Length of data in buffer.
 * returns BUFFER_E when there is not enough data to parse.
 *         ASN_PARSE_E when the INTEGER tag is not found or length is invalid.
 *         Otherwise, the 7-bit value.
 */
static int GetInteger7Bit(const byte* input, word32* inOutIdx, word32 maxIdx)
{
    word32 idx = *inOutIdx;
    byte   b;

    if ((idx + 3) > maxIdx)
        return BUFFER_E;

    if (GetASNTag(input, &idx, &b, maxIdx) != 0)
        return ASN_PARSE_E;
    if (b != ASN_INTEGER)
        return ASN_PARSE_E;
    if (input[idx++] != 1)
        return ASN_PARSE_E;
    b = input[idx++];

    *inOutIdx = idx;
    return b;
}
#endif /* !NO_CERTS */
#endif /* !WOLFSSL_ASN_TEMPLATE */

#if !defined(NO_DSA) && !defined(NO_SHA)
static const char sigSha1wDsaName[] = "SHAwDSA";
static const char sigSha256wDsaName[] = "SHA256wDSA";
#endif /* NO_DSA */
#ifndef NO_RSA
#ifdef WOLFSSL_MD2
    static const char  sigMd2wRsaName[] = "md2WithRSAEncryption";
#endif
#ifndef NO_MD5
    static const char  sigMd5wRsaName[] = "md5WithRSAEncryption";
#endif
#ifndef NO_SHA
    static const char  sigSha1wRsaName[] = "sha1WithRSAEncryption";
#endif
#ifdef WOLFSSL_SHA224
    static const char sigSha224wRsaName[] = "sha224WithRSAEncryption";
#endif
#ifndef NO_SHA256
    static const char sigSha256wRsaName[] = "sha256WithRSAEncryption";
#endif
#ifdef WOLFSSL_SHA384
    static const char sigSha384wRsaName[] = "sha384WithRSAEncryption";
#endif
#ifdef WOLFSSL_SHA512
    static const char sigSha512wRsaName[] = "sha512WithRSAEncryption";
#endif
#ifdef WOLFSSL_SHA3
#ifndef WOLFSSL_NOSHA3_224
    static const char sigSha3_224wRsaName[] = "sha3_224WithRSAEncryption";
#endif
#ifndef WOLFSSL_NOSHA3_256
    static const char sigSha3_256wRsaName[] = "sha3_256WithRSAEncryption";
#endif
#ifndef WOLFSSL_NOSHA3_384
    static const char sigSha3_384wRsaName[] = "sha3_384WithRSAEncryption";
#endif
#ifndef WOLFSSL_NOSHA3_512
    static const char sigSha3_512wRsaName[] = "sha3_512WithRSAEncryption";
#endif
#endif
#endif /* NO_RSA */
#ifdef HAVE_ECC
#ifndef NO_SHA
    static const char sigSha1wEcdsaName[] = "SHAwECDSA";
#endif
#ifdef WOLFSSL_SHA224
    static const char sigSha224wEcdsaName[] = "SHA224wECDSA";
#endif
#ifndef NO_SHA256
    static const char sigSha256wEcdsaName[] = "SHA256wECDSA";
#endif
#ifdef WOLFSSL_SHA384
    static const char sigSha384wEcdsaName[] = "SHA384wECDSA";
#endif
#ifdef WOLFSSL_SHA512
    static const char sigSha512wEcdsaName[] = "SHA512wECDSA";
#endif
#ifdef WOLFSSL_SHA3
#ifndef WOLFSSL_NOSHA3_224
    static const char sigSha3_224wEcdsaName[] = "SHA3_224wECDSA";
#endif
#ifndef WOLFSSL_NOSHA3_256
    static const char sigSha3_256wEcdsaName[] = "SHA3_256wECDSA";
#endif
#ifndef WOLFSSL_NOSHA3_384
    static const char sigSha3_384wEcdsaName[] = "SHA3_384wECDSA";
#endif
#ifndef WOLFSSL_NOSHA3_512
    static const char sigSha3_512wEcdsaName[] = "SHA3_512wECDSA";
#endif
#endif
#endif /* HAVE_ECC */
static const char sigUnknownName[] = "Unknown";


/* Get the human readable string for a signature type
 *
 * oid  Oid value for signature
 */
const char* GetSigName(int oid) {
    switch (oid) {
    #if !defined(NO_DSA) && !defined(NO_SHA)
        case CTC_SHAwDSA:
            return sigSha1wDsaName;
        case CTC_SHA256wDSA:
            return sigSha256wDsaName;
    #endif /* NO_DSA && NO_SHA */
    #ifndef NO_RSA
        #ifdef WOLFSSL_MD2
        case CTC_MD2wRSA:
            return sigMd2wRsaName;
        #endif
        #ifndef NO_MD5
        case CTC_MD5wRSA:
            return sigMd5wRsaName;
        #endif
        #ifndef NO_SHA
        case CTC_SHAwRSA:
            return sigSha1wRsaName;
        #endif
        #ifdef WOLFSSL_SHA224
        case CTC_SHA224wRSA:
            return sigSha224wRsaName;
        #endif
        #ifndef NO_SHA256
        case CTC_SHA256wRSA:
            return sigSha256wRsaName;
        #endif
        #ifdef WOLFSSL_SHA384
        case CTC_SHA384wRSA:
            return sigSha384wRsaName;
        #endif
        #ifdef WOLFSSL_SHA512
        case CTC_SHA512wRSA:
            return sigSha512wRsaName;
        #endif
        #ifdef WOLFSSL_SHA3
        #ifndef WOLFSSL_NOSHA3_224
        case CTC_SHA3_224wRSA:
            return sigSha3_224wRsaName;
        #endif
        #ifndef WOLFSSL_NOSHA3_256
        case CTC_SHA3_256wRSA:
            return sigSha3_256wRsaName;
        #endif
        #ifndef WOLFSSL_NOSHA3_384
        case CTC_SHA3_384wRSA:
            return sigSha3_384wRsaName;
        #endif
        #ifndef WOLFSSL_NOSHA3_512
        case CTC_SHA3_512wRSA:
            return sigSha3_512wRsaName;
        #endif
        #endif
    #endif /* NO_RSA */
    #ifdef HAVE_ECC
        #ifndef NO_SHA
        case CTC_SHAwECDSA:
            return sigSha1wEcdsaName;
        #endif
        #ifdef WOLFSSL_SHA224
        case CTC_SHA224wECDSA:
            return sigSha224wEcdsaName;
        #endif
        #ifndef NO_SHA256
        case CTC_SHA256wECDSA:
            return sigSha256wEcdsaName;
        #endif
        #ifdef WOLFSSL_SHA384
        case CTC_SHA384wECDSA:
            return sigSha384wEcdsaName;
        #endif
        #ifdef WOLFSSL_SHA512
        case CTC_SHA512wECDSA:
            return sigSha512wEcdsaName;
        #endif
        #ifdef WOLFSSL_SHA3
        #ifndef WOLFSSL_NOSHA3_224
        case CTC_SHA3_224wECDSA:
            return sigSha3_224wEcdsaName;
        #endif
        #ifndef WOLFSSL_NOSHA3_256
        case CTC_SHA3_256wECDSA:
            return sigSha3_256wEcdsaName;
        #endif
        #ifndef WOLFSSL_NOSHA3_384
        case CTC_SHA3_384wECDSA:
            return sigSha3_384wEcdsaName;
        #endif
        #ifndef WOLFSSL_NOSHA3_512
        case CTC_SHA3_512wECDSA:
            return sigSha3_512wEcdsaName;
        #endif
        #endif
    #endif /* HAVE_ECC */
        default:
            return sigUnknownName;
    }
}


#if !defined(WOLFSSL_ASN_TEMPLATE) || defined(HAVE_PKCS7) || \
    defined(OPENSSL_EXTRA)
#if !defined(NO_DSA) || defined(HAVE_ECC) || !defined(NO_CERTS) || \
   (!defined(NO_RSA) && \
        (defined(WOLFSSL_CERT_GEN) || \
        ((defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA)) && !defined(HAVE_USER_RSA))))
/* Set the DER/BER encoding of the ASN.1 INTEGER header.
 *
 * When output is NULL, calculate the header length only.
 *
 * @param [in]  len        Length of INTEGER data in bytes.
 * @param [in]  firstByte  First byte of data, most significant byte of integer, 
 *                         to encode.
 * @param [out] output     Buffer to write into.
 * @return  Number of bytes added to the buffer.
 */
int SetASNInt(int len, byte firstByte, byte* output)
{
    word32 idx = 0;

    if (output) {
        /* Write out tag. */
        output[idx] = ASN_INTEGER;
    }
    /* Step over tag. */
    idx += ASN_TAG_SZ;
    /* Check if first byte has top bit set in which case a 0 is needed to
     * maintain positive value. */
    if (firstByte & 0x80) {
        /* Add pre-prepended byte to length of data in INTEGER. */
        len++;
    }
    /* Encode length - passing NULL for output will not encode. */
    idx += SetLength(len, output ? output + idx : NULL);
    /* Put out pre-pended 0 as well. */
    if (firstByte & 0x80) {
        if (output) {
            /* Write out 0 byte. */
            output[idx] = 0x00;
        }
        /* Update index. */
        idx++;
    }

    /* Return index after header. */
    return idx;
}
#endif
#endif

#ifndef WOLFSSL_ASN_TEMPLATE
#if !defined(NO_DSA) || defined(HAVE_ECC) || (defined(WOLFSSL_CERT_GEN) && \
    !defined(NO_RSA)) || ((defined(WOLFSSL_KEY_GEN) || \
    (!defined(NO_DH) && defined(WOLFSSL_DH_EXTRA)) || \
    defined(OPENSSL_EXTRA)) && !defined(NO_RSA) && !defined(HAVE_USER_RSA))
/* Set the DER/BER encoding of the ASN.1 INTEGER element with an mp_int.
 * The number is assumed to be positive.
 *
 * n       Multi-precision integer to encode.
 * maxSz   Maximum size of the encoded integer.
 *         A negative value indicates no check of length requested.
 * output  Buffer to write into.
 * returns BUFFER_E when the data is too long for the buffer.
 *         MP_TO_E when encoding the integer fails.
 *         Otherwise, the number of bytes added to the buffer.
 */
static int SetASNIntMP(mp_int* n, int maxSz, byte* output)
{
    int idx = 0;
    int leadingBit;
    int length;
    int err;

    leadingBit = mp_leading_bit(n);
    length = mp_unsigned_bin_size(n);
    if (maxSz >= 0 && (1 + length + (leadingBit ? 1 : 0)) > maxSz)
        return BUFFER_E;
    idx = SetASNInt(length, leadingBit ? 0x80 : 0x00, output);
    if (maxSz >= 0 && (idx + length) > maxSz)
        return BUFFER_E;

    if (output) {
        err = mp_to_unsigned_bin(n, output + idx);
        if (err != MP_OKAY)
            return MP_TO_E;
    }
    idx += length;

    return idx;
}
#endif

#if !defined(NO_RSA) && defined(HAVE_USER_RSA) && \
    (defined(WOLFSSL_CERT_GEN) || defined(OPENSSL_EXTRA))
/* Set the DER/BER encoding of the ASN.1 INTEGER element with an mp_int from
 * an RSA key.
 * The number is assumed to be positive.
 *
 * n       Multi-precision integer to encode.
 * output  Buffer to write into.
 * returns BUFFER_E when the data is too long for the buffer.
 *         MP_TO_E when encoding the integer fails.
 *         Otherwise, the number of bytes added to the buffer.
 */
static int SetASNIntRSA(void* n, byte* output)
{
    int idx = 0;
    int leadingBit;
    int length;
    int err;

    leadingBit = wc_Rsa_leading_bit(n);
    length = wc_Rsa_unsigned_bin_size(n);
    idx = SetASNInt(length, leadingBit ? 0x80 : 0x00, output);
    if ((idx + length) > MAX_RSA_INT_SZ)
        return BUFFER_E;

    if (output) {
        err = wc_Rsa_to_unsigned_bin(n, output + idx, length);
        if (err != MP_OKAY)
            return MP_TO_E;
    }
    idx += length;

    return idx;
}
#endif /* !NO_RSA && HAVE_USER_RSA && WOLFSSL_CERT_GEN */
#endif /* !WOLFSSL_ASN_TEMPLATE */

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for an INTEGER. */
static const ASNItem intASN[] = {
    { 0, ASN_INTEGER, 0, 0, 0 }
};

/* Number of items in ASN.1 template for an INTEGER. */
#define intASN_Length (sizeof(intASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Windows header clash for WinCE using GetVersion */
/* Decode Version - one byte INTEGER.
 *
 * @param [in]      input     Buffer of BER data.
 * @param [in, out] inOutIdx  On in, start of encoded Version.
 *                            On out, start of next encode ASN.1 item.
 * @param [out]     version   Number encoded in INTEGER.
 * @param [in]      maxIdx    Maximum index of data in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when encoding is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the most significant bit is set.
 */
int GetMyVersion(const byte* input, word32* inOutIdx,
                               int* version, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *inOutIdx;
    byte   tag;

    if ((idx + MIN_VERSION_SZ) > maxIdx)
        return ASN_PARSE_E;

    if (GetASNTag(input, &idx, &tag, maxIdx) != 0)
        return ASN_PARSE_E;

    if (tag != ASN_INTEGER)
        return ASN_PARSE_E;

    if (input[idx++] != 0x01)
        return ASN_VERSION_E;

    *version  = input[idx++];
    *inOutIdx = idx;

    return *version;
#else
    ASNGetData dataASN[intASN_Length];
    int ret;
    byte num;

    /* Clear dynamic data and set the version number variable. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_Int8Bit(&dataASN[0], &num);
    /* Decode the version (INTEGER). */
    ret = GetASN_Items(intASN, dataASN, intASN_Length, 0, input, inOutIdx,
                       maxIdx);
    if (ret == 0) {
        /* Return version through variable and return value. */
        *version = num;
        ret = num;
    }
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


#ifndef NO_PWDBASED
/* Decode small integer, 32 bits or less.
 *
 * @param [in]      input     Buffer of BER data.
 * @param [in, out] inOutIdx  On in, start of encoded INTEGER.
 *                            On out, start of next encode ASN.1 item.
 * @param [out]     number    Number encoded in INTEGER.
 * @param [in]      maxIdx    Maximum index of data in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when encoding is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the most significant bit is set.
 */
int GetShortInt(const byte* input, word32* inOutIdx, int* number, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *inOutIdx;
    word32 len;
    byte   tag;

    *number = 0;

    /* check for type and length bytes */
    if ((idx + 2) > maxIdx)
        return BUFFER_E;

    if (GetASNTag(input, &idx, &tag, maxIdx) != 0)
        return ASN_PARSE_E;

    if (tag != ASN_INTEGER)
        return ASN_PARSE_E;

    len = input[idx++];
    if (len > 4)
        return ASN_PARSE_E;

    if (len + idx > maxIdx)
        return ASN_PARSE_E;

    while (len--) {
        *number  = *number << 8 | input[idx++];
    }

    *inOutIdx = idx;

    return *number;
#else
    ASNGetData dataASN[intASN_Length];
    int ret;
    word32 num;

    /* Clear dynamic data and set the 32-bit number variable. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_Int32Bit(&dataASN[0], &num);
    /* Decode the short int (INTEGER). */
    ret = GetASN_Items(intASN, dataASN, intASN_Length, 0, input, inOutIdx,
                       maxIdx);
    if (ret == 0) {
        /* Return number through variable and return value. */
        *number = num;
        ret = num;
    }
    return ret;
#endif
}


#if !defined(WOLFSSL_ASN_TEMPLATE) || defined(HAVE_PKCS12)
/* Set small integer, 32 bits or less. DER encoding with no leading 0s
 * returns total amount written including ASN tag and length byte on success */
int SetShortInt(byte* input, word32* inOutIdx, word32 number, word32 maxIdx)
{
    word32 idx = *inOutIdx;
    word32 len = 0;
    int    i;
    byte ar[MAX_LENGTH_SZ];

    /* check for room for type and length bytes */
    if ((idx + 2) > maxIdx)
        return BUFFER_E;

    input[idx++] = ASN_INTEGER;
    idx++; /* place holder for length byte */
    if (MAX_LENGTH_SZ + idx > maxIdx)
        return ASN_PARSE_E;

    /* find first non zero byte */
    XMEMSET(ar, 0, MAX_LENGTH_SZ);
    c32toa(number, ar);
    for (i = 0; i < MAX_LENGTH_SZ; i++) {
        if (ar[i] != 0) {
            break;
        }
    }

    /* handle case of 0 */
    if (i == MAX_LENGTH_SZ) {
        input[idx++] = 0; len++;
    }

    for (; i < MAX_LENGTH_SZ && idx < maxIdx; i++) {
        input[idx++] = ar[i]; len++;
    }

    /* jump back to beginning of input buffer using unaltered inOutIdx value
     * and set number of bytes for integer, then update the index value */
    input[*inOutIdx + 1] = (byte)len;
    *inOutIdx = idx;

    return len + 2; /* size of integer bytes plus ASN TAG and length byte */
}
#endif /* !WOLFSSL_ASN_TEMPLATE */
#endif /* !NO_PWDBASED */

#ifndef WOLFSSL_ASN_TEMPLATE
/* May not have one, not an error */
static int GetExplicitVersion(const byte* input, word32* inOutIdx, int* version,
                              word32 maxIdx)
{
    word32 idx = *inOutIdx;
    byte tag;

    WOLFSSL_ENTER("GetExplicitVersion");

    if (GetASNTag(input, &idx, &tag, maxIdx) != 0)
        return ASN_PARSE_E;

    if (tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED)) {
        int ret;

        *inOutIdx = ++idx;  /* skip header */
        ret = GetMyVersion(input, inOutIdx, version, maxIdx);
        if (ret >= 0) {
            /* check if version is expected value rfc 5280 4.1 {0, 1, 2} */
            if (*version > MAX_X509_VERSION || *version < MIN_X509_VERSION) {
                WOLFSSL_MSG("Unexpected certificate version");
                ret = ASN_VERSION_E;
            }
        }
        return ret;
    }

    /* go back as is */
    *version = 0;

    return 0;
}
#endif

/* Decode small integer, 32 bits or less.
 *
 * mp_int is initialized.
 *
 * @param [out]     mpi       mp_int to hold number.
 * @param [in]      input     Buffer of BER data.
 * @param [in, out] inOutIdx  On in, start of encoded INTEGER.
 *                            On out, start of next encode ASN.1 item.
 * @param [in]      maxIdx    Maximum index of data in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when encoding is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the most significant bit is set.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 */
int GetInt(mp_int* mpi, const byte* input, word32* inOutIdx, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *inOutIdx;
    int    ret;
    int    length;

    ret = GetASNInt(input, &idx, &length, maxIdx);
    if (ret != 0)
        return ret;

    if (mp_init(mpi) != MP_OKAY)
        return MP_INIT_E;

    if (mp_read_unsigned_bin(mpi, input + idx, length) != 0) {
        mp_clear(mpi);
        return ASN_GETINT_E;
    }

#ifdef HAVE_WOLF_BIGINT
    if (wc_bigint_from_unsigned_bin(&mpi->raw, input + idx, length) != 0) {
        mp_clear(mpi);
        return ASN_GETINT_E;
    }
#endif /* HAVE_WOLF_BIGINT */

    *inOutIdx = idx + length;

    return 0;
#else
    ASNGetData dataASN[intASN_Length];

    /* Clear dynamic data and set the mp_int to fill with value. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_MP_PosNeg(&dataASN[0], mpi);
    /* Decode the big number (INTEGER). */
    return GetASN_Items(intASN, dataASN, intASN_Length, 0, input, inOutIdx,
                        maxIdx);
#endif
}

#ifndef WOLFSSL_ASN_TEMPLATE
#if (!defined(WOLFSSL_KEY_GEN) && !defined(OPENSSL_EXTRA) && defined(RSA_LOW_MEM)) \
    || defined(WOLFSSL_RSA_PUBLIC_ONLY) || (!defined(NO_DSA))
#if (!defined(NO_RSA) && !defined(HAVE_USER_RSA)) || !defined(NO_DSA)
static int SkipInt(const byte* input, word32* inOutIdx, word32 maxIdx)
{
    word32 idx = *inOutIdx;
    int    ret;
    int    length;

    ret = GetASNInt(input, &idx, &length, maxIdx);
    if (ret != 0)
        return ret;

    *inOutIdx = idx + length;

    return 0;
}
#endif
#endif
#endif /* !WOLFSSL_ASN_TEMPLATE */

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for a BIT_STRING. */
static const ASNItem bitStringASN[] = {
    { 0, ASN_BIT_STRING, 0, 1, 0 }
};

/* Number of items in ASN.1 template for a BIT_STRING. */
#define bitStringASN_Length (sizeof(bitStringASN) / sizeof(ASNItem))
#endif

/* Decode and check the BIT_STRING is valid. Return length and unused bits.
 *
 * @param [in]      input       Buffer holding BER encoding.
 * @param [in, out] inOutIdx    On in, start of BIT_STRING.
 *                              On out, start of ASN.1 item after BIT_STRING.
 * @param [out]     len         Length of BIT_STRING data.
 * @param [in]      maxIdx      Maximum index of data in buffer.
 * @param [in]      zeroBits    Indicates whether zero unused bits is expected.
 * @param [in]      unusedBits  Number of unused bits in last byte.
 * @return  0 on success.
 * @return  ASN_PARSE_E when encoding is invalid.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when unused bits is not zero when expected.
 */
int CheckBitString(const byte* input, word32* inOutIdx, int* len,
                          word32 maxIdx, int zeroBits, byte* unusedBits)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *inOutIdx;
    int    length;
    byte   b;

    if (GetASNTag(input, &idx, &b, maxIdx) != 0) {
        return ASN_BITSTR_E;
    }

    if (b != ASN_BIT_STRING) {
        return ASN_BITSTR_E;
    }

    if (GetLength(input, &idx, &length, maxIdx) < 0)
        return ASN_PARSE_E;

    /* extra sanity check that length is greater than 0 */
    if (length <= 0) {
        WOLFSSL_MSG("Error length was 0 in CheckBitString");
        return BUFFER_E;
    }

    if (idx + 1 > maxIdx) {
        WOLFSSL_MSG("Attempted buffer read larger than input buffer");
        return BUFFER_E;
    }

    b = input[idx];
    if (zeroBits && b != 0x00)
        return ASN_EXPECT_0_E;
    if (b >= 0x08)
        return ASN_PARSE_E;
    if (b != 0) {
        if ((byte)(input[idx + length - 1] << (8 - b)) != 0)
            return ASN_PARSE_E;
    }
    idx++;
    length--; /* length has been checked for greater than 0 */

    *inOutIdx = idx;
    if (len != NULL)
        *len = length;
    if (unusedBits != NULL)
        *unusedBits = b;

    return 0;
#else
    ASNGetData dataASN[bitStringASN_Length];
    int ret;
    int bits;

    /* Parse BIT_STRING and check validity of unused bits. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    /* Decode BIT_STRING. */
    ret = GetASN_Items(bitStringASN, dataASN, bitStringASN_Length, 0, input,
            inOutIdx, maxIdx);
    if (ret == 0) {
        /* Get unused bits from dynamic ASN.1 data. */
        bits = GetASNItem_UnusedBits(dataASN[0]);
        /* Check unused bits is 0 when expected. */
        if (zeroBits && (bits != 0)) {
            ret = ASN_EXPECT_0_E;
        }
    }
    if (ret == 0) {
        /* Return length of data and unused bits if required. */
        if (len != NULL) {
            *len = dataASN[0].data.ref.length;
        }
        if (unusedBits != NULL) {
            *unusedBits = bits;
        }
    }

    return ret;
#endif
}

/* RSA (with CertGen or KeyGen) OR ECC OR ED25519 OR ED448 (with CertGen or
 * KeyGen) */
#if (!defined(NO_RSA) && !defined(HAVE_USER_RSA) && \
        (defined(WOLFSSL_CERT_GEN) || defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA))) || \
    (defined(HAVE_ECC) && defined(HAVE_ECC_KEY_EXPORT)) || \
    ((defined(HAVE_ED25519) || defined(HAVE_ED448)) && \
        (defined(WOLFSSL_CERT_GEN) || defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA))) || \
    (!defined(NO_DSA) && !defined(HAVE_SELFTEST) && defined(WOLFSSL_KEY_GEN))

/* Set the DER/BER encoding of the ASN.1 BIT STRING header.
 *
 * When output is NULL, calculate the header length only.
 *
 * @param [in]  len         Length of BIT STRING data.
 *                          That is, the number of least significant zero bits
 *                          before a one.
 *                          The last byte is the most-significant non-zero byte
 *                          of a number.
 * @param [out] output      Buffer to write into.
 * @return  Number of bytes added to the buffer.
 */
word32 SetBitString(word32 len, byte unusedBits, byte* output)
{
    word32 idx = 0;

    if (output) {
        /* Write out tag. */
        output[idx] = ASN_BIT_STRING;
    }
    /* Step over tag. */
    idx += ASN_TAG_SZ;

    /* Encode length - passing NULL for output will not encode.
     * Add one to length for unsued bits. */
    idx += SetLength(len + 1, output ? output + idx : NULL);
    if (output) {
        /* Write out unused bits. */
        output[idx] = unusedBits;
    }
    /* Skip over unused bits. */
    idx++;

    /* Return index after header. */
    return idx;
}
#endif /* !NO_RSA || HAVE_ECC || HAVE_ED25519 || HAVE_ED448 */

#ifdef ASN_BER_TO_DER
/* Convert BER to DER */

/* Pull informtation from the ASN.1 BER encoded item header */
static int GetBerHeader(const byte* data, word32* idx, word32 maxIdx,
                        byte* pTag, word32* pLen, int* indef)
{
    int len = 0;
    byte tag;
    word32 i = *idx;

    *indef = 0;

    /* Check there is enough data for a minimal header */
    if (i + 2 > maxIdx) {
        return ASN_PARSE_E;
    }

    /* Retrieve tag */
    tag = data[i++];

    /* Indefinite length handled specially */
    if (data[i] == 0x80) {
        /* Check valid tag for indefinite */
        if (((tag & 0xc0) == 0) && ((tag & ASN_CONSTRUCTED) == 0x00)) {
            return ASN_PARSE_E;
        }
        i++;
        *indef = 1;
    }
    else if (GetLength(data, &i, &len, maxIdx) < 0) {
        return ASN_PARSE_E;
    }

    /* Return tag, length and index after BER item header */
    *pTag = tag;
    *pLen = len;
    *idx = i;
    return 0;
}

#ifndef INDEF_ITEMS_MAX
#define INDEF_ITEMS_MAX       20
#endif

/* Indef length item data */
typedef struct Indef {
    word32 start;
    int depth;
    int headerLen;
    word32 len;
} Indef;

/* Indef length items */
typedef struct IndefItems
{
    Indef len[INDEF_ITEMS_MAX];
    int cnt;
    int idx;
    int depth;
} IndefItems;


/* Get header length of current item */
static int IndefItems_HeaderLen(IndefItems* items)
{
    return items->len[items->idx].headerLen;
}

/* Get data length of current item */
static word32 IndefItems_Len(IndefItems* items)
{
    return items->len[items->idx].len;
}

/* Add a indefinite length item */
static int IndefItems_AddItem(IndefItems* items, word32 start)
{
    int ret = 0;
    int i;

    if (items->cnt == INDEF_ITEMS_MAX) {
        ret = MEMORY_E;
    }
    else {
        i = items->cnt++;
        items->len[i].start = start;
        items->len[i].depth = items->depth++;
        items->len[i].headerLen = 1;
        items->len[i].len = 0;
        items->idx = i;
    }

    return ret;
}

/* Increase data length of current item */
static void IndefItems_AddData(IndefItems* items, word32 length)
{
    items->len[items->idx].len += length;
}

/* Update header length of current item to reflect data length */
static void IndefItems_UpdateHeaderLen(IndefItems* items)
{
    items->len[items->idx].headerLen +=
                                    SetLength(items->len[items->idx].len, NULL);
}

/* Go to indefinite parent of current item */
static void IndefItems_Up(IndefItems* items)
{
    int i;
    int depth = items->len[items->idx].depth - 1;

    for (i = items->cnt - 1; i >= 0; i--) {
        if (items->len[i].depth == depth) {
            break;
        }
    }
    items->idx = i;
    items->depth = depth + 1;
}

/* Calculate final length by adding length of indefinite child items */
static void IndefItems_CalcLength(IndefItems* items)
{
    int i;
    int idx = items->idx;

    for (i = idx + 1; i < items->cnt; i++) {
        if (items->len[i].depth == items->depth) {
            items->len[idx].len += items->len[i].headerLen;
            items->len[idx].len += items->len[i].len;
        }
    }
    items->len[idx].headerLen += SetLength(items->len[idx].len, NULL);
}

/* Add more data to indefinite length item */
static void IndefItems_MoreData(IndefItems* items, word32 length)
{
    if (items->cnt > 0 && items->idx >= 0) {
        items->len[items->idx].len += length;
    }
}

/* Convert a BER encoding with indefinite length items to DER.
 *
 * ber    BER encoded data.
 * berSz  Length of BER encoded data.
 * der    Buffer to hold DER encoded version of data.
 *        NULL indicates only the length is required.
 * derSz  The size of the buffer to hold the DER encoded data.
 *        Will be set if der is NULL, otherwise the value is checked as der is
 *        filled.
 * returns ASN_PARSE_E if the BER data is invalid and BAD_FUNC_ARG if ber or
 * derSz are NULL.
 */
int wc_BerToDer(const byte* ber, word32 berSz, byte* der, word32* derSz)
{
    int ret = 0;
    word32 i, j;
#ifdef WOLFSSL_SMALL_STACK
    IndefItems* indefItems = NULL;
#else
    IndefItems indefItems[1];
#endif
    byte tag, basic;
    word32 length;
    int indef;

    if (ber == NULL || derSz == NULL)
        return BAD_FUNC_ARG;

#ifdef WOLFSSL_SMALL_STACK
    indefItems = (IndefItems *)XMALLOC(sizeof(IndefItems), NULL,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
    if (indefItems == NULL) {
        ret = MEMORY_E;
        goto end;
    }
#endif

    XMEMSET(indefItems, 0, sizeof(*indefItems));

    /* Calculate indefinite item lengths */
    for (i = 0; i < berSz; ) {
        word32 start = i;

        /* Get next BER item */
        ret = GetBerHeader(ber, &i, berSz, &tag, &length, &indef);
        if (ret != 0) {
            goto end;
        }

        if (indef) {
            /* Indefinite item - add to list */
            ret = IndefItems_AddItem(indefItems, i);
            if (ret != 0) {
                goto end;
            }

            if ((tag & 0xC0) == 0 &&
                tag != (ASN_SEQUENCE | ASN_CONSTRUCTED) &&
                tag != (ASN_SET      | ASN_CONSTRUCTED)) {
                /* Constructed basic type - get repeating tag */
                basic = tag & (~ASN_CONSTRUCTED);

                /* Add up lengths of each item below */
                for (; i < berSz; ) {
                    /* Get next BER_item */
                    ret = GetBerHeader(ber, &i, berSz, &tag, &length, &indef);
                    if (ret != 0) {
                        goto end;
                    }

                    /* End of content closes item */
                    if (tag == ASN_EOC) {
                        /* Must be zero length */
                        if (length != 0) {
                            ret = ASN_PARSE_E;
                            goto end;
                        }
                        break;
                    }

                    /* Must not be indefinite and tag must match parent */
                    if (indef || tag != basic) {
                        ret = ASN_PARSE_E;
                        goto end;
                    }

                    /* Add to length */
                    IndefItems_AddData(indefItems, length);
                    /* Skip data */
                    i += length;
                }

                /* Ensure we got an EOC and not end of data */
                if (tag != ASN_EOC) {
                    ret = ASN_PARSE_E;
                    goto end;
                }

                /* Set the header length to include the length field */
                IndefItems_UpdateHeaderLen(indefItems);
                /* Go to indefinte parent item */
                IndefItems_Up(indefItems);
            }
        }
        else if (tag == ASN_EOC) {
            /* End-of-content must be 0 length */
            if (length != 0) {
                ret = ASN_PARSE_E;
                goto end;
            }
            /* Check there is an item to close - missing EOC */
            if (indefItems->depth == 0) {
                ret = ASN_PARSE_E;
                goto end;
            }

            /* Finish calculation of data length for indefinite item */
            IndefItems_CalcLength(indefItems);
            /* Go to indefinte parent item */
            IndefItems_Up(indefItems);
        }
        else {
            /* Known length item to add in - make sure enough data for it */
            if (i + length > berSz) {
                ret = ASN_PARSE_E;
                goto end;
            }

            /* Include all data - can't have indefinite inside definite */
            i += length;
            /* Add entire item to current indefinite item */
            IndefItems_MoreData(indefItems, i - start);
        }
    }
    /* Check we had a EOC for each indefinite item */
    if (indefItems->depth != 0) {
        ret = ASN_PARSE_E;
        goto end;
    }

    /* Write out DER */

    j = 0;
    /* Reset index */
    indefItems->idx = 0;
    for (i = 0; i < berSz; ) {
        word32 start = i;

        /* Get item - checked above */
        (void)GetBerHeader(ber, &i, berSz, &tag, &length, &indef);
        if (indef) {
            if (der != NULL) {
                /* Check enough space for header */
                if (j + IndefItems_HeaderLen(indefItems) > *derSz) {
                    ret = BUFFER_E;
                    goto end;
                }

                if ((tag & 0xC0) == 0 &&
                    tag != (ASN_SEQUENCE | ASN_CONSTRUCTED) &&
                    tag != (ASN_SET      | ASN_CONSTRUCTED)) {
                    /* Remove constructed tag for basic types */
                    tag &= ~ASN_CONSTRUCTED;
                }
                /* Add tag and length */
                der[j] = tag;
                (void)SetLength(IndefItems_Len(indefItems), der + j + 1);
            }
            /* Add header length of indefinite item */
            j += IndefItems_HeaderLen(indefItems);

            if ((tag & 0xC0) == 0 &&
                tag != (ASN_SEQUENCE | ASN_CONSTRUCTED) &&
                tag != (ASN_SET      | ASN_CONSTRUCTED)) {
                /* For basic type - get each child item and add data */
                for (; i < berSz; ) {
                    (void)GetBerHeader(ber, &i, berSz, &tag, &length, &indef);
                    if (tag == ASN_EOC) {
                        break;
                    }
                    if (der != NULL) {
                        if (j + length > *derSz) {
                            ret = BUFFER_E;
                            goto end;
                        }
                        XMEMCPY(der + j, ber + i, length);
                    }
                    j += length;
                    i += length;
                }
            }

            /* Move to next indef item in list */
            indefItems->idx++;
        }
        else if (tag == ASN_EOC) {
            /* End-Of-Content is not written out in DER */
        }
        else {
            /* Write out definite length item as is. */
            i += length;
            if (der != NULL) {
                /* Ensure space for item */
                if (j + i - start > *derSz) {
                    ret = BUFFER_E;
                    goto end;
                }
                /* Copy item as is */
                XMEMCPY(der + j, ber + start, i - start);
            }
            j += i - start;
        }
    }

    /* Return the length of the DER encoded ASN.1 */
    *derSz = j;
    if (der == NULL) {
        ret = LENGTH_ONLY_E;
    }
end:
#ifdef WOLFSSL_SMALL_STACK
    if (indefItems != NULL) {
        XFREE(indefItems, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    }
#endif
    return ret;
}
#endif

#ifndef WOLFSSL_ASN_TEMPLATE
#if defined(WOLFSSL_CERT_EXT) && defined(WOLFSSL_CERT_GEN)
/* Set the DER/BER encoding of the ASN.1 BIT_STRING with a 16-bit value.
 *
 * val         16-bit value to encode.
 * output      Buffer to write into.
 * returns the number of bytes added to the buffer.
 */
static word32 SetBitString16Bit(word16 val, byte* output)
{
    word32 idx;
    int    len;
    byte   lastByte;
    byte   unusedBits = 0;

    if ((val >> 8) != 0) {
        len = 2;
        lastByte = (byte)(val >> 8);
    }
    else {
        len = 1;
        lastByte = (byte)val;
    }

    while (((lastByte >> unusedBits) & 0x01) == 0x00)
        unusedBits++;

    idx = SetBitString(len, unusedBits, output);
    output[idx++] = (byte)val;
    if (len > 1)
        output[idx++] = (byte)(val >> 8);

    return idx;
}
#endif /* WOLFSSL_CERT_EXT || WOLFSSL_CERT_GEN */
#endif /* !WOLFSSL_ASN_TEMPLATE */

/* hashType */
#ifdef WOLFSSL_MD2
    static const byte hashMd2hOid[] = {42, 134, 72, 134, 247, 13, 2, 2};
#endif
#ifndef NO_MD5
    static const byte hashMd5hOid[] = {42, 134, 72, 134, 247, 13, 2, 5};
#endif
#ifndef NO_SHA
    static const byte hashSha1hOid[] = {43, 14, 3, 2, 26};
#endif
#ifdef WOLFSSL_SHA224
    static const byte hashSha224hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 4};
#endif
#ifndef NO_SHA256
    static const byte hashSha256hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 1};
#endif
#ifdef WOLFSSL_SHA384
    static const byte hashSha384hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 2};
#endif
#ifdef WOLFSSL_SHA512
    static const byte hashSha512hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 3};
#endif
#ifdef WOLFSSL_SHA3
#ifndef WOLFSSL_NOSHA3_224
    static const byte hashSha3_224hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 7};
#endif /* WOLFSSL_NOSHA3_224 */
#ifndef WOLFSSL_NOSHA3_256
    static const byte hashSha3_256hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 8};
#endif /* WOLFSSL_NOSHA3_256 */
#ifndef WOLFSSL_NOSHA3_384
    static const byte hashSha3_384hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 9};
#endif /* WOLFSSL_NOSHA3_384 */
#ifndef WOLFSSL_NOSHA3_512
    static const byte hashSha3_512hOid[] = {96, 134, 72, 1, 101, 3, 4, 2, 10};
#endif /* WOLFSSL_NOSHA3_512 */
#endif /* WOLFSSL_SHA3 */

/* hmacType */
#ifndef NO_HMAC
    #ifdef WOLFSSL_SHA224
    static const byte hmacSha224Oid[] = {42, 134, 72, 134, 247, 13, 2, 8};
    #endif
    #ifndef NO_SHA256
    static const byte hmacSha256Oid[] = {42, 134, 72, 134, 247, 13, 2, 9};
    #endif
    #ifdef WOLFSSL_SHA384
    static const byte hmacSha384Oid[] = {42, 134, 72, 134, 247, 13, 2, 10};
    #endif
    #ifdef WOLFSSL_SHA512
    static const byte hmacSha512Oid[] = {42, 134, 72, 134, 247, 13, 2, 11};
    #endif
#endif

/* sigType */
#if !defined(NO_DSA) && !defined(NO_SHA)
    static const byte sigSha1wDsaOid[] = {42, 134, 72, 206, 56, 4, 3};
    static const byte sigSha256wDsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 2};
#endif /* NO_DSA */
#ifndef NO_RSA
    #ifdef WOLFSSL_MD2
    static const byte sigMd2wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1, 2};
    #endif
    #ifndef NO_MD5
    static const byte sigMd5wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1, 4};
    #endif
    #ifndef NO_SHA
    static const byte sigSha1wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1, 5};
    #endif
    #ifdef WOLFSSL_SHA224
    static const byte sigSha224wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1,14};
    #endif
    #ifndef NO_SHA256
    static const byte sigSha256wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1,11};
    #endif
    #ifdef WOLFSSL_SHA384
    static const byte sigSha384wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1,12};
    #endif
    #ifdef WOLFSSL_SHA512
    static const byte sigSha512wRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1,13};
    #endif
    #ifdef WOLFSSL_SHA3
    #ifndef WOLFSSL_NOSHA3_224
    static const byte sigSha3_224wRsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 13};
    #endif
    #ifndef WOLFSSL_NOSHA3_256
    static const byte sigSha3_256wRsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 14};
    #endif
    #ifndef WOLFSSL_NOSHA3_384
    static const byte sigSha3_384wRsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 15};
    #endif
    #ifndef WOLFSSL_NOSHA3_512
    static const byte sigSha3_512wRsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 16};
    #endif
    #endif
#endif /* NO_RSA */
#ifdef HAVE_ECC
    #ifndef NO_SHA
    static const byte sigSha1wEcdsaOid[] = {42, 134, 72, 206, 61, 4, 1};
    #endif
    #ifdef WOLFSSL_SHA224
    static const byte sigSha224wEcdsaOid[] = {42, 134, 72, 206, 61, 4, 3, 1};
    #endif
    #ifndef NO_SHA256
    static const byte sigSha256wEcdsaOid[] = {42, 134, 72, 206, 61, 4, 3, 2};
    #endif
    #ifdef WOLFSSL_SHA384
    static const byte sigSha384wEcdsaOid[] = {42, 134, 72, 206, 61, 4, 3, 3};
    #endif
    #ifdef WOLFSSL_SHA512
    static const byte sigSha512wEcdsaOid[] = {42, 134, 72, 206, 61, 4, 3, 4};
    #endif
    #ifdef WOLFSSL_SHA3
    #ifndef WOLFSSL_NOSHA3_224
    static const byte sigSha3_224wEcdsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 9};
    #endif
    #ifndef WOLFSSL_NOSHA3_256
    static const byte sigSha3_256wEcdsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 10};
    #endif
    #ifndef WOLFSSL_NOSHA3_384
    static const byte sigSha3_384wEcdsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 11};
    #endif
    #ifndef WOLFSSL_NOSHA3_512
    static const byte sigSha3_512wEcdsaOid[] = {96, 134, 72, 1, 101, 3, 4, 3, 12};
    #endif
    #endif
#endif /* HAVE_ECC */
#ifdef HAVE_ED25519
    static const byte sigEd25519Oid[] = {43, 101, 112};
#endif /* HAVE_ED25519 */
#ifdef HAVE_ED448
    static const byte sigEd448Oid[] = {43, 101, 113};
#endif /* HAVE_ED448 */

/* keyType */
#ifndef NO_DSA
    static const byte keyDsaOid[] = {42, 134, 72, 206, 56, 4, 1};
#endif /* NO_DSA */
#ifndef NO_RSA
    static const byte keyRsaOid[] = {42, 134, 72, 134, 247, 13, 1, 1, 1};
#endif /* NO_RSA */
#ifdef HAVE_NTRU
    static const byte keyNtruOid[] = {43, 6, 1, 4, 1, 193, 22, 1, 1, 1, 1};
#endif /* HAVE_NTRU */
#ifdef HAVE_ECC
    static const byte keyEcdsaOid[] = {42, 134, 72, 206, 61, 2, 1};
#endif /* HAVE_ECC */
#ifdef HAVE_ED25519
    static const byte keyEd25519Oid[] = {43, 101, 112};
#endif /* HAVE_ED25519 */
#ifdef HAVE_CURVE25519
    static const byte keyCurve25519Oid[] = {43, 101, 110};
#endif
#ifdef HAVE_ED448
    static const byte keyEd448Oid[] = {43, 101, 113};
#endif /* HAVE_ED448 */
#ifdef HAVE_CURVE448
    static const byte keyCurve448Oid[] = {43, 101, 111};
#endif /* HAVE_CURVE448 */
#ifndef NO_DH
    static const byte keyDhOid[] = {42, 134, 72, 134, 247, 13, 1, 3, 1};
#endif /* !NO_DH */

/* curveType */
#ifdef HAVE_ECC
    /* See "ecc_sets" table in ecc.c */
#endif /* HAVE_ECC */

#ifdef HAVE_AES_CBC
/* blkType */
    #ifdef WOLFSSL_AES_128
    static const byte blkAes128CbcOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 2};
    #endif
    #ifdef WOLFSSL_AES_192
    static const byte blkAes192CbcOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 22};
    #endif
    #ifdef WOLFSSL_AES_256
    static const byte blkAes256CbcOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 42};
    #endif
#endif /* HAVE_AES_CBC */
#ifdef HAVE_AESGCM
    #ifdef WOLFSSL_AES_128
    static const byte blkAes128GcmOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 6};
    #endif
    #ifdef WOLFSSL_AES_192
    static const byte blkAes192GcmOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 26};
    #endif
    #ifdef WOLFSSL_AES_256
    static const byte blkAes256GcmOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 46};
    #endif
#endif /* HAVE_AESGCM */
#ifdef HAVE_AESCCM
    #ifdef WOLFSSL_AES_128
    static const byte blkAes128CcmOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 7};
    #endif
    #ifdef WOLFSSL_AES_192
    static const byte blkAes192CcmOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 27};
    #endif
    #ifdef WOLFSSL_AES_256
    static const byte blkAes256CcmOid[] = {96, 134, 72, 1, 101, 3, 4, 1, 47};
    #endif
#endif /* HAVE_AESCCM */

#ifndef NO_DES3
    static const byte blkDesCbcOid[]  = {43, 14, 3, 2, 7};
    static const byte blkDes3CbcOid[] = {42, 134, 72, 134, 247, 13, 3, 7};
#endif

/* keyWrapType */
#ifdef WOLFSSL_AES_128
    static const byte wrapAes128Oid[] = {96, 134, 72, 1, 101, 3, 4, 1, 5};
#endif
#ifdef WOLFSSL_AES_192
    static const byte wrapAes192Oid[] = {96, 134, 72, 1, 101, 3, 4, 1, 25};
#endif
#ifdef WOLFSSL_AES_256
    static const byte wrapAes256Oid[] = {96, 134, 72, 1, 101, 3, 4, 1, 45};
#endif
#ifdef HAVE_PKCS7
/* From RFC 3211 */
static const byte wrapPwriKekOid[] = {42, 134, 72, 134, 247, 13, 1, 9, 16, 3,9};
#endif

/* cmsKeyAgreeType */
#ifndef NO_SHA
    static const byte dhSinglePass_stdDH_sha1kdf_Oid[]   =
                                          {43, 129, 5, 16, 134, 72, 63, 0, 2};
#endif
#ifdef WOLFSSL_SHA224
    static const byte dhSinglePass_stdDH_sha224kdf_Oid[] = {43, 129, 4, 1, 11, 0};
#endif
#ifndef NO_SHA256
    static const byte dhSinglePass_stdDH_sha256kdf_Oid[] = {43, 129, 4, 1, 11, 1};
#endif
#ifdef WOLFSSL_SHA384
    static const byte dhSinglePass_stdDH_sha384kdf_Oid[] = {43, 129, 4, 1, 11, 2};
#endif
#ifdef WOLFSSL_SHA512
    static const byte dhSinglePass_stdDH_sha512kdf_Oid[] = {43, 129, 4, 1, 11, 3};
#endif

/* ocspType */
#ifdef HAVE_OCSP
    static const byte ocspBasicOid[]    = {43, 6, 1, 5, 5, 7, 48, 1, 1};
    static const byte ocspNonceOid[]    = {43, 6, 1, 5, 5, 7, 48, 1, 2};
    static const byte ocspNoCheckOid[]  = {43, 6, 1, 5, 5, 7, 48, 1, 5};
#endif /* HAVE_OCSP */

/* certExtType */
static const byte extBasicCaOid[] = {85, 29, 19};
static const byte extAltNamesOid[] = {85, 29, 17};
static const byte extCrlDistOid[] = {85, 29, 31};
static const byte extAuthInfoOid[] = {43, 6, 1, 5, 5, 7, 1, 1};
static const byte extAuthKeyOid[] = {85, 29, 35};
static const byte extSubjKeyOid[] = {85, 29, 14};
static const byte extCertPolicyOid[] = {85, 29, 32};
static const byte extKeyUsageOid[] = {85, 29, 15};
static const byte extInhibitAnyOid[] = {85, 29, 54};
static const byte extExtKeyUsageOid[] = {85, 29, 37};
#ifndef IGNORE_NAME_CONSTRAINTS
    static const byte extNameConsOid[] = {85, 29, 30};
#endif
#ifdef HAVE_CRL
static const byte extCrlNumberOid[] = {85, 29, 20};
#endif

/* certAuthInfoType */
static const byte extAuthInfoOcspOid[] = {43, 6, 1, 5, 5, 7, 48, 1};
static const byte extAuthInfoCaIssuerOid[] = {43, 6, 1, 5, 5, 7, 48, 2};

/* certPolicyType */
static const byte extCertPolicyAnyOid[] = {85, 29, 32, 0};

/* certAltNameType */
static const byte extAltNamesHwNameOid[] = {43, 6, 1, 5, 5, 7, 8, 4};

/* certKeyUseType */
static const byte extExtKeyUsageAnyOid[] = {85, 29, 37, 0};
static const byte extExtKeyUsageServerAuthOid[]   = {43, 6, 1, 5, 5, 7, 3, 1};
static const byte extExtKeyUsageClientAuthOid[]   = {43, 6, 1, 5, 5, 7, 3, 2};
static const byte extExtKeyUsageCodeSigningOid[]  = {43, 6, 1, 5, 5, 7, 3, 3};
static const byte extExtKeyUsageEmailProtectOid[] = {43, 6, 1, 5, 5, 7, 3, 4};
static const byte extExtKeyUsageTimestampOid[]    = {43, 6, 1, 5, 5, 7, 3, 8};
static const byte extExtKeyUsageOcspSignOid[]     = {43, 6, 1, 5, 5, 7, 3, 9};

#ifdef WOLFSSL_CERT_REQ
/* csrAttrType */
static const byte attrChallengePasswordOid[] = {42, 134, 72, 134, 247, 13, 1, 9, 7};
static const byte attrExtensionRequestOid[] = {42, 134, 72, 134, 247, 13, 1, 9, 14};
static const byte attrSerialNumberOid[] = {85, 4, 5};
#endif

/* kdfType */
static const byte pbkdf2Oid[] = {42, 134, 72, 134, 247, 13, 1, 5, 12};

/* PKCS5 */
#if !defined(NO_DES3) && !defined(NO_MD5)
static const byte pbeMd5Des[] = {42, 134, 72, 134, 247, 13, 1, 5, 3};
#endif
#if !defined(NO_DES3) && !defined(NO_SHA)
static const byte pbeSha1Des[] = {42, 134, 72, 134, 247, 13, 1, 5, 10};
#endif
static const byte pbes2[] = {42, 134, 72, 134, 247, 13, 1, 5, 13};

/* PKCS12 */
#if !defined(NO_RC4) && !defined(NO_SHA)
static const byte pbeSha1RC4128[] = {42, 134, 72, 134, 247, 13, 1, 12, 1, 1};
#endif
#if !defined(NO_DES3) && !defined(NO_SHA)
static const byte pbeSha1Des3[] = {42, 134, 72, 134, 247, 13, 1, 12, 1, 3};
#endif

#ifdef HAVE_LIBZ
/* zlib compression */
static const byte zlibCompress[] = {42, 134, 72, 134, 247, 13, 1, 9, 16, 3, 8};
#endif
#ifdef WOLFSSL_APACHE_HTTPD
/* tlsExtType */
static const byte tlsFeatureOid[] = {43, 6, 1, 5, 5, 7, 1, 24};
/* certNameType */
static const byte dnsSRVOid[] = {43, 6, 1, 5, 5, 7, 8, 7};
#endif


/* Looks up the ID/type of an OID.
 *
 * When known returns the OID as a byte array and its length.
 * ID-type are unique.
 *
 * Use oidIgnoreType to autofail.
 *
 * @param [in]  id     OID id.
 * @param [in]  type   Type of OID (enum Oid_Types).
 * @param [out] oidSz  Length of OID byte array returned.
 * @return  Array of bytes for the OID.
 * @return  NULL when ID/type not recognized.
 */
const byte* OidFromId(word32 id, word32 type, word32* oidSz)
{
    const byte* oid = NULL;

    *oidSz = 0;

    switch (type) {

        case oidHashType:
            switch (id) {
            #ifdef WOLFSSL_MD2
                case MD2h:
                    oid = hashMd2hOid;
                    *oidSz = sizeof(hashMd2hOid);
                    break;
            #endif
            #ifndef NO_MD5
                case MD5h:
                    oid = hashMd5hOid;
                    *oidSz = sizeof(hashMd5hOid);
                    break;
            #endif
            #ifndef NO_SHA
                case SHAh:
                    oid = hashSha1hOid;
                    *oidSz = sizeof(hashSha1hOid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA224
                case SHA224h:
                    oid = hashSha224hOid;
                    *oidSz = sizeof(hashSha224hOid);
                    break;
            #endif
            #ifndef NO_SHA256
                case SHA256h:
                    oid = hashSha256hOid;
                    *oidSz = sizeof(hashSha256hOid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA384
                case SHA384h:
                    oid = hashSha384hOid;
                    *oidSz = sizeof(hashSha384hOid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA512
                case SHA512h:
                    oid = hashSha512hOid;
                    *oidSz = sizeof(hashSha512hOid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA3
            #ifndef WOLFSSL_NOSHA3_224
                case SHA3_224h:
                    oid = hashSha3_224hOid;
                    *oidSz = sizeof(hashSha3_224hOid);
                    break;
            #endif /* WOLFSSL_NOSHA3_224 */
            #ifndef WOLFSSL_NOSHA3_256
                case SHA3_256h:
                    oid = hashSha3_256hOid;
                    *oidSz = sizeof(hashSha3_256hOid);
                    break;
            #endif /* WOLFSSL_NOSHA3_256 */
            #ifndef WOLFSSL_NOSHA3_384
                case SHA3_384h:
                    oid = hashSha3_384hOid;
                    *oidSz = sizeof(hashSha3_384hOid);
                    break;
            #endif /* WOLFSSL_NOSHA3_384 */
            #ifndef WOLFSSL_NOSHA3_512
                case SHA3_512h:
                    oid = hashSha3_512hOid;
                    *oidSz = sizeof(hashSha3_512hOid);
                    break;
            #endif /* WOLFSSL_NOSHA3_512 */
            #endif /* WOLFSSL_SHA3 */
                default:
                    break;
            }
            break;

        case oidSigType:
            switch (id) {
                #if !defined(NO_DSA) && !defined(NO_SHA)
                case CTC_SHAwDSA:
                    oid = sigSha1wDsaOid;
                    *oidSz = sizeof(sigSha1wDsaOid);
                    break;
                case CTC_SHA256wDSA:
                    oid = sigSha256wDsaOid;
                    *oidSz = sizeof(sigSha256wDsaOid);
                    break;
                #endif /* NO_DSA */
                #ifndef NO_RSA
                #ifdef WOLFSSL_MD2
                case CTC_MD2wRSA:
                    oid = sigMd2wRsaOid;
                    *oidSz = sizeof(sigMd2wRsaOid);
                    break;
                #endif
                #ifndef NO_MD5
                case CTC_MD5wRSA:
                    oid = sigMd5wRsaOid;
                    *oidSz = sizeof(sigMd5wRsaOid);
                    break;
                #endif
                #ifndef NO_SHA
                case CTC_SHAwRSA:
                    oid = sigSha1wRsaOid;
                    *oidSz = sizeof(sigSha1wRsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA224
                case CTC_SHA224wRSA:
                    oid = sigSha224wRsaOid;
                    *oidSz = sizeof(sigSha224wRsaOid);
                    break;
                #endif
                #ifndef NO_SHA256
                case CTC_SHA256wRSA:
                    oid = sigSha256wRsaOid;
                    *oidSz = sizeof(sigSha256wRsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA384
                case CTC_SHA384wRSA:
                    oid = sigSha384wRsaOid;
                    *oidSz = sizeof(sigSha384wRsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA512
                case CTC_SHA512wRSA:
                    oid = sigSha512wRsaOid;
                    *oidSz = sizeof(sigSha512wRsaOid);
                    break;
                #endif /* WOLFSSL_SHA512 */
                #ifdef WOLFSSL_SHA3
                #ifndef WOLFSSL_NOSHA3_224
                case CTC_SHA3_224wRSA:
                    oid = sigSha3_224wRsaOid;
                    *oidSz = sizeof(sigSha3_224wRsaOid);
                    break;
                #endif
                #ifndef WOLFSSL_NOSHA3_256
                case CTC_SHA3_256wRSA:
                    oid = sigSha3_256wRsaOid;
                    *oidSz = sizeof(sigSha3_256wRsaOid);
                    break;
                #endif
                #ifndef WOLFSSL_NOSHA3_384
                case CTC_SHA3_384wRSA:
                    oid = sigSha3_384wRsaOid;
                    *oidSz = sizeof(sigSha3_384wRsaOid);
                    break;
                #endif
                #ifndef WOLFSSL_NOSHA3_512
                case CTC_SHA3_512wRSA:
                    oid = sigSha3_512wRsaOid;
                    *oidSz = sizeof(sigSha3_512wRsaOid);
                    break;
                #endif
                #endif
                #endif /* NO_RSA */
                #ifdef HAVE_ECC
                #ifndef NO_SHA
                case CTC_SHAwECDSA:
                    oid = sigSha1wEcdsaOid;
                    *oidSz = sizeof(sigSha1wEcdsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA224
                case CTC_SHA224wECDSA:
                    oid = sigSha224wEcdsaOid;
                    *oidSz = sizeof(sigSha224wEcdsaOid);
                    break;
                #endif
                #ifndef NO_SHA256
                case CTC_SHA256wECDSA:
                    oid = sigSha256wEcdsaOid;
                    *oidSz = sizeof(sigSha256wEcdsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA384
                case CTC_SHA384wECDSA:
                    oid = sigSha384wEcdsaOid;
                    *oidSz = sizeof(sigSha384wEcdsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA512
                case CTC_SHA512wECDSA:
                    oid = sigSha512wEcdsaOid;
                    *oidSz = sizeof(sigSha512wEcdsaOid);
                    break;
                #endif
                #ifdef WOLFSSL_SHA3
                #ifndef WOLFSSL_NOSHA3_224
                case CTC_SHA3_224wECDSA:
                    oid = sigSha3_224wEcdsaOid;
                    *oidSz = sizeof(sigSha3_224wEcdsaOid);
                    break;
                #endif
                #ifndef WOLFSSL_NOSHA3_256
                case CTC_SHA3_256wECDSA:
                    oid = sigSha3_256wEcdsaOid;
                    *oidSz = sizeof(sigSha3_256wEcdsaOid);
                    break;
                #endif
                #ifndef WOLFSSL_NOSHA3_384
                case CTC_SHA3_384wECDSA:
                    oid = sigSha3_384wEcdsaOid;
                    *oidSz = sizeof(sigSha3_384wEcdsaOid);
                    break;
                #endif
                #ifndef WOLFSSL_NOSHA3_512
                case CTC_SHA3_512wECDSA:
                    oid = sigSha3_512wEcdsaOid;
                    *oidSz = sizeof(sigSha3_512wEcdsaOid);
                    break;
                #endif
                #endif
                #endif /* HAVE_ECC */
                #ifdef HAVE_ED25519
                case CTC_ED25519:
                    oid = sigEd25519Oid;
                    *oidSz = sizeof(sigEd25519Oid);
                    break;
                #endif
                #ifdef HAVE_ED448
                case CTC_ED448:
                    oid = sigEd448Oid;
                    *oidSz = sizeof(sigEd448Oid);
                    break;
                #endif
                default:
                    break;
            }
            break;

        case oidKeyType:
            switch (id) {
                #ifndef NO_DSA
                case DSAk:
                    oid = keyDsaOid;
                    *oidSz = sizeof(keyDsaOid);
                    break;
                #endif /* NO_DSA */
                #ifndef NO_RSA
                case RSAk:
                    oid = keyRsaOid;
                    *oidSz = sizeof(keyRsaOid);
                    break;
                #endif /* NO_RSA */
                #ifdef HAVE_NTRU
                case NTRUk:
                    oid = keyNtruOid;
                    *oidSz = sizeof(keyNtruOid);
                    break;
                #endif /* HAVE_NTRU */
                #ifdef HAVE_ECC
                case ECDSAk:
                    oid = keyEcdsaOid;
                    *oidSz = sizeof(keyEcdsaOid);
                    break;
                #endif /* HAVE_ECC */
                #ifdef HAVE_ED25519
                case ED25519k:
                    oid = keyEd25519Oid;
                    *oidSz = sizeof(keyEd25519Oid);
                    break;
                #endif /* HAVE_ED25519 */
                #ifdef HAVE_CURVE25519
                case X25519k:
                    oid = keyCurve25519Oid;
                    *oidSz = sizeof(keyCurve25519Oid);
                    break;
                #endif /* HAVE_CURVE25519 */
                #ifdef HAVE_ED448
                case ED448k:
                    oid = keyEd448Oid;
                    *oidSz = sizeof(keyEd448Oid);
                    break;
                #endif /* HAVE_ED448 */
                #ifdef HAVE_CURVE448
                case X448k:
                    oid = keyCurve448Oid;
                    *oidSz = sizeof(keyCurve448Oid);
                    break;
                #endif /* HAVE_CURVE448 */
                #ifndef NO_DH
                case DHk:
                    oid = keyDhOid;
                    *oidSz = sizeof(keyDhOid);
                    break;
                #endif /* !NO_DH */
                default:
                    break;
            }
            break;

        #ifdef HAVE_ECC
        case oidCurveType:
            if (wc_ecc_get_oid(id, &oid, oidSz) < 0) {
                WOLFSSL_MSG("ECC OID not found");
            }
            break;
        #endif /* HAVE_ECC */

        case oidBlkType:
            switch (id) {
    #ifdef HAVE_AES_CBC
        #ifdef WOLFSSL_AES_128
                case AES128CBCb:
                    oid = blkAes128CbcOid;
                    *oidSz = sizeof(blkAes128CbcOid);
                    break;
        #endif
        #ifdef WOLFSSL_AES_192
                case AES192CBCb:
                    oid = blkAes192CbcOid;
                    *oidSz = sizeof(blkAes192CbcOid);
                    break;
        #endif
        #ifdef WOLFSSL_AES_256
                case AES256CBCb:
                    oid = blkAes256CbcOid;
                    *oidSz = sizeof(blkAes256CbcOid);
                    break;
        #endif
    #endif /* HAVE_AES_CBC */
    #ifdef HAVE_AESGCM
        #ifdef WOLFSSL_AES_128
                case AES128GCMb:
                    oid = blkAes128GcmOid;
                    *oidSz = sizeof(blkAes128GcmOid);
                    break;
        #endif
        #ifdef WOLFSSL_AES_192
                case AES192GCMb:
                    oid = blkAes192GcmOid;
                    *oidSz = sizeof(blkAes192GcmOid);
                    break;
        #endif
        #ifdef WOLFSSL_AES_256
                case AES256GCMb:
                    oid = blkAes256GcmOid;
                    *oidSz = sizeof(blkAes256GcmOid);
                    break;
        #endif
    #endif /* HAVE_AESGCM */
    #ifdef HAVE_AESCCM
        #ifdef WOLFSSL_AES_128
                case AES128CCMb:
                    oid = blkAes128CcmOid;
                    *oidSz = sizeof(blkAes128CcmOid);
                    break;
        #endif
        #ifdef WOLFSSL_AES_192
                case AES192CCMb:
                    oid = blkAes192CcmOid;
                    *oidSz = sizeof(blkAes192CcmOid);
                    break;
        #endif
        #ifdef WOLFSSL_AES_256
                case AES256CCMb:
                    oid = blkAes256CcmOid;
                    *oidSz = sizeof(blkAes256CcmOid);
                    break;
        #endif
    #endif /* HAVE_AESCCM */
    #ifndef NO_DES3
                case DESb:
                    oid = blkDesCbcOid;
                    *oidSz = sizeof(blkDesCbcOid);
                    break;
                case DES3b:
                    oid = blkDes3CbcOid;
                    *oidSz = sizeof(blkDes3CbcOid);
                    break;
    #endif /* !NO_DES3 */
                default:
                    break;
            }
            break;

        #ifdef HAVE_OCSP
        case oidOcspType:
            switch (id) {
                case OCSP_BASIC_OID:
                    oid = ocspBasicOid;
                    *oidSz = sizeof(ocspBasicOid);
                    break;
                case OCSP_NONCE_OID:
                    oid = ocspNonceOid;
                    *oidSz = sizeof(ocspNonceOid);
                    break;
                default:
                    break;
            }
            break;
        #endif /* HAVE_OCSP */

        case oidCertExtType:
            switch (id) {
                case BASIC_CA_OID:
                    oid = extBasicCaOid;
                    *oidSz = sizeof(extBasicCaOid);
                    break;
                case ALT_NAMES_OID:
                    oid = extAltNamesOid;
                    *oidSz = sizeof(extAltNamesOid);
                    break;
                case CRL_DIST_OID:
                    oid = extCrlDistOid;
                    *oidSz = sizeof(extCrlDistOid);
                    break;
                case AUTH_INFO_OID:
                    oid = extAuthInfoOid;
                    *oidSz = sizeof(extAuthInfoOid);
                    break;
                case AUTH_KEY_OID:
                    oid = extAuthKeyOid;
                    *oidSz = sizeof(extAuthKeyOid);
                    break;
                case SUBJ_KEY_OID:
                    oid = extSubjKeyOid;
                    *oidSz = sizeof(extSubjKeyOid);
                    break;
                case CERT_POLICY_OID:
                    oid = extCertPolicyOid;
                    *oidSz = sizeof(extCertPolicyOid);
                    break;
                case KEY_USAGE_OID:
                    oid = extKeyUsageOid;
                    *oidSz = sizeof(extKeyUsageOid);
                    break;
                case INHIBIT_ANY_OID:
                    oid = extInhibitAnyOid;
                    *oidSz = sizeof(extInhibitAnyOid);
                    break;
                case EXT_KEY_USAGE_OID:
                    oid = extExtKeyUsageOid;
                    *oidSz = sizeof(extExtKeyUsageOid);
                    break;
            #ifndef IGNORE_NAME_CONSTRAINTS
                case NAME_CONS_OID:
                    oid = extNameConsOid;
                    *oidSz = sizeof(extNameConsOid);
                    break;
            #endif
            #ifdef HAVE_OCSP
                case OCSP_NOCHECK_OID:
                    oid = ocspNoCheckOid;
                    *oidSz = sizeof(ocspNoCheckOid);
                    break;
            #endif
                default:
                    break;
            }
            break;

        case oidCrlExtType:
            #ifdef HAVE_CRL
            switch (id) {
                case AUTH_KEY_OID:
                    oid = extAuthKeyOid;
                    *oidSz = sizeof(extAuthKeyOid);
                    break;
                case CRL_NUMBER_OID:
                    oid = extCrlNumberOid;
                    *oidSz = sizeof(extCrlNumberOid);
                    break;
                default:
                    break;
            }
            #endif
            break;

        case oidCertAuthInfoType:
            switch (id) {
                case AIA_OCSP_OID:
                    oid = extAuthInfoOcspOid;
                    *oidSz = sizeof(extAuthInfoOcspOid);
                    break;
                case AIA_CA_ISSUER_OID:
                    oid = extAuthInfoCaIssuerOid;
                    *oidSz = sizeof(extAuthInfoCaIssuerOid);
                    break;
                default:
                    break;
            }
            break;

        case oidCertPolicyType:
            switch (id) {
                case CP_ANY_OID:
                    oid = extCertPolicyAnyOid;
                    *oidSz = sizeof(extCertPolicyAnyOid);
                    break;
                default:
                    break;
            }
            break;

        case oidCertAltNameType:
            switch (id) {
                case HW_NAME_OID:
                    oid = extAltNamesHwNameOid;
                    *oidSz = sizeof(extAltNamesHwNameOid);
                    break;
                default:
                    break;
            }
            break;

        case oidCertKeyUseType:
            switch (id) {
                case EKU_ANY_OID:
                    oid = extExtKeyUsageAnyOid;
                    *oidSz = sizeof(extExtKeyUsageAnyOid);
                    break;
                case EKU_SERVER_AUTH_OID:
                    oid = extExtKeyUsageServerAuthOid;
                    *oidSz = sizeof(extExtKeyUsageServerAuthOid);
                    break;
                case EKU_CLIENT_AUTH_OID:
                    oid = extExtKeyUsageClientAuthOid;
                    *oidSz = sizeof(extExtKeyUsageClientAuthOid);
                    break;
                case EKU_CODESIGNING_OID:
                    oid = extExtKeyUsageCodeSigningOid;
                    *oidSz = sizeof(extExtKeyUsageCodeSigningOid);
                    break;
                case EKU_EMAILPROTECT_OID:
                    oid = extExtKeyUsageEmailProtectOid;
                    *oidSz = sizeof(extExtKeyUsageEmailProtectOid);
                    break;
                case EKU_TIMESTAMP_OID:
                    oid = extExtKeyUsageTimestampOid;
                    *oidSz = sizeof(extExtKeyUsageTimestampOid);
                    break;
                case EKU_OCSP_SIGN_OID:
                    oid = extExtKeyUsageOcspSignOid;
                    *oidSz = sizeof(extExtKeyUsageOcspSignOid);
                    break;
                default:
                    break;
            }
            break;

        case oidKdfType:
            switch (id) {
                case PBKDF2_OID:
                    oid = pbkdf2Oid;
                    *oidSz = sizeof(pbkdf2Oid);
                    break;
                default:
                    break;
            }
            break;

        case oidPBEType:
            switch (id) {
        #if !defined(NO_SHA) && !defined(NO_RC4)
                case PBE_SHA1_RC4_128_SUM:
                case PBE_SHA1_RC4_128:
                    oid = pbeSha1RC4128;
                    *oidSz = sizeof(pbeSha1RC4128);
                    break;
        #endif
        #if !defined(NO_MD5) && !defined(NO_DES3)
                case PBE_MD5_DES_SUM:
                case PBE_MD5_DES:
                    oid = pbeMd5Des;
                    *oidSz = sizeof(pbeMd5Des);
                    break;

        #endif
        #if !defined(NO_SHA) && !defined(NO_DES3)
                case PBE_SHA1_DES_SUM:
                case PBE_SHA1_DES:
                    oid = pbeSha1Des;
                    *oidSz = sizeof(pbeSha1Des);
                    break;

        #endif
        #if !defined(NO_SHA) && !defined(NO_DES3)
                case PBE_SHA1_DES3_SUM:
                case PBE_SHA1_DES3:
                    oid = pbeSha1Des3;
                    *oidSz = sizeof(pbeSha1Des3);
                    break;
        #endif
                case PBES2_SUM:
                case PBES2:
                    oid = pbes2;
                    *oidSz = sizeof(pbes2);
                    break;
                default:
                    break;
            }
            break;

        case oidKeyWrapType:
            switch (id) {
            #ifdef WOLFSSL_AES_128
                case AES128_WRAP:
                    oid = wrapAes128Oid;
                    *oidSz = sizeof(wrapAes128Oid);
                    break;
            #endif
            #ifdef WOLFSSL_AES_192
                case AES192_WRAP:
                    oid = wrapAes192Oid;
                    *oidSz = sizeof(wrapAes192Oid);
                    break;
            #endif
            #ifdef WOLFSSL_AES_256
                case AES256_WRAP:
                    oid = wrapAes256Oid;
                    *oidSz = sizeof(wrapAes256Oid);
                    break;
            #endif
            #ifdef HAVE_PKCS7
                case PWRI_KEK_WRAP:
                    oid = wrapPwriKekOid;
                    *oidSz = sizeof(wrapPwriKekOid);
                    break;
            #endif
                default:
                    break;
            }
            break;

        case oidCmsKeyAgreeType:
            switch (id) {
            #ifndef NO_SHA
                case dhSinglePass_stdDH_sha1kdf_scheme:
                    oid = dhSinglePass_stdDH_sha1kdf_Oid;
                    *oidSz = sizeof(dhSinglePass_stdDH_sha1kdf_Oid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA224
                case dhSinglePass_stdDH_sha224kdf_scheme:
                    oid = dhSinglePass_stdDH_sha224kdf_Oid;
                    *oidSz = sizeof(dhSinglePass_stdDH_sha224kdf_Oid);
                    break;
            #endif
            #ifndef NO_SHA256
                case dhSinglePass_stdDH_sha256kdf_scheme:
                    oid = dhSinglePass_stdDH_sha256kdf_Oid;
                    *oidSz = sizeof(dhSinglePass_stdDH_sha256kdf_Oid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA384
                case dhSinglePass_stdDH_sha384kdf_scheme:
                    oid = dhSinglePass_stdDH_sha384kdf_Oid;
                    *oidSz = sizeof(dhSinglePass_stdDH_sha384kdf_Oid);
                    break;
            #endif
            #ifdef WOLFSSL_SHA512
                case dhSinglePass_stdDH_sha512kdf_scheme:
                    oid = dhSinglePass_stdDH_sha512kdf_Oid;
                    *oidSz = sizeof(dhSinglePass_stdDH_sha512kdf_Oid);
                    break;
            #endif
                default:
                    break;
            }
            break;

#ifndef NO_HMAC
        case oidHmacType:
            switch (id) {
        #ifdef WOLFSSL_SHA224
                case HMAC_SHA224_OID:
                    oid = hmacSha224Oid;
                    *oidSz = sizeof(hmacSha224Oid);
                    break;
        #endif
        #ifndef NO_SHA256
                case HMAC_SHA256_OID:
                    oid = hmacSha256Oid;
                    *oidSz = sizeof(hmacSha256Oid);
                    break;
        #endif
        #ifdef WOLFSSL_SHA384
                case HMAC_SHA384_OID:
                    oid = hmacSha384Oid;
                    *oidSz = sizeof(hmacSha384Oid);
                    break;
        #endif
        #ifdef WOLFSSL_SHA512
                case HMAC_SHA512_OID:
                    oid = hmacSha512Oid;
                    *oidSz = sizeof(hmacSha512Oid);
                    break;
        #endif
                default:
                    break;
            }
            break;
#endif /* !NO_HMAC */

#ifdef HAVE_LIBZ
        case oidCompressType:
            switch (id) {
                case ZLIBc:
                    oid = zlibCompress;
                    *oidSz = sizeof(zlibCompress);
                    break;
                default:
                    break;
            }
            break;
#endif /* HAVE_LIBZ */
#ifdef WOLFSSL_APACHE_HTTPD
        case oidCertNameType:
            switch (id) {
                 case NID_id_on_dnsSRV:
                    oid = dnsSRVOid;
                    *oidSz = sizeof(dnsSRVOid);
                    break;
                default:
                    break;
            }
            break;
        case oidTlsExtType:
            switch (id) {
                case TLS_FEATURE_OID:
                    oid = tlsFeatureOid;
                    *oidSz = sizeof(tlsFeatureOid);
                    break;
                default:
                    break;
            }
            break;
#endif /* WOLFSSL_APACHE_HTTPD */
#ifdef WOLFSSL_CERT_REQ
        case oidCsrAttrType:
            switch (id) {
                case CHALLENGE_PASSWORD_OID:
                    oid = attrChallengePasswordOid;
                    *oidSz = sizeof(attrChallengePasswordOid);
                    break;
                case SERIAL_NUMBER_OID:
                    oid = attrSerialNumberOid;
                    *oidSz = sizeof(attrSerialNumberOid);
                    break;
                case EXTENSION_REQUEST_OID:
                    oid = attrExtensionRequestOid;
                    *oidSz = sizeof(attrExtensionRequestOid);
                    break;
                default:
                    break;
            }
            break;
#endif
        case oidIgnoreType:
        default:
            break;
    }

    return oid;
}

#ifdef HAVE_ECC

/* Check the OID id is for a known elliptic curve.
 *
 * @param [in]  oid  OID id.
 * @return  ECC set id on success.
 * @return  ALGO_ID_E when OID id is 0 or not supported.
 */
static int CheckCurve(word32 oid)
{
    int ret;
    word32 oidSz;

    /* Lookup OID id. */
    ret = wc_ecc_get_oid(oid, NULL, &oidSz);
    /* Check for error or zero length OID size (can't get OID for encoding). */
    if ((ret < 0) || (oidSz == 0)) {
        WOLFSSL_MSG("CheckCurve not found");
        ret = ALGO_ID_E;
    }

    /* Return ECC set id or error code. */
    return ret;
}

#endif

#ifdef HAVE_OID_ENCODING
/* Encode dotted form of OID into byte array version.
 *
 * @param [in]      in     Dotted form of OID.
 * @param [in]      inSz   Count of numbers in dotted form.
 * @param [in]      out    Buffer to hold OID.
 * @param [in, out] outSz  On in, size of buffer.
 *                         On out, number of bytes in buffer.
 * @return  0 on success
 * @return  BAD_FUNC_ARG when in or outSz is NULL.
 * @return  BUFFER_E when buffer too small.
 */
int EncodeObjectId(const word16* in, word32 inSz, byte* out, word32* outSz)
{
    int i, x, len;
    word32 d, t;

    /* check args */
    if (in == NULL || outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* compute length of encoded OID */
    d = (in[0] * 40) + in[1];
    len = 0;
    for (i = 1; i < (int)inSz; i++) {
        x = 0;
        t = d;
        while (t) {
            x++;
            t >>= 1;
        }
        len += (x / 7) + ((x % 7) ? 1 : 0) + (d == 0 ? 1 : 0);

        if (i < (int)inSz - 1) {
            d = in[i + 1];
        }
    }

    if (out) {
        /* verify length */
        if ((int)*outSz < len) {
            return BUFFER_E; /* buffer provided is not large enough */
        }

        /* calc first byte */
        d = (in[0] * 40) + in[1];

        /* encode bytes */
        x = 0;
        for (i = 1; i < (int)inSz; i++) {
            if (d) {
                int y = x, z;
                byte mask = 0;
                while (d) {
                    out[x++] = (byte)((d & 0x7F) | mask);
                    d     >>= 7;
                    mask  |= 0x80;  /* upper bit is set on all but the last byte */
                }
                /* now swap bytes y...x-1 */
                z = x - 1;
                while (y < z) {
                    mask = out[y];
                    out[y] = out[z];
                    out[z] = mask;
                    ++y;
                    --z;
                }
            }
            else {
              out[x++] = 0x00; /* zero value */
            }

            /* next word */
            if (i < (int)inSz - 1) {
                d = in[i + 1];
            }
        }
    }

    /* return length */
    *outSz = len;

    return 0;
}
#endif /* HAVE_OID_ENCODING */

#ifdef HAVE_OID_DECODING
/* Encode dotted form of OID into byte array version.
 *
 * @param [in]      in     Byte array containing OID.
 * @param [in]      inSz   Size of OID in bytes.
 * @param [in]      out    Array to hold dotted form of OID.
 * @param [in, out] outSz  On in, number of elemnts in array.
 *                         On out, count of numbers in dotted form.
 * @return  0 on success
 * @return  BAD_FUNC_ARG when in or outSz is NULL.
 * @return  BUFFER_E when dotted form buffer too small.
 */
int DecodeObjectId(const byte* in, word32 inSz, word16* out, word32* outSz)
{
    int x = 0, y = 0;
    word32 t = 0;

    /* check args */
    if (in == NULL || outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* decode bytes */
    while (inSz--) {
        t = (t << 7) | (in[x] & 0x7F);
        if (!(in[x] & 0x80)) {
            if (y >= (int)*outSz) {
                return BUFFER_E;
            }
            if (y == 0) {
                out[0] = (t / 40);
                out[1] = (t % 40);
                y = 2;
            }
            else {
                out[y++] = t;
            }
            t = 0; /* reset tmp */
        }
        x++;
    }

    /* return length */
    *outSz = y;

    return 0;
}
#endif /* HAVE_OID_DECODING */

/* Decode the header of a BER/DER encoded OBJECT ID.
 *
 * @param [in]      input     Buffer holding DER/BER encoded data.
 * @param [in, out] inOutIdx  On in, starting index of header.
 *                            On out, end of parsed header.
 * @param [out]     len       Number of bytes in the ASN.1 data.
 * @param [in]      maxIdx    Length of data in buffer.
 * @return  0 on success.
 * @return  BUFFER_E when there is not enough data to parse.
 * @return  ASN_PARSE_E when the tag is not a OBJECT ID or length is invalid.
 */
int GetASNObjectId(const byte* input, word32* inOutIdx, int* len, word32 maxIdx)
{
    int ret = GetASNHeader(input, ASN_OBJECT_ID, inOutIdx, len, maxIdx);
    if (ret > 0) {
        /* Only return 0 on success. */
        ret = 0;
    }
    return ret;
}

/* Set the DER/BER encoding of the ASN.1 OBJECT ID header.
 *
 * When output is NULL, calculate the header length only.
 *
 * @param [in]  len        Length of OBJECT ID data in bytes.
 * @param [out] output     Buffer to write into.
 * @return  Number of bytes added to the buffer.
 */
int SetObjectId(int len, byte* output)
{
    int idx = 0;

    if (output) {
        /* Write out tag. */
        output[idx] = ASN_OBJECT_ID;
    }
    /* Skip tag. */
    idx += ASN_TAG_SZ;
    /* Encode length - passing NULL for output will not encode. */
    idx += SetLength(len, output ? output + idx : NULL);

    /* Return index after header. */
    return idx;
}

#ifdef ASN_DUMP_OID
/* Dump the OID information.
 *
 * Decode the OID too if function available.
 *
 * @param [in] oidData  OID data from buffer.
 * @param [in] oidSz    Size of OID data in buffer.
 * @param [in] oid      OID id.
 * @param [in] oidType  Type of OID.
 * @return  0 on success.
 * @return  BUFFER_E when not enough bytes for proper decode.
 *          (HAVE_OID_DECODING)
 */
static int DumpOID(const byte* oidData, word32 oidSz, word32 oid,
                   word32 oidType)
{
    int    ret = 0;
    word32 i;

    /* support for dumping OID information */
    printf("OID (Type %d, Sz %d, Sum %d): ", oidType, oidSz, oid);
    /* Dump bytes in decimal. */
    for (i = 0; i < oidSz; i++) {
        printf("%d, ", oidData[i]);
    }
    printf("\n");
    /* Dump bytes in hexadecimal. */
    for (i = 0; i < oidSz; i++) {
        printf("%02x, ", oidData[i]);
    }
    printf("\n");

    #ifdef HAVE_OID_DECODING
    {
        word16 decOid[16];
        word32 decOidSz = sizeof(decOid);
        /* Decode the OID into dotted form. */
        ret = DecodeObjectId(oidData, oidSz, decOid, &decOidSz);
        if (ret == 0) {
            printf("  Decoded (Sz %d): ", decOidSz);
            for (i=0; i<decOidSz; i++) {
                printf("%d.", decOid[i]);
            }
            printf("\n");
        }
        else {
            printf("DecodeObjectId failed: %d\n", ret);
        }
    }
    #endif /* HAVE_OID_DECODING */

    return ret;
}
#endif /* ASN_DUMP_OID */

/* Get the OID data and verify it is of the type specified when compiled in.
 *
 * @param [in]      input     Buffer holding OID.
 * @param [in, out] inOutIdx  On in, starting index of OID.
 *                            On out, end of parsed OID.
 * @param [out]     oid       OID id.
 * @param [in]      oidType   Expected type of OID. Define NO_VERIFY_OID to
 *                            not compile in check.
 * @param [in]      length    Length of OID data in buffer.
 * @return  0 on success.
 * @return  ASN_UNKNOWN_OID_E when OID is not recognized.
 * @return  BUFFER_E when not enough bytes for proper decode. (ASN_DUMP_OID and
 *          HAVE_OID_DECODING)
 */
static int GetOID(const byte* input, word32* inOutIdx, word32* oid,
                  word32 oidType, int length)
{
    int    ret = 0;
    word32 idx = *inOutIdx;
#ifndef NO_VERIFY_OID
    word32 actualOidSz;
    const byte* actualOid;
    const byte* checkOid = NULL;
    word32 checkOidSz;
#endif /* NO_VERIFY_OID */

    (void)oidType;
    *oid = 0;

#ifndef NO_VERIFY_OID
    /* Keep references to OID data and length for check. */
    actualOid = &input[idx];
    actualOidSz = (word32)length;
#endif /* NO_VERIFY_OID */

    /* Sum it up for now. */
    while (length--) {
        /* odd HC08 compiler behavior here when input[idx++] */
        *oid += (word32)input[idx];
        idx++;
    }

    /* Return the index after the OID data. */
    *inOutIdx = idx;

#ifndef NO_VERIFY_OID
    /* 'Ignore' type means we don't care which OID it is. */
    if (oidType != oidIgnoreType) {
        /* Get the OID data for the id-type. */
        checkOid = OidFromId(*oid, oidType, &checkOidSz);

    #ifdef ASN_DUMP_OID
        /* Dump out the data for debug. */
        ret = DumpOID(actualOid, actualOidSz, *oid, oidType);
    #endif

        /* TODO: Want to fail when checkOid is NULL.
         * Can't as too many situations where unknown OID is to be
         * supported. Extra parameter for must not be NULL?
         */
        /* Check that the OID data matches what we found for the OID id. */
        if ((ret == 0) && (checkOid != NULL) && ((checkOidSz != actualOidSz) ||
                (XMEMCMP(actualOid, checkOid, checkOidSz) != 0))) {
            WOLFSSL_MSG("OID Check Failed");
            ret = ASN_UNKNOWN_OID_E;
        }
    }
#endif /* NO_VERIFY_OID */

    return ret;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for an OBJECT_ID. */
static const ASNItem objectIdASN[] = {
    { 0, ASN_OBJECT_ID, 0, 0, 0 }
};

/* Number of items in ASN.1 template for an OBJECT_ID. */
#define objectIdASN_Length (sizeof(objectIdASN) / sizeof(ASNItem))
#endif

/* Get the OID id/sum from the BER encoded OBJECT_ID.
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of OBJECT_ID.
 *                            On out, start of ASN.1 item after OBJECT_ID.
 * @param [out]     oid       Id of OID in OBJECT_ID data.
 * @param [in]      oidType   Type of OID to expect.
 * @param [in]      maxIdx    Maximum index of data in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when encoding is invalid.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int GetObjectId(const byte* input, word32* inOutIdx, word32* oid,
                                  word32 oidType, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret, length;

    WOLFSSL_ENTER("GetObjectId()");

    ret = GetASNObjectId(input, inOutIdx, &length, maxIdx);
    if (ret != 0)
        return ret;

    return GetOID(input, inOutIdx, oid, oidType, length);
#else
    ASNGetData dataASN[objectIdASN_Length];
    int ret;

    WOLFSSL_ENTER("GetObjectId()");

    /* Clear dynamic data and set OID type expected. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_OID(&dataASN[0], oidType);
    /* Decode OBJECT_ID. */
    ret = GetASN_Items(objectIdASN, dataASN, objectIdASN_Length, 0, input,
                       inOutIdx, maxIdx);
    if (ret == 0) {
        /* Return the id/sum. */
        *oid = dataASN[0].data.oid.sum;
    }
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifndef WOLFSSL_ASN_TEMPLATE
static int SkipObjectId(const byte* input, word32* inOutIdx, word32 maxIdx)
{
    word32 idx = *inOutIdx;
    int    length;
    int ret;

    ret = GetASNObjectId(input, &idx, &length, maxIdx);
    if (ret != 0)
        return ret;

    idx += length;
    *inOutIdx = idx;

    return 0;
}
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for an algorithm identifier. */
static const ASNItem algoIdASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
/*  2 */        { 1, ASN_TAG_NULL, 0, 0, 1 },
};

/* Number of items in ASN.1 template for an algorithm identifier. */
#define algoIdASN_Length (sizeof(algoIdASN) / sizeof(ASNItem))
#endif

/* Get the OID id/sum from the BER encoding of an algorithm identifier.
 *
 * NULL tag is skipped if present.
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of algorithm identifier.
 *                            On out, start of ASN.1 item after algorithm id.
 * @param [out]     oid       Id of OID in algorithm identifier data.
 * @param [in]      oidType   Type of OID to expect.
 * @param [in]      maxIdx    Maximum index of data in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when encoding is invalid.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int GetAlgoId(const byte* input, word32* inOutIdx, word32* oid,
                     word32 oidType, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    length;
    word32 idx = *inOutIdx;
    int    ret;
    *oid = 0;

    WOLFSSL_ENTER("GetAlgoId");

    if (GetSequence(input, &idx, &length, maxIdx) < 0)
        return ASN_PARSE_E;

    if (GetObjectId(input, &idx, oid, oidType, maxIdx) < 0)
        return ASN_OBJECT_ID_E;

    /* could have NULL tag and 0 terminator, but may not */
    if (idx < maxIdx) {
        word32 localIdx = idx; /*use localIdx to not advance when checking tag*/
        byte   tag;

        if (GetASNTag(input, &localIdx, &tag, maxIdx) == 0) {
            if (tag == ASN_TAG_NULL) {
                ret = GetASNNull(input, &idx, maxIdx);
                if (ret != 0)
                    return ret;
            }
        }
    }

    *inOutIdx = idx;

    return 0;
#else
    ASNGetData dataASN[algoIdASN_Length];
    int ret;

    WOLFSSL_ENTER("GetAlgoId");

    /* Clear dynamic data and set OID type expected. */
    XMEMSET(dataASN, 0, sizeof(*dataASN) * algoIdASN_Length);
    GetASN_OID(&dataASN[1], oidType);
    /* Decode the algorithm identifier. */
    ret = GetASN_Items(algoIdASN, dataASN, algoIdASN_Length, 0, input, inOutIdx,
                       maxIdx);
    if (ret == 0) {
        /* Return the OID id/sum. */
        *oid = dataASN[1].data.oid.sum;
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifndef NO_RSA

#ifndef HAVE_USER_RSA
#if defined(WOLFSSL_ASN_TEMPLATE) || (!defined(NO_CERTS) && \
    (defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA)))
/* Byte offset of numbers in RSA key. */
size_t rsaIntOffset[] = {
    OFFSETOF(RsaKey, n),
    OFFSETOF(RsaKey, e),
#if !defined(WOLFSSL_RSA_PUBLIC_ONLY) || defined(WOLFSSL_KEY_GEN)
    OFFSETOF(RsaKey, d),
    OFFSETOF(RsaKey, p),
    OFFSETOF(RsaKey, q),
    OFFSETOF(RsaKey, dP),
    OFFSETOF(RsaKey, dQ),
    OFFSETOF(RsaKey, u)
#endif
};

/* Get a number from the RSA key based on an index.
 *
 * Order: { n, e, d, p, q, dP, dQ, u }
 *
 * Caller must ensure index is not invalid!
 *
 * @param [in] key  RSA key object.
 * @param [in] idx  Index of number.
 * @return  A pointer to an mp_int when valid index.
 * @return  NULL when invalid index.
 */
static mp_int* GetRsaInt(RsaKey* key, byte idx)
{
    /* Cast key to byte array to and use offset to get to mp_int field. */
    return (mp_int*)(((byte*)key) + rsaIntOffset[idx]);
}
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for an RSA private key.
 * PKCS #1: RFC 8017, A.1.2 - RSAPrivateKey
 */
static const ASNItem rsaKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  2 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  3 */        { 1, ASN_INTEGER, 0, 0, 0 },
#if !defined(WOLFSSL_RSA_PUBLIC_ONLY) || defined(WOLFSSL_KEY_GEN)
/*  4 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  5 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  6 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  7 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  8 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  9 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* otherPrimeInfos  OtherPrimeInfos OPTIONAL
                 * v2 - multiprime */
#endif
};

/* Number of items in ASN.1 template for an RSA private key. */
#define rsaKeyASN_Length (sizeof(rsaKeyASN) / sizeof(ASNItem))
#endif

/* Decode RSA private key.
 *
 * PKCS #1: RFC 8017, A.1.2 - RSAPrivateKey
 *
 * Compiling with WOLFSSL_RSA_PUBLIC_ONLY will result in only the public fields
 * being extracted.
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of RSA private key.
 *                            On out, start of ASN.1 item after RSA private key.
 * @param [in, out] key       RSA key object.
 * @param [in]      inSz      Number of bytes in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 */
int wc_RsaPrivateKeyDecode(const byte* input, word32* inOutIdx, RsaKey* key,
                        word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int version, length;
    word32 algId = 0;

    if (inOutIdx == NULL || input == NULL || key == NULL) {
        return BAD_FUNC_ARG;
    }

    /* if has pkcs8 header skip it */
    if (ToTraditionalInline_ex(input, inOutIdx, inSz, &algId) < 0) {
        /* ignore error, did not have pkcs8 header */
    }

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    if (GetMyVersion(input, inOutIdx, &version, inSz) < 0)
        return ASN_PARSE_E;

    key->type = RSA_PRIVATE;

    if (GetInt(&key->n,  input, inOutIdx, inSz) < 0 ||
        GetInt(&key->e,  input, inOutIdx, inSz) < 0 ||
#ifndef WOLFSSL_RSA_PUBLIC_ONLY
        GetInt(&key->d,  input, inOutIdx, inSz) < 0 ||
        GetInt(&key->p,  input, inOutIdx, inSz) < 0 ||
        GetInt(&key->q,  input, inOutIdx, inSz) < 0
#else
        SkipInt(input, inOutIdx, inSz) < 0 ||
        SkipInt(input, inOutIdx, inSz) < 0 ||
        SkipInt(input, inOutIdx, inSz) < 0
#endif
       ) {
            return ASN_RSA_KEY_E;
       }
#if (defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA) || !defined(RSA_LOW_MEM)) \
    && !defined(WOLFSSL_RSA_PUBLIC_ONLY)
    if (GetInt(&key->dP, input, inOutIdx, inSz) < 0 ||
        GetInt(&key->dQ, input, inOutIdx, inSz) < 0 ||
        GetInt(&key->u,  input, inOutIdx, inSz) < 0 )  return ASN_RSA_KEY_E;
#else
    if (SkipInt(input, inOutIdx, inSz) < 0 ||
        SkipInt(input, inOutIdx, inSz) < 0 ||
        SkipInt(input, inOutIdx, inSz) < 0 )  return ASN_RSA_KEY_E;
#endif

#if defined(WOLFSSL_XILINX_CRYPT) || defined(WOLFSSL_CRYPTOCELL)
    if (wc_InitRsaHw(key) != 0) {
        return BAD_STATE_E;
    }
#endif

    return 0;
#else
    DECL_ASNGETDATA(dataASN, rsaKeyASN_Length);
    int        ret = 0;
    byte       i;
    byte       version;
#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
    word32 algId = 0;
#endif

    /* Check validity of parameters. */
    if (inOutIdx == NULL || input == NULL || key == NULL) {
        ret = BAD_FUNC_ARG;
    }

#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
    if (ret == 0) {
        /* if has pkcs8 header skip it */
        if (ToTraditionalInline_ex(input, inOutIdx, inSz, &algId) < 0) {
            /* ignore error, did not have pkcs8 header */
        }
    }
#endif

    CALLOC_ASNGETDATA(dataASN, rsaKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Register variable to hold version field. */
        GetASN_Int8Bit(&dataASN[1], &version);
        /* Setup data to store INTEGER data in mp_int's in RSA object. */
    #if defined(WOLFSSL_RSA_PUBLIC_ONLY)
        /* Extract all public fields. */
        for (i = 0; i < RSA_PUB_INTS; i++) {
            GetASN_MP(&dataASN[2 + i], GetRsaInt(key, i));
        }
        /* Not extracting all data from BER encoding. */
        #define RSA_ASN_COMPLETE    0
    #else
        /* Extract all private fields. */
        for (i = 0; i < RSA_INTS; i++) {
            GetASN_MP(&dataASN[2 + i], GetRsaInt(key, i));
        }
        /* Extracting all data from BER encoding. */
        #define RSA_ASN_COMPLETE    1
    #endif
        /* Parse BER encoding for RSA private key. */
        ret = GetASN_Items(rsaKeyASN, dataASN, rsaKeyASN_Length,
            RSA_ASN_COMPLETE, input, inOutIdx, inSz);
    }
    /* Check version: 0 - two prime, 1 - multi-prime
     * Multi-prime has optional sequence after coefficient for extra primes.
     * If extra primes, parsing will fail as not all the buffer was used.
     */
    if ((ret == 0) && (version > PKCS1v1)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
    #if !defined(WOLFSSL_RSA_PUBLIC_ONLY)
        /* RSA key object has all private key values. */
        key->type = RSA_PRIVATE;
    #else
        /* RSA key object has all public key values. */
        key->type = RSA_PUBLIC;
    #endif

    #ifdef WOLFSSL_XILINX_CRYPT
        if (wc_InitRsaHw(key) != 0)
            ret = BAD_STATE_E;
    #endif
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* HAVE_USER_RSA */
#endif /* NO_RSA */

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for a PKCS #8 key.
 * Ignoring optional attributes and public key.
 * PKCS #8: RFC 5958, 2 - PrivateKeyInfo
 */
static const ASNItem pkcs8KeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  2 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  4 */            { 2, ASN_OBJECT_ID, 0, 0, 1 },
/*  5 */            { 2, ASN_TAG_NULL, 0, 0, 1 },
/*  6 */        { 1, ASN_OCTET_STRING, 0, 0, 0 },
                /* attributes            [0] Attributes OPTIONAL */
                /* [[2: publicKey        [1] PublicKey OPTIONAL ]] */
};

/* Number of items in ASN.1 template for a PKCS #8 key. */
#define pkcs8KeyASN_Length (sizeof(pkcs8KeyASN) / sizeof(ASNItem))
#endif

/* Remove PKCS #8 header around an RSA, ECDSA, Ed25519 or Ed448 key.
 *
 * @param [in]       input     Buffer holding BER data.
 * @param [in, out]  inOutIdx  On in, start of PKCS #8 encoding.
 *                             On out, start of encoded key.
 * @param [in]       sz        Size of data in buffer.
 * @param [out]      algId     Key's algorithm id from PKCS #8 header.
 * @return  Length of key data on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 */
int ToTraditionalInline_ex(const byte* input, word32* inOutIdx, word32 sz,
                           word32* algId)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx;
    int    version, length;
    int    ret;
    byte   tag;

    if (input == NULL || inOutIdx == NULL)
        return BAD_FUNC_ARG;

    idx = *inOutIdx;

    if (GetSequence(input, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    if (GetMyVersion(input, &idx, &version, sz) < 0)
        return ASN_PARSE_E;

    if (GetAlgoId(input, &idx, algId, oidKeyType, sz) < 0)
        return ASN_PARSE_E;

    if (GetASNTag(input, &idx, &tag, sz) < 0)
        return ASN_PARSE_E;
    idx = idx - 1; /* reset idx after finding tag */

    if (tag == ASN_OBJECT_ID) {
        if (SkipObjectId(input, &idx, sz) < 0)
            return ASN_PARSE_E;
    }

    ret = GetOctetString(input, &idx, &length, sz);
    if (ret < 0) {
        if (ret == BUFFER_E)
            return ASN_PARSE_E;
        /* Some private keys don't expect an octet string */
        WOLFSSL_MSG("Couldn't find Octet string");
    }

    *inOutIdx = idx;

    return length;
#else
    DECL_ASNGETDATA(dataASN, pkcs8KeyASN_Length);
    int ret = 0;
    word32 oid = 9;
    byte version;
    word32 idx = *inOutIdx;

    /* Check validity of parameters. */
    if (input == NULL || inOutIdx == NULL) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNGETDATA(dataASN, pkcs8KeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Get version, check key type and curve type. */
        GetASN_Int8Bit(&dataASN[1], &version);
        GetASN_OID(&dataASN[3], oidKeyType);
        GetASN_OID(&dataASN[4], oidCurveType);
        /* Parse data. */
        ret = GetASN_Items(pkcs8KeyASN, dataASN, pkcs8KeyASN_Length, 1, input,
                           &idx, sz);
    }

    if (ret == 0) {
        /* Key type OID. */
        oid = dataASN[3].data.oid.sum;

        /* Version 1 includes an optional public key.
         * If public key is included then the parsing will fail as it did not
         * use all the data.
         */
        if (version > PKCS8v1) {
            ret = ASN_PARSE_E;
        }
    }
    if (ret == 0) {
        switch (oid) {
        #ifndef NO_RSA
            case RSAk:
                /* Must have NULL item but not OBJECT_ID item. */
                if ((dataASN[5].tag == 0) ||
                    (dataASN[4].tag != 0)) {
                    ret = ASN_PARSE_E;
                }
                break;
        #endif
        #ifdef HAVE_ECC
            case ECDSAk:
                /* Must not have NULL item. */
                if (dataASN[5].tag != 0) {
                    ret = ASN_PARSE_E;
                }
                break;
        #endif
        #ifdef HAVE_ED25519
            case ED25519k:
                /* Neither NULL item nor OBJECT_ID item allowed. */
                if ((dataASN[5].tag != 0) ||
                    (dataASN[4].tag != 0)) {
                    ret = ASN_PARSE_E;
                }
                break;
        #endif
        #ifdef HAVE_CURVE25519
            case X25519k:
                /* Neither NULL item nor OBJECT_ID item allowed. */
                if ((dataASN[5].tag != 0) ||
                    (dataASN[4].tag != 0)) {
                    ret = ASN_PARSE_E;
                }
                break;
        #endif
        #ifdef HAVE_ED448
            case ED448k:
                /* Neither NULL item nor OBJECT_ID item allowed. */
                if ((dataASN[5].tag != 0) ||
                    (dataASN[4].tag != 0)) {
                    ret = ASN_PARSE_E;
                }
                break;
        #endif
        #ifdef HAVE_CURVE448
            case X448k:
                /* Neither NULL item nor OBJECT_ID item allowed. */
                if ((dataASN[5].tag != 0) ||
                    (dataASN[4].tag != 0)) {
                    ret = ASN_PARSE_E;
                }
                break;
        #endif
            /* DSAk not supported. */
            /* Ignore OID lookup failures. */
            default:
                break;
        }
    }
    if (ret == 0) {
        /* Return algorithm id of internal key. */
        *algId = oid;
        /* Return index to start of internal key. */
        *inOutIdx = GetASNItem_DataIdx(dataASN[6], input);
        /* Return value is length of internal key. */
        ret = dataASN[6].data.ref.length;
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif
}

/* TODO: test case  */
int ToTraditionalInline(const byte* input, word32* inOutIdx, word32 sz)
{
    word32 oid;

    return ToTraditionalInline_ex(input, inOutIdx, sz, &oid);
}

#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)

/* Remove PKCS8 header, move beginning of traditional to beginning of input */
int ToTraditional_ex(byte* input, word32 sz, word32* algId)
{
    word32 inOutIdx = 0;
    int    length;

    if (input == NULL)
        return BAD_FUNC_ARG;

    length = ToTraditionalInline_ex(input, &inOutIdx, sz, algId);
    if (length < 0)
        return length;

    if (length + inOutIdx > sz)
        return BUFFER_E;

    XMEMMOVE(input, input + inOutIdx, length);

    return length;
}

int ToTraditional(byte* input, word32 sz)
{
    word32 oid;

    return ToTraditional_ex(input, sz, &oid);
}

#endif /* HAVE_PKCS8 || HAVE_PKCS12 */

#if defined(HAVE_PKCS8) && !defined(NO_CERTS)

int wc_GetPkcs8TraditionalOffset(byte* input, word32* inOutIdx, word32 sz)
{
    int length;
    word32 algId;

    if (input == NULL || inOutIdx == NULL || (*inOutIdx > sz))
        return BAD_FUNC_ARG;

    length = ToTraditionalInline_ex(input, inOutIdx, sz, &algId);

    return length;
}

int wc_CreatePKCS8Key(byte* out, word32* outSz, byte* key, word32 keySz,
        int algoID, const byte* curveOID, word32 oidSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 keyIdx = 0;
    word32 tmpSz  = 0;
    word32 sz;
    word32 tmpAlgId = 0;

    /* If out is NULL then return the max size needed
     * + 2 for ASN_OBJECT_ID and ASN_OCTET_STRING tags */
    if (out == NULL && outSz != NULL) {
        *outSz = keySz + MAX_SEQ_SZ + MAX_VERSION_SZ + MAX_ALGO_SZ
                 + MAX_LENGTH_SZ + MAX_LENGTH_SZ + 2;

        if (curveOID != NULL)
            *outSz += oidSz + MAX_LENGTH_SZ + 1;

        WOLFSSL_MSG("Checking size of PKCS8");

        return LENGTH_ONLY_E;
    }

    WOLFSSL_ENTER("wc_CreatePKCS8Key()");

    if (key == NULL || out == NULL || outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* check the buffer has enough room for largest possible size */
    if (curveOID != NULL) {
        if (*outSz < (keySz + MAX_SEQ_SZ + MAX_VERSION_SZ + MAX_ALGO_SZ
               + MAX_LENGTH_SZ + MAX_LENGTH_SZ + 3 + oidSz + MAX_LENGTH_SZ))
            return BUFFER_E;
    }
    else {
        oidSz = 0; /* with no curveOID oid size must be 0 */
        if (*outSz < (keySz + MAX_SEQ_SZ + MAX_VERSION_SZ + MAX_ALGO_SZ
                  + MAX_LENGTH_SZ + MAX_LENGTH_SZ + 2))
            return BUFFER_E;
    }

    /* sanity check: make sure the key doesn't already have a PKCS 8 header */
    if (ToTraditionalInline_ex(key, &keyIdx, keySz, &tmpAlgId) >= 0) {
        (void)tmpAlgId;
        return ASN_PARSE_E;
    }

    /* PrivateKeyInfo ::= SEQUENCE */
    keyIdx = MAX_SEQ_SZ; /* save room for sequence */

    /*  version Version
     *  no header information just INTEGER */
    sz = SetMyVersion(PKCS8v0, out + keyIdx, 0);
    tmpSz += sz; keyIdx += sz;
    /*  privateKeyAlgorithm PrivateKeyAlgorithmIdentifier */
    sz = 0; /* set sz to 0 and get privateKey oid buffer size needed */
    if (curveOID != NULL && oidSz > 0) {
        byte buf[MAX_LENGTH_SZ];
        sz = SetLength(oidSz, buf);
        sz += 1; /* plus one for ASN object id */
    }
    sz = SetAlgoID(algoID, out + keyIdx, oidKeyType, oidSz + sz);
    tmpSz += sz; keyIdx += sz;

    /*  privateKey          PrivateKey *
     * pkcs8 ecc uses slightly different format. Places curve oid in
     * buffer */
    if (curveOID != NULL && oidSz > 0) {
        sz = SetObjectId(oidSz, out + keyIdx);
        keyIdx += sz; tmpSz += sz;
        XMEMCPY(out + keyIdx, curveOID, oidSz);
        keyIdx += oidSz; tmpSz += oidSz;
    }

    sz = SetOctetString(keySz, out + keyIdx);
    keyIdx += sz; tmpSz += sz;
    XMEMCPY(out + keyIdx, key, keySz);
    tmpSz += keySz;

    /*  attributes          optional
     * No attributes currently added */

    /* rewind and add sequence */
    sz = SetSequence(tmpSz, out);
    XMEMMOVE(out + sz, out + MAX_SEQ_SZ, tmpSz);

    *outSz = tmpSz + sz;
    return tmpSz + sz;
#else
    DECL_ASNSETDATA(dataASN, pkcs8KeyASN_Length);
    int sz;
    int ret = 0;
    word32 keyIdx = 0;
    word32 tmpAlgId = 0;

    WOLFSSL_ENTER("wc_CreatePKCS8Key()");

    /* Check validity of parameters. */
    if (out == NULL && outSz != NULL) {
    }
    else if (key == NULL || out == NULL || outSz == NULL) {
        ret = BAD_FUNC_ARG;
    }

    /* Sanity check: make sure key doesn't have PKCS #8 header. */
    if (ToTraditionalInline_ex(key, &keyIdx, keySz, &tmpAlgId) >= 0) {
        (void)tmpAlgId;
        ret = ASN_PARSE_E;
    }

    CALLOC_ASNSETDATA(dataASN, pkcs8KeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Only support default PKCS #8 format - v0. */
        SetASN_Int8Bit(&dataASN[1], PKCS8v0);
        /* Set key OID that corresponds to key data. */
        SetASN_OID(&dataASN[3], algoID, oidKeyType);
        if (curveOID != NULL && oidSz > 0) {
            /* ECC key and curveOID set to write. */
            SetASN_Buffer(&dataASN[4], curveOID, oidSz);
        }
        else {
            /* EC curve OID to encode. */
            dataASN[4].noOut = 1;
        }
        /* Only RSA keys have NULL tagged item after OID. */
        dataASN[5].noOut = (algoID != RSAk);
        /* Set key data to encode. */
        SetASN_Buffer(&dataASN[6], key, keySz);

        /* Get the size of the DER encoding. */
        ret = SizeASN_Items(pkcs8KeyASN, dataASN, pkcs8KeyASN_Length, &sz);
    }
    if (ret == 0) {
        /* Always return the calculated size. */
        *outSz = sz;
    }
    /* Check for buffer to encoded into. */
    if ((ret == 0) && (out == NULL)) {
        WOLFSSL_MSG("Checking size of PKCS8");
        ret = LENGTH_ONLY_E;
    }
    if (ret == 0) {
        /*  Encode PKCS #8 key into buffer. */
        SetASN_Items(pkcs8KeyASN, dataASN, pkcs8KeyASN_Length, out);
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, NULL);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#endif /* HAVE_PKCS8 && !NO_CERTS */

#if defined(HAVE_PKCS12) || !defined(NO_CHECK_PRIVATE_KEY)
/* check that the private key is a pair for the public key
 * return 1 (true) on match
 * return 0 or negative value on failure/error
 *
 * privKey   : buffer holding DER format private key
 * privKeySz : size of private key buffer
 * pubKey    : buffer holding DER format public key
 * pubKeySz  : size of public key buffer
 * ks        : type of key */
int wc_CheckPrivateKey(const byte* privKey, word32 privKeySz,
                       const byte* pubKey, word32 pubKeySz, enum Key_Sum ks)
{
    int ret;
    (void)privKeySz;
    (void)pubKeySz;
    (void)ks;

    if (privKey == NULL || pubKey == NULL) {
        return BAD_FUNC_ARG;
    }

    #if !defined(NO_RSA) && !defined(NO_ASN_CRYPT)
    /* test if RSA key */
    if (ks == RSAk) {
    #ifdef WOLFSSL_SMALL_STACK
        RsaKey* a;
        RsaKey* b = NULL;
    #else
        RsaKey a[1], b[1];
    #endif
        word32 keyIdx = 0;

    #ifdef WOLFSSL_SMALL_STACK
        a = (RsaKey*)XMALLOC(sizeof(RsaKey), NULL, DYNAMIC_TYPE_RSA);
        if (a == NULL)
            return MEMORY_E;
        b = (RsaKey*)XMALLOC(sizeof(RsaKey), NULL, DYNAMIC_TYPE_RSA);
        if (b == NULL) {
            XFREE(a, NULL, DYNAMIC_TYPE_RSA);
            return MEMORY_E;
        }
    #endif

        if ((ret = wc_InitRsaKey(a, NULL)) < 0) {
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(b, NULL, DYNAMIC_TYPE_RSA);
            XFREE(a, NULL, DYNAMIC_TYPE_RSA);
    #endif
            return ret;
        }
        if ((ret = wc_InitRsaKey(b, NULL)) < 0) {
            wc_FreeRsaKey(a);
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(b, NULL, DYNAMIC_TYPE_RSA);
            XFREE(a, NULL, DYNAMIC_TYPE_RSA);
    #endif
            return ret;
        }
        if ((ret = wc_RsaPrivateKeyDecode(privKey, &keyIdx, a, privKeySz)) == 0) {
            WOLFSSL_MSG("Checking RSA key pair");
            keyIdx = 0; /* reset to 0 for parsing public key */

            if ((ret = wc_RsaPublicKeyDecode(pubKey, &keyIdx, b,
                    pubKeySz)) == 0) {
                /* limit for user RSA crypto because of RsaKey
                 * dereference. */
            #if defined(HAVE_USER_RSA)
                WOLFSSL_MSG("Cannot verify RSA pair with user RSA");
                ret = 1; /* return first RSA cert as match */
            #else
                /* both keys extracted successfully now check n and e
                 * values are the same. This is dereferencing RsaKey */
                if (mp_cmp(&(a->n), &(b->n)) != MP_EQ ||
                    mp_cmp(&(a->e), &(b->e)) != MP_EQ) {
                    ret = MP_CMP_E;
                }
                else
                    ret = 1;
            #endif
            }
        }
        wc_FreeRsaKey(b);
        wc_FreeRsaKey(a);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(b, NULL, DYNAMIC_TYPE_RSA);
        XFREE(a, NULL, DYNAMIC_TYPE_RSA);
    #endif
    }
    else
    #endif /* !NO_RSA && !NO_ASN_CRYPT */

    #if defined(HAVE_ECC) && defined(HAVE_ECC_KEY_EXPORT) && !defined(NO_ASN_CRYPT)
    if (ks == ECDSAk) {
    #ifdef WOLFSSL_SMALL_STACK
        ecc_key* key_pair;
        byte*    privDer;
    #else
        ecc_key  key_pair[1];
        byte     privDer[MAX_ECC_BYTES];
    #endif
        word32   privSz = MAX_ECC_BYTES;
        word32   keyIdx = 0;

    #ifdef WOLFSSL_SMALL_STACK
        key_pair = (ecc_key*)XMALLOC(sizeof(ecc_key), NULL, DYNAMIC_TYPE_ECC);
        if (key_pair == NULL)
            return MEMORY_E;
        privDer = (byte*)XMALLOC(MAX_ECC_BYTES, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        if (privDer == NULL) {
            XFREE(key_pair, NULL, DYNAMIC_TYPE_ECC);
            return MEMORY_E;
        }
    #endif

        if ((ret = wc_ecc_init(key_pair)) < 0) {
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(privDer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(key_pair, NULL, DYNAMIC_TYPE_ECC);
    #endif
            return ret;
        }

        if ((ret = wc_EccPrivateKeyDecode(privKey, &keyIdx, key_pair,
                privKeySz)) == 0) {
            WOLFSSL_MSG("Checking ECC key pair");

            if ((ret = wc_ecc_export_private_only(key_pair, privDer, &privSz))
                                                                         == 0) {
                wc_ecc_free(key_pair);
                ret = wc_ecc_init(key_pair);
                if (ret == 0) {
                    ret = wc_ecc_import_private_key(privDer,
                                            privSz, pubKey,
                                            pubKeySz, key_pair);
                }

                /* public and private extracted successfully now check if is
                 * a pair and also do sanity checks on key. wc_ecc_check_key
                 * checks that private * base generator equals pubkey */
                if (ret == 0) {
                    if ((ret = wc_ecc_check_key(key_pair)) == 0) {
                        ret = 1;
                    }
                }
                ForceZero(privDer, privSz);
            }
        }
        wc_ecc_free(key_pair);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(privDer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(key_pair, NULL, DYNAMIC_TYPE_ECC);
    #endif
    }
    else
    #endif /* HAVE_ECC && HAVE_ECC_KEY_EXPORT && !NO_ASN_CRYPT */

    #if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_IMPORT) && !defined(NO_ASN_CRYPT)
    if (ks == ED25519k) {
    #ifdef WOLFSSL_SMALL_STACK
        ed25519_key* key_pair;
    #else
        ed25519_key  key_pair[1];
    #endif
        word32       keyIdx = 0;

    #ifdef WOLFSSL_SMALL_STACK
        key_pair = (ed25519_key*)XMALLOC(sizeof(ed25519_key), NULL,
                                                          DYNAMIC_TYPE_ED25519);
        if (key_pair == NULL)
            return MEMORY_E;
    #endif

        if ((ret = wc_ed25519_init(key_pair)) < 0) {
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(key_pair, NULL, DYNAMIC_TYPE_ED25519);
    #endif
            return ret;
        }
        if ((ret = wc_Ed25519PrivateKeyDecode(privKey, &keyIdx, key_pair,
                privKeySz)) == 0) {
            WOLFSSL_MSG("Checking ED25519 key pair");
            keyIdx = 0;
            if ((ret = wc_ed25519_import_public(pubKey, pubKeySz,
                    key_pair)) == 0) {
                /* public and private extracted successfully no check if is
                 * a pair and also do sanity checks on key. wc_ecc_check_key
                 * checks that private * base generator equals pubkey */
                if ((ret = wc_ed25519_check_key(key_pair)) == 0)
                    ret = 1;
            }
        }
        wc_ed25519_free(key_pair);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(key_pair, NULL, DYNAMIC_TYPE_ED25519);
    #endif
    }
    else
    #endif /* HAVE_ED25519 && HAVE_ED25519_KEY_IMPORT && !NO_ASN_CRYPT */

    #if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_IMPORT) && !defined(NO_ASN_CRYPT)
    if (ks == ED448k) {
    #ifdef WOLFSSL_SMALL_STACK
        ed448_key* key_pair = NULL;
    #else
        ed448_key  key_pair[1];
    #endif
        word32     keyIdx = 0;

    #ifdef WOLFSSL_SMALL_STACK
        key_pair = (ed448_key*)XMALLOC(sizeof(ed448_key), NULL,
                                                            DYNAMIC_TYPE_ED448);
        if (key_pair == NULL)
            return MEMORY_E;
    #endif

        if ((ret = wc_ed448_init(key_pair)) < 0) {
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(key_pair, NULL, DYNAMIC_TYPE_ED448);
    #endif
            return ret;
        }
        if ((ret = wc_Ed448PrivateKeyDecode(privKey, &keyIdx, key_pair,
                privKeySz)) == 0) {
            WOLFSSL_MSG("Checking ED448 key pair");
            keyIdx = 0;
            if ((ret = wc_ed448_import_public(pubKey, pubKeySz,
                    key_pair)) == 0) {
                /* public and private extracted successfully no check if is
                 * a pair and also do sanity checks on key. wc_ecc_check_key
                 * checks that private * base generator equals pubkey */
                if ((ret = wc_ed448_check_key(key_pair)) == 0)
                    ret = 1;
            }
        }
        wc_ed448_free(key_pair);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(key_pair, NULL, DYNAMIC_TYPE_ED448);
    #endif
    }
    else
    #endif /* HAVE_ED448 && HAVE_ED448_KEY_IMPORT && !NO_ASN_CRYPT */
    {
        ret = 0;
    }
    (void)ks;

    return ret;
}

/* check that the private key is a pair for the public key in certificate
 * return 1 (true) on match
 * return 0 or negative value on failure/error
 *
 * key   : buffer holding DER format key
 * keySz : size of key buffer
 * der   : a initialized and parsed DecodedCert holding a certificate */
int wc_CheckPrivateKeyCert(const byte* key, word32 keySz, DecodedCert* der)
{
    if (key == NULL || der == NULL) {
        return BAD_FUNC_ARG;
    }

    return wc_CheckPrivateKey(key, keySz, der->publicKey,
            der->pubKeySize, (enum Key_Sum) der->keyOID);
}

#endif /* HAVE_PKCS12 || !NO_CHECK_PRIVATE_KEY */

#ifndef NO_PWDBASED

#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
/* Check the PBE algorithm is supported and return wolfSSL id, version and block
 * size of encryption algorithm.
 *
 * When PBES2, version is PKCS5v2, CheckAlgoV2() must be called to get id and
 * blockSz based on encryption algorithm.
 *
 * @param [in]  first    First byte of OID to use in check.
 * @param [in]  second   Second byte of OID to use in check.
 * @param [out] id       wolfSSL id for PBE algorithm.
 * @param [out] version  Version of PBE OID:
 *                       PKCS12v1 (PBE), PKCS5 (PBES1), PKCS5v2 (PBES2).
 * @param [out] blockSz  Block size of encryption algorithm.
 * @return  0 on success.
 * @return  ALGO_ID_E when OID not supported.
 * @return  ASN_INPUT_E when first byte is invalid.
 */
static int CheckAlgo(int first, int second, int* id, int* version, int* blockSz)
{
    int ret = 0;

    (void)id;
    (void)version;
    (void)blockSz;

    /* pkcs-12 1 = pkcs-12PbeIds */
    if (first == 1) {
        /* PKCS #12: Appendix C */
        switch (second) {
#if !defined(NO_SHA)
    #ifndef NO_RC4
        case PBE_SHA1_RC4_128:
            *id = PBE_SHA1_RC4_128;
            *version = PKCS12v1;
            if (blockSz != NULL) {
                *blockSz = 1;
            }
            break;
    #endif
    #ifndef NO_DES3
        case PBE_SHA1_DES3:
            *id = PBE_SHA1_DES3;
            *version = PKCS12v1;
            if (blockSz != NULL) {
                *blockSz = DES_BLOCK_SIZE;
            }
            break;
    #endif
    #ifdef WC_RC2
        case PBE_SHA1_40RC2_CBC:
            *id = PBE_SHA1_40RC2_CBC;
            *version = PKCS12v1;
            if (blockSz != NULL) {
                *blockSz = RC2_BLOCK_SIZE;
            }
            break;
    #endif
#endif /* !NO_SHA */
        default:
            ret = ALGO_ID_E;
            break;
        }
    }
    else if (first != PKCS5) {
        /* Bad OID. */
        ret = ASN_INPUT_E;
    }
    /* PKCS #5 PBES2: Appendix A.4
     * pkcs-5 13 = id-PBES2 */
    else if (second == PBES2) {
        *version = PKCS5v2;
        /* Id and block size come from CheckAlgoV2() */
    }
    else  {
        /* PKCS #5 PBES1: Appendix A.3 */
        /* see RFC 2898 for ids */
        switch (second) {
    #ifndef NO_DES3
        #ifndef NO_MD5
        case PBES1_MD5_DES:
            *id = PBE_MD5_DES;
            *version = PKCS5;
            if (blockSz != NULL) {
                *blockSz = DES_BLOCK_SIZE;
            }
            break;
        #endif
        #ifndef NO_SHA
        case PBES1_SHA1_DES:
            *id = PBE_SHA1_DES;
            *version = PKCS5;
            if (blockSz != NULL) {
                *blockSz = DES_BLOCK_SIZE;
            }
            break;
        #endif
    #endif /* !NO_DES3 */
        default:
            ret = ALGO_ID_E;
            break;
        }
    }

    /* Return error code. */
    return ret;
}

#endif /* HAVE_PKCS8 || HAVE_PKCS12 */

#ifdef HAVE_PKCS8

/* Check the encryption algorithm with PBES2 is supported and return block size
 * and wolfSSL id for the PBE.
 *
 * @param [in]  oid      Encryption algorithm OID id.
 * @param [out] id       wolfSSL id for PBE algorithm.
 * @param [out] version  Version of PBE OID:
 *                       PKCS12v1 (PBE), PKCS5 (PBES1), PKCS5v2 (PBES2).
 * @return  0 on success.
 * @return  ALGO_ID_E when encryption algorithm is not supported with PBES2.
 */
static int CheckAlgoV2(int oid, int* id, int* blockSz)
{
    int ret = 0;

    (void)id;
    (void)blockSz;

    switch (oid) {
#if !defined(NO_DES3) && !defined(NO_SHA)
    case DESb:
        *id = PBE_SHA1_DES;
        if (blockSz != NULL) {
            *blockSz = DES_BLOCK_SIZE;
        }
        break;
    case DES3b:
        *id = PBE_SHA1_DES3;
        if (blockSz != NULL) {
            *blockSz = DES_BLOCK_SIZE;
        }
        break;
#endif
#ifdef WOLFSSL_AES_256
    case AES256CBCb:
        *id = PBE_AES256_CBC;
        if (blockSz != NULL) {
            *blockSz = AES_BLOCK_SIZE;
        }
        break;
#endif
#ifdef WOLFSSL_AES_128
    case AES128CBCb:
        *id = PBE_AES128_CBC;
        if (blockSz != NULL) {
            *blockSz = AES_BLOCK_SIZE;
        }
        break;
#endif
    default:
        WOLFSSL_MSG("No PKCS v2 algo found");
        ret = ALGO_ID_E;
        break;
    }

    /* Return error code. */
    return ret;
}

#endif /* HAVE_PKCS8 */

#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)

int wc_GetKeyOID(byte* key, word32 keySz, const byte** curveOID, word32* oidSz,
        int* algoID, void* heap)
{
    word32 tmpIdx = 0;

    if (key == NULL || algoID == NULL)
        return BAD_FUNC_ARG;

    *algoID = 0;

    #if !defined(NO_RSA) && !defined(NO_ASN_CRYPT)
    {
        RsaKey *rsa = (RsaKey *)XMALLOC(sizeof *rsa, heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (rsa == NULL)
            return MEMORY_E;

        wc_InitRsaKey(rsa, heap);
        if (wc_RsaPrivateKeyDecode(key, &tmpIdx, rsa, keySz) == 0) {
            *algoID = RSAk;
        }
        else {
            WOLFSSL_MSG("Not RSA DER key");
        }
        wc_FreeRsaKey(rsa);
        XFREE(rsa, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
    #endif /* !NO_RSA && !NO_ASN_CRYPT */
    #if defined(HAVE_ECC) && !defined(NO_ASN_CRYPT)
    if (*algoID == 0) {
        ecc_key *ecc = (ecc_key *)XMALLOC(sizeof *ecc, heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (ecc == NULL)
            return MEMORY_E;

        tmpIdx = 0;
        wc_ecc_init_ex(ecc, heap, INVALID_DEVID);
        if (wc_EccPrivateKeyDecode(key, &tmpIdx, ecc, keySz) == 0) {
            *algoID = ECDSAk;

            /* now find oid */
            if (wc_ecc_get_oid(ecc->dp->oidSum, curveOID, oidSz) < 0) {
                WOLFSSL_MSG("Error getting ECC curve OID");
                wc_ecc_free(ecc);
                XFREE(ecc, heap, DYNAMIC_TYPE_TMP_BUFFER);
                return BAD_FUNC_ARG;
            }
        }
        else {
            WOLFSSL_MSG("Not ECC DER key either");
        }
        wc_ecc_free(ecc);
        XFREE(ecc, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
#endif /* HAVE_ECC && !NO_ASN_CRYPT */
#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_IMPORT) && !defined(NO_ASN_CRYPT)
    if (*algoID == 0) {
        ed25519_key *ed25519 = (ed25519_key *)XMALLOC(sizeof *ed25519, heap,
            DYNAMIC_TYPE_TMP_BUFFER);
        if (ed25519 == NULL)
            return MEMORY_E;

        tmpIdx = 0;
        if (wc_ed25519_init(ed25519) == 0) {
            if (wc_Ed25519PrivateKeyDecode(key, &tmpIdx, ed25519, keySz) == 0) {
                *algoID = ED25519k;
            }
            else {
                WOLFSSL_MSG("Not ED25519 DER key");
            }
            wc_ed25519_free(ed25519);
        }
        else {
            WOLFSSL_MSG("GetKeyOID wc_ed25519_init failed");
        }
        XFREE(ed25519, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
#endif /* HAVE_ED25519 && HAVE_ED25519_KEY_IMPORT && !NO_ASN_CRYPT */
#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_IMPORT) && !defined(NO_ASN_CRYPT)
    if (*algoID == 0) {
        ed448_key *ed448 = (ed448_key *)XMALLOC(sizeof *ed448, heap,
            DYNAMIC_TYPE_TMP_BUFFER);
        if (ed448 == NULL)
            return MEMORY_E;

        tmpIdx = 0;
        if (wc_ed448_init(ed448) == 0) {
            if (wc_Ed448PrivateKeyDecode(key, &tmpIdx, ed448, keySz) == 0) {
                *algoID = ED448k;
            }
            else {
                WOLFSSL_MSG("Not ED448 DER key");
            }
            wc_ed448_free(ed448);
        }
        else {
            WOLFSSL_MSG("GetKeyOID wc_ed448_init failed");
        }
        XFREE(ed448, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
#endif /* HAVE_ED448 && HAVE_ED448_KEY_IMPORT && !NO_ASN_CRYPT */

    /* if flag is not set then is neither RSA or ECC key that could be
     * found */
    if (*algoID == 0) {
        WOLFSSL_MSG("Bad key DER or compile options");
        return BAD_FUNC_ARG;
    }

    (void)tmpIdx;
    (void)curveOID;
    (void)oidSz;
    (void)keySz;
    (void)heap;

    return 1;
}

#endif /* HAVE_PKCS8 || HAVE_PKCS12 */

#ifdef WOLFSSL_ASN_TEMPLATE
#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
/* ASN.1 template for PBES2 parameters.
 * PKCS #5: RFC 8018, A.4 - PBES2-params without outer SEQUENCE
 *                    A.2 - PBKDF2-params
 *                    B.2 - Encryption schemes
 */
static const ASNItem pbes2ParamsASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* PBKDF2 */
/*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
/*  2 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* Salt */
/*  3 */            { 2, ASN_OCTET_STRING, 0, 0, 0 },
                    /* Iteration count */
/*  4 */            { 2, ASN_INTEGER, 0, 0, 0 },
                    /* Key length */
/*  5 */            { 2, ASN_INTEGER, 0, 0, 1 },
                    /* PRF - default is HMAC-SHA1 */
/*  6 */            { 2, ASN_SEQUENCE, 1, 1, 1 },
/*  7 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
/*  8 */                { 3, ASN_TAG_NULL, 0, 0, 1 },
/*  9 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
               /* Encryption algorithm */
/* 10 */       { 1, ASN_OBJECT_ID, 0, 0, 0 },
               /* IV for CBC */
/* 11 */       { 1, ASN_OCTET_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for PBES2 parameters. */
#define pbes2ParamsASN_Length (sizeof(pbes2ParamsASN) / sizeof(ASNItem))

/* ASN.1 template for PBES1 parameters.
 * PKCS #5: RFC 8018, A.3. - PBEParameter without outer SEQUENCE
 */
static const ASNItem pbes1ParamsASN[] = {
            /* Salt */
/*  0 */    { 0, ASN_OCTET_STRING, 0, 0, 0 },
            /* Iteration count */
/*  1 */    { 0, ASN_INTEGER, 0, 0, 0 },
};

/* Number of items in ASN.1 template for PBES1 parameters. */
#define pbes1ParamsASN_Length (sizeof(pbes1ParamsASN) / sizeof(ASNItem))
#endif /* HAVE_PKCS8 || HAVE_PKCS12 */
#endif /* WOLFSSL_ASN_TEMPLATE */

#ifdef HAVE_PKCS8

/*
 * Equivalent to calling TraditionalEnc with the same parameters but with
 * encAlgId set to 0. This function must be kept alive because it's sometimes
 * part of the API (WOLFSSL_ASN_API).
 */
int UnTraditionalEnc(byte* key, word32 keySz, byte* out, word32* outSz,
        const char* password, int passwordSz, int vPKCS, int vAlgo,
        byte* salt, word32 saltSz, int itt, WC_RNG* rng, void* heap)
{
    return TraditionalEnc(key, keySz, out, outSz, password, passwordSz,
                vPKCS, vAlgo, 0, salt, saltSz, itt, rng, heap);
}

static int GetAlgoV2(int encAlgId, const byte** oid, int *len, int* id,
                     int *blkSz)
{
    int ret = 0;

    switch (encAlgId) {
#if !defined(NO_DES3) && !defined(NO_SHA)
    case DESb:
        *len = sizeof(blkDesCbcOid);
        *oid = blkDesCbcOid;
        *id = PBE_SHA1_DES;
        *blkSz = 8;
        break;
    case DES3b:
        *len = sizeof(blkDes3CbcOid);
        *oid = blkDes3CbcOid;
        *id = PBE_SHA1_DES3;
        *blkSz = 8;
        break;
#endif
#if defined(WOLFSSL_AES_256) && defined(HAVE_AES_CBC)
    case AES256CBCb:
        *len = sizeof(blkAes256CbcOid);
        *oid = blkAes256CbcOid;
        *id = PBE_AES256_CBC;
        *blkSz = 16;
        break;
#endif
    default:
        (void)len;
        (void)oid;
        (void)id;
        (void)blkSz;
        ret = ALGO_ID_E;
    }

    return ret;
}

int wc_EncryptPKCS8Key(byte* key, word32 keySz, byte* out, word32* outSz,
        const char* password, int passwordSz, int vPKCS, int pbeOid,
        int encAlgId, byte* salt, word32 saltSz, int itt, WC_RNG* rng,
        void* heap)
{
#ifdef WOLFSSL_SMALL_STACK
    byte* saltTmp = NULL;
#else
    byte saltTmp[MAX_SALT_SIZE];
#endif
    int genSalt = 0;
    int ret = 0;
    int version = 0;
    int pbeId = 0;
    int blockSz = 0;
    const byte* encOid = NULL;
    int encOidSz = 0;
    word32 padSz = 0;
    word32 innerLen = 0;
    word32 outerLen = 0;
    const byte* pbeOidBuf = NULL;
    word32 pbeOidBufSz = 0;
    word32 pbeLen = 0;
    word32 kdfLen = 0;
    word32 encLen = 0;
    byte cbcIv[MAX_IV_SIZE];
    word32 idx = 0;
    word32 encIdx = 0;

    (void)heap;

    WOLFSSL_ENTER("wc_EncryptPKCS8Key");

    if (key == NULL || outSz == NULL || password == NULL) {
        ret = BAD_FUNC_ARG;
    }

    if (ret == 0) {
        ret = CheckAlgo(vPKCS, pbeOid, &pbeId, &version, &blockSz);
    }
    if (ret == 0 && (salt == NULL || saltSz == 0)) {
        genSalt = 1;
        saltSz = 8;
    }
    if (ret == 0 && version == PKCS5v2) {
        ret = GetAlgoV2(encAlgId, &encOid, &encOidSz, &pbeId, &blockSz);
    }
    if (ret == 0) {
        padSz = (blockSz - (keySz & (blockSz - 1))) & (blockSz - 1);
        /* inner = OCT salt INT itt */
        innerLen = 2 + saltSz + 2 + (itt < 256 ? 1 : 2);

        if (version != PKCS5v2) {
            pbeOidBuf = OidFromId(pbeId, oidPBEType, &pbeOidBufSz);
            /* pbe = OBJ pbse1 SEQ [ inner ] */
            pbeLen = 2 + pbeOidBufSz + 2 + innerLen;
        }
        else {
            pbeOidBuf = pbes2;
            pbeOidBufSz = sizeof(pbes2);
            /* kdf = OBJ pbkdf2 [ SEQ innerLen ] */
            kdfLen = 2 + sizeof(pbkdf2Oid) + 2 + innerLen;
            /* enc = OBJ enc_alg OCT iv */
            encLen = 2 + encOidSz + 2 + blockSz;
            /* pbe = OBJ pbse2 SEQ [ SEQ [ kdf ] SEQ [ enc ] ] */
            pbeLen = 2 + sizeof(pbes2) + 2 + 2 + kdfLen + 2 + encLen;

            ret = wc_RNG_GenerateBlock(rng, cbcIv, blockSz);
        }
    }
    if (ret == 0) {
        /* outerLen = length of PBE encoding + octet string data */
        /* Plus 2 for tag and length for pbe */
        outerLen = 2 + pbeLen;
        /* Octet string tag, length */
        outerLen += 1 + SetLength(keySz + padSz, NULL);
        /* Octet string bytes */
        outerLen += keySz + padSz;
        if (out == NULL) {
            /* Sequence tag, length */
            *outSz = 1 + SetLength(outerLen, NULL) + outerLen;
            return LENGTH_ONLY_E;
        }
        SetOctetString(keySz + padSz, out);

        idx += SetSequence(outerLen, out + idx);

        encIdx = idx + outerLen - keySz - padSz;
        /* Put Encrypted content in place. */
        XMEMCPY(out + encIdx, key, keySz);
        if (padSz > 0) {
            XMEMSET(out + encIdx + keySz, padSz, padSz);
            keySz += padSz;
        }

        if (genSalt == 1) {
        #ifdef WOLFSSL_SMALL_STACK
            saltTmp = (byte*)XMALLOC(saltSz, heap, DYNAMIC_TYPE_TMP_BUFFER);
            if (saltTmp == NULL) {
                ret = MEMORY_E;
            }
            else
        #endif
            {
                salt = saltTmp;
                if ((ret = wc_RNG_GenerateBlock(rng, saltTmp, saltSz)) != 0) {
                    WOLFSSL_MSG("Error generating random salt");
                }
            }
        }
    }
    if (ret == 0) {
        ret = wc_CryptKey(password, passwordSz, salt, saltSz, itt, pbeId,
                  out + encIdx, keySz, version, cbcIv, 1, 0);
    }
    if (ret == 0) {
        if (version != PKCS5v2) {
            /* PBE algorithm */
            idx += SetSequence(pbeLen, out + idx);
            idx += SetObjectId(pbeOidBufSz, out + idx);
            XMEMCPY(out + idx, pbeOidBuf, pbeOidBufSz);
            idx += pbeOidBufSz;
        }
        else {
            /* PBES2 algorithm identifier */
            idx += SetSequence(pbeLen, out + idx);
            idx += SetObjectId(pbeOidBufSz, out + idx);
            XMEMCPY(out + idx, pbeOidBuf, pbeOidBufSz);
            idx += pbeOidBufSz;
            /* PBES2 Parameters: SEQ [ kdf ] SEQ [ enc ] */
            idx += SetSequence(2 + kdfLen + 2 + encLen, out + idx);
            /* KDF Algorithm Identifier */
            idx += SetSequence(kdfLen, out + idx);
            idx += SetObjectId(sizeof(pbkdf2Oid), out + idx);
            XMEMCPY(out + idx, pbkdf2Oid, sizeof(pbkdf2Oid));
            idx += sizeof(pbkdf2Oid);
        }
        idx += SetSequence(innerLen, out + idx);
        idx += SetOctetString(saltSz, out + idx);
        XMEMCPY(out + idx, salt, saltSz); idx += saltSz;
        ret = SetShortInt(out, &idx, itt, *outSz);
        if (ret > 0)
            ret = 0;
    }
    if (ret == 0) {
        if (version == PKCS5v2) {
            /* Encryption Algorithm Identifier */
            idx += SetSequence(encLen, out + idx);
            idx += SetObjectId(encOidSz, out + idx);
            XMEMCPY(out + idx, encOid, encOidSz);
            idx += encOidSz;
            /* Encryption Algorithm Parameter: CBC IV */
            idx += SetOctetString(blockSz, out + idx);
            XMEMCPY(out + idx, cbcIv, blockSz);
            idx += blockSz;
        }
        idx += SetOctetString(keySz, out + idx);
        /* Default PRF - no need to write out OID */
        idx += keySz;

        ret = idx;
    }

#ifdef WOLFSSL_SMALL_STACK
    if (saltTmp != NULL) {
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
#endif

    WOLFSSL_LEAVE("wc_EncryptPKCS8Key", ret);

    return ret;
}

int wc_DecryptPKCS8Key(byte* input, word32 sz, const char* password,
        int passwordSz)
{
    int ret;
    int length;
    word32 inOutIdx = 0;

    if (input == NULL || password == NULL) {
        return BAD_FUNC_ARG;
    }

    if (GetSequence(input, &inOutIdx, &length, sz) < 0) {
        ret = ASN_PARSE_E;
    }
    else {
        ret = DecryptContent(input + inOutIdx, sz - inOutIdx, password,
                passwordSz);
        if (ret > 0) {
            XMEMMOVE(input, input + inOutIdx, ret);
        }
    }

    if (ret > 0) {
        /* DecryptContent will decrypt the data, but it will leave any padding
         * bytes intact. This code calculates the length without the padding
         * and we return that to the user. */
        inOutIdx = 0;
        if (GetSequence(input, &inOutIdx, &length, ret) < 0) {
            ret = ASN_PARSE_E;
        }
        else {
            ret = inOutIdx + length;
        }
    }

    return ret;
}

/* Takes an unencrypted, traditional DER-encoded key and converts it to a PKCS#8
 * encrypted key. If out is not NULL, it will hold the encrypted key. If it's
 * NULL, LENGTH_ONLY_E will be returned and outSz will have the required out
 * buffer size. */
int TraditionalEnc(byte* key, word32 keySz, byte* out, word32* outSz,
        const char* password, int passwordSz, int vPKCS, int vAlgo,
        int encAlgId, byte* salt, word32 saltSz, int itt, WC_RNG* rng,
        void* heap)
{
    int ret = 0;
    byte *pkcs8Key = NULL;
    word32 pkcs8KeySz = 0;
    int algId = 0;
    const byte* curveOid = NULL;
    word32 curveOidSz = 0;

    if (ret == 0) {
        /* check key type and get OID if ECC */
        ret = wc_GetKeyOID(key, keySz, &curveOid, &curveOidSz, &algId, heap);
        if (ret == 1)
            ret = 0;
    }
    if (ret == 0) {
        ret = wc_CreatePKCS8Key(NULL, &pkcs8KeySz, key, keySz, algId, curveOid,
                                                                    curveOidSz);
        if (ret == LENGTH_ONLY_E)
            ret = 0;
    }
    if (ret == 0) {
        pkcs8Key = (byte*)XMALLOC(pkcs8KeySz, heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (pkcs8Key == NULL)
            ret = MEMORY_E;
    }
    if (ret == 0) {
        ret = wc_CreatePKCS8Key(pkcs8Key, &pkcs8KeySz, key, keySz, algId,
            curveOid, curveOidSz);
        if (ret >= 0) {
            pkcs8KeySz = ret;
            ret = 0;
        }
    }
    if (ret == 0) {
        ret = wc_EncryptPKCS8Key(pkcs8Key, pkcs8KeySz, out, outSz, password,
            passwordSz, vPKCS, vAlgo, encAlgId, salt, saltSz, itt, rng, heap);
    }

    if (pkcs8Key != NULL) {
        ForceZero(pkcs8Key, pkcs8KeySz);
        XFREE(pkcs8Key, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }

    (void)rng;

    return ret;
}

/* Same as TraditionalEnc, but in the public API. */
int wc_CreateEncryptedPKCS8Key(byte* key, word32 keySz, byte* out,
        word32* outSz, const char* password, int passwordSz, int vPKCS,
        int pbeOid, int encAlgId, byte* salt, word32 saltSz, int itt,
        WC_RNG* rng, void* heap)
{
    return TraditionalEnc(key, keySz, out, outSz, password, passwordSz, vPKCS,
        pbeOid, encAlgId, salt, saltSz, itt, rng, heap);
}


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for PKCS #8/#7 encrypted key for decrypting
 * PKCS #8: RFC 5958, 3 - EncryptedPrivateKeyInfo without outer SEQUENCE
 * PKCS #7: RFC 2315, 10.1 - EncryptedContentInfo without outer SEQUENCE
 */
static const ASNItem pkcs8DecASN[] = {
/*  0 */    { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  2 */        { 2, ASN_SEQUENCE, 1, 0, 0 },
            /* PKCS #7 */
/*  3 */    { 1, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 2 },
            /* PKCS #8 */
/*  4 */    { 1, ASN_OCTET_STRING, 0, 0, 2 },
};

/* Number of items in ASN.1 template for PKCS #8/#7 encrypted key. */
#define pkcs8DecASN_Length (sizeof(pkcs8DecASN) / sizeof(ASNItem))
#endif

/* Decrypt data using PBE algorithm.
 *
 * PKCS #8: RFC 5958, 3 - EncryptedPrivateKeyInfo without outer SEQUENCE
 * PKCS #7: RFC 2315, 10.1 - EncryptedContentInfo without outer SEQUENCE
 *
 * Note: input buffer is overwritten with decrypted data!
 *
 * Salt is in KDF parameters and IV is PBE parameters when needed.
 *
 * @param [in] input       Data to decrypt and unwrap.
 * @param [in] sz          Size of encrypted data.
 * @param [in] password    Password to derive encryption key with.
 * @param [in] passwordSz  Size of password in bytes.
 * @return  Length of decrypted data on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  Other when decryption fails.
 */
int DecryptContent(byte* input, word32 sz, const char* password, int passwordSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 inOutIdx = 0, seqEnd, oid, shaOid = 0;
    int    ret = 0, first, second, length = 0, version, saltSz, id = 0;
    int    iterations = 0, keySz = 0;
#ifdef WOLFSSL_SMALL_STACK
    byte*  salt = NULL;
    byte*  cbcIv = NULL;
#else
    byte   salt[MAX_SALT_SIZE];
    byte   cbcIv[MAX_IV_SIZE];
#endif
    byte   tag;

    if (passwordSz < 0) {
        WOLFSSL_MSG("Bad password size");
        return BAD_FUNC_ARG;
    }

    if (GetAlgoId(input, &inOutIdx, &oid, oidIgnoreType, sz) < 0) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }

    first  = input[inOutIdx - 2];   /* PKCS version always 2nd to last byte */
    second = input[inOutIdx - 1];   /* version.algo, algo id last byte */

    if (CheckAlgo(first, second, &id, &version, NULL) < 0) {
        ERROR_OUT(ASN_INPUT_E, exit_dc); /* Algo ID error */
    }

    if (version == PKCS5v2) {
        if (GetSequence(input, &inOutIdx, &length, sz) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }

        if (GetAlgoId(input, &inOutIdx, &oid, oidKdfType, sz) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }

        if (oid != PBKDF2_OID) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }
    }

    if (GetSequence(input, &inOutIdx, &length, sz) <= 0) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }
    /* Find the end of this SEQUENCE so we can check for the OPTIONAL and
     * DEFAULT items. */
    seqEnd = inOutIdx + length;

    ret = GetOctetString(input, &inOutIdx, &saltSz, sz);
    if (ret < 0)
        goto exit_dc;

    if (saltSz > MAX_SALT_SIZE) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }

#ifdef WOLFSSL_SMALL_STACK
    salt = (byte*)XMALLOC(MAX_SALT_SIZE, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    if (salt == NULL) {
        ERROR_OUT(MEMORY_E, exit_dc);
    }
#endif

    XMEMCPY(salt, &input[inOutIdx], saltSz);
    inOutIdx += saltSz;

    if (GetShortInt(input, &inOutIdx, &iterations, sz) < 0) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }

    /* OPTIONAL key length */
    if (seqEnd > inOutIdx) {
        word32 localIdx = inOutIdx;

        if (GetASNTag(input, &localIdx, &tag, sz) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }

        if (tag == ASN_INTEGER &&
                GetShortInt(input, &inOutIdx, &keySz, sz) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }
    }

    /* DEFAULT HMAC is SHA-1 */
    if (seqEnd > inOutIdx) {
        if (GetAlgoId(input, &inOutIdx, &oid, oidHmacType, sz) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }

        shaOid = oid;
    }

#ifdef WOLFSSL_SMALL_STACK
    cbcIv = (byte*)XMALLOC(MAX_IV_SIZE, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    if (cbcIv == NULL) {
        ERROR_OUT(MEMORY_E, exit_dc);
    }
#endif

    if (version == PKCS5v2) {
        /* get encryption algo */
        if (GetAlgoId(input, &inOutIdx, &oid, oidBlkType, sz) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }

        if (CheckAlgoV2(oid, &id, NULL) < 0) {
            ERROR_OUT(ASN_PARSE_E, exit_dc); /* PKCS v2 algo id error */
        }

        if (shaOid == 0)
            shaOid = oid;

        ret = GetOctetString(input, &inOutIdx, &length, sz);
        if (ret < 0)
            goto exit_dc;

        if (length > MAX_IV_SIZE) {
            ERROR_OUT(ASN_PARSE_E, exit_dc);
        }

        XMEMCPY(cbcIv, &input[inOutIdx], length);
        inOutIdx += length;
    }

    if (GetASNTag(input, &inOutIdx, &tag, sz) < 0) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }

    if (tag != (ASN_CONTEXT_SPECIFIC | 0) && tag != ASN_OCTET_STRING) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }

    if (GetLength(input, &inOutIdx, &length, sz) < 0) {
        ERROR_OUT(ASN_PARSE_E, exit_dc);
    }

    ret = wc_CryptKey(password, passwordSz, salt, saltSz, iterations, id,
                   input + inOutIdx, length, version, cbcIv, 0, shaOid);

exit_dc:
#ifdef WOLFSSL_SMALL_STACK
    XFREE(salt,  NULL, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(cbcIv, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    if (ret == 0) {
        XMEMMOVE(input, input + inOutIdx, length);
        ret = length;
    }

    return ret;
#else
    /* pbes2ParamsASN longer than pkcs8DecASN_Length/pbes1ParamsASN_Length. */
    DECL_ASNGETDATA(dataASN, pbes2ParamsASN_Length);
    int    ret = 0;
    int    id;
    int    version;
    word32 idx = 0;
    word32 pIdx = 0;
    word32 iterations;
    word32 keySz = 0;
    word32 saltSz;
    word32 shaOid = 0;
    byte*  salt = NULL;
    byte*  key = NULL;
    byte   cbcIv[MAX_IV_SIZE];
    byte*  params;

    WOLFSSL_ENTER("DecryptContent");

    ALLOC_ASNGETDATA(dataASN, pbes2ParamsASN_Length, ret, NULL);

    if (ret == 0) {
        /* Check OID is a PBE Type */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * pkcs8DecASN_Length);
        GetASN_OID(&dataASN[1], oidPBEType);
        ret = GetASN_Items(pkcs8DecASN, dataASN, pkcs8DecASN_Length, 0, input,
                           &idx, sz);
    }
    if (ret == 0) {
        /* Check the PBE algorithm and get the version and id. */
        idx = dataASN[1].data.oid.length;
        /* Second last byte: 1 (PKCS #12 PBE Id) or 5 (PKCS #5)
         * Last byte: Alg or PBES2 */
        CheckAlgo(dataASN[1].data.oid.data[idx - 2],
                  dataASN[1].data.oid.data[idx - 1], &id, &version, NULL);

        /* Get the parameters data. */
        GetASN_GetRef(&dataASN[2], &params, &sz);
        /* Having a numbered choice means none or both will have errored out. */
        if (dataASN[3].tag != 0)
            GetASN_GetRef(&dataASN[3], &key, &keySz);
        else if (dataASN[4].tag != 0)
            GetASN_GetRef(&dataASN[4], &key, &keySz);
    }

    if (ret == 0) {
        if (version != PKCS5v2) {
            /* Initialize for PBES1 parameters and put iterations in var. */
            XMEMSET(dataASN, 0, sizeof(*dataASN) * pbes1ParamsASN_Length);
            GetASN_Int32Bit(&dataASN[1], &iterations);
            /* Parse the PBES1 parameters. */
            ret = GetASN_Items(pbes1ParamsASN, dataASN, pbes1ParamsASN_Length,
                               0, params, &pIdx, sz);
            if (ret == 0) {
                /* Get the salt data. */
                GetASN_GetRef(&dataASN[0], &salt, &saltSz);
            }
        }
        else {
            word32 ivSz = MAX_IV_SIZE;

            /* Initialize for PBES2 parameters. Put iterations in var; match
             * KDF, HMAC and cipher, and copy CBC into buffer. */
            XMEMSET(dataASN, 0, sizeof(*dataASN) * pbes2ParamsASN_Length);
            GetASN_ExpBuffer(&dataASN[1], pbkdf2Oid, sizeof(pbkdf2Oid));
            GetASN_Int32Bit(&dataASN[4], &iterations);
            GetASN_OID(&dataASN[7], oidHmacType);
            GetASN_OID(&dataASN[10], oidBlkType);
            GetASN_Buffer(&dataASN[11], cbcIv, &ivSz);
            /* Parse the PBES2 parameters  */
            ret = GetASN_Items(pbes2ParamsASN, dataASN, pbes2ParamsASN_Length,
                               0, params, &pIdx, sz);
            if (ret == 0) {
                /* Get the salt data. */
                GetASN_GetRef(&dataASN[3], &salt, &saltSz);
                /* Get the digest and encryption algorithm id. */
                shaOid = dataASN[7].data.oid.sum; /* Default HMAC-SHA1 */
                id     = dataASN[10].data.oid.sum;
                /* Convert encryption algorithm to a PBE algorithm if needed. */
                CheckAlgoV2(id, &id, NULL);
            }
        }
    }

    if (ret == 0) {
        /* Decrypt the key. */
        ret = wc_CryptKey(password, passwordSz, salt, saltSz, iterations, id,
                          key, keySz, version, cbcIv, 0, shaOid);
    }
    if (ret == 0) {
        /* Copy the decrypted key into the input (inline). */
        XMEMMOVE(input, key, keySz);
        ret = keySz;
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif
}

/* Decrypt data using PBE algorithm and get key from PKCS#8 wrapping.
 *
 * PKCS #8: RFC 5958, 3 - EncryptedPrivateKeyInfo
 * PKCS #7: RFC 2315, 10.1 - EncryptedContentInfo
 *
 * Note: input buffer is overwritten with decrypted key!
 *
 * Salt is in KDF parameters and IV is PBE parameters when needed.
 *
 * @param [in]  input       Data to decrypt and unwrap.
 * @param [in]  sz          Size of encrypted data.
 * @param [in]  password    Password to derive encryption key with.
 * @param [in]  passwordSz  Size of password in bytes.
 * @param [out] algId       Key algorithm from PKCS#8 wrapper.
 * @return  Length of decrypted data on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  Other when decryption fails.
 */
int ToTraditionalEnc(byte* input, word32 sz, const char* password,
                     int passwordSz, word32* algId)
{
    int ret;

    ret = wc_DecryptPKCS8Key(input, sz, password, passwordSz);
    if (ret > 0) {
        ret = ToTraditional_ex(input, ret, algId);
    }

    return ret;
}

#endif /* HAVE_PKCS8 */

#ifdef HAVE_PKCS12

#define PKCS8_MIN_BLOCK_SIZE 8
static int Pkcs8Pad(byte* buf, int sz, int blockSz)
{
    int i, padSz;

    /* calculate pad size */
    padSz = blockSz - (sz & (blockSz - 1));

    /* pad with padSz value */
    if (buf) {
        for (i = 0; i < padSz; i++) {
            buf[sz+i] = (byte)(padSz & 0xFF);
        }
    }

    /* return adjusted length */
    return sz + padSz;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for PKCS #8 encrypted key with PBES1 parameters.
 * PKCS #8: RFC 5958, 3 - EncryptedPrivateKeyInfo
 * PKCS #5: RFC 8018, A.3 - PBEParameter
 */
static const ASNItem p8EncPbes1ASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* PBE algorithm */
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  3 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                        /* Salt */
/*  4 */                { 3, ASN_OCTET_STRING, 0, 0, 0 },
                        /* Iteration Count */
/*  5 */                { 3, ASN_INTEGER, 0, 0, 0 },
/*  6 */        { 1, ASN_OCTET_STRING, 0, 0, 0 },
};

#define p8EncPbes1ASN_Length (sizeof(p8EncPbes1ASN) / sizeof(ASNItem))
#endif

/* Wrap a private key in PKCS#8 and encrypt.
 *
 * Used for PKCS#12 and PKCS#7.
 * vPKCS is the version of PKCS to use.
 * vAlgo is the algorithm version to use.
 *
 * When salt is NULL, a random number is generated.
 *
 * data returned is :
 * [ seq - obj [ seq -salt,itt]] , construct with encrypted data
 *
 * @param [in]  input       Data to encrypt.
 * @param [in]  inputSz     Length of data in bytes.
 * @param [out] out         Buffer to write wrapped encrypted data into.
 * @param [out] outSz       Length of encrypted data in bytes.
 * @param [in]  password    Password used to create encryption key.
 * @param [in]  passwordSz  Length of password in bytes.
 * @param [in]  vPKCS       First byte used to determine PBE algorithm.
 * @param [in]  vAlgo       Second byte used to determine PBE algorithm.
 * @param [in]  salt        Salt to use with KDF.
 * @param [in]  saltSz      Length of salt in bytes.
 * @param [in]  itt         Number of iterations to use in KDF.
 * @param [in]  rng         Random number generator to use to generate salt.
 * @param [in]  heap        Dynamic memory allocator hint.
 * @return  The size of encrypted data on success
 * @return  LENGTH_ONLY_E when out is NULL and able to encode.
 * @return  ASN_PARSE_E when the salt size is too large.
 * @return  ASN_VERSION_E when attempting to use a PBES2 algorithm (use
 *          TraditionalEnc).
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  Other when encryption or random number generation fails.
 */
int EncryptContent(byte* input, word32 inputSz, byte* out, word32* outSz,
        const char* password, int passwordSz, int vPKCS, int vAlgo,
        byte* salt, word32 saltSz, int itt, WC_RNG* rng, void* heap)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 sz;
    word32 inOutIdx = 0;
    word32 tmpIdx   = 0;
    word32 totalSz  = 0;
    word32 seqSz;
    word32 innerSz;
    int    ret;
    int    version, id, blockSz = 0;
#ifdef WOLFSSL_SMALL_STACK
    byte*  saltTmp = NULL;
    byte*  cbcIv   = NULL;
#else
    byte   saltTmp[MAX_SALT_SIZE];
    byte   cbcIv[MAX_IV_SIZE];
#endif
    byte   seq[MAX_SEQ_SZ];
    byte   shr[MAX_SHORT_SZ];
    word32 maxShr = MAX_SHORT_SZ;
    word32 algoSz;
    const  byte* algoName;

    (void)heap;

    WOLFSSL_ENTER("EncryptContent()");

    if (CheckAlgo(vPKCS, vAlgo, &id, &version, &blockSz) < 0)
        return ASN_INPUT_E;  /* Algo ID error */

    if (version == PKCS5v2) {
        WOLFSSL_MSG("PKCS#5 version 2 not supported yet");
        return BAD_FUNC_ARG;
    }

    if (saltSz > MAX_SALT_SIZE)
        return ASN_PARSE_E;

    if (outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* calculate size */
    /* size of constructed string at end */
    sz = Pkcs8Pad(NULL, inputSz, blockSz);
    totalSz  = ASN_TAG_SZ;
    totalSz += SetLength(sz, seq);
    totalSz += sz;

    /* size of sequence holding object id and sub sequence of salt and itt */
    algoName = OidFromId(id, oidPBEType, &algoSz);
    if (algoName == NULL) {
        WOLFSSL_MSG("Unknown Algorithm");
        return 0;
    }
    innerSz = SetObjectId(algoSz, seq);
    innerSz += algoSz;

    /* get subsequence of salt and itt */
    if (salt == NULL || saltSz == 0) {
        sz = 8;
    }
    else {
        sz = saltSz;
    }
    seqSz  = SetOctetString(sz, seq);
    seqSz += sz;

    tmpIdx = 0;
    ret = SetShortInt(shr, &tmpIdx, itt, maxShr);
    if (ret >= 0) {
        seqSz += ret;
    }
    else {
        return ret;
    }
    innerSz += seqSz + SetSequence(seqSz, seq);
    totalSz += innerSz + SetSequence(innerSz, seq);

    if (out == NULL) {
        *outSz = totalSz;
        return LENGTH_ONLY_E;
    }

    inOutIdx = 0;
    if (totalSz > *outSz)
        return BUFFER_E;

    inOutIdx += SetSequence(innerSz, out + inOutIdx);
    inOutIdx += SetObjectId(algoSz, out + inOutIdx);
    XMEMCPY(out + inOutIdx, algoName, algoSz);
    inOutIdx += algoSz;
    inOutIdx += SetSequence(seqSz, out + inOutIdx);

    /* create random salt if one not provided */
    if (salt == NULL || saltSz == 0) {
        saltSz = 8;
    #ifdef WOLFSSL_SMALL_STACK
        saltTmp = (byte*)XMALLOC(saltSz, heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (saltTmp == NULL)
            return MEMORY_E;
    #endif
        salt = saltTmp;

        if ((ret = wc_RNG_GenerateBlock(rng, saltTmp, saltSz)) != 0) {
            WOLFSSL_MSG("Error generating random salt");
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            return ret;
        }
    }
    inOutIdx += SetOctetString(saltSz, out + inOutIdx);
    if (saltSz + inOutIdx > *outSz) {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return BUFFER_E;
    }
    XMEMCPY(out + inOutIdx, salt, saltSz);
    inOutIdx += saltSz;

    /* place iteration setting in buffer */
    ret = SetShortInt(out, &inOutIdx, itt, *outSz);
    if (ret < 0) {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;
    }

    if (inOutIdx + 1 > *outSz) {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return BUFFER_E;
    }
    out[inOutIdx++] = ASN_CONTEXT_SPECIFIC | 0;

    /* get pad size and verify buffer room */
    sz = Pkcs8Pad(NULL, inputSz, blockSz);
    if (sz + inOutIdx > *outSz) {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return BUFFER_E;
    }
    inOutIdx += SetLength(sz, out + inOutIdx);

    /* copy input to output buffer and pad end */
    XMEMCPY(out + inOutIdx, input, inputSz);
    sz = Pkcs8Pad(out + inOutIdx, inputSz, blockSz);
#ifdef WOLFSSL_SMALL_STACK
    cbcIv = (byte*)XMALLOC(MAX_IV_SIZE, heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (cbcIv == NULL) {
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }
#endif

    /* encrypt */
    if ((ret = wc_CryptKey(password, passwordSz, salt, saltSz, itt, id,
                   out + inOutIdx, sz, version, cbcIv, 1, 0)) < 0) {

    #ifdef WOLFSSL_SMALL_STACK
        XFREE(cbcIv,   heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;  /* encrypt failure */
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(cbcIv,   heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(saltTmp, heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    (void)rng;

    return inOutIdx + sz;
#else
    DECL_ASNSETDATA(dataASN, p8EncPbes1ASN_Length);
    int ret = 0;
    int sz;
    int version;
    int id;
    int blockSz = 0;
    byte* pkcs8;
    word32 pkcs8Sz;
    byte cbcIv[MAX_IV_SIZE];

    (void)heap;

    WOLFSSL_ENTER("EncryptContent()");

    /* Must have a output size to return or check. */
    if (outSz == NULL) {
        ret = BAD_FUNC_ARG;
    }
    /* Check salt size is valid. */
    if ((ret == 0) && (saltSz > MAX_SALT_SIZE)) {
        ret = ASN_PARSE_E;
    }
    /* Get algorithm parameters for algorithm identifer. */
    if ((ret == 0) && CheckAlgo(vPKCS, vAlgo, &id, &version, &blockSz) < 0) {
        ret = ASN_INPUT_E;
    }
    /* Check PKCS #5 version - only PBSE1 parameters supported. */
    if ((ret == 0) && (version == PKCS5v2)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNSETDATA(dataASN, p8EncPbes1ASN_Length, ret, heap);

    if (ret == 0) {
        /* Setup data to go into encoding including PBE algorithm, salt,
         * iteration count, and padded key length. */
        SetASN_OID(&dataASN[2], id, oidPBEType);
        if (salt == NULL || saltSz == 0) {
            salt = NULL;
            saltSz = PKCS5_SALT_SZ;
            /* Salt generated into encoding below. */
        }
        SetASN_Buffer(&dataASN[4], salt, saltSz);
        SetASN_Int16Bit(&dataASN[5], itt);
        pkcs8Sz = Pkcs8Pad(NULL, inputSz, blockSz);
        SetASN_Buffer(&dataASN[6], NULL, pkcs8Sz);

        /* Calculate size of encoding. */
        ret = SizeASN_Items(p8EncPbes1ASN + 1, dataASN + 1,
                            p8EncPbes1ASN_Length - 1, &sz);
    }
    /* Return size when no output buffer. */
    if ((ret == 0) && (out == NULL)) {
        *outSz = sz;
        ret = LENGTH_ONLY_E;
    }
    /* Check output buffer is big enough for encoded data. */
    if ((ret == 0) && (sz > (int)*outSz)) {
        ret = BAD_FUNC_ARG;
    }
    if (ret == 0) {
        /* Encode PKCS#8 key. */
        SetASN_Items(p8EncPbes1ASN + 1, dataASN + 1, p8EncPbes1ASN_Length - 1,
                     out);

        if (salt == NULL) {
            /* Generate salt into encoding. */
            salt = (byte*)dataASN[4].data.buffer.data;
            ret = wc_RNG_GenerateBlock(rng, salt, saltSz);
        }
    }
    if (ret == 0) {
        /* Store PKCS#8 key in output buffer. */
        pkcs8 = (byte*)dataASN[6].data.buffer.data;
        XMEMCPY(pkcs8, input, inputSz);

        /* Encrypt PKCS#8 key inline. */
        ret = wc_CryptKey(password, passwordSz, salt, saltSz, itt, id, pkcs8,
                          pkcs8Sz, version, cbcIv, 1, 0);
    }
    if (ret == 0) {
        /* Returning size on success. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


#endif /* HAVE_PKCS12 */
#endif /* NO_PWDBASED */

#ifndef NO_RSA

#ifndef HAVE_USER_RSA
#ifdef WOLFSSL_RENESAS_TSIP
/* This function is to retrieve key position information in a cert.*
 * The information will be used to call TSIP TLS-linked API for    *
 * certificate verification.                                       */
static int RsaPublicKeyDecodeRawIndex(const byte* input, word32* inOutIdx,
                                      word32 inSz, word32* key_n,
                                      word32* key_n_len, word32* key_e,
                                      word32* key_e_len)
{

    int ret = 0;
    int length = 0;
#if defined(OPENSSL_EXTRA) || defined(RSA_DECODE_EXTRA)
    byte b;
#endif

    if (input == NULL || inOutIdx == NULL)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

#if defined(OPENSSL_EXTRA) || defined(RSA_DECODE_EXTRA)
    if ((*inOutIdx + 1) > inSz)
        return BUFFER_E;

    b = input[*inOutIdx];
    if (b != ASN_INTEGER) {
        /* not from decoded cert, will have algo id, skip past */
        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        if (SkipObjectId(input, inOutIdx, inSz) < 0)
            return ASN_PARSE_E;

        /* Option NULL ASN.1 tag */
        if (*inOutIdx  >= inSz) {
            return BUFFER_E;
        }
        if (input[*inOutIdx] == ASN_TAG_NULL) {
            ret = GetASNNull(input, inOutIdx, inSz);
            if (ret != 0)
                return ret;
        }

        /* should have bit tag length and seq next */
        ret = CheckBitString(input, inOutIdx, NULL, inSz, 1, NULL);
        if (ret != 0)
            return ret;

        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;
    }
#endif /* OPENSSL_EXTRA */

    /* Get modulus */
    ret = GetASNInt(input, inOutIdx, &length, inSz);
    *key_n += *inOutIdx;
    if (ret < 0) {
        return ASN_RSA_KEY_E;
    }
    if (key_n_len)
        *key_n_len = length;
    *inOutIdx += length;

    /* Get exponent */
    ret = GetASNInt(input, inOutIdx, &length, inSz);
    *key_e += *inOutIdx;
    if (ret < 0) {
        return ASN_RSA_KEY_E;
    }
    if (key_e_len)
        *key_e_len = length;

    return ret;
}
#endif /* WOLFSSL_RENESAS_TSIP */

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for an RSA public key.
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * PKCS #1: RFC 8017, A.1.1 - RSAPublicKey
 */
static const ASNItem rsaPublicKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  3 */            { 2, ASN_TAG_NULL, 0, 0, 1 },
/*  4 */        { 1, ASN_BIT_STRING, 0, 1, 0 },
                    /* RSAPublicKey */
/*  5 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
/*  6 */                { 3, ASN_INTEGER, 0, 0, 0 },
/*  7 */                { 3, ASN_INTEGER, 0, 0, 0 },
};

/* Number of items in ASN.1 template for an RSA public key. */
#define rsaPublicKeyASN_Length (sizeof(rsaPublicKeyASN) / sizeof(ASNItem))
#endif

/* Decode RSA public key.
 *
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * PKCS #1: RFC 8017, A.1.1 - RSAPublicKey
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of RSA public key.
 *                            On out, start of ASN.1 item after RSA public key.
 * @param [in]      inSz      Number of bytes in buffer.
 * @param [out]     n         Pointer to modulus in buffer.
 * @param [out]     nSz       Size of modulus in bytes.
 * @param [out]     e         Pointer to exponent in buffer.
 * @param [out]     eSz       Size of exponent in bytes.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int wc_RsaPublicKeyDecode_ex(const byte* input, word32* inOutIdx, word32 inSz,
    const byte** n, word32* nSz, const byte** e, word32* eSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret = 0;
    int length = 0;
#if defined(OPENSSL_EXTRA) || defined(RSA_DECODE_EXTRA)
    word32 localIdx;
    byte   tag;
#endif

    if (input == NULL || inOutIdx == NULL)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

#if defined(OPENSSL_EXTRA) || defined(RSA_DECODE_EXTRA)
    localIdx = *inOutIdx;
    if (GetASNTag(input, &localIdx, &tag, inSz) < 0)
        return BUFFER_E;

    if (tag != ASN_INTEGER) {
        /* not from decoded cert, will have algo id, skip past */
        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        if (SkipObjectId(input, inOutIdx, inSz) < 0)
            return ASN_PARSE_E;

        /* Option NULL ASN.1 tag */
        if (*inOutIdx  >= inSz) {
            return BUFFER_E;
        }

        localIdx = *inOutIdx;
        if (GetASNTag(input, &localIdx, &tag, inSz) < 0)
            return ASN_PARSE_E;

        if (tag == ASN_TAG_NULL) {
            ret = GetASNNull(input, inOutIdx, inSz);
            if (ret != 0)
                return ret;
        }

        /* should have bit tag length and seq next */
        ret = CheckBitString(input, inOutIdx, NULL, inSz, 1, NULL);
        if (ret != 0)
            return ret;

        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;
    }
#endif /* OPENSSL_EXTRA */

    /* Get modulus */
    ret = GetASNInt(input, inOutIdx, &length, inSz);
    if (ret < 0) {
        return ASN_RSA_KEY_E;
    }
    if (nSz)
        *nSz = length;
    if (n)
        *n = &input[*inOutIdx];
    *inOutIdx += length;

    /* Get exponent */
    ret = GetASNInt(input, inOutIdx, &length, inSz);
    if (ret < 0) {
        return ASN_RSA_KEY_E;
    }
    if (eSz)
        *eSz = length;
    if (e)
        *e = &input[*inOutIdx];
    *inOutIdx += length;

    return ret;
#else
    DECL_ASNGETDATA(dataASN, rsaPublicKeyASN_Length);
    int ret = 0;

    /* Check validity of parameters. */
    if (input == NULL || inOutIdx == NULL) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNGETDATA(dataASN, rsaPublicKeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Try decoding PKCS #1 public key by ignoring rest of ASN.1. */
        ret = GetASN_Items(&rsaPublicKeyASN[5], &dataASN[5],
                           rsaPublicKeyASN_Length - 5, 0, input, inOutIdx,
                           inSz);
        if (ret != 0) {
            /* Didn't work - try whole SubjectKeyInfo instead. */
            /* Set the OID to expect. */
            GetASN_ExpBuffer(&dataASN[2], keyRsaOid, sizeof(keyRsaOid));
            /* Decode SubjectKeyInfo. */
            ret = GetASN_Items(rsaPublicKeyASN, dataASN,
                               rsaPublicKeyASN_Length, 1, input, inOutIdx,
                               inSz);
        }
    }
    if (ret == 0) {
        /* Return the buffers and lengths asked for. */
        if (n != NULL) {
            *n   = dataASN[6].data.ref.data;
        }
        if (nSz != NULL) {
            *nSz = dataASN[6].data.ref.length;
        }
        if (e != NULL) {
            *e   = dataASN[7].data.ref.data;
        }
        if (eSz != NULL) {
            *eSz = dataASN[7].data.ref.length;
        }
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* Decode RSA public key.
 *
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * PKCS #1: RFC 8017, A.1.1 - RSAPublicKey
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of RSA public key.
 *                            On out, start of ASN.1 item after RSA public key.
 * @param [in, out] key       RSA key object.
 * @param [in]      inSz      Number of bytes in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int wc_RsaPublicKeyDecode(const byte* input, word32* inOutIdx, RsaKey* key,
                       word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
    const byte *n = NULL, *e = NULL;
    word32 nSz = 0, eSz = 0;

    if (key == NULL)
        return BAD_FUNC_ARG;

    ret = wc_RsaPublicKeyDecode_ex(input, inOutIdx, inSz, &n, &nSz, &e, &eSz);
    if (ret == 0) {
        ret = wc_RsaPublicKeyDecodeRaw(n, nSz, e, eSz, key);
    }

    return ret;
#else
    DECL_ASNGETDATA(dataASN, rsaPublicKeyASN_Length);
    int ret = 0;

    /* Check validity of parameters. */
    if ((input == NULL) || (inOutIdx == NULL) || (key == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNGETDATA(dataASN, rsaPublicKeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Set mp_ints to fill with modulus and exponent data. */
        GetASN_MP(&dataASN[6], &key->n);
        GetASN_MP(&dataASN[7], &key->e);
        /* Try decoding PKCS #1 public key by ignoring rest of ASN.1. */
        ret = GetASN_Items(&rsaPublicKeyASN[5], &dataASN[5],
                           rsaPublicKeyASN_Length - 5, 0, input, inOutIdx,
                           inSz);
        if (ret != 0) {
            /* Didn't work - try whole SubjectKeyInfo instead. */
            /* Set the OID to expect. */
            GetASN_ExpBuffer(&dataASN[2], keyRsaOid, sizeof(keyRsaOid));
            /* Decode SubjectKeyInfo. */
            ret = GetASN_Items(rsaPublicKeyASN, dataASN,
                               rsaPublicKeyASN_Length, 1, input, inOutIdx,
                               inSz);
        }
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif
}

/* import RSA public key elements (n, e) into RsaKey structure (key) */
int wc_RsaPublicKeyDecodeRaw(const byte* n, word32 nSz, const byte* e,
                             word32 eSz, RsaKey* key)
{
    if (n == NULL || e == NULL || key == NULL)
        return BAD_FUNC_ARG;

    key->type = RSA_PUBLIC;

    if (mp_init(&key->n) != MP_OKAY)
        return MP_INIT_E;

    if (mp_read_unsigned_bin(&key->n, n, nSz) != 0) {
        mp_clear(&key->n);
        return ASN_GETINT_E;
    }
#ifdef HAVE_WOLF_BIGINT
    if ((int)nSz > 0 && wc_bigint_from_unsigned_bin(&key->n.raw, n, nSz) != 0) {
        mp_clear(&key->n);
        return ASN_GETINT_E;
    }
#endif /* HAVE_WOLF_BIGINT */

    if (mp_init(&key->e) != MP_OKAY) {
        mp_clear(&key->n);
        return MP_INIT_E;
    }

    if (mp_read_unsigned_bin(&key->e, e, eSz) != 0) {
        mp_clear(&key->n);
        mp_clear(&key->e);
        return ASN_GETINT_E;
    }
#ifdef HAVE_WOLF_BIGINT
    if ((int)eSz > 0 && wc_bigint_from_unsigned_bin(&key->e.raw, e, eSz) != 0) {
        mp_clear(&key->n);
        mp_clear(&key->e);
        return ASN_GETINT_E;
    }
#endif /* HAVE_WOLF_BIGINT */

#ifdef WOLFSSL_XILINX_CRYPT
    if (wc_InitRsaHw(key) != 0) {
        return BAD_STATE_E;
    }
#endif

    return 0;
}
#endif /* HAVE_USER_RSA */
#endif /* !NO_RSA */

#ifndef NO_DH
#if defined(WOLFSSL_DH_EXTRA)
/*
 * Decodes DH public key to fill specified DhKey.
 *
 * return 0 on success, negative on failure
 */
int wc_DhPublicKeyDecode(const byte* input, word32* inOutIdx,
                DhKey* key, word32 inSz)
{
    int ret = 0;
    int length;
    word32 oid = 0;

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    ret = GetObjectId(input, inOutIdx, &oid, oidKeyType, inSz);
    if (oid != DHk || ret < 0)
        return ASN_DH_KEY_E;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    if (GetInt(&key->p, input, inOutIdx, inSz) < 0)
        return ASN_DH_KEY_E;

    if (GetInt(&key->g, input, inOutIdx, inSz) < 0) {
        mp_clear(&key->p);
        return ASN_DH_KEY_E;
    }
    ret = (CheckBitString(input, inOutIdx, &length, inSz, 0, NULL) == 0);
    if (ret > 0) {
        /* Found Bit String WOLFSSL_DH_EXTRA is required to access DhKey.pub */
        if (GetInt(&key->pub, input, inOutIdx, inSz) < 0) {
            mp_clear(&key->p);
            mp_clear(&key->g);
            return ASN_DH_KEY_E;
        }
    }
    else {
        mp_clear(&key->p);
        mp_clear(&key->g);
        return ASN_DH_KEY_E;
    }
    return 0;
}
#endif /* WOLFSSL_DH_EXTRA */

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for DH key.
 * PKCS #3, 9 - DHParameter.
 * (Also in: RFC 2786, 3)
 */
static const ASNItem dhParamASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* prime */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* base */
/*  2 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* privateValueLength */
/*  3 */        { 1, ASN_INTEGER, 0, 0, 1 },
};

/* Number of items in ASN.1 template for DH key. */
#define dhParamASN_Length (sizeof(dhParamASN) / sizeof(ASNItem))

#ifdef WOLFSSL_DH_EXTRA
/* ASN.1 template for DH key wrapped in PKCS #8 or SubjectPublicKeyInfo.
 * PKCS #8: RFC 5208, 5 - PrivateKeyInfo
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * RFC 3279, 2.3.2 - DH in SubjectPublicKeyInfo
 */
static const ASNItem dhKeyPkcs8ASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_INTEGER, 0, 0, 1 },
/*  2 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
                    /* DHParameter */
/*  4 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                        /* p */
/*  5 */                { 3, ASN_INTEGER, 0, 0, 0 },
                        /* g */
/*  6 */                { 3, ASN_INTEGER, 0, 0, 0 },
                        /* q - factor of p-1 */
/*  7 */                { 3, ASN_INTEGER, 0, 0, 1 },
                        /* j - subgroup factor */
/*  8 */                { 3, ASN_INTEGER, 0, 0, 1 },
/*  9 */                { 3, ASN_SEQUENCE, 0, 0, 1 },
                /* PrivateKey - PKCS #8 */
/* 10 */        { 1, ASN_OCTET_STRING, 0, 1, 2 },
/* 11 */            { 2, ASN_INTEGER, 0, 0, 0 },
                /* PublicKey - SubjectPublicKeyInfo. */
/* 12 */        { 1, ASN_BIT_STRING, 0, 1, 2 },
/* 13 */            { 2, ASN_INTEGER, 0, 0, 0 },
};

#define dhKeyPkcs8ASN_Length (sizeof(dhKeyPkcs8ASN) / sizeof(ASNItem))
#endif
#endif

/* Decodes either PKCS#3 DH parameters or PKCS#8 DH key file (WOLFSSL_DH_EXTRA).
 *
 * See also wc_DhParamsLoad(). Loads directly into buffers rather than key
 * object.
 *
 * @param [in]      input     BER/DER encoded data.
 * @param [in, out] inOutIdx  On in, start of DH key data.
 *                            On out, end of DH key data.
 * @param [in, out] key       DH key object.
 * @param [in]      inSz      Size of data in bytes.
 * @return  0 on success.
 * @return  BAD_FUNC_ARG when input, inOutIDx or key is NULL.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  MP_INIT_E when the unable to initialize an mp_int.
 * @return  ASN_GETINT_E when the unable to convert data to an mp_int.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int wc_DhKeyDecode(const byte* input, word32* inOutIdx, DhKey* key, word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret = 0;
    int length;
#ifdef WOLFSSL_DH_EXTRA
    #if !defined(HAVE_FIPS) || \
        (defined(HAVE_FIPS_VERSION) && (HAVE_FIPS_VERSION > 2))
    word32 oid = 0, temp = 0;
    #endif
#endif

    WOLFSSL_ENTER("wc_DhKeyDecode");

    if (inOutIdx == NULL)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

#ifdef WOLFSSL_DH_EXTRA
    #if !defined(HAVE_FIPS) || \
        (defined(HAVE_FIPS_VERSION) && (HAVE_FIPS_VERSION > 2))
    temp = *inOutIdx;
    #endif
#endif
    /* Assume input started after 1.2.840.113549.1.3.1 dhKeyAgreement */
    if (GetInt(&key->p, input, inOutIdx, inSz) < 0) {
        ret = ASN_DH_KEY_E;
    }
    if (ret == 0 && GetInt(&key->g, input, inOutIdx, inSz) < 0) {
        mp_clear(&key->p);
        ret = ASN_DH_KEY_E;
    }

#ifdef WOLFSSL_DH_EXTRA
    #if !defined(HAVE_FIPS) || \
        (defined(HAVE_FIPS_VERSION) && (HAVE_FIPS_VERSION > 2))
    /* If ASN_DH_KEY_E: Check if input started at beginning of key */
    if (ret == ASN_DH_KEY_E) {
        *inOutIdx = temp;

        /* the version (0) - private only (for public skip) */
        if (GetASNInt(input, inOutIdx, &length, inSz) == 0) {
            *inOutIdx += length;
        }

        /* Size of dhKeyAgreement section */
        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        /* Check for dhKeyAgreement */
        ret = GetObjectId(input, inOutIdx, &oid, oidKeyType, inSz);
        if (oid != DHk || ret < 0)
            return ASN_DH_KEY_E;

        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        if (GetInt(&key->p, input, inOutIdx, inSz) < 0) {
            return ASN_DH_KEY_E;
        }
        if (ret == 0 && GetInt(&key->g, input, inOutIdx, inSz) < 0) {
            mp_clear(&key->p);
            return ASN_DH_KEY_E;
        }
    }

    temp = *inOutIdx;
    ret = (CheckBitString(input, inOutIdx, &length, inSz, 0, NULL) == 0);
    if (ret > 0) {
        /* Found Bit String */
        if (GetInt(&key->pub, input, inOutIdx, inSz) == 0) {
            WOLFSSL_MSG("Found Public Key");
            ret = 0;
        }
    } else {
        *inOutIdx = temp;
        ret = (GetOctetString(input, inOutIdx, &length, inSz) >= 0);
        if (ret > 0) {
            /* Found Octet String */
            if (GetInt(&key->priv, input, inOutIdx, inSz) == 0) {
                WOLFSSL_MSG("Found Private Key");

                /* Compute public */
                ret = mp_exptmod(&key->g, &key->priv, &key->p, &key->pub);
            }
        } else {
            /* Don't use length from failed CheckBitString/GetOctetString */
            *inOutIdx = temp;
            ret = 0;
        }
    }
    #endif /* !HAVE_FIPS || HAVE_FIPS_VERSION > 2 */
#endif /* WOLFSSL_DH_EXTRA */

    WOLFSSL_LEAVE("wc_DhKeyDecode", ret);

    return ret;
#else
#ifdef WOLFSSL_DH_EXTRA
    DECL_ASNGETDATA(dataASN, dhKeyPkcs8ASN_Length);
#else
    DECL_ASNGETDATA(dataASN, dhParamASN_Length);
#endif
    int ret = 0;

    /* Check input parameters are valid. */
    if ((input == NULL) || (inOutIdx == NULL) || (key == NULL)) {
        ret = BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_DH_EXTRA
    ALLOC_ASNGETDATA(dataASN, dhKeyPkcs8ASN_Length, ret, key->heap);
#else
    ALLOC_ASNGETDATA(dataASN, dhParamASN_Length, ret, key->heap);
#endif

    if (ret == 0) {
        /* Initialize data and set mp_ints to hold p and g. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * dhParamASN_Length);
        GetASN_MP(&dataASN[1], &key->p);
        GetASN_MP(&dataASN[2], &key->g);
        /* Try simple PKCS #3 template. */
        ret = GetASN_Items(dhParamASN, dataASN, dhParamASN_Length, 1, input,
                           inOutIdx, inSz);
#ifdef WOLFSSL_DH_EXTRA
        if (ret != 0) {
            /* Initialize data and set mp_ints to hold p, g, q, priv and pub. */
            XMEMSET(dataASN, 0, sizeof(*dataASN) * dhKeyPkcs8ASN_Length);
            GetASN_ExpBuffer(&dataASN[3], keyDhOid, sizeof(keyDhOid));
            GetASN_MP(&dataASN[5], &key->p);
            GetASN_MP(&dataASN[6], &key->g);
            GetASN_MP(&dataASN[7], &key->q);
            GetASN_MP(&dataASN[11], &key->priv);
            GetASN_MP(&dataASN[13], &key->pub);
            /* Try PKCS #8 wrapped template. */
            ret = GetASN_Items(dhKeyPkcs8ASN, dataASN, dhKeyPkcs8ASN_Length, 1,
                               input, inOutIdx, inSz);
            if (ret == 0) {
                if ((dataASN[11].length != 0) && (dataASN[1].length == 0)) {
                    ret = ASN_PARSE_E;
                }
                else if ((dataASN[13].length != 0) &&
                                                     (dataASN[1].length != 0)) {
                    ret = ASN_PARSE_E;
                }
            }
        }
#endif
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_DH_EXTRA

/* Export DH Key (private or public) */
int wc_DhKeyToDer(DhKey* key, byte* output, word32* outSz, int exportPriv)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret, privSz = 0, pubSz = 0, keySz;
    word32 idx, total;

    if (key == NULL || outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* determine size */
    if (exportPriv) {
        /* octect string: priv */
        privSz = SetASNIntMP(&key->priv, -1, NULL);
        idx = 1 + SetLength(privSz, NULL) + privSz; /* +1 for ASN_OCTET_STRING */
    }
    else {
        /* bit string: public */
        pubSz = SetASNIntMP(&key->pub, -1, NULL);
        idx = SetBitString(pubSz, 0, NULL) + pubSz;
    }
    keySz = idx;

    /* DH Parameters sequence with P and G */
    total = 0;
    ret = wc_DhParamsToDer(key, NULL, &total);
    if (ret != LENGTH_ONLY_E)
        return ret;
    idx += total;

    /* object dhKeyAgreement 1.2.840.113549.1.3.1 */
    idx += SetObjectId(sizeof(keyDhOid), NULL);
    idx += sizeof(keyDhOid);
    /* sequence */
    idx += SetSequence(idx, NULL);
    if (exportPriv) {
        /* version: 0 (ASN_INTEGER, 0x01, 0x00) */
        idx += 3;
    }
    /* sequence */
    total = idx + SetSequence(idx, NULL);

    /* if no output, then just getting size */
    if (output == NULL) {
        *outSz = total;
        return LENGTH_ONLY_E;
    }

    /* make sure output fits in buffer */
    if (total > *outSz) {
        return BUFFER_E;
    }
    total = idx;

    /* sequence */
    idx = SetSequence(total, output);
    if (exportPriv) {
        /* version: 0 */
        idx += SetMyVersion(0, output + idx, 0);
    }
    /* sequence - all but pub/priv */
    idx += SetSequence(total - keySz - idx, output + idx);
    /* object dhKeyAgreement 1.2.840.113549.1.3.1 */
    idx += SetObjectId(sizeof(keyDhOid), output + idx);
    XMEMCPY(output + idx, keyDhOid, sizeof(keyDhOid));
    idx += sizeof(keyDhOid);

    /* DH Parameters sequence with P and G */
    total = *outSz - idx;
    ret = wc_DhParamsToDer(key, output + idx, &total);
    if (ret < 0)
        return ret;
    idx += total;

    /* octect string: priv */
    if (exportPriv) {
        idx += SetOctetString(privSz, output + idx);
        idx += SetASNIntMP(&key->priv, -1, output + idx);
    }
    else {
        /* bit string: public */
        idx += SetBitString(pubSz, 0, output + idx);
        idx += SetASNIntMP(&key->pub, -1, output + idx);
    }
    *outSz = idx;

    return idx;
#else
    ASNSetData dataASN[dhKeyPkcs8ASN_Length];
    int ret = 0;
    int sz;

    WOLFSSL_ENTER("wc_DhKeyToDer");

    XMEMSET(dataASN, 0, sizeof(dataASN));
    SetASN_Int8Bit(&dataASN[1], 0);
    SetASN_OID(&dataASN[3], DHk, oidKeyType);
    /* Set mp_int containing p and g. */
    SetASN_MP(&dataASN[5], &key->p);
    SetASN_MP(&dataASN[6], &key->g);
    dataASN[7].noOut = 1;
    dataASN[8].noOut = 1;
    dataASN[9].noOut = 1;

    if (exportPriv) {
        SetASN_MP(&dataASN[11], &key->priv);
        dataASN[12].noOut = 1;
        dataASN[13].noOut = 1;
    }
    else {
        dataASN[1].noOut = 1;
        dataASN[10].noOut = 1;
        dataASN[11].noOut = 1;
        SetASN_MP(&dataASN[13], &key->pub);
    }

    /* Calculate the size of the DH parameters. */
    ret = SizeASN_Items(dhKeyPkcs8ASN, dataASN, dhKeyPkcs8ASN_Length, &sz);
    if (output == NULL) {
        *outSz = sz;
        ret = LENGTH_ONLY_E;
    }
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && ((int)*outSz < sz)) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode the DH parameters into buffer. */
        SetASN_Items(dhKeyPkcs8ASN, dataASN, dhKeyPkcs8ASN_Length, output);
        /* Set the actual encoding size. */
        *outSz = sz;
        /* Return the actual encoding size. */
        ret = sz;
    }

    return ret;
#endif
}

int wc_DhPubKeyToDer(DhKey* key, byte* out, word32* outSz)
{
    return wc_DhKeyToDer(key, out, outSz, 0);
}
int wc_DhPrivKeyToDer(DhKey* key, byte* out, word32* outSz)
{
    return wc_DhKeyToDer(key, out, outSz, 1);
}


/* Convert DH key parameters to DER format, write to output (outSz)
 * If output is NULL then max expected size is set to outSz and LENGTH_ONLY_E is
 * returned.
 *
 * Note : static function due to redefinition complications with DhKey and FIPS
 * version 2 build.
 *
 * return bytes written on success */
int wc_DhParamsToDer(DhKey* key, byte* output, word32* outSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx, total;

    if (key == NULL || outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* determine size */
    /* integer - g */
    idx = SetASNIntMP(&key->g, -1, NULL);
    /* integer - p */
    idx += SetASNIntMP(&key->p, -1, NULL);
    total = idx;
     /* sequence */
    idx += SetSequence(idx, NULL);

    if (output == NULL) {
        *outSz = idx;
        return LENGTH_ONLY_E;
    }
    /* make sure output fits in buffer */
    if (idx > *outSz) {
        return BUFFER_E;
    }


    /* write DH parameters */
    /* sequence - for P and G only */
    idx = SetSequence(total, output);
    /* integer - p */
    idx += SetASNIntMP(&key->p, -1, output + idx);
    /* integer - g */
    idx += SetASNIntMP(&key->g, -1, output + idx);
    *outSz = idx;

    return idx;
#else
    ASNSetData dataASN[dhParamASN_Length];
    int ret = 0;
    int sz;

    WOLFSSL_ENTER("wc_DhParamsToDer");

    if (key == NULL || outSz == NULL) {
        ret = BAD_FUNC_ARG;
    }

    if (ret == 0) {
        XMEMSET(dataASN, 0, sizeof(dataASN));
        /* Set mp_int containing p and g. */
        SetASN_MP(&dataASN[1], &key->p);
        SetASN_MP(&dataASN[2], &key->g);
        /* privateValueLength not encoded. */
        dataASN[3].noOut = 1;

        /* Calculate the size of the DH parameters. */
        ret = SizeASN_Items(dhParamASN, dataASN, dhParamASN_Length, &sz);
    }
    if ((ret == 0) && (output == NULL)) {
        *outSz = sz;
        ret = LENGTH_ONLY_E;
    }
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && ((int)*outSz < sz)) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode the DH parameters into buffer. */
        SetASN_Items(dhParamASN, dataASN, dhParamASN_Length, output);
        /* Set the actual encoding size. */
        *outSz = sz;
        /* Return count of bytes written. */
        ret = sz;
    }

    return ret;
#endif
}

#endif /* WOLFSSL_DH_EXTRA */

/* Decode DH parameters.
 *
 * PKCS #3, 9 - DHParameter.
 * (Also in: RFC 2786, 3)
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of RSA public key.
 *                            On out, start of ASN.1 item after RSA public key.
 * @param [in]      inSz      Number of bytes in buffer.
 * @param [in, out] p         Buffer to hold prime.
 * @param [out]     pInOutSz  On in, size of buffer to hold prime in bytes.
 *                            On out, size of prime in bytes.
 * @param [in, out] g         Buffer to hold base.
 * @param [out]     gInOutSz  On in, size of buffer to hold base in bytes.
 *                            On out, size of base in bytes.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set.
 */
int wc_DhParamsLoad(const byte* input, word32 inSz, byte* p, word32* pInOutSz,
                 byte* g, word32* gInOutSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int    ret;
    int    length;

    if (GetSequence(input, &idx, &length, inSz) <= 0)
        return ASN_PARSE_E;

    ret = GetASNInt(input, &idx, &length, inSz);
    if (ret != 0)
        return ret;

    if (length <= (int)*pInOutSz) {
        XMEMCPY(p, &input[idx], length);
        *pInOutSz = length;
    }
    else {
        return BUFFER_E;
    }
    idx += length;

    ret = GetASNInt(input, &idx, &length, inSz);
    if (ret != 0)
        return ret;

    if (length <= (int)*gInOutSz) {
        XMEMCPY(g, &input[idx], length);
        *gInOutSz = length;
    }
    else {
        return BUFFER_E;
    }

    return 0;
#else
    DECL_ASNGETDATA(dataASN, dhParamASN_Length);
    word32 idx = 0;
    int ret = 0;

    /* Make sure pointers are valid before use. */
    if ((input == NULL) || (p == NULL) || (pInOutSz == NULL) || (g == NULL) ||
            (gInOutSz == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNGETDATA(dataASN, dhParamASN_Length, ret, NULL);

    if (ret == 0) {
        /* Set the buffers to copy p and g into. */
        GetASN_Buffer(&dataASN[1], p, pInOutSz);
        GetASN_Buffer(&dataASN[2], g, gInOutSz);
        /* Decode the DH Parameters. */
        ret = GetASN_Items(dhParamASN, dataASN, dhParamASN_Length, 1, input,
                           &idx, inSz);
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* !NO_DH */


#ifndef NO_DSA

static mp_int* GetDsaInt(DsaKey* key, int idx)
{
    if (idx == 0)
        return &key->p;
    if (idx == 1)
        return &key->q;
    if (idx == 2)
        return &key->g;
    if (idx == 3)
        return &key->y;
    if (idx == 4)
        return &key->x;

    return NULL;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for DSA public and private keys.
 * Public key: seq, p, q, g, y
 * Private key: seq, version, p, q, g, y, x
 * RFC 3279, 2.3.2 - DSA in SubjectPublicKeyInfo
 */
static const ASNItem dsaKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  2 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  3 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  4 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  5 */        { 1, ASN_INTEGER, 0, 0, 0 },
/*  6 */        { 1, ASN_INTEGER, 0, 0, 0 },
};

/* Number of items in ASN.1 template for DSA private key. */
#define dsaKeyASN_Length (sizeof(dsaKeyASN) / sizeof(ASNItem))
/* Number of items in ASN.1 template for DSA public key. */
#define dsaPublicKeyASN_Length ((sizeof(dsaKeyASN) / sizeof(ASNItem)) - 2)

/* ASN.1 template for PublicKeyInfo with DSA.
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * RFC 3279, 2.3.2 - DSA in SubjectPublicKeyInfo
 */
static const ASNItem dsaPubKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  3 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                        /* p */
/*  4 */                { 3, ASN_INTEGER, 0, 0, 0 },
                        /* q */
/*  5 */                { 3, ASN_INTEGER, 0, 0, 0 },
                        /* g */
/*  6 */                { 3, ASN_INTEGER, 0, 0, 0 },
/*  7 */        { 1, ASN_BIT_STRING, 0, 1, 1 },
                    /* y */
/*  8 */            { 2, ASN_INTEGER, 0, 0, 0 },
};

/* Number of items in ASN.1 template for PublicKeyInfo with DSA. */
#define dsaPubKeyASN_Length (sizeof(dsaPubKeyASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Decode DSA public key.
 *
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * RFC 3279, 2.3.2 - DSA in SubjectPublicKeyInfo
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of DSA public key.
 *                            On out, start of ASN.1 item after DSA public key.
 * @param [in, out] key       DSA key object.
 * @param [in]      inSz      Number of bytes in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int DsaPublicKeyDecode(const byte* input, word32* inOutIdx, DsaKey* key,
                        word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    length;
    int    ret = 0;
    word32 oid;
    word32 maxIdx;

    if (input == NULL || inOutIdx == NULL || key == NULL)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    maxIdx = (word32)(*inOutIdx + length);
    if (GetInt(&key->p,  input, inOutIdx, maxIdx) < 0 ||
        GetInt(&key->q,  input, inOutIdx, maxIdx) < 0 ||
        GetInt(&key->g,  input, inOutIdx, maxIdx) < 0 ||
        GetInt(&key->y,  input, inOutIdx, maxIdx) < 0 )
        ret = ASN_DH_KEY_E;

    if (ret != 0) {
        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        ret = GetObjectId(input, inOutIdx, &oid, oidIgnoreType, inSz);
        if (ret != 0)
            return ret;

        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        if (GetInt(&key->p,  input, inOutIdx, inSz) < 0 ||
            GetInt(&key->q,  input, inOutIdx, inSz) < 0 ||
            GetInt(&key->g,  input, inOutIdx, inSz) < 0)
            return ASN_DH_KEY_E;

        if (CheckBitString(input, inOutIdx, &length, inSz, 0, NULL) < 0)
            return ASN_PARSE_E;

        if (GetInt(&key->y,  input, inOutIdx, inSz) < 0 )
            return ASN_DH_KEY_E;

        ret = 0;
    }

    key->type = DSA_PUBLIC;
    return ret;
#else
    /* dsaPubKeyASN is longer than dsaPublicKeyASN. */
    DECL_ASNGETDATA(dataASN, dsaPubKeyASN_Length);
    int ret = 0;
    int i;

    /* Validated parameters. */
    if ((input == NULL) || (inOutIdx == NULL) || (key == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    ALLOC_ASNGETDATA(dataASN, dsaPubKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Clear dynamic data items. */
        XMEMSET(dataASN, 0, sizeof(ASNGetData) * dsaPublicKeyASN_Length);
        /* p, q, g, y */
        for (i = 0; i < DSA_INTS - 1; i++)
            GetASN_MP(&dataASN[1 + i], GetDsaInt(key, i));
        /* Parse as simple form. */
        ret = GetASN_Items(dsaKeyASN, dataASN, dsaPublicKeyASN_Length, 1, input,
                           inOutIdx, inSz);
        if (ret != 0) {
            /* Clear dynamic data items. */
            XMEMSET(dataASN, 0, sizeof(ASNGetData) * dsaPubKeyASN_Length);
            /* Set DSA OID to expect. */
            GetASN_ExpBuffer(&dataASN[2], keyDsaOid, sizeof(keyDsaOid));
            /* p, q, g */
            for (i = 0; i < DSA_INTS - 2; i++)
                GetASN_MP(&dataASN[4 + i], GetDsaInt(key, i));
            /* y */
            GetASN_MP(&dataASN[8], GetDsaInt(key, i));
            /* Parse as SubjectPublicKeyInfo. */
            ret = GetASN_Items(dsaPubKeyASN, dataASN, dsaPubKeyASN_Length, 1,
                input, inOutIdx, inSz);
        }
    }

    if (ret == 0) {
        /* Data parsed - set type of key parsed. */
        key->type = DSA_PUBLIC;
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
#endif
}

int wc_DsaParamsDecode(const byte* input, word32* inOutIdx, DsaKey* key,
                        word32 inSz)
{
    int    length;
    word32 maxIdx;

    if (input == NULL || inOutIdx == NULL || key == NULL)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    maxIdx = (word32)(*inOutIdx + length);
    if (GetInt(&key->p, input, inOutIdx, maxIdx) < 0 ||
        GetInt(&key->q, input, inOutIdx, maxIdx) < 0 ||
        GetInt(&key->g, input, inOutIdx, maxIdx) < 0)
        return ASN_DH_KEY_E;

    return 0;
}


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for a DSA key holding private key in an OCTET_STRING. */
static const ASNItem dsaKeyOctASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* p */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* q */
/*  2 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* g */
/*  3 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* Private key */
/*  4 */        { 1, ASN_OCTET_STRING, 0, 1, 0 },
                    /* x */
/*  5 */            { 2, ASN_INTEGER, 0, 0, 0 },
};

/* Number of items in ASN.1 template for a DSA key (OCTET_STRING version). */
#define dsaKeyOctASN_Length (sizeof(dsaKeyOctASN) / sizeof(ASNItem))
#endif

/* Decode DSA private key.
 *
 * @param [in]      input     Buffer holding BER encoded data.
 * @param [in, out] inOutIdx  On in, start of DSA public key.
 *                            On out, start of ASN.1 item after DSA public key.
 * @param [in, out] key       DSA key object.
 * @param [in]      inSz      Number of bytes in buffer.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 */
int wc_DsaPrivateKeyDecode(const byte* input, word32* inOutIdx, DsaKey* key,
                           word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int length, version, ret = 0, temp = 0;
    word32 algId = 0;

    /* Sanity checks on input */
    if (input == NULL || inOutIdx == NULL || key == NULL) {
        return BAD_FUNC_ARG;
    }

    /* if has pkcs8 header skip it */
    if (ToTraditionalInline_ex(input, inOutIdx, inSz, &algId) < 0) {
        /* ignore error, did not have pkcs8 header */
    }

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    temp = (int)*inOutIdx;

    /* Default case expects a certificate with OctetString but no version ID */
    ret = GetInt(&key->p, input, inOutIdx, inSz);
    if (ret < 0) {
        mp_clear(&key->p);
        ret = ASN_PARSE_E;
    }
    else {
        ret = GetInt(&key->q, input, inOutIdx, inSz);
        if (ret < 0) {
            mp_clear(&key->p);
            mp_clear(&key->q);
            ret = ASN_PARSE_E;
        }
        else {
            ret = GetInt(&key->g, input, inOutIdx, inSz);
            if (ret < 0) {
                mp_clear(&key->p);
                mp_clear(&key->q);
                mp_clear(&key->g);
                ret = ASN_PARSE_E;
            }
            else {
                ret = GetOctetString(input, inOutIdx, &length, inSz);
                if (ret < 0) {
                    mp_clear(&key->p);
                    mp_clear(&key->q);
                    mp_clear(&key->g);
                    ret = ASN_PARSE_E;
                }
                else {
                    ret = GetInt(&key->y, input, inOutIdx, inSz);
                    if (ret < 0) {
                        mp_clear(&key->p);
                        mp_clear(&key->q);
                        mp_clear(&key->g);
                        mp_clear(&key->y);
                        ret = ASN_PARSE_E;
                    }
                }
            }
        }
    }
    /* An alternate pass if default certificate fails parsing */
    if (ret == ASN_PARSE_E) {
        *inOutIdx = temp;
        if (GetMyVersion(input, inOutIdx, &version, inSz) < 0)
            return ASN_PARSE_E;

        if (GetInt(&key->p,  input, inOutIdx, inSz) < 0 ||
            GetInt(&key->q,  input, inOutIdx, inSz) < 0 ||
            GetInt(&key->g,  input, inOutIdx, inSz) < 0 ||
            GetInt(&key->y,  input, inOutIdx, inSz) < 0 ||
            GetInt(&key->x,  input, inOutIdx, inSz) < 0 )
            return ASN_DH_KEY_E;
    }

    key->type = DSA_PRIVATE;
    return 0;
#else
    /* dsaKeyASN is longer than dsaKeyOctASN. */
    DECL_ASNGETDATA(dataASN, dsaKeyASN_Length);
    int ret = 0;
    int i;
    byte version = 0;

    /* Sanity checks on input */
    if ((input == NULL) || (inOutIdx == NULL) || (key == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    ALLOC_ASNGETDATA(dataASN, dsaKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Initialize key data and set mp_ints for params and priv/pub. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * dsaKeyOctASN_Length);
        GetASN_Int8Bit(&dataASN[1], &version);
        for (i = 0; i < DSA_INTS - 2; i++) {
            GetASN_MP(&dataASN[1 + i], GetDsaInt(key, i));
        }
        GetASN_MP(&dataASN[2 + i], GetDsaInt(key, i));
        /* Try simple form. */
        ret = GetASN_Items(dsaKeyOctASN, dataASN, dsaKeyOctASN_Length, 1, input,
                           inOutIdx, inSz);
        if ((ret == 0) && (version != 0)) {
            ret = ASN_PARSE_E;
        }
        else if (ret != 0) {
            /* Initialize key data and set mp_ints for params and priv/pub. */
            XMEMSET(dataASN, 0, sizeof(*dataASN) * dsaKeyASN_Length);
            for (i = 0; i < DSA_INTS; i++) {
                GetASN_MP(&dataASN[2 + i], GetDsaInt(key, i));
            }

            /* Try simple OCTET_STRING form. */
            ret = GetASN_Items(dsaKeyASN, dataASN, dsaKeyASN_Length, 1, input,
                               inOutIdx, inSz);
        }
    }

    if (ret == 0) {
        /* Set the contents to be a private key. */
        key->type = DSA_PRIVATE;
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
#endif
}

#ifndef WOLFSSL_ASN_TEMPLATE
/* Release Tmp DSA resources */
static WC_INLINE void FreeTmpDsas(byte** tmps, void* heap, int ints)
{
    int i;

    for (i = 0; i < ints; i++)
        XFREE(tmps[i], heap, DYNAMIC_TYPE_DSA);

    (void)heap;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */

#if !defined(HAVE_SELFTEST) && (defined(WOLFSSL_KEY_GEN) || \
        defined(WOLFSSL_CERT_GEN))
/* Encode a DSA public key into buffer.
 *
 * @param [out] output       Buffer to hold encoded data.
 * @param [in]  key          DSA key object.
 * @param [out] outLen       Length of buffer.
 * @param [out] with_header  Whether to encode in SubjectPublicKeyInfo block.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when output or key is NULL, or buffer size is less
 *          than a minimal size (5 bytes), or buffer size is smaller than
 *          encoding size.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
int wc_SetDsaPublicKey(byte* output, DsaKey* key, int outLen, int with_header)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    /* p, g, q = DSA params, y = public exponent */
#ifdef WOLFSSL_SMALL_STACK
    byte* p = NULL;
    byte* g = NULL;
    byte* q = NULL;
    byte* y = NULL;
#else
    byte p[MAX_DSA_INT_SZ];
    byte g[MAX_DSA_INT_SZ];
    byte q[MAX_DSA_INT_SZ];
    byte y[MAX_DSA_INT_SZ];
#endif
    byte innerSeq[MAX_SEQ_SZ];
    byte outerSeq[MAX_SEQ_SZ];
    byte bitString[1 + MAX_LENGTH_SZ + 1];
    int  idx, pSz, gSz, qSz, ySz, innerSeqSz, outerSeqSz, bitStringSz = 0;

    WOLFSSL_ENTER("wc_SetDsaPublicKey");

    if (output == NULL || key == NULL || outLen < MAX_SEQ_SZ) {
        return BAD_FUNC_ARG;
    }

    /* p */
#ifdef WOLFSSL_SMALL_STACK
    p = (byte*)XMALLOC(MAX_DSA_INT_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (p == NULL)
        return MEMORY_E;
#endif
    if ((pSz = SetASNIntMP(&key->p, MAX_DSA_INT_SZ, p)) < 0) {
        WOLFSSL_MSG("SetASNIntMP Error with p");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(p, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return pSz;
    }

    /* q */
#ifdef WOLFSSL_SMALL_STACK
    q = (byte*)XMALLOC(MAX_DSA_INT_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (q == NULL)
        return MEMORY_E;
#endif
    if ((qSz = SetASNIntMP(&key->q, MAX_DSA_INT_SZ, q)) < 0) {
        WOLFSSL_MSG("SetASNIntMP Error with q");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(p, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(q, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return qSz;
    }

    /* g */
#ifdef WOLFSSL_SMALL_STACK
    g = (byte*)XMALLOC(MAX_DSA_INT_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (g == NULL)
        return MEMORY_E;
#endif
    if ((gSz = SetASNIntMP(&key->g, MAX_DSA_INT_SZ, g)) < 0) {
        WOLFSSL_MSG("SetASNIntMP Error with g");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(p, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(q, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(g, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return gSz;
    }

    /* y */
#ifdef WOLFSSL_SMALL_STACK
    y = (byte*)XMALLOC(MAX_DSA_INT_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (y == NULL)
        return MEMORY_E;
#endif
    if ((ySz = SetASNIntMP(&key->y, MAX_DSA_INT_SZ, y)) < 0) {
        WOLFSSL_MSG("SetASNIntMP Error with y");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(p, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(q, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(g, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(y, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ySz;
    }

    innerSeqSz  = SetSequence(pSz + qSz + gSz, innerSeq);

    /* check output size */
    if ((innerSeqSz + pSz + qSz + gSz) > outLen) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(p,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(q,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(g,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(y,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        WOLFSSL_MSG("Error, output size smaller than outlen");
        return BUFFER_E;
    }

    if (with_header) {
        int algoSz;
#ifdef WOLFSSL_SMALL_STACK
        byte* algo = NULL;

        algo = (byte*)XMALLOC(MAX_ALGO_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (algo == NULL) {
            XFREE(p,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(q,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(g,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(y,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            return MEMORY_E;
        }
#else
        byte algo[MAX_ALGO_SZ];
#endif
        algoSz = SetAlgoID(DSAk, algo, oidKeyType, 0);
        bitStringSz  = SetBitString(ySz, 0, bitString);
        outerSeqSz = SetSequence(algoSz + innerSeqSz + pSz + qSz + gSz,
                                                                      outerSeq);

        idx = SetSequence(algoSz + innerSeqSz + pSz + qSz + gSz + bitStringSz +
                                                      ySz + outerSeqSz, output);

        /* check output size */
        if ((idx + algoSz + bitStringSz + innerSeqSz + pSz + qSz + gSz + ySz) >
                                                                       outLen) {
            #ifdef WOLFSSL_SMALL_STACK
                XFREE(p,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(q,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(g,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(y,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(algo, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            #endif
            WOLFSSL_MSG("Error, output size smaller than outlen");
            return BUFFER_E;
        }

        /* outerSeq */
        XMEMCPY(output + idx, outerSeq, outerSeqSz);
        idx += outerSeqSz;
        /* algo */
        XMEMCPY(output + idx, algo, algoSz);
        idx += algoSz;
#ifdef WOLFSSL_SMALL_STACK
        XFREE(algo, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
    } else {
        idx = 0;
    }

    /* innerSeq */
    XMEMCPY(output + idx, innerSeq, innerSeqSz);
    idx += innerSeqSz;
    /* p */
    XMEMCPY(output + idx, p, pSz);
    idx += pSz;
    /* q */
    XMEMCPY(output + idx, q, qSz);
    idx += qSz;
    /* g */
    XMEMCPY(output + idx, g, gSz);
    idx += gSz;
    /* bit string */
    if (bitStringSz > 0) {
        XMEMCPY(output + idx, bitString, bitStringSz);
        idx += bitStringSz;
    }
    /* y */
    XMEMCPY(output + idx, y, ySz);
    idx += ySz;

#ifdef WOLFSSL_SMALL_STACK
    XFREE(p,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(q,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(g,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(y,    key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
    return idx;
#else
    DECL_ASNSETDATA(dataASN, dsaPubKeyASN_Length);
    int ret = 0;
    int i;
    int sz;
    int o;

    WOLFSSL_ENTER("wc_SetDsaPublicKey");

    if ((output == NULL) || (key == NULL) || (outLen < MAX_SEQ_SZ)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNSETDATA(dataASN, dsaPubKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* With header - include the SubjectPublicKeyInfo wrapping. */
        if (with_header) {
            o = 0;
            /* Set the algorithm OID to write out. */
            SetASN_OID(&dataASN[2], DSAk, oidKeyType);
        }
        else {
            o = 3;
            /* Skip BIT_STRING but include 'y'. */
            dataASN[7].noOut = 1;
        }
        /* Set the mp_ints to encode - parameters and public value. */
        for (i = 0; i < DSA_INTS - 2; i++) {
            SetASN_MP(&dataASN[4 + i], GetDsaInt(key, i));
        }
        SetASN_MP(&dataASN[5 + i], GetDsaInt(key, i));
        /* Calculate size of the encoding. */
        ret = SizeASN_Items(dsaPubKeyASN + o, dataASN, dsaPubKeyASN_Length - o,
                            &sz);
    }
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && (sz > (int)outLen)) {
        ret = BAD_FUNC_ARG;
    }
    if (ret == 0) {
        /* Encode the DSA public key into output buffer.
         * 'o' indicates offset when no header.
         */
        SetASN_Items(dsaPubKeyASN + o, dataASN, dsaPubKeyASN_Length - o,
                     output);
        /* Return the size of the encoding. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, key->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* Encode a DSA public key into buffer.
 *
 * @param [out] output       Buffer to hold encoded data.
 * @param [in]  key          DSA key object.
 * @param [out] outLen       Length of buffer.
 * @param [out] with_header  Whether to encode in SubjectPublicKeyInfo block.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when output or key is NULL, or buffer size is less
 *          than a minimal size (5 bytes), or buffer size is smaller than
 *          encoding size.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
int wc_DsaKeyToPublicDer(DsaKey* key, byte* output, word32 inLen)
{
    return wc_SetDsaPublicKey(output, key, inLen, 1);
}
#endif /* !HAVE_SELFTEST && (WOLFSSL_KEY_GEN || WOLFSSL_CERT_GEN) */

static int DsaKeyIntsToDer(DsaKey* key, byte* output, word32* inLen,
                           int ints, int includeVersion)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 seqSz = 0, verSz = 0, rawLen, intTotalLen = 0;
    word32 sizes[DSA_INTS];
    int    i, j, outLen, ret = 0, mpSz;

    byte  seq[MAX_SEQ_SZ];
    byte  ver[MAX_VERSION_SZ];
    byte* tmps[DSA_INTS];

    if (ints > DSA_INTS || inLen == NULL)
        return BAD_FUNC_ARG;

    XMEMSET(sizes, 0, sizeof(sizes));
    for (i = 0; i < ints; i++)
        tmps[i] = NULL;

    /* write all big ints from key to DER tmps */
    for (i = 0; i < ints; i++) {
        mp_int* keyInt = GetDsaInt(key, i);

        rawLen = mp_unsigned_bin_size(keyInt) + 1;
        tmps[i] = (byte*)XMALLOC(rawLen + MAX_SEQ_SZ, key->heap,
                                                              DYNAMIC_TYPE_DSA);
        if (tmps[i] == NULL) {
            ret = MEMORY_E;
            break;
        }

        mpSz = SetASNIntMP(keyInt, -1, tmps[i]);
        if (mpSz < 0) {
            ret = mpSz;
            break;
        }
        intTotalLen += (sizes[i] = mpSz);
    }

    if (ret != 0) {
        FreeTmpDsas(tmps, key->heap, ints);
        return ret;
    }

    /* make headers */
    if (includeVersion)
        verSz = SetMyVersion(0, ver, FALSE);
    seqSz = SetSequence(verSz + intTotalLen, seq);

    outLen = seqSz + verSz + intTotalLen;
    *inLen = outLen;
    if (output == NULL) {
        FreeTmpDsas(tmps, key->heap, ints);
        return LENGTH_ONLY_E;
    }
    if (outLen > (int)*inLen) {
        FreeTmpDsas(tmps, key->heap, ints);
        return BAD_FUNC_ARG;
    }

    /* write to output */
    XMEMCPY(output, seq, seqSz);
    j = seqSz;
    if (includeVersion) {
        XMEMCPY(output + j, ver, verSz);
        j += verSz;
    }

    for (i = 0; i < ints; i++) {
        XMEMCPY(output + j, tmps[i], sizes[i]);
        j += sizes[i];
    }
    FreeTmpDsas(tmps, key->heap, ints);

    return outLen;
#else
    DECL_ASNSETDATA(dataASN, dsaKeyASN_Length);
    int ret = 0;
    int i;
    int sz;

    (void)ints;

    if ((key == NULL) || (inLen == NULL)) {
        ret = BAD_FUNC_ARG;
    }
    if ((ret == 0) && (ints > DSA_INTS)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNSETDATA(dataASN, dsaKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        if (includeVersion) {
            /* Set the version. */
            SetASN_Int8Bit(&dataASN[1], 0);
        }
        else {
            dataASN[1].noOut = 1;
        }
        dataASN[5].noOut = mp_iszero(&key->y);
        dataASN[6].noOut = mp_iszero(&key->x);
        /* Set the mp_ints to encode - params, public and private value. */
        for (i = 0; i < DSA_INTS; i++) {
            SetASN_MP(&dataASN[2 + i], GetDsaInt(key, i));
        }
        /* Calculate size of the encoding. */
        ret = SizeASN_Items(dsaKeyASN, dataASN, dsaKeyASN_Length, &sz);
    }
    if ((ret == 0) && (output == NULL)) {
        *inLen = sz;
        ret = LENGTH_ONLY_E;
    }
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && (sz > (int)*inLen)) {
        ret = BAD_FUNC_ARG;
    }
    if (ret == 0) {
        /* Encode the DSA private key into output buffer. */
        SetASN_Items(dsaKeyASN, dataASN, dsaKeyASN_Length, output);
        /* Return the size of the encoding. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, key->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* Encode a DSA private key into buffer.
 *
 * @param [in]  key          DSA key object.
 * @param [out] output       Buffer to hold encoded data.
 * @param [out] inLen        Length of buffer.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key or output is NULL, or key is not a private key
 *          or, buffer size is smaller than encoding size.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
int wc_DsaKeyToDer(DsaKey* key, byte* output, word32 inLen)
{
    if (!key || !output)
        return BAD_FUNC_ARG;

    if (key->type != DSA_PRIVATE)
        return BAD_FUNC_ARG;

    return DsaKeyIntsToDer(key, output, &inLen, DSA_INTS, 1);
}

/* Convert DsaKey parameters to DER format, write to output (inLen),
   return bytes written. Version is excluded to be compatible with
   OpenSSL d2i_DSAparams */
int wc_DsaKeyToParamsDer(DsaKey* key, byte* output, word32 inLen)
{
    if (!key || !output)
        return BAD_FUNC_ARG;

    return DsaKeyIntsToDer(key, output, &inLen, DSA_PARAM_INTS, 0);
}

/* This version of the function allows output to be NULL. In that case, the
   DsaKeyIntsToDer will return LENGTH_ONLY_E and the required output buffer
   size will be pointed to by inLen. */
int wc_DsaKeyToParamsDer_ex(DsaKey* key, byte* output, word32* inLen)
{
    if (!key || !inLen)
        return BAD_FUNC_ARG;

    return DsaKeyIntsToDer(key, output, inLen, DSA_PARAM_INTS, 0);
}

#endif /* NO_DSA */

/* Initialize decoded certificate object with buffer of DER encoding.
 *
 * @param [in, out] cert    Decoded certificate object.
 * @param [in]      source  Buffer containing DER encoded certificate.
 * @param [in]      inSz    Size of DER data in buffer in bytes.
 * @param [in]      heap    Dynamic memory hint.
 */
void InitDecodedCert(DecodedCert* cert,
                     const byte* source, word32 inSz, void* heap)
{
    if (cert != NULL) {
        XMEMSET(cert, 0, sizeof(DecodedCert));

        cert->subjectCNEnc    = CTC_UTF8;
        cert->issuer[0]       = '\0';
        cert->subject[0]      = '\0';
        cert->source          = source;  /* don't own */
        cert->maxIdx          = inSz;    /* can't go over this index */
        cert->heap            = heap;
        cert->maxPathLen      = WOLFSSL_MAX_PATH_LEN;
    #ifdef WOLFSSL_CERT_GEN
        cert->subjectSNEnc    = CTC_UTF8;
        cert->subjectCEnc     = CTC_PRINTABLE;
        cert->subjectLEnc     = CTC_UTF8;
        cert->subjectSTEnc    = CTC_UTF8;
        cert->subjectOEnc     = CTC_UTF8;
        cert->subjectOUEnc    = CTC_UTF8;
    #endif /* WOLFSSL_CERT_GEN */

    #ifndef NO_CERTS
        InitSignatureCtx(&cert->sigCtx, heap, INVALID_DEVID);
    #endif
    }
}

/* Free the alternative names object.
 *
 * Frees each linked list items and its name.
 *
 * @param [in, out] altNames  Alternative names.
 * @param [in]      heap      Dynamic memory hint.
 */
void FreeAltNames(DNS_entry* altNames, void* heap)
{
    (void)heap;

    while (altNames) {
        DNS_entry* tmp = altNames->next;

        XFREE(altNames->name, heap, DYNAMIC_TYPE_ALTNAME);
        XFREE(altNames,       heap, DYNAMIC_TYPE_ALTNAME);
        altNames = tmp;
    }
}

#ifndef IGNORE_NAME_CONSTRAINTS

/* Free the subtree names object.
 *
 * Frees each linked list items and its name.
 *
 * @param [in, out] names  Subtree names.
 * @param [in]      heap   Dynamic memory hint.
 */
void FreeNameSubtrees(Base_entry* names, void* heap)
{
    (void)heap;

    while (names) {
        Base_entry* tmp = names->next;

        XFREE(names->name, heap, DYNAMIC_TYPE_ALTNAME);
        XFREE(names,       heap, DYNAMIC_TYPE_ALTNAME);
        names = tmp;
    }
}

#endif /* IGNORE_NAME_CONSTRAINTS */

/* Free the decoded cert object's dynamic data.
 *
 * @param [in, out] cert  Decoded certificate object.
 */
void FreeDecodedCert(DecodedCert* cert)
{
    if (cert == NULL)
        return;
    if (cert->subjectCNStored == 1)
        XFREE(cert->subjectCN, cert->heap, DYNAMIC_TYPE_SUBJECT_CN);
    if (cert->pubKeyStored == 1)
        XFREE((void*)cert->publicKey, cert->heap, DYNAMIC_TYPE_PUBLIC_KEY);
    if (cert->weOwnAltNames && cert->altNames)
        FreeAltNames(cert->altNames, cert->heap);
#ifndef IGNORE_NAME_CONSTRAINTS
    if (cert->altEmailNames)
        FreeAltNames(cert->altEmailNames, cert->heap);
    if (cert->altDirNames)
        FreeAltNames(cert->altDirNames, cert->heap);
    if (cert->permittedNames)
        FreeNameSubtrees(cert->permittedNames, cert->heap);
    if (cert->excludedNames)
        FreeNameSubtrees(cert->excludedNames, cert->heap);
#endif /* IGNORE_NAME_CONSTRAINTS */
#ifdef WOLFSSL_SEP
    XFREE(cert->deviceType, cert->heap, DYNAMIC_TYPE_X509_EXT);
    XFREE(cert->hwType, cert->heap, DYNAMIC_TYPE_X509_EXT);
    XFREE(cert->hwSerialNum, cert->heap, DYNAMIC_TYPE_X509_EXT);
#endif /* WOLFSSL_SEP */
#ifdef WOLFSSL_X509_NAME_AVAILABLE
    if (cert->issuerName != NULL)
        wolfSSL_X509_NAME_free((WOLFSSL_X509_NAME*)cert->issuerName);
    if (cert->subjectName != NULL)
        wolfSSL_X509_NAME_free((WOLFSSL_X509_NAME*)cert->subjectName);
#endif /* WOLFSSL_X509_NAME_AVAILABLE */
#ifdef WOLFSSL_RENESAS_TSIP_TLS
    if (cert->tsip_encRsaKeyIdx != NULL)
        XFREE(cert->tsip_encRsaKeyIdx, cert->heap, DYNAMIC_TYPE_RSA);
#endif
#ifndef NO_CERTS
    FreeSignatureCtx(&cert->sigCtx);
#endif
}

#ifndef WOLFSSL_ASN_TEMPLATE
static int GetCertHeader(DecodedCert* cert)
{
    int ret = 0, len;

    if (GetSequence(cert->source, &cert->srcIdx, &len, cert->maxIdx) < 0)
        return ASN_PARSE_E;

    /* Reset the max index for the size indicated in the outer wrapper. */
    cert->maxIdx = len + cert->srcIdx;
    cert->certBegin = cert->srcIdx;

    if (GetSequence(cert->source, &cert->srcIdx, &len, cert->maxIdx) < 0)
        return ASN_PARSE_E;

    cert->sigIndex = len + cert->srcIdx;
    if (cert->sigIndex > cert->maxIdx)
        return ASN_PARSE_E;

    if (GetExplicitVersion(cert->source, &cert->srcIdx, &cert->version,
                                                            cert->sigIndex) < 0)
        return ASN_PARSE_E;

    if (GetSerialNumber(cert->source, &cert->srcIdx, cert->serial,
                                           &cert->serialSz, cert->sigIndex) < 0)
        return ASN_PARSE_E;

    return ret;
}
#endif

#if defined(HAVE_ED25519) || defined(HAVE_ED448)
/* Store the key data under the BIT_STRING in dynamicly allocated data.
 *
 * @param [in, out] cert    Certificate object.
 * @param [in]      source  Buffer containing encoded key.
 * @param [in, out] srcIdx  On in, start of key data.
 *                          On out, start of element after key data.
 * @param [in]      maxIdx  Maximum index of certificate data.
 */
static int StoreKey(DecodedCert* cert, const byte* source, word32* srcIdx,
                    word32 maxIdx)
{
    int ret;
    int length;
    byte* publicKey;

    ret = CheckBitString(source, srcIdx, &length, maxIdx, 1, NULL);
    if (ret == 0) {
    #ifdef HAVE_OCSP
        ret = CalcHashId(source + *srcIdx, length, cert->subjectKeyHash);
    }
    if (ret == 0) {
    #endif
        publicKey = (byte*)XMALLOC(length, cert->heap, DYNAMIC_TYPE_PUBLIC_KEY);
        if (publicKey == NULL) {
            ret = MEMORY_E;
        }
    }
    if (ret == 0) {
        XMEMCPY(publicKey, &source[*srcIdx], length);
        cert->publicKey = publicKey;
        cert->pubKeyStored = 1;
        cert->pubKeySize   = length;

        *srcIdx += length;
    }

    return ret;
}
#endif /* HAVE_ED25519 || HAVE_ED448 */

#if !defined(NO_RSA)
#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for header before RSA key in certificate. */
static const ASNItem rsaCertKeyASN[] = {
/*  0 */    { 0, ASN_BIT_STRING, 0, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 0, 0 },
};

/* Number of items in ASN.1 template for header before RSA key in cert. */
#define rsaCertKeyASN_Length (sizeof(rsaCertKeyASN) / sizeof(ASNItem))
#endif

/* Store RSA key pointer and length in certificate object.
 *
 * @param [in, out] cert    Certificate object.
 * @param [in]      source  Buffer containing encoded key.
 * @param [in, out] srcIdx  On in, start of RSA key data.
 *                          On out, start of element after RSA key data.
 * @param [in]      maxIdx  Maximum index of key data.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 */
static int StoreRsaKey(DecodedCert* cert, const byte* source, word32* srcIdx,
                       word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    length;
    int    pubLen;
    word32 pubIdx;

    if (CheckBitString(source, srcIdx, &pubLen, maxIdx, 1, NULL) != 0)
        return ASN_PARSE_E;
    pubIdx = *srcIdx;

    if (GetSequence(source, srcIdx, &length, pubIdx + pubLen) < 0)
        return ASN_PARSE_E;

#if defined(WOLFSSL_RENESAS_TSIP)
    cert->sigCtx.pubkey_n_start = cert->sigCtx.pubkey_e_start = pubIdx;
#endif
    cert->pubKeySize = pubLen;
    cert->publicKey = source + pubIdx;
    *srcIdx += length;

#ifdef HAVE_OCSP
    return CalcHashId(cert->publicKey, cert->pubKeySize, cert->subjectKeyHash);
#else
    return 0;
#endif
#else
    ASNGetData dataASN[rsaCertKeyASN_Length];
    int ret;

    /* No dynamic data. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    /* Decode the header before the key data. */
    ret = GetASN_Items(rsaCertKeyASN, dataASN, rsaCertKeyASN_Length, 1, source,
                       srcIdx, maxIdx);
    if (ret == 0) {
        /* Store the pointer and length in certificate object starting at
         * SEQUENCE. */
        GetASN_GetConstRef(&dataASN[0], &cert->publicKey, &cert->pubKeySize);

    #if defined(WOLFSSL_RENESAS_TSIP)
        /* Start of SEQUENCE. */
        cert->sigCtx.pubkey_n_start = cert->sigCtx.pubkey_e_start =
                dataASN[1].offset;
    #endif
    #ifdef HAVE_OCSP
        /* Calculate the hash of the public key for OCSP. */
        ret = CalcHashId(cert->publicKey, cert->pubKeySize,
                         cert->subjectKeyHash);
    #endif
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* !NO_RSA */

#ifdef HAVE_ECC

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for header before ECC key in certificate. */
static const ASNItem eccCertKeyASN[] = {
/*  0 */        { 1, ASN_OBJECT_ID, 0, 0, 2 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 0, 2 },
/*  2 */    { 0, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for header before ECC key in cert. */
#define eccCertKeyASN_Length (sizeof(eccCertKeyASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Store public ECC key in certificate object.
 *
 * Parse parameters and store public key data.
 *
 * @param [in, out] cert       Certificate object.
 * @param [in]      source     Buffer containing encoded key.
 * @param [in, out] srcIdx     On in, start of ECC key data.
 *                             On out, start of element after ECC key data.
 * @param [in]      maxIdx     Maximum index of key data.
 * @param [in]      pubKey     Buffer holding encoded public key.
 * @param [in]      pubKeyLen  Length of encoded public key in bytes.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 */
static int StoreEccKey(DecodedCert* cert, const byte* source, word32* srcIdx,
                       word32 maxIdx, const byte* pubKey, word32 pubKeyLen)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
    word32 localIdx;
    byte* publicKey;
    byte  tag;
    int length;

    localIdx = *srcIdx;
    if (GetASNTag(source, &localIdx, &tag, maxIdx) < 0)
        return ASN_PARSE_E;

    if (tag != (ASN_SEQUENCE | ASN_CONSTRUCTED)) {
        if (GetObjectId(source, srcIdx, &cert->pkCurveOID, oidCurveType,
                                                                    maxIdx) < 0)
            return ASN_PARSE_E;

        if (CheckCurve(cert->pkCurveOID) < 0)
            return ECC_CURVE_OID_E;

        /* key header */
        ret = CheckBitString(source, srcIdx, &length, maxIdx, 1, NULL);
        if (ret != 0)
            return ret;
    #ifdef HAVE_OCSP
        ret = CalcHashId(source + *srcIdx, length, cert->subjectKeyHash);
        if (ret != 0)
            return ret;
    #endif
        *srcIdx += length;
    }

    publicKey = (byte*)XMALLOC(pubKeyLen, cert->heap, DYNAMIC_TYPE_PUBLIC_KEY);
    if (publicKey == NULL)
        return MEMORY_E;
    XMEMCPY(publicKey, pubKey, pubKeyLen);
    cert->publicKey = publicKey;
    cert->pubKeyStored = 1;
    cert->pubKeySize   = pubKeyLen;

    return 0;
#else
    ASNGetData dataASN[eccCertKeyASN_Length];
    int ret;
    byte* publicKey;

    /* Clear dynamic data and check OID is a curve. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_OID(&dataASN[0], oidCurveType);
    /* Parse ECC public key header. */
    ret = GetASN_Items(eccCertKeyASN, dataASN, eccCertKeyASN_Length, 1, source,
                       srcIdx, maxIdx);
    if (ret == 0) {
        if (dataASN[0].tag != 0) {
            /* Store curve OID. */
            cert->pkCurveOID = dataASN[0].data.oid.sum;
        }
        /* Ignore explicit parameters. */

    #ifdef HAVE_OCSP
        /* Calculate the hash of the subject public key for OCSP. */
        ret = CalcHashId(dataASN[2].data.ref.data, dataASN[2].data.ref.length,
                         cert->subjectKeyHash);
    }
    if (ret == 0) {
    #endif
        /* Store publc key data length. */
        cert->pubKeySize = pubKeyLen;
        /* Must allocated space for key.
         * Don't memcpy into constant pointer so use temp. */
        publicKey = (byte*)XMALLOC(cert->pubKeySize, cert->heap,
                                   DYNAMIC_TYPE_PUBLIC_KEY);
        if (publicKey == NULL) {
            ret = MEMORY_E;
        }
    }
    if (ret == 0) {
        /* Copy in whole public key and store pointer. */
        XMEMCPY(publicKey, pubKey, cert->pubKeySize);
        cert->publicKey = publicKey;
        /* Indicate publicKey needs to be freed. */
        cert->pubKeyStored = 1;
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* HAVE_ECC */

#if !defined(NO_DSA)
#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for DSA key in certificate.
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * RFC 3279, 2.3.2 - DSA in SubjectPublicKeyInfo
 */
static const ASNItem dsaCertKeyASN[] = {
/*  0 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */            { 2, ASN_INTEGER, 0, 0, 0 },
/*  2 */            { 2, ASN_INTEGER, 0, 0, 0 },
/*  3 */            { 2, ASN_INTEGER, 0, 0, 0 },
/*  4 */    { 0, ASN_BIT_STRING, 0, 1, 0 },
/*  5 */        { 1, ASN_INTEGER, 0, 0, 0 },
};

/* Number of items in ASN.1 template for DSA key in certificate. */
#define dsaCertKeyASN_Length (sizeof(dsaCertKeyASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Parse DSA parameters to ensure valid.
 *
 * @param [in]      source  Buffer containing encoded key.
 * @param [in, out] srcIdx  On in, start of DSA key data.
 *                          On out, start of element after DSA key data.
 * @param [in]      maxIdx  Maximum index of key data.
 * @param [in]      heap    Dynamic memory hint.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 */
static int ParseDsaKey(const byte* source, word32* srcIdx, word32 maxIdx,
                       void* heap)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
    int length;

    (void)heap;

    ret = GetSequence(source, srcIdx, &length, maxIdx);
    if (ret < 0)
        return ret;

    ret = SkipInt(source, srcIdx, maxIdx);
    if (ret != 0)
        return ret;
    ret = SkipInt(source, srcIdx, maxIdx);
    if (ret != 0)
        return ret;
    ret = SkipInt(source, srcIdx, maxIdx);
    if (ret != 0)
        return ret;

    ret = CheckBitString(source, srcIdx, &length, maxIdx, 1, NULL);
    if (ret != 0)
        return ret;

    ret = GetASNInt(source, srcIdx, &length, maxIdx);
    if (ret != 0)
        return ASN_PARSE_E;

    *srcIdx += length;

    return 0;
#else
    DECL_ASNGETDATA(dataASN, dsaCertKeyASN_Length);
    int ret = 0;

    (void)heap;

    CALLOC_ASNGETDATA(dataASN, dsaCertKeyASN_Length, ret, heap);
    if (ret == 0) {
        /* Parse the DSA key data to ensure valid. */
        ret = GetASN_Items(dsaCertKeyASN, dataASN, dsaCertKeyASN_Length, 1,
                           source, srcIdx, maxIdx);
    }

    FREE_ASNGETDATA(dataASN, heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* !NO_DSA */

#ifdef HAVE_NTRU
/* Store NTRU key data and length in certificate object.
 *
 * @param [in, out] cert    Certificate object.
 * @param [in]      source  Buffer containing encoded key.
 * @param [in, out] srcIdx  On in, start of RSA key data.
 *                          On out, start of element after RSA key data.
 * @param [in]      maxIdx  Maximum index of key data.
 * @param [in]      pubIdx  Index of into buffer of public key.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_NTRU_KEY_E when BER encoding is invalid.
 */
static int StoreNtruKey(DecodedCert* cert, const byte* source, word32* srcIdx,
                        word32 maxIdx, word32 pubIdx)
{
    const byte* key = &source[pubIdx];
    byte*       next = (byte*)key;
    word16      keyLen;
    word32      rc;
    word32      remaining = maxIdx - *srcIdx;
    byte*       publicKey;
#ifdef WOLFSSL_SMALL_STACK
    byte*       keyBlob = NULL;
#else
    byte        keyBlob[MAX_NTRU_KEY_SZ];
#endif
    rc = ntru_crypto_ntru_encrypt_subjectPublicKeyInfo2PublicKey(key, &keyLen,
                                                     NULL, &next, &remaining);
    if (rc != NTRU_OK)
        return ASN_NTRU_KEY_E;
    if (keyLen > MAX_NTRU_KEY_SZ)
        return ASN_NTRU_KEY_E;

#ifdef WOLFSSL_SMALL_STACK
    keyBlob = (byte*)XMALLOC(MAX_NTRU_KEY_SZ, cert->heap,
                             DYNAMIC_TYPE_TMP_BUFFER);
    if (keyBlob == NULL)
        return MEMORY_E;
#endif

    rc = ntru_crypto_ntru_encrypt_subjectPublicKeyInfo2PublicKey(key, &keyLen,
                                                  keyBlob, &next, &remaining);
    if (rc != NTRU_OK) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(keyBlob, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ASN_NTRU_KEY_E;
    }

    if ( (next - key) < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(keyBlob, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ASN_NTRU_KEY_E;
    }

    *srcIdx = pubIdx + (int)(next - key);

    publicKey = (byte*)XMALLOC(keyLen, cert->heap, DYNAMIC_TYPE_PUBLIC_KEY);
    if (publicKey == NULL) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(keyBlob, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return MEMORY_E;
    }
    XMEMCPY(publicKey, keyBlob, keyLen);
    cert->publicKey    = publicKey;
    cert->pubKeyStored = 1;
    cert->pubKeySize   = keyLen;

#ifdef WOLFSSL_SMALL_STACK
    XFREE(keyBlob, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return 0;
}
#endif /* HAVE_NTRU */

/* Decode the SubjectPublicKeyInfo block in a certificate.
 *
 * Stores the public key in fields of the certificate object.
 * Validates the BER/DER items and does not store in a key object.
 *
 * @param [in, out] cert      Decoded certificate oject.
 * @param [in]      source    BER/DER encoded SubjectPublicKeyInfo block.
 * @param [in, out] inOutIdx  On in, start of public key.
 *                            On out, start of ASN.1 item after public key.
 * @param [in]      maxIdx    Maximum index of key data.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 */
static int GetCertKey(DecodedCert* cert, const byte* source, word32* inOutIdx,
                      word32 maxIdx)
{
    word32 srcIdx = *inOutIdx;
#if defined(HAVE_ECC) || !defined(NO_DSA)
    int pubLen;
#endif
#if defined(HAVE_ECC) || defined(HAVE_NTRU) || !defined(NO_DSA)
    int pubIdx = srcIdx;
#endif
    int ret = 0;
    int length;

#ifndef WOLFSSL_ASN_TEMPLATE
    if (GetSequence(source, &srcIdx, &length, maxIdx) < 0)
#else
    /* Get SEQUENCE and expect all data to be accounted for. */
    if (GetASN_Sequence(source, &srcIdx, &length, maxIdx, 1) != 0)
#endif
    {
        return ASN_PARSE_E;
    }

#if defined(HAVE_ECC) || !defined(NO_DSA)
    pubLen = srcIdx - pubIdx + length;
#endif
    maxIdx = srcIdx + length;

    /* Decode the algorithm identifier for the key. */
    if (GetAlgoId(source, &srcIdx, &cert->keyOID, oidKeyType, maxIdx) < 0) {
        return ASN_PARSE_E;
    }

    (void)length;

    /* Parse each type of public key. */
    switch (cert->keyOID) {
    #ifndef NO_RSA
        case RSAk:
            ret = StoreRsaKey(cert, source, &srcIdx, maxIdx);
            break;

    #endif /* NO_RSA */
    #ifdef HAVE_NTRU
        case NTRUk:
            ret = StoreNtruKey(cert, source, &srcIdx, maxIdx, pubIdx);
            break;
    #endif /* HAVE_NTRU */
    #ifdef HAVE_ECC
        case ECDSAk:
            ret = StoreEccKey(cert, source, &srcIdx, maxIdx, source + pubIdx,
                              pubLen);
            break;
    #endif /* HAVE_ECC */
    #ifdef HAVE_ED25519
        case ED25519k:
            cert->pkCurveOID = ED25519k;
            ret = StoreKey(cert, source, &srcIdx, maxIdx);
            break;
    #endif /* HAVE_ED25519 */
    #ifdef HAVE_ED448
        case ED448k:
            cert->pkCurveOID = ED448k;
            ret = StoreKey(cert, source, &srcIdx, maxIdx);
            break;
    #endif /* HAVE_ED448 */
    #ifndef NO_DSA
        case DSAk:
            cert->publicKey = source + pubIdx;
            cert->pubKeySize = pubLen;
            ret = ParseDsaKey(source, &srcIdx, maxIdx, cert->heap);
            break;
    #endif /* NO_DSA */
        default:
            WOLFSSL_MSG("Unknown or not compiled in key OID");
            ret = ASN_UNKNOWN_OID_E;
    }

    /* Return index after public key. */
    *inOutIdx = srcIdx;

    /* Return error code. */
    return ret;
}

#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
#if defined(HAVE_ECC)
/* Converts ECC curve enum values in ecc_curve_id to the associated OpenSSL NID
 * value.
 *
 * @param [in] n  ECC curve id.
 * @return  ECC curve NID (OpenSSL compatable value).
 */
WOLFSSL_API int EccEnumToNID(int n)
{
    WOLFSSL_ENTER("EccEnumToNID()");

    switch(n) {
        case ECC_SECP192R1:
            return NID_X9_62_prime192v1;
        case ECC_PRIME192V2:
            return NID_X9_62_prime192v2;
        case ECC_PRIME192V3:
            return NID_X9_62_prime192v3;
        case ECC_PRIME239V1:
            return NID_X9_62_prime239v1;
        case ECC_PRIME239V2:
            return NID_X9_62_prime239v2;
        case ECC_PRIME239V3:
            return NID_X9_62_prime239v3;
        case ECC_SECP256R1:
            return NID_X9_62_prime256v1;
        case ECC_SECP112R1:
            return NID_secp112r1;
        case ECC_SECP112R2:
            return NID_secp112r2;
        case ECC_SECP128R1:
            return NID_secp128r1;
        case ECC_SECP128R2:
            return NID_secp128r2;
        case ECC_SECP160R1:
            return NID_secp160r1;
        case ECC_SECP160R2:
            return NID_secp160r2;
        case ECC_SECP224R1:
            return NID_secp224r1;
        case ECC_SECP384R1:
            return NID_secp384r1;
        case ECC_SECP521R1:
            return NID_secp521r1;
        case ECC_SECP160K1:
            return NID_secp160k1;
        case ECC_SECP192K1:
            return NID_secp192k1;
        case ECC_SECP224K1:
            return NID_secp224k1;
        case ECC_SECP256K1:
            return NID_secp256k1;
        case ECC_BRAINPOOLP160R1:
            return NID_brainpoolP160r1;
        case ECC_BRAINPOOLP192R1:
            return NID_brainpoolP192r1;
        case ECC_BRAINPOOLP224R1:
            return NID_brainpoolP224r1;
        case ECC_BRAINPOOLP256R1:
            return NID_brainpoolP256r1;
        case ECC_BRAINPOOLP320R1:
            return NID_brainpoolP320r1;
        case ECC_BRAINPOOLP384R1:
            return NID_brainpoolP384r1;
        case ECC_BRAINPOOLP512R1:
            return NID_brainpoolP512r1;
        default:
            WOLFSSL_MSG("NID not found");
            return -1;
    }
}
#endif /* HAVE_ECC */
#endif /* OPENSSL_EXTRA || OPENSSL_EXTRA_X509_SMALL */

#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
/* Convert shortname to NID.
 *
 * For OpenSSL compatability.
 *
 * @param [in] sn  Short name of OID.
 * @return  NID corresponding to shortname on success.
 * @return  NID_undef when not recognized.
 */
int wc_OBJ_sn2nid(const char *sn)
{
    const struct {
        const char *sn;
        int  nid;
    } sn2nid[] = {
        {WOLFSSL_COMMON_NAME, NID_commonName},
        {WOLFSSL_COUNTRY_NAME, NID_countryName},
        {WOLFSSL_LOCALITY_NAME, NID_localityName},
        {WOLFSSL_STATE_NAME, NID_stateOrProvinceName},
        {WOLFSSL_ORG_NAME, NID_organizationName},
        {WOLFSSL_ORGUNIT_NAME, NID_organizationalUnitName},
        {WOLFSSL_EMAIL_ADDR, NID_emailAddress},
        {"SHA1", NID_sha1},
        {NULL, -1}};
    int i;
    #ifdef HAVE_ECC
    char curveName[16]; /* Same as MAX_CURVE_NAME_SZ but can't include that
                         * symbol in this file */
    int eccEnum;
    #endif
    WOLFSSL_ENTER("OBJ_sn2nid");
    for(i=0; sn2nid[i].sn != NULL; i++) {
        if(XSTRNCMP(sn, sn2nid[i].sn, XSTRLEN(sn2nid[i].sn)) == 0) {
            return sn2nid[i].nid;
        }
    }
    #ifdef HAVE_ECC
    /* Nginx uses this OpenSSL string. */
    if (XSTRNCMP(sn, "prime256v1", 10) == 0)
        sn = "SECP256R1";
    /* OpenSSL allows lowercase curve names */
    for (i = 0; i < (int)(sizeof(curveName) - 1) && *sn; i++) {
        curveName[i] = (char)XTOUPPER(*sn++);
    }
    curveName[i] = '\0';
    /* find based on name and return NID */
    for (i = 0;
#ifndef WOLFSSL_ECC_CURVE_STATIC
         ecc_sets[i].size != 0 && ecc_sets[i].name != NULL;
#else
         ecc_sets[i].size != 0;
#endif
         i++) {
        if (XSTRNCMP(curveName, ecc_sets[i].name, ECC_MAXNAME) == 0) {
            eccEnum = ecc_sets[i].id;
            /* Convert enum value in ecc_curve_id to OpenSSL NID */
            return EccEnumToNID(eccEnum);
        }
    }
    #endif

    return NID_undef;
}
#endif

/* Calculate hash of the id using the SHA-1 or SHA-256.
 *
 * @param [in]  data  Data to hash.
 * @param [in]  len   Length of data to hash.
 * @param [out] hash  Buffer to hold hash.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
int CalcHashId(const byte* data, word32 len, byte* hash)
{
    int ret;

#if defined(NO_SHA) && !defined(NO_SHA256)
    ret = wc_Sha256Hash(data, len, hash);
#elif !defined(NO_SHA)
    ret = wc_ShaHash(data, len, hash);
#else
    ret = NOT_COMPILED_IN;
    (void)data;
    (void)len;
    (void)hash;
#endif

    return ret;
}

#ifndef NO_CERTS
/* Get the hash of the id using the SHA-1 or SHA-256.
 *
 * If the id is not the length of the hash, then hash it.
 *
 * @param [in]  id    Id to get hash for.
 * @param [in]  len   Length of id in bytes.
 * @param [out] hash  Buffer to hold hash.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int GetHashId(const byte* id, int length, byte* hash)
{
    int ret;

    if (length == KEYID_SIZE) {
        XMEMCPY(hash, id, length);
        ret = 0;
    }
    else {
        ret = CalcHashId(id, length, hash);
    }

    return ret;
}
#endif /* !NO_CERTS */

#ifdef WOLFSSL_ASN_TEMPLATE
/* Id for street address - not used. */
#define ASN_STREET    9
/* Id for email address. */
#define ASN_EMAIL     0x100
/* Id for user id. */
#define ASN_UID       0x101
/* Id for domain component. */
#define ASN_DC        0x102
/* Id for juristiction country. */
#define ASN_JURIS_C   0x203
/* Id for juristiction state. */
#define ASN_JURIS_ST  0x203

/* Set the string for a name component into the subject name. */
#define SetCertNameSubject(cert, id, val) \
    *((char**)(((byte *)cert) + certNameSubject[(id) - 3].data)) = val
/* Set the string length for a name component into the subject name. */
#define SetCertNameSubjectLen(cert, id, val) \
    *((int*)(((byte *)cert) + certNameSubject[(id) - 3].len)) = val
/* Set the encoding for a name component into the subject name. */
#define SetCertNameSubjectEnc(cert, id, val) \
    *((byte*)(((byte *)cert) + certNameSubject[(id) - 3].enc)) = val

/* Get the string of a name component from the subject name. */
#define GetCertNameSubjectStr(id) \
    (certNameSubject[(id) - 3].str)
/* Get the string length of a name component from the subject name. */
#define GetCertNameSubjectStrLen(id) \
    (certNameSubject[(id) - 3].strLen)
/* Get the NID of a name component from the subject name. */
#define GetCertNameSubjectNID(id) \
    (certNameSubject[(id) - 3].nid)

/* Mapping of certificate name component to useful information. */
typedef struct CertNameData {
    /* Type string of name component. */
    const char* str;
    /* Length of type string of name component. */
    byte        strLen;
#ifdef WOLFSSL_CERT_GEN
    /* Offset of data in subject name component. */
    size_t      data;
    /* Offset of length in subject name component. */
    size_t      len;
    /* Offset of encoding in subject name component. */
    size_t      enc;
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
    /* NID of type for subject name component. */
    int         nid;
#endif
} CertNameData;

/* List of data for common name components. */
static const CertNameData certNameSubject[] = {
    /* Common Name */
    {
        "/CN=", 4,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectCN),
        OFFSETOF(DecodedCert, subjectCNLen),
        OFFSETOF(DecodedCert, subjectCNEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_commonName
#endif
    },
    /* Surname */
    {
        "/SN=", 4,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectSN),
        OFFSETOF(DecodedCert, subjectSNLen),
        OFFSETOF(DecodedCert, subjectSNEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_surname
#endif
    },
    /* Serial Number */
    {
        "/serialNumber=", 14,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectSND),
        OFFSETOF(DecodedCert, subjectSNDLen),
        OFFSETOF(DecodedCert, subjectSNDEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_serialNumber
#endif
    },
    /* Country Name */
    {
        "/C=", 3,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectC),
        OFFSETOF(DecodedCert, subjectCLen),
        OFFSETOF(DecodedCert, subjectCEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_countryName
#endif
    },
    /* Locality Name */
    {
        "/L=", 3,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectL),
        OFFSETOF(DecodedCert, subjectLLen),
        OFFSETOF(DecodedCert, subjectLEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_localityName
#endif
    },
    /* State Name */
    {
        "/ST=", 4,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectST),
        OFFSETOF(DecodedCert, subjectSTLen),
        OFFSETOF(DecodedCert, subjectSTEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_stateOrProvinceName
#endif
    },
    /* Undefined - Street */
    {
        NULL, 0,
#ifdef WOLFSSL_CERT_GEN
        0,
        0,
        0,
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        0,
#endif
    },
    /* Organization Name */
    {
        "/O=", 3,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectO),
        OFFSETOF(DecodedCert, subjectOLen),
        OFFSETOF(DecodedCert, subjectOEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_organizationName
#endif
    },
    /* Organization Unit Name */
    {
        "/OU=", 4,
#ifdef WOLFSSL_CERT_GEN
        OFFSETOF(DecodedCert, subjectOU),
        OFFSETOF(DecodedCert, subjectOULen),
        OFFSETOF(DecodedCert, subjectOUEnc),
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_organizationalUnitName
#endif
    },
    /* Title */
    {
        NULL, 0,
#ifdef WOLFSSL_CERT_GEN
        0,
        0,
        0,
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        0,
#endif
    },
    /* Undefined */
    {
        NULL, 0,
#ifdef WOLFSSL_CERT_GEN
        0,
        0,
        0,
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        0,
#endif
    },
    /* Undefined */
    {
        NULL, 0,
#ifdef WOLFSSL_CERT_GEN
        0,
        0,
        0,
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        0,
#endif
    },
    /* Business Category */
    {
        "/businessCategory=", 18,
#ifdef WOLFSSL_CERT_GEN
#ifdef WOLFSSL_CERT_EXT
        OFFSETOF(DecodedCert, subjectBC),
        OFFSETOF(DecodedCert, subjectBCLen),
        OFFSETOF(DecodedCert, subjectBCEnc),
#else
        0,
        0,
        0,
#endif
#endif
#ifdef WOLFSSL_X509_NAME_AVAILABLE
        NID_businessCategory
#endif
    },
};

/* Full email OID. */
static const byte emailOid[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x01
};
/* Full user id OID. */
static const byte uidOid[] = {
    0x09, 0x92, 0x26, 0x89, 0x93, 0xF2, 0x2C, 0x64, 0x01, 0x01
};
/* Full domain component OID. */
static const byte dcOid[] = {
    0x09, 0x92, 0x26, 0x89, 0x93, 0xF2, 0x2C, 0x64, 0x01, 0x19
};


/* ASN.1 template for an RDN.
 * X.509: RFC 5280, 4.1.2.4 - RelativeDistinguishedName
 */
static const ASNItem rdnASN[] = {
/*  0 */        { 1, ASN_SET, 1, 1, 0 },
                    /* AttributeTypeAndValue */
/*  1 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                        /* AttributeType */
/*  2 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
                        /* AttributeValue: Choice of tags - rdnChoice. */
/*  3 */                { 3, 0, 0, 0, 0 },
};

/* Number of items in ASN.1 template for an RDN. */
#define rdnASN_Length (sizeof(rdnASN) / sizeof(ASNItem))

/* Supported types of encodings (tags) for RDN strings.
 * X.509: RFC 5280, 4.1.2.4 - DirectoryString
 * (IA5 String not listed in RFC but required for alternative types)
 */
static const byte rdnChoice[] = {
    ASN_PRINTABLE_STRING, ASN_IA5_STRING, ASN_UTF8STRING, ASN_T61STRING,
    ASN_UNIVERSALSTRING, ASN_BMPSTRING, 0
};
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
#if defined(WOLFSSL_CERT_GEN) || \
    (!defined(NO_CERTS) && !defined(IGNORE_NAME_CONSTRAINTS))
/* Allocate a DNS entry and set the fields.
 *
 * @param [in]      cert     Certificate object.
 * @param [in]      str      DNS name string.
 * @param [in]      strLen   Length of DNS name string.
 * @param [in]      type     Type of DNS name string.
 * @param [in, out] entries  Linked list of DNS name entries.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int SetDNSEntry(DecodedCert* cert, const char* str, int strLen,
                       int type, DNS_entry** entries)
{
    DNS_entry* dnsEntry;
    int ret = 0;

    /* Only used for heap. */
    (void)cert;

    /* TODO: consider one malloc. */
    /* Allocate DNS Entry object. */
    dnsEntry = (DNS_entry*)XMALLOC(sizeof(DNS_entry), cert->heap,
                                                          DYNAMIC_TYPE_ALTNAME);
    if (dnsEntry == NULL) {
        ret = MEMORY_E;
    }
    if (ret == 0) {
        /* Allocate DNS Entry name - length of string plus 1 for NUL. */
        dnsEntry->name = (char*)XMALLOC(strLen + 1, cert->heap,
                                                          DYNAMIC_TYPE_ALTNAME);
        if (dnsEntry->name == NULL) {
            XFREE(dnsEntry, cert->heap, DYNAMIC_TYPE_ALTNAME);
            ret = MEMORY_E;
        }
    }
    if (ret == 0) {
        /* Set tag type, name length, name and NUL terminate name. */
        dnsEntry->type = type;
        dnsEntry->len = strLen;
        XMEMCPY(dnsEntry->name, str, strLen);
        dnsEntry->name[strLen] = '\0';

        /* Prepend entry to linked list. */
        dnsEntry->next = *entries;
        *entries = dnsEntry;
    }

    return ret;
}
#endif

/* Set the details of a subject name component into a certificate.
 *
 * @param [in, out] cert    Certificate object.
 * @param [in]      id      Id of component.
 * @param [in]      str     String for component.
 * @param [in]      strLen  Length of string.
 * @param [in]      tag     BER tag representing encoding of string.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int SetSubject(DecodedCert* cert, int id, byte* str, word32 strLen,
                      byte tag)
{
    int ret = 0;

    /* Put string and encoding into certificate. */
    if (id == ASN_COMMON_NAME) {
        cert->subjectCN = (char *)str;
        cert->subjectCNLen = strLen;
        cert->subjectCNEnc = tag;
    }
#if defined(WOLFSSL_CERT_GEN) || defined(WOLFSSL_CERT_EXT)
    else if (id > ASN_COMMON_NAME && id <= ASN_BUS_CAT) {
        /* Use table and offsets to put data into appropriate fields. */
        SetCertNameSubject(cert, id, (char*)str);
        SetCertNameSubjectLen(cert, id, strLen);
        SetCertNameSubjectEnc(cert, id, tag);
    }
    else if (id == ASN_EMAIL) {
        cert->subjectEmail = (char*)str;
        cert->subjectEmailLen = strLen;
    #if !defined(IGNORE_NAME_CONSTRAINTS)
        ret = SetDNSEntry(cert, cert->subjectEmail, strLen, 0,
                          &cert->altEmailNames);
    #endif
    }
#ifdef WOLFSSL_CERT_EXT
    /* TODO: consider mapping id to an index and using SetCertNameSubect*(). */
    else if (id == ASN_JURIS_C) {
        cert->subjectJC = (char*)str;
        cert->subjectJCLen = strLen;
        cert->subjectJCEnc = tag;
    }
    else if (id == ASN_JURIS_ST) {
        cert->subjectJS = (char*)str;
        cert->subjectJSLen = strLen;
        cert->subjectJSEnc = tag;
    }
#endif
#endif

    return ret;
}

/* Get a RelativeDistignuishedName from the encoding and put in certificate.
 *
 * @param [in, out] cert       Certificate object.
 * @param [in, out] full       Full name string. ([/<type>=<value>]*)
 * @param [in, out] idx        Index int full name to place next component.
 * @param [in, out] nid        NID of component type.
 * @param [in]      isSubject  Whether this data is for a subject name.
 * @param [in]      dataASN    Decoded data of RDN.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_PARSE_E when type not supported.
 */
static int GetRDN(DecodedCert* cert, char* full, word32* idx, int* nid,
                  int isSubject, ASNGetData* dataASN)
{
    int         ret = 0;
    const char* typeStr = NULL;
    byte        typeStrLen = 0;
    byte*       oid;
    word32      oidSz;
    int         id = 0;

    (void)nid;

    /* Get name type OID from data items. */
    GetASN_OIDData(&dataASN[2], &oid, &oidSz);

    /* v1 name types */
    if ((oidSz == 3) && (oid[0] == 0x55) && (oid[1] == 0x04)) {
        id = oid[2];
        /* Check range of supported ids in table. */
        if (((id >= ASN_COMMON_NAME) && (id <= ASN_ORGUNIT_NAME) &&
                (id != ASN_STREET)) || (id == ASN_BUS_CAT)) {
            /* Get the type string, length and NID from table. */
            typeStr = GetCertNameSubjectStr(id);
            typeStrLen = GetCertNameSubjectStrLen(id);
        #ifdef WOLFSSL_X509_NAME_AVAILABLE
            *nid = GetCertNameSubjectNID(id);
        #endif
        }
    }
    else if (oidSz == sizeof(emailOid) && XMEMCMP(oid, emailOid, oidSz) == 0) {
        /* Set the email id, type string, length and NID. */
        id = ASN_EMAIL;
        typeStr =  WOLFSSL_EMAIL_ADDR;
        typeStrLen = sizeof(WOLFSSL_EMAIL_ADDR) - 1;
    #ifdef WOLFSSL_X509_NAME_AVAILABLE
        *nid = NID_emailAddress;
    #endif
    }
    else if (oidSz == sizeof(uidOid) && XMEMCMP(oid, uidOid, oidSz) == 0) {
        /* Set the user id, type string, length and NID. */
        id = ASN_UID;
        typeStr = WOLFSSL_USER_ID;
        typeStrLen = sizeof(WOLFSSL_USER_ID) - 1;
    #ifdef WOLFSSL_X509_NAME_AVAILABLE
        *nid = NID_userId;
    #endif
    }
    else if (oidSz == sizeof(dcOid) && XMEMCMP(oid, dcOid, oidSz) == 0) {
        /* Set the domain component, type string, length and NID. */
        id = ASN_DC;
        typeStr = WOLFSSL_DOMAIN_COMPONENT;
        typeStrLen = sizeof(WOLFSSL_DOMAIN_COMPONENT) - 1;
    #ifdef WOLFSSL_X509_NAME_AVAILABLE
        *nid = NID_domainComponent;
    #endif
    }
    /* Other OIDs that start with the same values. */
    else if (oidSz == sizeof(dcOid) && XMEMCMP(oid, dcOid, oidSz-1) == 0) {
        WOLFSSL_MSG("Unknown pilot attribute type");
        ret = ASN_PARSE_E;
    }
    else if (oidSz == ASN_JOI_PREFIX_SZ + 1 &&
                         XMEMCMP(oid, ASN_JOI_PREFIX, ASN_JOI_PREFIX_SZ) == 0) {
        /* Set the jurisdiction id. */
        id = 0x200 + oid[ASN_JOI_PREFIX_SZ];

        /* Set the jurisdiction type string, length and NID if known. */
        if (oid[ASN_JOI_PREFIX_SZ] == ASN_JOI_C) {
            typeStr = WOLFSSL_JOI_C;
            typeStrLen = sizeof(WOLFSSL_JOI_C) - 1;
        #ifdef WOLFSSL_X509_NAME_AVAILABLE
            *nid = NID_jurisdictionCountryName;
        #endif /* WOLFSSL_X509_NAME_AVAILABLE */
        }
        else if (oid[ASN_JOI_PREFIX_SZ] == ASN_JOI_ST) {
            typeStr = WOLFSSL_JOI_ST;
            typeStrLen = sizeof(WOLFSSL_JOI_ST) - 1;
        #ifdef WOLFSSL_X509_NAME_AVAILABLE
            *nid = NID_jurisdictionStateOrProvinceName;
        #endif /* WOLFSSL_X509_NAME_AVAILABLE */
        }
        else {
            WOLFSSL_MSG("Unknown Jurisdiction, skipping");
        }
    }

    if ((ret == 0) && (typeStr != NULL)) {
        /* OID type to store for subject name and add to full string. */
        byte*  str;
        word32 strLen;
        byte   tag = dataASN[3].tag;

        /* Get the string reference and length. */
        GetASN_GetRef(&dataASN[3], &str, &strLen);

        if (isSubject) {
            /* Store subject field components. */
            ret = SetSubject(cert, id, str, strLen, tag);
        }
        if (ret == 0) {
            /* Check there is space for this in the full name string and
             * terminating NUL characher. */
            if ((typeStrLen + strLen) < (word32)(ASN_NAME_MAX - *idx))
            {
                /* Add RDN to full string. */
                XMEMCPY(&full[*idx], typeStr, typeStrLen);
                *idx += typeStrLen;
                XMEMCPY(&full[*idx], str, strLen);
                *idx += strLen;
            }
            else {
                WOLFSSL_MSG("ASN Name too big, skipping");
            }
        }
    }

    return ret;
}
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Get a certificate name into the certificate object.
 *
 * @param [in, out] cert      Decoded certificate object.
 * @param [out]     full      Buffer to hold full name as a string.
 * @param [out]     hash      Buffer to hold hash of name.
 * @param [in]      nameType  IUSSUER or SUBJECT.
 * @param [in]      input     Buffer holding certificate name.
 * @param [in, out] inOutIdx  On in, start of certifica namtey.
 *                            On out, start of ASN.1 item after cert name.
 * @param [in]      maxIdx    Index of next item after certificate name.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int GetCertName(DecodedCert* cert, char* full, byte* hash, int nameType,
                       const byte* input, word32* inOutIdx, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    length;  /* length of all distinguished names */
    int    dummy;
    int    ret;
    word32 idx;
    word32 srcIdx = *inOutIdx;
#if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
    !defined(WOLFCRYPT_ONLY)
    WOLFSSL_X509_NAME* dName = NULL;
#endif

    WOLFSSL_MSG("Getting Cert Name");

    /* For OCSP, RFC2560 section 4.1.1 states the issuer hash should be
     * calculated over the entire DER encoding of the Name field, including
     * the tag and length. */
    if (CalcHashId(input + *inOutIdx, maxIdx - *inOutIdx, hash) != 0)
        return ASN_PARSE_E;

#if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
    !defined(WOLFCRYPT_ONLY)
    dName = wolfSSL_X509_NAME_new();
    if (dName == NULL) {
        return MEMORY_E;
    }
#endif /* OPENSSL_EXTRA */

    if (GetSequence(input, &srcIdx, &length, maxIdx) < 0) {
#if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
        wolfSSL_X509_NAME_free(dName);
#endif /* OPENSSL_EXTRA */
        return ASN_PARSE_E;
    }

#if defined(HAVE_PKCS7) || defined(WOLFSSL_CERT_EXT)
    /* store pointer to raw issuer */
    if (nameType == ISSUER) {
        cert->issuerRaw = &input[srcIdx];
        cert->issuerRawLen = length;
    }
#endif
#ifndef IGNORE_NAME_CONSTRAINTS
    if (nameType == SUBJECT) {
        cert->subjectRaw = &input[srcIdx];
        cert->subjectRawLen = length;
    }
#endif

    length += srcIdx;
    idx = 0;

    while (srcIdx < (word32)length) {
        byte        b       = 0;
        byte        joint[3];
        byte        tooBig  = FALSE;
        int         oidSz;
        const char* copy    = NULL;
        int         copyLen = 0;
        int         strLen  = 0;
        byte        id      = 0;
    #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) \
                && !defined(WOLFCRYPT_ONLY)
         int        nid = NID_undef;
         int        enc;
    #endif /* OPENSSL_EXTRA */

        if (GetSet(input, &srcIdx, &dummy, maxIdx) < 0) {
            WOLFSSL_MSG("Cert name lacks set header, trying sequence");
        }

        if (GetSequence(input, &srcIdx, &dummy, maxIdx) <= 0) {
        #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
            wolfSSL_X509_NAME_free(dName);
        #endif /* OPENSSL_EXTRA */
            return ASN_PARSE_E;
        }

        ret = GetASNObjectId(input, &srcIdx, &oidSz, maxIdx);
        if (ret != 0) {
        #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
            wolfSSL_X509_NAME_free(dName);
        #endif /* OPENSSL_EXTRA */
            return ret;
        }

        /* make sure there is room for joint */
        if ((srcIdx + sizeof(joint)) > (word32)maxIdx) {
        #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
            wolfSSL_X509_NAME_free(dName);
        #endif /* OPENSSL_EXTRA */
            return ASN_PARSE_E;
        }

        XMEMCPY(joint, &input[srcIdx], sizeof(joint));

        /* v1 name types */
        if (joint[0] == 0x55 && joint[1] == 0x04) {
            srcIdx += 3;
            id = joint[2];
            if (GetHeader(input, &b, &srcIdx, &strLen, maxIdx, 1) < 0) {
            #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
                wolfSSL_X509_NAME_free(dName);
            #endif /* OPENSSL_EXTRA */
                return ASN_PARSE_E;
            }

            if (id == ASN_COMMON_NAME) {
                if (nameType == SUBJECT) {
                    cert->subjectCN = (char *)&input[srcIdx];
                    cert->subjectCNLen = strLen;
                    cert->subjectCNEnc = b;
                }

                copy = WOLFSSL_COMMON_NAME;
                copyLen = sizeof(WOLFSSL_COMMON_NAME) - 1;
            #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) \
                && !defined(WOLFCRYPT_ONLY)
                nid = NID_commonName;
            #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_SUR_NAME) {
                copy = WOLFSSL_SUR_NAME;
                copyLen = sizeof(WOLFSSL_SUR_NAME) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectSN = (char*)&input[srcIdx];
                        cert->subjectSNLen = strLen;
                        cert->subjectSNEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_surname;
                #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_COUNTRY_NAME) {
                copy = WOLFSSL_COUNTRY_NAME;
                copyLen = sizeof(WOLFSSL_COUNTRY_NAME) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectC = (char*)&input[srcIdx];
                        cert->subjectCLen = strLen;
                        cert->subjectCEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_countryName;
                #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_LOCALITY_NAME) {
                copy = WOLFSSL_LOCALITY_NAME;
                copyLen = sizeof(WOLFSSL_LOCALITY_NAME) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectL = (char*)&input[srcIdx];
                        cert->subjectLLen = strLen;
                        cert->subjectLEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_localityName;
                #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_STATE_NAME) {
                copy = WOLFSSL_STATE_NAME;
                copyLen = sizeof(WOLFSSL_STATE_NAME) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectST = (char*)&input[srcIdx];
                        cert->subjectSTLen = strLen;
                        cert->subjectSTEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_stateOrProvinceName;
                #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_ORG_NAME) {
                copy = WOLFSSL_ORG_NAME;
                copyLen = sizeof(WOLFSSL_ORG_NAME) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectO = (char*)&input[srcIdx];
                        cert->subjectOLen = strLen;
                        cert->subjectOEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_organizationName;
                #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_ORGUNIT_NAME) {
                copy = WOLFSSL_ORGUNIT_NAME;
                copyLen = sizeof(WOLFSSL_ORGUNIT_NAME) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectOU = (char*)&input[srcIdx];
                        cert->subjectOULen = strLen;
                        cert->subjectOUEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_organizationalUnitName;
                #endif /* OPENSSL_EXTRA */
            }
            else if (id == ASN_SERIAL_NUMBER) {
                copy = WOLFSSL_SERIAL_NUMBER;
                copyLen = sizeof(WOLFSSL_SERIAL_NUMBER) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectSND = (char*)&input[srcIdx];
                        cert->subjectSNDLen = strLen;
                        cert->subjectSNDEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_serialNumber;
                #endif /* OPENSSL_EXTRA */
            }
        #ifdef WOLFSSL_CERT_EXT
            else if (id == ASN_BUS_CAT) {
                copy = WOLFSSL_BUS_CAT;
                copyLen = sizeof(WOLFSSL_BUS_CAT) - 1;
            #ifdef WOLFSSL_CERT_GEN
                if (nameType == SUBJECT) {
                    cert->subjectBC = (char*)&input[srcIdx];
                    cert->subjectBCLen = strLen;
                    cert->subjectBCEnc = b;
                }
            #endif /* WOLFSSL_CERT_GEN */
            #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                nid = NID_businessCategory;
            #endif /* OPENSSL_EXTRA */
            }
        #endif /* WOLFSSL_CERT_EXT */
        }
    #ifdef WOLFSSL_CERT_EXT
        else if ((srcIdx + ASN_JOI_PREFIX_SZ + 2 <= (word32)maxIdx) &&
                 (0 == XMEMCMP(&input[srcIdx], ASN_JOI_PREFIX,
                               ASN_JOI_PREFIX_SZ)) &&
                 ((input[srcIdx+ASN_JOI_PREFIX_SZ] == ASN_JOI_C) ||
                  (input[srcIdx+ASN_JOI_PREFIX_SZ] == ASN_JOI_ST)))
        {
            srcIdx += ASN_JOI_PREFIX_SZ;
            id = input[srcIdx++];
            b = input[srcIdx++]; /* encoding */

            if (GetLength(input, &srcIdx, &strLen,
                          maxIdx) < 0) {
            #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
                wolfSSL_X509_NAME_free(dName);
            #endif /* OPENSSL_EXTRA */
                return ASN_PARSE_E;
            }

            /* Check for jurisdiction of incorporation country name */
            if (id == ASN_JOI_C) {
                copy = WOLFSSL_JOI_C;
                copyLen = sizeof(WOLFSSL_JOI_C) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectJC = (char*)&input[srcIdx];
                        cert->subjectJCLen = strLen;
                        cert->subjectJCEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_jurisdictionCountryName;
                #endif /* OPENSSL_EXTRA */
            }

            /* Check for jurisdiction of incorporation state name */
            else if (id == ASN_JOI_ST) {
                copy = WOLFSSL_JOI_ST;
                copyLen = sizeof(WOLFSSL_JOI_ST) - 1;
                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectJS = (char*)&input[srcIdx];
                        cert->subjectJSLen = strLen;
                        cert->subjectJSEnc = b;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_jurisdictionStateOrProvinceName;
                #endif /* OPENSSL_EXTRA */
            }

            if ((strLen + copyLen) > (int)(ASN_NAME_MAX - idx)) {
                WOLFSSL_MSG("ASN Name too big, skipping");
                tooBig = TRUE;
            }
        }
    #endif /* WOLFSSL_CERT_EXT */
        else {
            /* skip */
            byte email = FALSE;
            byte pilot = FALSE;

            if (joint[0] == 0x2a && joint[1] == 0x86) {  /* email id hdr */
                id = ASN_EMAIL_NAME;
                email = TRUE;
            }

            if (joint[0] == 0x9  && joint[1] == 0x92) { /* uid id hdr */
                /* last value of OID is the type of pilot attribute */
                id    = input[srcIdx + oidSz - 1];
                pilot = TRUE;
            }

            srcIdx += oidSz + 1;

            if (GetLength(input, &srcIdx, &strLen, maxIdx) < 0) {
            #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
                wolfSSL_X509_NAME_free(dName);
            #endif /* OPENSSL_EXTRA */
                return ASN_PARSE_E;
            }

            if (strLen > (int)(ASN_NAME_MAX - idx)) {
                WOLFSSL_MSG("ASN name too big, skipping");
                tooBig = TRUE;
            }

            if (email) {
                copyLen = sizeof(WOLFSSL_EMAIL_ADDR) - 1;
                if ((copyLen + strLen) > (int)(ASN_NAME_MAX - idx)) {
                    WOLFSSL_MSG("ASN name too big, skipping");
                    tooBig = TRUE;
                }
                else {
                    copy = WOLFSSL_EMAIL_ADDR;
                }

                #ifdef WOLFSSL_CERT_GEN
                    if (nameType == SUBJECT) {
                        cert->subjectEmail = (char*)&input[srcIdx];
                        cert->subjectEmailLen = strLen;
                    }
                #endif /* WOLFSSL_CERT_GEN */
                #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                    nid = NID_emailAddress;
                #endif /* OPENSSL_EXTRA */
                #ifndef IGNORE_NAME_CONSTRAINTS
                    {
                        DNS_entry* emailName;

                        emailName = (DNS_entry*)XMALLOC(sizeof(DNS_entry),
                                              cert->heap, DYNAMIC_TYPE_ALTNAME);
                        if (emailName == NULL) {
                            WOLFSSL_MSG("\tOut of Memory");
                        #if (defined(OPENSSL_EXTRA) || \
                                defined(OPENSSL_EXTRA_X509_SMALL)) && \
                                !defined(WOLFCRYPT_ONLY)
                            wolfSSL_X509_NAME_free(dName);
                        #endif /* OPENSSL_EXTRA */
                            return MEMORY_E;
                        }
                        emailName->type = 0;
                        emailName->name = (char*)XMALLOC(strLen + 1,
                                              cert->heap, DYNAMIC_TYPE_ALTNAME);
                        if (emailName->name == NULL) {
                            WOLFSSL_MSG("\tOut of Memory");
                            XFREE(emailName, cert->heap, DYNAMIC_TYPE_ALTNAME);
                        #if (defined(OPENSSL_EXTRA) || \
                                defined(OPENSSL_EXTRA_X509_SMALL)) && \
                                !defined(WOLFCRYPT_ONLY)
                            wolfSSL_X509_NAME_free(dName);
                        #endif /* OPENSSL_EXTRA */
                            return MEMORY_E;
                        }
                        emailName->len = strLen;
                        XMEMCPY(emailName->name, &input[srcIdx], strLen);
                        emailName->name[strLen] = '\0';

                        emailName->next = cert->altEmailNames;
                        cert->altEmailNames = emailName;
                    }
                #endif /* IGNORE_NAME_CONSTRAINTS */
            }

            if (pilot) {
                switch (id) {
                    case ASN_USER_ID:
                        copy = WOLFSSL_USER_ID;
                        copyLen = sizeof(WOLFSSL_USER_ID) - 1;
                    #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                        nid = NID_userId;
                    #endif /* OPENSSL_EXTRA */
                        break;

                    case ASN_DOMAIN_COMPONENT:
                        copy = WOLFSSL_DOMAIN_COMPONENT;
                        copyLen = sizeof(WOLFSSL_DOMAIN_COMPONENT) - 1;
                    #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                        nid = NID_domainComponent;
                    #endif /* OPENSSL_EXTRA */
                        break;
                    case ASN_FAVOURITE_DRINK:
                        copy = WOLFSSL_FAVOURITE_DRINK;
                        copyLen = sizeof(WOLFSSL_FAVOURITE_DRINK) - 1;
                    #if (defined(OPENSSL_EXTRA) || \
                        defined(OPENSSL_EXTRA_X509_SMALL)) \
                        && !defined(WOLFCRYPT_ONLY)
                        nid = NID_favouriteDrink;
                    #endif /* OPENSSL_EXTRA */
                        break;

                    default:
                        WOLFSSL_MSG("Unknown pilot attribute type");
                    #if (defined(OPENSSL_EXTRA) || \
                                defined(OPENSSL_EXTRA_X509_SMALL)) && \
                                !defined(WOLFCRYPT_ONLY)
                        wolfSSL_X509_NAME_free(dName);
                    #endif /* OPENSSL_EXTRA */
                        return ASN_PARSE_E;
                }
            }
        }
        if ((copyLen + strLen) > (int)(ASN_NAME_MAX - idx))
        {
            WOLFSSL_MSG("ASN Name too big, skipping");
            tooBig = TRUE;
        }
        if ((copy != NULL) && !tooBig) {
            XMEMCPY(&full[idx], copy, copyLen);
            idx += copyLen;
            XMEMCPY(&full[idx], &input[srcIdx], strLen);
            idx += strLen;
        }
        #if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
        switch (b) {
            case CTC_UTF8:
                enc = MBSTRING_UTF8;
                break;
            case CTC_PRINTABLE:
                enc = V_ASN1_PRINTABLESTRING;
                break;
            default:
                WOLFSSL_MSG("Unknown encoding type, using UTF8 by default");
                enc = MBSTRING_UTF8;
        }

        if (nid != NID_undef) {
            if (wolfSSL_X509_NAME_add_entry_by_NID(dName, nid, enc,
                            &input[srcIdx], strLen, -1, -1) !=
                            WOLFSSL_SUCCESS) {
                wolfSSL_X509_NAME_free(dName);
                return ASN_PARSE_E;
            }
        }
        #endif /* OPENSSL_EXTRA */
        srcIdx += strLen;
    }
    full[idx++] = 0;

#if (defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)) && \
            !defined(WOLFCRYPT_ONLY)
    if (nameType == ISSUER) {
#if (defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX) || defined(HAVE_LIGHTY)) && \
    (defined(HAVE_PKCS7) || defined(WOLFSSL_CERT_EXT))
        dName->rawLen = min(cert->issuerRawLen, ASN_NAME_MAX);
        XMEMCPY(dName->raw, cert->issuerRaw, dName->rawLen);
#endif
        cert->issuerName = dName;
    }
    else {
#if defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX)
        dName->rawLen = min(cert->subjectRawLen, ASN_NAME_MAX);
        XMEMCPY(dName->raw, cert->subjectRaw, dName->rawLen);
#endif
        cert->subjectName = dName;
    }
#endif

    *inOutIdx = srcIdx;

    return 0;
#else
    DECL_ASNGETDATA(dataASN, rdnASN_Length);
    int    ret = 0;
    word32 idx = 0;
    int    len;
    word32 srcIdx = *inOutIdx;
#ifdef WOLFSSL_X509_NAME_AVAILABLE
    WOLFSSL_X509_NAME* dName = NULL;
#endif /* WOLFSSL_X509_NAME_AVAILABLE */

    WOLFSSL_MSG("Getting Cert Name");

    /* For OCSP, RFC2560 section 4.1.1 states the issuer hash should be
     * calculated over the entire DER encoding of the Name field, including
     * the tag and length. */
    if (CalcHashId(input + srcIdx, maxIdx - srcIdx, hash) != 0) {
        ret = ASN_PARSE_E;
    }

    ALLOC_ASNGETDATA(dataASN, rdnASN_Length, ret, cert->heap);

#ifdef WOLFSSL_X509_NAME_AVAILABLE
    if (ret == 0) {
        /* Create an X509_NAME to hold data for OpenSSL compatability APIs. */
        dName = wolfSSL_X509_NAME_new();
        if (dName == NULL) {
            ret = MEMORY_E;
        }
    }
#endif /* WOLFSSL_X509_NAME_AVAILABLE */

    if (ret == 0) {
        /* Expecing a SEQUENCE using up all data. */
        ret = GetASN_Sequence(input, &srcIdx, &len, maxIdx, 1);
    }
    if (ret == 0) {
    #if defined(HAVE_PKCS7) || defined(WOLFSSL_CERT_EXT)
        /* Store pointer and length to raw issuer. */
        if (nameType == ISSUER) {
            cert->issuerRaw = &input[srcIdx];
            cert->issuerRawLen = len;
        }
    #endif
    #ifndef IGNORE_NAME_CONSTRAINTS
        /* Store pointer and length to raw subject. */
        if (nameType == SUBJECT) {
            cert->subjectRaw = &input[srcIdx];
            cert->subjectRawLen = len;
        }
    #endif

        /* Process all RDNs in name. */
        while ((ret == 0) && (srcIdx < maxIdx)) {
            int nid = 0;

            /* Initialize for data and setup RDN choice. */
            XMEMSET(dataASN, 0, sizeof(*dataASN) * rdnASN_Length);
            GetASN_Choice(&dataASN[3], rdnChoice);
            /* Ignore type OID as too many to store in table. */
            GetASN_OID(&dataASN[2], oidIgnoreType);
            /* Parse RDN. */
            ret = GetASN_Items(rdnASN, dataASN, rdnASN_Length, 1, input,
                               &srcIdx, maxIdx);
            if (ret == 0) {
                /* Put RDN data into certificate. */
                ret = GetRDN(cert, full, &idx, &nid, nameType == SUBJECT,
                             dataASN);
            }
        #ifdef WOLFSSL_X509_NAME_AVAILABLE
            /* TODO: push this back up to ssl.c
             * (do parsing for WOLFSSL_X509_NAME on demand) */
            if (ret == 0) {
                int enc;
                byte*  str;
                word32 strLen;
                byte   tag = dataASN[3].tag;

                /* Get string reference. */
                GetASN_GetRef(&dataASN[3], &str, &strLen);

                /* Convert BER tag to a OpenSSL type. */
                switch (tag) {
                    case CTC_UTF8:
                        enc = MBSTRING_UTF8;
                        break;
                    case CTC_PRINTABLE:
                        enc = V_ASN1_PRINTABLESTRING;
                        break;
                    default:
                        WOLFSSL_MSG("Unknown encoding type, default UTF8");
                        enc = MBSTRING_UTF8;
                }
                if (nid != 0) {
                    /* Add an entry to the X509_NAME. */
                    if (wolfSSL_X509_NAME_add_entry_by_NID(dName, nid, enc, str,
                            strLen, -1, -1) != WOLFSSL_SUCCESS) {
                        ret = ASN_PARSE_E;
                    }
                }
            }
        #endif
        }
    }
    if (ret == 0) {
        /* Terminate string. */
        full[idx] = 0;
        /* Return index into encoding after name. */
        *inOutIdx = srcIdx;

#ifdef WOLFSSL_X509_NAME_AVAILABLE
        /* Store X509_NAME in certificate. */
        if (nameType == ISSUER) {
            cert->issuerName = dName;
        }
        else {
            cert->subjectName = dName;
        }
    }
    else {
        /* Dispose of unused X509_NAME. */
        wolfSSL_X509_NAME_free(dName);
#endif
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for certificate name. */
static const ASNItem certNameASN[] = {
/*  0 */    { 0, ASN_OBJECT_ID, 0, 0, 1 },
/*  1 */    { 0, ASN_SEQUENCE, 1, 0, 0 },
};

/* Number of items in ASN.1 template for certificate name. */
#define certNameASN_Length (sizeof(certNameASN) / sizeof(ASNItem))
#endif

/* Get a certificate name into the certificate object.
 *
 * Either the issuer or subject name.
 *
 * @param [in, out] cert      Decoded certificate object.
 * @param [in]      nameType  Type of name being decoded: ISSUER or SUBJECT.
 * @param [in]      maxIdx    Index of next item after certificate name.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
int GetName(DecodedCert* cert, int nameType, int maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    char*  full;
    byte*  hash;
    int    length;
    word32 localIdx;
    byte   tag;

    WOLFSSL_MSG("Getting Cert Name");

    if (nameType == ISSUER) {
        full = cert->issuer;
        hash = cert->issuerHash;
    }
    else {
        full = cert->subject;
        hash = cert->subjectHash;
    }

    if (cert->srcIdx >= (word32)maxIdx) {
        return BUFFER_E;
    }

    localIdx = cert->srcIdx;
    if (GetASNTag(cert->source, &localIdx, &tag, maxIdx) < 0) {
        return ASN_PARSE_E;
    }

    if (tag == ASN_OBJECT_ID) {
        WOLFSSL_MSG("Trying optional prefix...");

        if (SkipObjectId(cert->source, &cert->srcIdx, maxIdx) < 0)
            return ASN_PARSE_E;
        WOLFSSL_MSG("Got optional prefix");
    }

    localIdx = cert->srcIdx;
    if (GetASNTag(cert->source, &localIdx, &tag, maxIdx) < 0) {
        return ASN_PARSE_E;
    }
    localIdx = cert->srcIdx + 1;
    if (GetLength(cert->source, &localIdx, &length, maxIdx) < 0) {
        return ASN_PARSE_E;
    }
    length += localIdx - cert->srcIdx;

    return GetCertName(cert, full, hash, nameType, cert->source, &cert->srcIdx,
                       cert->srcIdx + length);
#else
    ASNGetData dataASN[certNameASN_Length];
    word32 idx = cert->srcIdx;
    int    ret;
    char*  full;
    byte*  hash;

    WOLFSSL_MSG("Getting Cert Name");

    /* Initialize for data and don't check optional prefix OID. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_OID(&dataASN[0], oidIgnoreType);
    ret = GetASN_Items(certNameASN, dataASN, certNameASN_Length, 0,
                       cert->source, &idx, maxIdx);
    if (ret == 0) {
        /* Store offset of SEQUENCE that is start of name. */
        cert->srcIdx = dataASN[1].offset;

        /* Get fields to fill in based on name type. */
        if (nameType == ISSUER) {
            full = cert->issuer;
            hash = cert->issuerHash;
        }
        else {
            full = cert->subject;
            hash = cert->subjectHash;
        }

        /* Parse certificate name. */
        ret = GetCertName(cert, full, hash, nameType, cert->source,
                          &cert->srcIdx, idx);
    }

    return ret;
#endif
}

#ifndef NO_ASN_TIME

/* two byte date/time, add to value */
static WC_INLINE int GetTime(int* value, const byte* date, int* idx)
{
    int i = *idx;

    if (date[i] < 0x30 || date[i] > 0x39 || date[i+1] < 0x30 ||
                                                             date[i+1] > 0x39) {
        return ASN_PARSE_E;
    }

    *value += btoi(date[i++]) * 10;
    *value += btoi(date[i++]);

    *idx = i;

    return 0;
}

#ifdef WOLFSSL_LINUXKM
static WC_INLINE int GetTime_Long(long* value, const byte* date, int* idx)
{
    int i = *idx;

    if (date[i] < 0x30 || date[i] > 0x39 || date[i+1] < 0x30 ||
                                                             date[i+1] > 0x39) {
        return ASN_PARSE_E;
    }

    *value += (long)btoi(date[i++]) * 10;
    *value += (long)btoi(date[i++]);

    *idx = i;

    return 0;
}
#endif

int ExtractDate(const unsigned char* date, unsigned char format,
                                                  struct tm* certTime, int* idx)
{
    XMEMSET(certTime, 0, sizeof(struct tm));

    if (format == ASN_UTC_TIME) {
        if (btoi(date[*idx]) >= 5)
            certTime->tm_year = 1900;
        else
            certTime->tm_year = 2000;
    }
    else  { /* format == GENERALIZED_TIME */
#ifdef WOLFSSL_LINUXKM
        if (GetTime_Long(&certTime->tm_year, date, idx) != 0) return 0;
#else
        if (GetTime(&certTime->tm_year, date, idx) != 0) return 0;
#endif
        certTime->tm_year *= 100;
    }

#ifdef AVR
    /* Extract the time from the struct tm and adjust tm_year, tm_mon */
    /* AVR libc stores these as uint8_t instead of int */
    /* AVR time_t also offsets from midnight 1 Jan 2000 */
    int tm_year = certTime->tm_year - 2000;
    int tm_mon  = certTime->tm_mon - 1;
    int tm_mday = certTime->tm_mday;
    int tm_hour = certTime->tm_hour;
    int tm_min  = certTime->tm_min;
    int tm_sec  = certTime->tm_sec;

#ifdef WOLFSSL_LINUXKM
    if (GetTime_Long(&tm_year, date, idx) != 0) return 0;
#else
    if (GetTime(&tm_year, date, idx) != 0) return 0;
#endif
    if (GetTime(&tm_mon , date, idx) != 0) return 0;
    if (GetTime(&tm_mday, date, idx) != 0) return 0;
    if (GetTime(&tm_hour, date, idx) != 0) return 0;
    if (GetTime(&tm_min , date, idx) != 0) return 0;
    if (GetTime(&tm_sec , date, idx) != 0) return 0;

    /* Re-populate certTime with computed values */
    certTime->tm_year = tm_year;
    certTime->tm_mon  = tm_mon;
    certTime->tm_mday = tm_mday;
    certTime->tm_hour = tm_hour;
    certTime->tm_min  = tm_min;
    certTime->tm_sec  = tm_sec;
#else
    /* adjust tm_year, tm_mon */
#ifdef WOLFSSL_LINUXKM
    if (GetTime_Long(&certTime->tm_year, date, idx) != 0) return 0;
#else
    if (GetTime(&certTime->tm_year, date, idx) != 0) return 0;
#endif
    certTime->tm_year -= 1900;
    if (GetTime(&certTime->tm_mon , date, idx) != 0) return 0;
    certTime->tm_mon  -= 1;
    if (GetTime(&certTime->tm_mday, date, idx) != 0) return 0;
    if (GetTime(&certTime->tm_hour, date, idx) != 0) return 0;
    if (GetTime(&certTime->tm_min , date, idx) != 0) return 0;
    if (GetTime(&certTime->tm_sec , date, idx) != 0) return 0;
#endif

    return 1;
}


#if defined(OPENSSL_ALL) || defined(WOLFSSL_MYSQL_COMPATIBLE) || \
    defined(OPENSSL_EXTRA) || defined(WOLFSSL_NGINX) || defined(WOLFSSL_HAPROXY)
int GetTimeString(byte* date, int format, char* buf, int len)
{
    struct tm t;
    int idx = 0;

    if (!ExtractDate(date, (unsigned char)format, &t, &idx)) {
        return 0;
    }

    if (date[idx] != 'Z') {
        WOLFSSL_MSG("UTCtime, not Zulu") ;
        return 0;
    }

    /* place month in buffer */
    buf[0] = '\0';
    switch(t.tm_mon) {
        case 0:  XSTRNCAT(buf, "Jan ", 5); break;
        case 1:  XSTRNCAT(buf, "Feb ", 5); break;
        case 2:  XSTRNCAT(buf, "Mar ", 5); break;
        case 3:  XSTRNCAT(buf, "Apr ", 5); break;
        case 4:  XSTRNCAT(buf, "May ", 5); break;
        case 5:  XSTRNCAT(buf, "Jun ", 5); break;
        case 6:  XSTRNCAT(buf, "Jul ", 5); break;
        case 7:  XSTRNCAT(buf, "Aug ", 5); break;
        case 8:  XSTRNCAT(buf, "Sep ", 5); break;
        case 9:  XSTRNCAT(buf, "Oct ", 5); break;
        case 10: XSTRNCAT(buf, "Nov ", 5); break;
        case 11: XSTRNCAT(buf, "Dec ", 5); break;
        default:
            return 0;

    }
    idx = 4; /* use idx now for char buffer */

    XSNPRINTF(buf + idx, len - idx, "%2d %02d:%02d:%02d %d GMT",
              t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)t.tm_year + 1900);

    return 1;
}
#endif /* OPENSSL_ALL || WOLFSSL_MYSQL_COMPATIBLE || WOLFSSL_NGINX || WOLFSSL_HAPROXY */


#if !defined(NO_ASN_TIME) && defined(HAVE_PKCS7)

/* Set current time string, either UTC or GeneralizedTime.
 * (void*) tm should be a pointer to time_t, output is placed in buf.
 *
 * Return time string length placed in buf on success, negative on error */
int GetAsnTimeString(void* currTime, byte* buf, word32 len)
{
    struct tm* ts      = NULL;
    struct tm* tmpTime = NULL;
    byte* data_ptr  = buf;
    word32 data_len = 0;
    int year, mon, day, hour, mini, sec;
#if defined(NEED_TMP_TIME)
    struct tm tmpTimeStorage;
    tmpTime = &tmpTimeStorage;
#else
    (void)tmpTime;
#endif

    WOLFSSL_ENTER("SetAsnTimeString");

    if (buf == NULL || len == 0)
        return BAD_FUNC_ARG;

    ts = (struct tm *)XGMTIME((time_t*)currTime, tmpTime);
    if (ts == NULL){
        WOLFSSL_MSG("failed to get time data.");
        return ASN_TIME_E;
    }

    /* Note ASN_UTC_TIME_SIZE and ASN_GENERALIZED_TIME_SIZE include space for
     * the null terminator. ASN encoded values leave off the terminator. */

    if (ts->tm_year >= 50 && ts->tm_year < 150) {
        /* UTC Time */
        char utc_str[ASN_UTC_TIME_SIZE];
        data_len = ASN_UTC_TIME_SIZE - 1 + 2;

        if (len < data_len)
            return BUFFER_E;

        if (ts->tm_year >= 50 && ts->tm_year < 100) {
            year = ts->tm_year;
        } else if (ts->tm_year >= 100 && ts->tm_year < 150) {
            year = ts->tm_year - 100;
        }
        else {
            WOLFSSL_MSG("unsupported year range");
            return BAD_FUNC_ARG;
        }
        mon  = ts->tm_mon + 1;
        day  = ts->tm_mday;
        hour = ts->tm_hour;
        mini = ts->tm_min;
        sec  = ts->tm_sec;
        XSNPRINTF((char *)utc_str, ASN_UTC_TIME_SIZE,
                  "%02d%02d%02d%02d%02d%02dZ", year, mon, day, hour, mini, sec);
        *data_ptr = (byte) ASN_UTC_TIME; data_ptr++;
        /* -1 below excludes null terminator */
        *data_ptr = (byte) ASN_UTC_TIME_SIZE - 1; data_ptr++;
        XMEMCPY(data_ptr,(byte *)utc_str, ASN_UTC_TIME_SIZE - 1);

    } else {
        /* GeneralizedTime */
        char gt_str[ASN_GENERALIZED_TIME_SIZE];
        data_len = ASN_GENERALIZED_TIME_SIZE - 1 + 2;

        if (len < data_len)
            return BUFFER_E;

        year = ts->tm_year + 1900;
        mon  = ts->tm_mon + 1;
        day  = ts->tm_mday;
        hour = ts->tm_hour;
        mini = ts->tm_min;
        sec  = ts->tm_sec;
        XSNPRINTF((char *)gt_str, ASN_GENERALIZED_TIME_SIZE,
                  "%4d%02d%02d%02d%02d%02dZ", year, mon, day, hour, mini, sec);
        *data_ptr = (byte) ASN_GENERALIZED_TIME; data_ptr++;
        /* -1 below excludes null terminator */
        *data_ptr = (byte) ASN_GENERALIZED_TIME_SIZE - 1; data_ptr++;
        XMEMCPY(data_ptr,(byte *)gt_str, ASN_GENERALIZED_TIME_SIZE - 1);
    }

    return data_len;
}

#endif /* !NO_ASN_TIME && HAVE_PKCS7 */


#if defined(USE_WOLF_VALIDDATE)

/* to the second */
int DateGreaterThan(const struct tm* a, const struct tm* b)
{
    if (a->tm_year > b->tm_year)
        return 1;

    if (a->tm_year == b->tm_year && a->tm_mon > b->tm_mon)
        return 1;

    if (a->tm_year == b->tm_year && a->tm_mon == b->tm_mon &&
           a->tm_mday > b->tm_mday)
        return 1;

    if (a->tm_year == b->tm_year && a->tm_mon == b->tm_mon &&
        a->tm_mday == b->tm_mday && a->tm_hour > b->tm_hour)
        return 1;

    if (a->tm_year == b->tm_year && a->tm_mon == b->tm_mon &&
        a->tm_mday == b->tm_mday && a->tm_hour == b->tm_hour &&
        a->tm_min > b->tm_min)
        return 1;

    if (a->tm_year == b->tm_year && a->tm_mon == b->tm_mon &&
        a->tm_mday == b->tm_mday && a->tm_hour == b->tm_hour &&
        a->tm_min  == b->tm_min  && a->tm_sec > b->tm_sec)
        return 1;

    return 0; /* false */
}


static WC_INLINE int DateLessThan(const struct tm* a, const struct tm* b)
{
    return DateGreaterThan(b,a);
}

/* like atoi but only use first byte */
/* Make sure before and after dates are valid */
int wc_ValidateDate(const byte* date, byte format, int dateType)
{
    time_t ltime;
    struct tm  certTime;
    struct tm* localTime;
    struct tm* tmpTime;
    int    i = 0;
    int    timeDiff = 0 ;
    int    diffHH = 0 ; int diffMM = 0 ;
    int    diffSign = 0 ;

#if defined(NEED_TMP_TIME)
    struct tm tmpTimeStorage;
    tmpTime = &tmpTimeStorage;
#else
    tmpTime = NULL;
#endif
    (void)tmpTime;

    ltime = XTIME(0);

#ifdef WOLFSSL_BEFORE_DATE_CLOCK_SKEW
    if (dateType == BEFORE) {
        WOLFSSL_MSG("Skewing local time for before date check");
        ltime += WOLFSSL_BEFORE_DATE_CLOCK_SKEW;
    }
#endif

#ifdef WOLFSSL_AFTER_DATE_CLOCK_SKEW
    if (dateType == AFTER) {
        WOLFSSL_MSG("Skewing local time for after date check");
        ltime -= WOLFSSL_AFTER_DATE_CLOCK_SKEW;
    }
#endif

    if (!ExtractDate(date, format, &certTime, &i)) {
        WOLFSSL_MSG("Error extracting the date");
        return 0;
    }

    if ((date[i] == '+') || (date[i] == '-')) {
        WOLFSSL_MSG("Using time differential, not Zulu") ;
        diffSign = date[i++] == '+' ? 1 : -1 ;
        if (GetTime(&diffHH, date, &i) != 0)
            return 0;
        if (GetTime(&diffMM, date, &i) != 0)
            return 0;
        timeDiff = diffSign * (diffHH*60 + diffMM) * 60 ;
    } else if (date[i] != 'Z') {
        WOLFSSL_MSG("UTCtime, neither Zulu or time differential") ;
        return 0;
    }

    ltime -= (time_t)timeDiff ;
    localTime = XGMTIME(&ltime, tmpTime);

    if (localTime == NULL) {
        WOLFSSL_MSG("XGMTIME failed");
        return 0;
    }

    if (dateType == BEFORE) {
        if (DateLessThan(localTime, &certTime)) {
            WOLFSSL_MSG("Date BEFORE check failed");
            return 0;
        }
    }
    else {  /* dateType == AFTER */
        if (DateGreaterThan(localTime, &certTime)) {
            WOLFSSL_MSG("Date AFTER check failed");
            return 0;
        }
    }

    return 1;
}
#endif /* USE_WOLF_VALIDDATE */

int wc_GetTime(void* timePtr, word32 timeSize)
{
    time_t* ltime = (time_t*)timePtr;

    if (timePtr == NULL) {
        return BAD_FUNC_ARG;
    }

    if ((word32)sizeof(time_t) > timeSize) {
        return BUFFER_E;
    }

    *ltime = XTIME(0);

    return 0;
}

#endif /* !NO_ASN_TIME */


#ifdef WOLFSSL_ASN_TEMPLATE
/* TODO: use a CHOICE instead of two items? */
/* ASN.1 template for a date - either UTC or Generatlized Time. */
static const ASNItem dateASN[] = {
/*  0 */    { 0, ASN_UTC_TIME, 0, 0, 2 },
/*  1 */    { 0, ASN_GENERALIZED_TIME, 0, 0, 2 },
};

/* Number of items in ASN.1 template for a date. */
#define dateASN_Length (sizeof(dateASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Get date buffer, format and length. Returns 0=success or error */
/* Decode a DateInfo - choice of UTC TIME or GENERALIZED TIME.
 *
 * @param [in]      source   Buffer containing encoded date.
 * @param [in, out] idx      On in, the index of the date.
 *                           On out, index after date.
 * @param [out]     pDate    Pointer into buffer of data bytes.
 * @param [out]     pFormat  Format of date - BER/DER tag.
 * @param [out]     pLength  Length of date bytes.
 * @param [in]      maxIdx   Index of next item after date.
 * @return  0 on success.
 * @return  BAD_FUNC_ARG when source or idx is NULL.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 */
static int GetDateInfo(const byte* source, word32* idx, const byte** pDate,
                        byte* pFormat, int* pLength, word32 maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int length;
    byte format;

    if (source == NULL || idx == NULL)
        return BAD_FUNC_ARG;

    /* get ASN format header */
    if (*idx+1 > maxIdx)
        return BUFFER_E;
    format = source[*idx];
    *idx += 1;
    if (format != ASN_UTC_TIME && format != ASN_GENERALIZED_TIME)
        return ASN_TIME_E;

    /* get length */
    if (GetLength(source, idx, &length, maxIdx) < 0)
        return ASN_PARSE_E;
    if (length > MAX_DATE_SIZE || length < MIN_DATE_SIZE)
        return ASN_DATE_SZ_E;

    /* return format, date and length */
    if (pFormat)
        *pFormat = format;
    if (pDate)
        *pDate = &source[*idx];
    if (pLength)
        *pLength = length;

    *idx += length;

    return 0;
#else
    ASNGetData dataASN[dateASN_Length];
    int i;
    int ret = 0;

    if ((source == NULL) || (idx == NULL)) {
        ret = BAD_FUNC_ARG;
    }
    if (ret == 0) {
        /* Initialize data. */
        XMEMSET(dataASN, 0, sizeof(dataASN));
        /* Parse date. */
        ret = GetASN_Items(dateASN, dataASN, dateASN_Length, 0, source, idx,
                           maxIdx);
    }
    if (ret == 0) {
        /* Determine which tag was seen. */
        i = (dataASN[0].tag != 0) ? 0 : 1;
        /* Return data from seen item. */
        if (pFormat != NULL) {
            *pFormat = dataASN[i].tag;
        }
        if (pDate != NULL) {
            *pDate = dataASN[i].data.ref.data;
        }
        if (pLength != NULL) {
            *pLength = dataASN[i].data.ref.length;
        }
    }

    return ret;
#endif
}

#ifndef WOLFSSL_ASN_TEMPLATE
static int GetDate(DecodedCert* cert, int dateType, int verify, int maxIdx)
{
    int    ret, length;
    const byte *datePtr = NULL;
    byte   date[MAX_DATE_SIZE];
    byte   format;
    word32 startIdx = 0;

    if (dateType == BEFORE)
        cert->beforeDate = &cert->source[cert->srcIdx];
    else
        cert->afterDate = &cert->source[cert->srcIdx];
    startIdx = cert->srcIdx;

    ret = GetDateInfo(cert->source, &cert->srcIdx, &datePtr, &format,
                      &length, maxIdx);
    if (ret < 0)
        return ret;

    XMEMSET(date, 0, MAX_DATE_SIZE);
    XMEMCPY(date, datePtr, length);

    if (dateType == BEFORE)
        cert->beforeDateLen = cert->srcIdx - startIdx;
    else
        cert->afterDateLen  = cert->srcIdx - startIdx;

#ifndef NO_ASN_TIME
    if (verify != NO_VERIFY && verify != VERIFY_SKIP_DATE &&
            !XVALIDATE_DATE(date, format, dateType)) {
        if (dateType == BEFORE)
            return ASN_BEFORE_DATE_E;
        else
            return ASN_AFTER_DATE_E;
    }
#else
    (void)verify;
#endif

    return 0;
}

static int GetValidity(DecodedCert* cert, int verify, int maxIdx)
{
    int length;
    int badDate = 0;

    if (GetSequence(cert->source, &cert->srcIdx, &length, maxIdx) < 0)
        return ASN_PARSE_E;

    maxIdx = cert->srcIdx + length;

    if (GetDate(cert, BEFORE, verify, maxIdx) < 0)
        badDate = ASN_BEFORE_DATE_E; /* continue parsing */

    if (GetDate(cert, AFTER, verify, maxIdx) < 0)
        return ASN_AFTER_DATE_E;

    if (badDate != 0)
        return badDate;

    return 0;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */


int wc_GetDateInfo(const byte* certDate, int certDateSz, const byte** date,
    byte* format, int* length)
{
    int ret;
    word32 idx = 0;

    ret = GetDateInfo(certDate, &idx, date, format, length, certDateSz);
    if (ret < 0)
        return ret;

    return 0;
}

#ifndef NO_ASN_TIME
int wc_GetDateAsCalendarTime(const byte* date, int length, byte format,
    struct tm* timearg)
{
    int idx = 0;
    (void)length;
    if (!ExtractDate(date, format, timearg, &idx))
        return ASN_TIME_E;
    return 0;
}

#if defined(WOLFSSL_CERT_GEN) && defined(WOLFSSL_ALT_NAMES)
int wc_GetCertDates(Cert* cert, struct tm* before, struct tm* after)
{
    int ret = 0;
    const byte* date;
    byte format;
    int length;

    if (cert == NULL)
        return BAD_FUNC_ARG;

    if (before && cert->beforeDateSz > 0) {
        ret = wc_GetDateInfo(cert->beforeDate, cert->beforeDateSz, &date,
                             &format, &length);
        if (ret == 0)
            ret = wc_GetDateAsCalendarTime(date, length, format, before);
    }
    if (after && cert->afterDateSz > 0) {
        ret = wc_GetDateInfo(cert->afterDate, cert->afterDateSz, &date,
                             &format, &length);
        if (ret == 0)
            ret = wc_GetDateAsCalendarTime(date, length, format, after);
    }

    return ret;
}
#endif /* WOLFSSL_CERT_GEN && WOLFSSL_ALT_NAMES */
#endif /* !NO_ASN_TIME */

#ifdef WOLFSSL_ASN_TEMPLATE
/* TODO: move code around to not require this. */
static int DecodeCertInternal(DecodedCert* cert, int verify, int* criticalExt,
                              int* badDateRet, int stopAtPubKey,
                              int stopAfterPubKey);
#endif

/* Parse the ceritifcate up to the X.509 public key.
 *
 * If cert data is invalid then badDate get set to error value.
 *
 * @param [in, out] cert     Decoded certificate object.
 * @param [in]      verify   Whether to verify dates.
 * @param [out]     badDate  Error code when verify dates.
 * @return  0 on success.
 * @return  ASN_TIME_E when date BER tag is nor UTC or GENERALIZED time.
 * @return  ASN_DATE_SZ_E when time data is not supported.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set.
 */
int wc_GetPubX509(DecodedCert* cert, int verify, int* badDate)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;

    if (cert == NULL || badDate == NULL)
        return BAD_FUNC_ARG;

    *badDate = 0;
    if ( (ret = GetCertHeader(cert)) < 0)
        return ret;

    WOLFSSL_MSG("Got Cert Header");

#ifdef WOLFSSL_CERT_REQ
    if (!cert->isCSR) {
#endif
        /* Using the sigIndex as the upper bound because that's where the
         * actual certificate data ends. */
        if ( (ret = GetAlgoId(cert->source, &cert->srcIdx, &cert->signatureOID,
                              oidSigType, cert->sigIndex)) < 0)
            return ret;

        WOLFSSL_MSG("Got Algo ID");

        if ( (ret = GetName(cert, ISSUER, cert->sigIndex)) < 0)
            return ret;

        if ( (ret = GetValidity(cert, verify, cert->sigIndex)) < 0)
            *badDate = ret;
#ifdef WOLFSSL_CERT_REQ
    }
#endif

    if ( (ret = GetName(cert, SUBJECT, cert->sigIndex)) < 0)
        return ret;

    WOLFSSL_MSG("Got Subject Name");
    return ret;
#else
    /* Use common decode routine and stop at public key. */
    int ret;

    *badDate = 0;

    ret = DecodeCertInternal(cert, verify, NULL, badDate, 1, 0);
    if (ret >= 0) {
        /* Store current index: public key. */
        cert->srcIdx = ret;
    }
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* Parse the ceritifcate up to and including X.509 public key.
 *
 * @param [in, out] cert     Decoded certificate object.
 * @param [in]      verify   Whether to verify dates.
 * @return  0 on success.
 * @return  ASN_TIME_E when date BER tag is nor UTC or GENERALIZED time.
 * @return  ASN_DATE_SZ_E when time data is not supported.
 * @return  ASN_BEFORE_DATE_E when BEFORE date is invalid.
 * @return  ASN_AFTER_DATE_E when AFTER date is invalid.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set.
 */
int DecodeToKey(DecodedCert* cert, int verify)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int badDate = 0;
    int ret;

    if ( (ret = wc_GetPubX509(cert, verify, &badDate)) < 0)
        return ret;

    /* Determine if self signed */
    cert->selfSigned = XMEMCMP(cert->issuerHash,
                               cert->subjectHash,
                               KEYID_SIZE) == 0 ? 1 : 0;

    ret = GetCertKey(cert, cert->source, &cert->srcIdx, cert->maxIdx);
    if (ret != 0)
        return ret;

    WOLFSSL_MSG("Got Key");

    if (badDate != 0)
        return badDate;

    return ret;
#else
    int ret;
    int badDate = 0;

    /* Call internal version and stop after public key. */
    ret = DecodeCertInternal(cert, verify, NULL, &badDate, 0, 1);
    /* Always return date errors. */
    if (ret == 0) {
        ret = badDate;
    }
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#if !defined(NO_CERTS) && !defined(WOLFSSL_ASN_TEMPLATE)
static int GetSignature(DecodedCert* cert)
{
    int length;
    int ret;

    ret = CheckBitString(cert->source, &cert->srcIdx, &length, cert->maxIdx, 1,
                         NULL);
    if (ret != 0)
        return ret;

    cert->sigLength = length;
    cert->signature = &cert->source[cert->srcIdx];
    cert->srcIdx += cert->sigLength;

    if (cert->srcIdx != cert->maxIdx)
        return ASN_PARSE_E;

    return 0;
}
#endif /* !NO_CERTS && !WOLFSSL_ASN_TEMPLATE */

#ifndef WOLFSSL_ASN_TEMPLATE
static word32 SetOctetString8Bit(word32 len, byte* output)
{
    output[0] = ASN_OCTET_STRING;
    output[1] = (byte)len;
    return 2;
}
static word32 SetDigest(const byte* digest, word32 digSz, byte* output)
{
    word32 idx = SetOctetString8Bit(digSz, output);
    XMEMCPY(&output[idx], digest, digSz);

    return idx + digSz;
}
#endif


/* Encode a length for DER.
 *
 * @param [in]  length  Value to encode.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
word32 SetLength(word32 length, byte* output)
{
    /* Start encoding at start of buffer. */
    word32 i = 0;

    if (length < ASN_LONG_LENGTH) {
        /* Only one byte needed to encode. */
        if (output) {
            /* Write out length value. */
            output[i] = (byte)length;
        }
        /* Skip over length. */
        i++;
    }
    else {
        /* Calculate the number of bytes required to encode value. */
        byte j = (byte)BytePrecision(length);

        if (output) {
            /* Encode count byte. */
            output[i] = j | ASN_LONG_LENGTH;
        }
        /* Skip over count byte. */
        i++;

        /* Encode value as a big-endian byte array. */
        for (; j > 0; --j) {
            if (output) {
                /* Encode next most-significant byte. */
                output[i] = (byte)(length >> ((j - 1) * WOLFSSL_BIT_SIZE));
            }
            /* Skip over byte. */
            i++;
        }
    }

    /* Return number of bytes in encoded length. */
    return i;
}

/* Encode a DER header - type/tag and length.
 *
 * @param [in]  tag     DER tag of ASN.1 item.
 * @param [in]  len     Length of data in ASN.1 item.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
static word32 SetHeader(byte tag, word32 len, byte* output)
{
    if (output) {
        /* Encode tag first. */
        output[0] = tag;
    }
    /* Encode the length. */
    return SetLength(len, output ? output + ASN_TAG_SZ : NULL) + ASN_TAG_SZ;
}

/* Encode a SEQUENCE header in DER.
 *
 * @param [in]  len     Length of data in SEQUENCE.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
word32 SetSequence(word32 len, byte* output)
{
    return SetHeader(ASN_SEQUENCE | ASN_CONSTRUCTED, len, output);
}

/* Encode an OCTET STRING header in DER.
 *
 * @param [in]  len     Length of data in OCTET STRING.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
word32 SetOctetString(word32 len, byte* output)
{
    return SetHeader(ASN_OCTET_STRING, len, output);
}

/* Encode a SET header in DER.
 *
 * @param [in]  len     Length of data in SET.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
word32 SetSet(word32 len, byte* output)
{
    return SetHeader(ASN_SET | ASN_CONSTRUCTED, len, output);
}

/* Encode an implicit context specific header in DER.
 *
 * Implicit means that it is constructed only if the included ASN.1 item is.
 *
 * @param [in]  tag     Tag for the implicit ASN.1 item.
 * @param [in]  number  Context specific number.
 * @param [in]  len     Length of data in SET.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
word32 SetImplicit(byte tag, byte number, word32 len, byte* output)
{
    tag = ((tag == ASN_SEQUENCE || tag == ASN_SET) ? ASN_CONSTRUCTED : 0)
                    | ASN_CONTEXT_SPECIFIC | number;
    return SetHeader(tag, len, output);
}

/* Encode an explicit context specific header in DER.
 *
 * Explicit means that there will be an ASN.1 item underneath.
 *
 * @param [in]  number  Context specific number.
 * @param [in]  len     Length of data in SET.
 * @param [out] output  Buffer to encode into.
 * @return  Number of bytes encoded.
 */
word32 SetExplicit(byte number, word32 len, byte* output)
{
    return SetHeader(ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | number, len,
                     output);
}


#if defined(HAVE_ECC) && defined(HAVE_ECC_KEY_EXPORT)

#ifndef WOLFSSL_ASN_TEMPLATE
static int SetCurve(ecc_key* key, byte* output)
{
#ifdef HAVE_OID_ENCODING
    int ret;
#endif
    int idx = 0;
    word32 oidSz = 0;

    /* validate key */
    if (key == NULL || key->dp == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef HAVE_OID_ENCODING
    ret = EncodeObjectId(key->dp->oid, key->dp->oidSz, NULL, &oidSz);
    if (ret != 0) {
        return ret;
    }
#else
    oidSz = key->dp->oidSz;
#endif

    idx += SetObjectId(oidSz, output);

#ifdef HAVE_OID_ENCODING
    ret = EncodeObjectId(key->dp->oid, key->dp->oidSz, output+idx, &oidSz);
    if (ret != 0) {
        return ret;
    }
#else
    XMEMCPY(output+idx, key->dp->oid, oidSz);
#endif
    idx += oidSz;

    return idx;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */

#endif /* HAVE_ECC && HAVE_ECC_KEY_EXPORT */


#ifdef HAVE_ECC
/* Determines whether the signature algorithm is using ECDSA.
 *
 * @param [in] algoOID  Signature algorithm identifier.
 * @return  1 when algorithm is using ECDSA.
 * @return  0 otherwise.
 */
static WC_INLINE int IsSigAlgoECDSA(int algoOID)
{
    /* ECDSA sigAlgo must not have ASN1 NULL parameters */
    if (algoOID == CTC_SHAwECDSA || algoOID == CTC_SHA256wECDSA ||
        algoOID == CTC_SHA384wECDSA || algoOID == CTC_SHA512wECDSA) {
        return 1;
    }

    return 0;
}
#endif

/* Determines if OID is for an EC signing algorithm including ECDSA and EdDSA.
 *
 * @param [in] algoOID  Algorithm OID.
 * @return  1 when is EC signing algorithm.
 * @return  0 otherwise.
 */
static WC_INLINE int IsSigAlgoECC(int algoOID)
{
    (void)algoOID;

    return (0
        #ifdef HAVE_ECC
              || IsSigAlgoECDSA(algoOID)
        #endif
        #ifdef HAVE_ED25519
              || (algoOID == ED25519k)
        #endif
        #ifdef HAVE_CURVE25519
              || (algoOID == X25519k)
        #endif
        #ifdef HAVE_ED448
              || (algoOID == ED448k)
        #endif
        #ifdef HAVE_CURVE448
              || (algoOID == X448k)
        #endif
    );
}

/* Encode an algorithm identifier.
 *
 * [algoOID, type] is unique.
 *
 * @param [in]  algoOID   Algorithm identifier.
 * @param [out] output    Buffer to hold encoding.
 * @param [in]  type      Type of OID being encoded.
 * @param [in]  curveSz   Add extra space for curve data.
 * @return  Encoded data size on success.
 * @return  0 when dynamic memory allocation fails.
 */
word32 SetAlgoID(int algoOID, byte* output, int type, int curveSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 tagSz, idSz, seqSz, algoSz = 0;
    const  byte* algoName = 0;
    byte   ID_Length[1 + MAX_LENGTH_SZ];
    byte   seqArray[MAX_SEQ_SZ + 1];  /* add object_id to end */
    int    length = 0;

    tagSz = (type == oidHashType ||
             (type == oidSigType && !IsSigAlgoECC(algoOID)) ||
             (type == oidKeyType && algoOID == RSAk)) ? 2 : 0;

    algoName = OidFromId(algoOID, type, &algoSz);
    if (algoName == NULL) {
        WOLFSSL_MSG("Unknown Algorithm");
        return 0;
    }

    idSz  = SetObjectId(algoSz, ID_Length);
    seqSz = SetSequence(idSz + algoSz + tagSz + curveSz, seqArray);

    /* Copy only algo to output for DSA keys */
    if (algoOID == DSAk && output) {
        XMEMCPY(output, ID_Length, idSz);
        XMEMCPY(output + idSz, algoName, algoSz);
        if (tagSz == 2)
            SetASNNull(&output[seqSz + idSz + algoSz]);
    }
    else if (output) {
        XMEMCPY(output, seqArray, seqSz);
        XMEMCPY(output + seqSz, ID_Length, idSz);
        XMEMCPY(output + seqSz + idSz, algoName, algoSz);
        if (tagSz == 2)
            SetASNNull(&output[seqSz + idSz + algoSz]);
    }

    if (algoOID == DSAk)
        length = idSz + algoSz + tagSz;
    else
        length = seqSz + idSz + algoSz + tagSz;

    return length;
#else
    DECL_ASNSETDATA(dataASN, algoIdASN_Length);
    int sz;
    int ret = 0;
    int o = 0;

    CALLOC_ASNSETDATA(dataASN, algoIdASN_Length, ret, NULL);

    /* Set the OID and OID type to encode. */
    SetASN_OID(&dataASN[1], algoOID, type);
    /* Hashes, signatures not ECC and keys not RSA put put NULL tag. */
    if (!(type == oidHashType ||
             (type == oidSigType && !IsSigAlgoECC(algoOID)) ||
             (type == oidKeyType && algoOID == RSAk))) {
        /* Don't put out NULL DER item. */
        dataASN[2].noOut = 1;
    }
    if (algoOID == DSAk) {
        /* Don't include SEQUENCE for DSA keys. */
        o = 1;
    }
    else if (curveSz > 0) {
        /* Don't put out NULL DER item. */
        dataASN[2].noOut = 0;
        /* Include space for extra data of length curveSz.
         * Subtract 1 for sequence and 1 for length encoding. */
        SetASN_Buffer(&dataASN[2], NULL, curveSz - 2);
    }

    /* Calculate size of encoding. */
    ret = SizeASN_Items(algoIdASN + o, dataASN + o, algoIdASN_Length - o, &sz);
    if (ret == 0 && output != NULL) {
        /* Encode into buffer. */
        SetASN_Items(algoIdASN + o, dataASN + o, algoIdASN_Length - o, output);
        if (curveSz > 0) {
            /* Return size excluding curve data. */
            sz = dataASN[o].offset - dataASN[2].offset;
        }
    }

    if (ret == 0) {
        /* Return encoded size. */
        ret = sz;
    }
    else {
        /* Unsigned return type so 0 indicates error. */
        ret = 0;
    }

    FREE_ASNSETDATA(dataASN, NULL);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* Always encode PKCS#1 v1.5 RSA signature and compare to encoded data. */
/* ASN.1 template for DigestInfo for a PKCS#1 v1.5 RSA signature.
 * PKCS#1 v2.2: RFC 8017, A.2.4 - DigestInfo
 */
static const ASNItem digestInfoASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* digestAlgorithm */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  3 */            { 2, ASN_TAG_NULL, 0, 0, 0 },
                /* digest */
/*  4 */        { 1, ASN_OCTET_STRING, 0, 0, 0 }
};

/* Number of items in ASN.1 template for DigestInfo for RSA. */
#define digestInfoASN_Length (sizeof(digestInfoASN) / sizeof(ASNItem))
#endif

/* Encode signature.
 *
 * @param [out] out     Buffer to hold encoding.
 * @param [in]  digest  Buffer holding digest.
 * @param [in]  digSz   Length of digest in bytes.
 * @return  Encoded data size on success.
 * @return  0 when dynamic memory allocation fails.
 */
word32 wc_EncodeSignature(byte* out, const byte* digest, word32 digSz,
                          int hashOID)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    byte digArray[MAX_ENCODED_DIG_SZ];
    byte algoArray[MAX_ALGO_SZ];
    byte seqArray[MAX_SEQ_SZ];
    word32 encDigSz, algoSz, seqSz;

    encDigSz = SetDigest(digest, digSz, digArray);
    algoSz   = SetAlgoID(hashOID, algoArray, oidHashType, 0);
    seqSz    = SetSequence(encDigSz + algoSz, seqArray);

    XMEMCPY(out, seqArray, seqSz);
    XMEMCPY(out + seqSz, algoArray, algoSz);
    XMEMCPY(out + seqSz + algoSz, digArray, encDigSz);

    return encDigSz + algoSz + seqSz;
#else
    DECL_ASNSETDATA(dataASN, digestInfoASN_Length);
    int ret = 0;
    int sz;

    CALLOC_ASNSETDATA(dataASN, digestInfoASN_Length, ret, NULL);

    if (ret == 0) {
        /* Set hash OID and type. */
        SetASN_OID(&dataASN[2], hashOID, oidHashType);
        /* Set digest. */
        SetASN_Buffer(&dataASN[4], digest, digSz);

        /* Calculate size of encoding. */
        ret = SizeASN_Items(digestInfoASN, dataASN, digestInfoASN_Length, &sz);
    }
    if (ret == 0) {
        /* Encode PKCS#1 v1.5 RSA signature. */
        SetASN_Items(digestInfoASN, dataASN, digestInfoASN_Length, out);
        ret = sz;
    }
    else {
        /* Unsigned return type so 0 indicates error. */
        ret = 0;
    }

    FREE_ASNSETDATA(dataASN, NULL);
    return ret;
#endif
}


#ifndef NO_CERTS

int wc_GetCTC_HashOID(int type)
{
    int ret;
    enum wc_HashType hType;

    hType = wc_HashTypeConvert(type);
    ret = wc_HashGetOID(hType);
    if (ret < 0) {
        ret = 0; /* backwards compatibility */
    }

    return ret;
}

/* Initialize a signature context object.
 *
 * Object used for signing and verifying a certificate signature.
 *
 * @param [in, out] sigCtx  Signature context object.
 * @param [in]      heap    Dynamic memory hint.
 * @param [in]      devId   Hardware device identifier.
 */
void InitSignatureCtx(SignatureCtx* sigCtx, void* heap, int devId)
{
    if (sigCtx) {
        XMEMSET(sigCtx, 0, sizeof(SignatureCtx));
        sigCtx->devId = devId;
        sigCtx->heap = heap;
    }
}

/* Free dynamic data in a signature context object.
 *
 * @param [in, out] sigCtx  Signature context object.
 */
void FreeSignatureCtx(SignatureCtx* sigCtx)
{
    if (sigCtx == NULL)
        return;

    if (sigCtx->digest) {
        XFREE(sigCtx->digest, sigCtx->heap, DYNAMIC_TYPE_DIGEST);
        sigCtx->digest = NULL;
    }
#if !(defined(NO_RSA) && defined(NO_DSA))
    if (sigCtx->sigCpy) {
        XFREE(sigCtx->sigCpy, sigCtx->heap, DYNAMIC_TYPE_SIGNATURE);
        sigCtx->sigCpy = NULL;
    }
#endif
#ifndef NO_ASN_CRYPT
    if (sigCtx->key.ptr) {
        switch (sigCtx->keyOID) {
        #ifndef NO_RSA
            case RSAk:
                wc_FreeRsaKey(sigCtx->key.rsa);
                XFREE(sigCtx->key.ptr, sigCtx->heap, DYNAMIC_TYPE_RSA);
                break;
        #endif /* !NO_RSA */
        #ifndef NO_DSA
            case DSAk:
                wc_FreeDsaKey(sigCtx->key.dsa);
                XFREE(sigCtx->key.dsa, sigCtx->heap, DYNAMIC_TYPE_DSA);
                break;
        #endif
        #ifdef HAVE_ECC
            case ECDSAk:
                wc_ecc_free(sigCtx->key.ecc);
                XFREE(sigCtx->key.ecc, sigCtx->heap, DYNAMIC_TYPE_ECC);
                break;
        #endif /* HAVE_ECC */
        #ifdef HAVE_ED25519
            case ED25519k:
                wc_ed25519_free(sigCtx->key.ed25519);
                XFREE(sigCtx->key.ed25519, sigCtx->heap, DYNAMIC_TYPE_ED25519);
                break;
        #endif /* HAVE_ED25519 */
        #ifdef HAVE_ED448
            case ED448k:
                wc_ed448_free(sigCtx->key.ed448);
                XFREE(sigCtx->key.ed448, sigCtx->heap, DYNAMIC_TYPE_ED448);
                break;
        #endif /* HAVE_ED448 */
            default:
                break;
        } /* switch (keyOID) */
        sigCtx->key.ptr = NULL;
    }
#endif

    /* reset state, we are done */
    sigCtx->state = SIG_STATE_BEGIN;
}

#ifndef NO_ASN_CRYPT
static int HashForSignature(const byte* buf, word32 bufSz, word32 sigOID,
                            byte* digest, int* typeH, int* digestSz, int verify)
{
    int ret = 0;

    switch (sigOID) {
    #if defined(WOLFSSL_MD2)
        case CTC_MD2wRSA:
            if (!verify) {
                ret = HASH_TYPE_E;
                WOLFSSL_MSG("MD2 not supported for signing");
            }
            else if ((ret = wc_Md2Hash(buf, bufSz, digest)) == 0) {
                *typeH    = MD2h;
                *digestSz = MD2_DIGEST_SIZE;
            }
        break;
    #endif
    #ifndef NO_MD5
        case CTC_MD5wRSA:
            if ((ret = wc_Md5Hash(buf, bufSz, digest)) == 0) {
                *typeH    = MD5h;
                *digestSz = WC_MD5_DIGEST_SIZE;
            }
            break;
    #endif
    #ifndef NO_SHA
        case CTC_SHAwRSA:
        case CTC_SHAwDSA:
        case CTC_SHAwECDSA:
            if ((ret = wc_ShaHash(buf, bufSz, digest)) == 0) {
                *typeH    = SHAh;
                *digestSz = WC_SHA_DIGEST_SIZE;
            }
            break;
    #endif
    #ifdef WOLFSSL_SHA224
        case CTC_SHA224wRSA:
        case CTC_SHA224wECDSA:
            if ((ret = wc_Sha224Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA224h;
                *digestSz = WC_SHA224_DIGEST_SIZE;
            }
            break;
    #endif
    #ifndef NO_SHA256
        case CTC_SHA256wRSA:
        case CTC_SHA256wECDSA:
        case CTC_SHA256wDSA:
            if ((ret = wc_Sha256Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA256h;
                *digestSz = WC_SHA256_DIGEST_SIZE;
            }
            break;
    #endif
    #ifdef WOLFSSL_SHA384
        case CTC_SHA384wRSA:
        case CTC_SHA384wECDSA:
            if ((ret = wc_Sha384Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA384h;
                *digestSz = WC_SHA384_DIGEST_SIZE;
            }
            break;
    #endif
    #ifdef WOLFSSL_SHA512
        case CTC_SHA512wRSA:
        case CTC_SHA512wECDSA:
            if ((ret = wc_Sha512Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA512h;
                *digestSz = WC_SHA512_DIGEST_SIZE;
            }
            break;
    #endif
    #ifdef WOLFSSL_SHA3
    #ifndef WOLFSSL_NOSHA3_224
        case CTC_SHA3_224wRSA:
        case CTC_SHA3_224wECDSA:
            if ((ret = wc_Sha3_224Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA3_224h;
                *digestSz = WC_SHA3_224_DIGEST_SIZE;
            }
            break;
    #endif
    #ifndef WOLFSSL_NOSHA3_256
        case CTC_SHA3_256wRSA:
        case CTC_SHA3_256wECDSA:
            if ((ret = wc_Sha3_256Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA3_256h;
                *digestSz = WC_SHA3_256_DIGEST_SIZE;
            }
            break;
    #endif
    #ifndef WOLFSSL_NOSHA3_384
        case CTC_SHA3_384wRSA:
        case CTC_SHA3_384wECDSA:
            if ((ret = wc_Sha3_384Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA3_384h;
                *digestSz = WC_SHA3_384_DIGEST_SIZE;
            }
            break;
    #endif
    #ifndef WOLFSSL_NOSHA3_512
        case CTC_SHA3_512wRSA:
        case CTC_SHA3_512wECDSA:
            if ((ret = wc_Sha3_512Hash(buf, bufSz, digest)) == 0) {
                *typeH    = SHA3_512h;
                *digestSz = WC_SHA3_512_DIGEST_SIZE;
            }
            break;
    #endif
    #endif
    #ifdef HAVE_ED25519
        case CTC_ED25519:
            /* Hashes done in signing operation.
             * Two dependent hashes with prefixes performed.
             */
            break;
    #endif
    #ifdef HAVE_ED448
        case CTC_ED448:
            /* Hashes done in signing operation.
             * Two dependent hashes with prefixes performed.
             */
            break;
    #endif
        default:
            ret = HASH_TYPE_E;
            WOLFSSL_MSG("Hash for Signature has unsupported type");
    }

    (void)buf;
    (void)bufSz;
    (void)sigOID;
    (void)digest;
    (void)digestSz;
    (void)typeH;
    (void)verify;

    return ret;
}
#endif /* !NO_ASN_CRYPT */

/* Return codes: 0=Success, Negative (see error-crypt.h), ASN_SIG_CONFIRM_E */
static int ConfirmSignature(SignatureCtx* sigCtx,
    const byte* buf, word32 bufSz,
    const byte* key, word32 keySz, word32 keyOID,
    const byte* sig, word32 sigSz, word32 sigOID, byte* rsaKeyIdx)
{
    int ret = 0;
#ifndef WOLFSSL_RENESAS_TSIP_TLS
    (void)rsaKeyIdx;
#endif
    if (sigCtx == NULL || buf == NULL || bufSz == 0 || key == NULL ||
        keySz == 0 || sig == NULL || sigSz == 0) {
        return BAD_FUNC_ARG;
    }

    (void)key;
    (void)keySz;
    (void)sig;
    (void)sigSz;

    WOLFSSL_ENTER("ConfirmSignature");

#ifndef NO_ASN_CRYPT
    switch (sigCtx->state) {
        case SIG_STATE_BEGIN:
        {
            sigCtx->keyOID = keyOID; /* must set early for cleanup */

            sigCtx->digest = (byte*)XMALLOC(WC_MAX_DIGEST_SIZE, sigCtx->heap,
                                                    DYNAMIC_TYPE_DIGEST);
            if (sigCtx->digest == NULL) {
                ERROR_OUT(MEMORY_E, exit_cs);
            }

            sigCtx->state = SIG_STATE_HASH;
        } /* SIG_STATE_BEGIN */
        FALL_THROUGH;

        case SIG_STATE_HASH:
        {
            ret = HashForSignature(buf, bufSz, sigOID, sigCtx->digest,
                                   &sigCtx->typeH, &sigCtx->digestSz, 1);
            if (ret != 0) {
                goto exit_cs;
            }

            sigCtx->state = SIG_STATE_KEY;
        } /* SIG_STATE_HASH */
        FALL_THROUGH;

        case SIG_STATE_KEY:
        {
            switch (keyOID) {
            #ifndef NO_RSA
                case RSAk:
                {
                    word32 idx = 0;

                    sigCtx->key.rsa = (RsaKey*)XMALLOC(sizeof(RsaKey),
                                                sigCtx->heap, DYNAMIC_TYPE_RSA);
                    sigCtx->sigCpy = (byte*)XMALLOC(MAX_ENCODED_SIG_SZ,
                                         sigCtx->heap, DYNAMIC_TYPE_SIGNATURE);
                    if (sigCtx->key.rsa == NULL || sigCtx->sigCpy == NULL) {
                        ERROR_OUT(MEMORY_E, exit_cs);
                    }
                    if ((ret = wc_InitRsaKey_ex(sigCtx->key.rsa, sigCtx->heap,
                                                        sigCtx->devId)) != 0) {
                        goto exit_cs;
                    }
                    if (sigSz > MAX_ENCODED_SIG_SZ) {
                        WOLFSSL_MSG("Verify Signature is too big");
                        ERROR_OUT(BUFFER_E, exit_cs);
                    }
                    if ((ret = wc_RsaPublicKeyDecode(key, &idx, sigCtx->key.rsa,
                                                                 keySz)) != 0) {
                        WOLFSSL_MSG("ASN Key decode error RSA");
                        goto exit_cs;
                    }
                    XMEMCPY(sigCtx->sigCpy, sig, sigSz);
                    sigCtx->out = NULL;

                #ifdef WOLFSSL_ASYNC_CRYPT
                    sigCtx->asyncDev = &sigCtx->key.rsa->asyncDev;
                #endif
                    break;
                }
            #endif /* !NO_RSA */
            #if !defined(NO_DSA) && !defined(HAVE_SELFTEST)
                case DSAk:
                {
                    word32 idx = 0;

                    if (sigSz < DSA_MIN_SIG_SIZE) {
                        WOLFSSL_MSG("Verify Signature is too small");
                        ERROR_OUT(BUFFER_E, exit_cs);
                    }
                    sigCtx->key.dsa = (DsaKey*)XMALLOC(sizeof(DsaKey),
                                                sigCtx->heap, DYNAMIC_TYPE_DSA);
                    sigCtx->sigCpy = (byte*)XMALLOC(sigSz,
                                         sigCtx->heap, DYNAMIC_TYPE_SIGNATURE);
                    if (sigCtx->key.dsa == NULL || sigCtx->sigCpy == NULL) {
                        ERROR_OUT(MEMORY_E, exit_cs);
                    }
                    if ((ret = wc_InitDsaKey_h(sigCtx->key.dsa, sigCtx->heap)) != 0) {
                        WOLFSSL_MSG("wc_InitDsaKey_h error");
                        goto exit_cs;
                    }
                    if ((ret = wc_DsaPublicKeyDecode(key, &idx, sigCtx->key.dsa,
                                                                 keySz)) != 0) {
                        WOLFSSL_MSG("ASN Key decode error DSA");
                        goto exit_cs;
                    }
                    if (sigSz != DSA_160_SIG_SIZE &&
                            sigSz != DSA_256_SIG_SIZE) {
                        /* Try to parse it as the contents of a bitstring */
                    #ifdef WOLFSSL_SMALL_STACK
                        mp_int* r;
                        mp_int* s;
                    #else
                        mp_int r[1];
                        mp_int s[1];
                    #endif
                        int rSz;
                        int sSz;

                    #ifdef WOLFSSL_SMALL_STACK
                        r = (mp_int*)XMALLOC(sizeof(*r), sigCtx->heap,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
                        if (r == NULL) {
                            ERROR_OUT(MEMORY_E, exit_cs);
                        }
                        s = (mp_int*)XMALLOC(sizeof(*s), sigCtx->heap,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
                        if (s == NULL) {
                            XFREE(r, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                            ERROR_OUT(MEMORY_E, exit_cs);
                        }
                    #endif
                        mp_init(r);
                        mp_init(s);

                        idx = 0;
                        if (DecodeECC_DSA_Sig(sig + idx, sigSz - idx, r, s)
                                              != 0) {
                            WOLFSSL_MSG("DSA Sig is in unrecognized or "
                                        "incorrect format");
                            mp_free(r);
                            mp_free(s);
                    #ifdef WOLFSSL_SMALL_STACK
                            XFREE(r, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                            XFREE(s, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                    #endif
                            ERROR_OUT(ASN_SIG_CONFIRM_E, exit_cs);
                        }
                        rSz = mp_unsigned_bin_size(r);
                        sSz = mp_unsigned_bin_size(s);
                        if (rSz + sSz > (int)sigSz) {
                            WOLFSSL_MSG("DSA Sig is in unrecognized or "
                                        "incorrect format");
                            mp_free(r);
                            mp_free(s);
                    #ifdef WOLFSSL_SMALL_STACK
                            XFREE(r, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                            XFREE(s, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                    #endif
                            ERROR_OUT(ASN_SIG_CONFIRM_E, exit_cs);
                        }
                        if (mp_to_unsigned_bin(r, sigCtx->sigCpy) != MP_OKAY ||
                                mp_to_unsigned_bin(s,
                                        sigCtx->sigCpy + rSz) != MP_OKAY) {
                            WOLFSSL_MSG("DSA Sig is in unrecognized or "
                                        "incorrect format");
                            mp_free(r);
                            mp_free(s);
                    #ifdef WOLFSSL_SMALL_STACK
                            XFREE(r, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                            XFREE(s, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                    #endif
                            ERROR_OUT(ASN_SIG_CONFIRM_E, exit_cs);
                        }
                        mp_free(r);
                        mp_free(s);
                    #ifdef WOLFSSL_SMALL_STACK
                        XFREE(r, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                        XFREE(s, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                    #endif
                    }
                    else {
                        XMEMCPY(sigCtx->sigCpy, sig, sigSz);
                    }
                    break;
                }
            #endif /* !NO_DSA && !HAVE_SELFTEST */
            #ifdef HAVE_ECC
                case ECDSAk:
                {
                    word32 idx = 0;

                    sigCtx->verify = 0;
                    sigCtx->key.ecc = (ecc_key*)XMALLOC(sizeof(ecc_key),
                                                sigCtx->heap, DYNAMIC_TYPE_ECC);
                    if (sigCtx->key.ecc == NULL) {
                        ERROR_OUT(MEMORY_E, exit_cs);
                    }
                    if ((ret = wc_ecc_init_ex(sigCtx->key.ecc, sigCtx->heap,
                                                          sigCtx->devId)) < 0) {
                        goto exit_cs;
                    }
                    ret = wc_EccPublicKeyDecode(key, &idx, sigCtx->key.ecc,
                                                                         keySz);
                    if (ret < 0) {
                        WOLFSSL_MSG("ASN Key import error ECC");
                        goto exit_cs;
                    }
                #ifdef WOLFSSL_ASYNC_CRYPT
                    sigCtx->asyncDev = &sigCtx->key.ecc->asyncDev;
                #endif
                    break;
                }
            #endif /* HAVE_ECC */
            #if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_IMPORT)
                case ED25519k:
                {
                    sigCtx->verify = 0;
                    sigCtx->key.ed25519 = (ed25519_key*)XMALLOC(
                                              sizeof(ed25519_key), sigCtx->heap,
                                              DYNAMIC_TYPE_ED25519);
                    if (sigCtx->key.ed25519 == NULL) {
                        ERROR_OUT(MEMORY_E, exit_cs);
                    }
                    if ((ret = wc_ed25519_init(sigCtx->key.ed25519)) < 0) {
                        goto exit_cs;
                    }
                    if ((ret = wc_ed25519_import_public(key, keySz,
                                                    sigCtx->key.ed25519)) < 0) {
                        WOLFSSL_MSG("ASN Key import error ED25519");
                        goto exit_cs;
                    }
                #ifdef WOLFSSL_ASYNC_CRYPT
                    sigCtx->asyncDev = &sigCtx->key.ed25519->asyncDev;
                #endif
                    break;
                }
            #endif
            #if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_IMPORT)
                case ED448k:
                {
                    sigCtx->verify = 0;
                    sigCtx->key.ed448 = (ed448_key*)XMALLOC(
                                                sizeof(ed448_key), sigCtx->heap,
                                                DYNAMIC_TYPE_ED448);
                    if (sigCtx->key.ed448 == NULL) {
                        ERROR_OUT(MEMORY_E, exit_cs);
                    }
                    if ((ret = wc_ed448_init(sigCtx->key.ed448)) < 0) {
                        goto exit_cs;
                    }
                    if ((ret = wc_ed448_import_public(key, keySz,
                                                      sigCtx->key.ed448)) < 0) {
                        WOLFSSL_MSG("ASN Key import error ED448");
                        goto exit_cs;
                    }
                #ifdef WOLFSSL_ASYNC_CRYPT
                    sigCtx->asyncDev = &sigCtx->key.ed448->asyncDev;
                #endif
                    break;
                }
            #endif
                default:
                    WOLFSSL_MSG("Verify Key type unknown");
                    ret = ASN_UNKNOWN_OID_E;
                    break;
            } /* switch (keyOID) */

            if (ret != 0) {
                goto exit_cs;
            }

            sigCtx->state = SIG_STATE_DO;

        #ifdef WOLFSSL_ASYNC_CRYPT
            if (sigCtx->devId != INVALID_DEVID && sigCtx->asyncDev && sigCtx->asyncCtx) {
                /* make sure event is initialized */
                WOLF_EVENT* event = &sigCtx->asyncDev->event;
                ret = wolfAsync_EventInit(event, WOLF_EVENT_TYPE_ASYNC_WOLFSSL,
                    sigCtx->asyncCtx, WC_ASYNC_FLAG_CALL_AGAIN);
            }
        #endif
        } /* SIG_STATE_KEY */
        FALL_THROUGH;

        case SIG_STATE_DO:
        {
            switch (keyOID) {
            #ifndef NO_RSA
                case RSAk:
                {
                #ifdef HAVE_PK_CALLBACKS
                    if (sigCtx->pkCbRsa) {
                        ret = sigCtx->pkCbRsa(
                                sigCtx->sigCpy, sigSz, &sigCtx->out,
                                key, keySz,
                                sigCtx->pkCtxRsa);
                    }
                    else
                #endif /* HAVE_PK_CALLBACKS */
                    {
                     #ifdef WOLFSSL_RENESAS_TSIP_TLS
                        if (rsaKeyIdx != NULL)
                        {
                            ret = tsip_tls_CertVerify(buf, bufSz, sigCtx->sigCpy,
                                sigSz,
                                sigCtx->pubkey_n_start - sigCtx->certBegin,
                                sigCtx->pubkey_n_len - 1,
                                sigCtx->pubkey_e_start - sigCtx->certBegin,
                                sigCtx->pubkey_e_len - 1,
                                rsaKeyIdx);

                            if (ret == 0){
                                sigCtx->verifyByTSIP = 1;
                                ret = 0;
                            } else {
                                WOLFSSL_MSG("RSA Verify by tsip didn't match");
                                ret = ASN_SIG_CONFIRM_E;
                            }
                        } else
                    #endif
                        ret = wc_RsaSSL_VerifyInline(sigCtx->sigCpy, sigSz,
                                                 &sigCtx->out, sigCtx->key.rsa);
                    }
                    break;
                }
            #endif /* !NO_RSA */
            #if !defined(NO_DSA) && !defined(HAVE_SELFTEST)
                case DSAk:
                {
                    ret = wc_DsaVerify(sigCtx->digest, sigCtx->sigCpy,
                            sigCtx->key.dsa, &sigCtx->verify);
                    break;
                }
            #endif /* !NO_DSA && !HAVE_SELFTEST */
            #if defined(HAVE_ECC)
                case ECDSAk:
                {
                #ifdef HAVE_PK_CALLBACKS
                    if (sigCtx->pkCbEcc) {
                        ret = sigCtx->pkCbEcc(
                                sig, sigSz,
                                sigCtx->digest, sigCtx->digestSz,
                                key, keySz, &sigCtx->verify,
                                sigCtx->pkCtxEcc);
                    }
                    else
                #endif /* HAVE_PK_CALLBACKS */
                    {
                        ret = wc_ecc_verify_hash(sig, sigSz, sigCtx->digest,
                                            sigCtx->digestSz, &sigCtx->verify,
                                            sigCtx->key.ecc);
                    }
                    break;
                }
            #endif /* HAVE_ECC */
            #if defined(HAVE_ED25519) && defined(HAVE_ED25519_VERIFY)
                case ED25519k:
                {
                    ret = wc_ed25519_verify_msg(sig, sigSz, buf, bufSz,
                                          &sigCtx->verify, sigCtx->key.ed25519);
                    break;
                }
            #endif
            #if defined(HAVE_ED448) && defined(HAVE_ED448_VERIFY)
                case ED448k:
                {
                    ret = wc_ed448_verify_msg(sig, sigSz, buf, bufSz,
                                             &sigCtx->verify, sigCtx->key.ed448,
                                             NULL, 0);
                    break;
                }
            #endif
                default:
                    break;
            }  /* switch (keyOID) */

        #ifdef WOLFSSL_ASYNC_CRYPT
            if (ret == WC_PENDING_E) {
                goto exit_cs;
            }
        #endif

            if (ret < 0) {
                /* treat all RSA errors as ASN_SIG_CONFIRM_E */
                ret = ASN_SIG_CONFIRM_E;
                goto exit_cs;
            }

            sigCtx->state = SIG_STATE_CHECK;
        } /* SIG_STATE_DO */
        FALL_THROUGH;

        case SIG_STATE_CHECK:
        {
            switch (keyOID) {
            #ifndef NO_RSA
                case RSAk:
                {
                    int encodedSigSz, verifySz;
                #ifdef WOLFSSL_RENESAS_TSIP
                    if (sigCtx->verifyByTSIP == 1) break;
                #endif
                #ifdef WOLFSSL_SMALL_STACK
                    byte* encodedSig = (byte*)XMALLOC(MAX_ENCODED_SIG_SZ,
                                        sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                    if (encodedSig == NULL) {
                        ERROR_OUT(MEMORY_E, exit_cs);
                    }
                #else
                    byte encodedSig[MAX_ENCODED_SIG_SZ];
                #endif

                    verifySz = ret;

                    /* make sure we're right justified */
                    encodedSigSz = wc_EncodeSignature(encodedSig,
                            sigCtx->digest, sigCtx->digestSz, sigCtx->typeH);
                    if (encodedSigSz == verifySz && sigCtx->out != NULL &&
                        XMEMCMP(sigCtx->out, encodedSig, encodedSigSz) == 0) {
                        ret = 0;
                    }
                    else {
                        WOLFSSL_MSG("RSA SSL verify match encode error");
                        ret = ASN_SIG_CONFIRM_E;
                    }

                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encodedSig, sigCtx->heap, DYNAMIC_TYPE_TMP_BUFFER);
                #endif
                    break;
                }
            #endif /* NO_RSA */
            #if !defined(NO_DSA) && !defined(HAVE_SELFTEST)
                case DSAk:
                {
                    if (sigCtx->verify == 1) {
                        ret = 0;
                    }
                    else {
                        WOLFSSL_MSG("DSA Verify didn't match");
                        ret = ASN_SIG_CONFIRM_E;
                    }
                    break;
                }
            #endif /* !NO_DSA && !HAVE_SELFTEST */
            #ifdef HAVE_ECC
                case ECDSAk:
                {
                    if (sigCtx->verify == 1) {
                        ret = 0;
                    }
                    else {
                        WOLFSSL_MSG("ECC Verify didn't match");
                        ret = ASN_SIG_CONFIRM_E;
                    }
                    break;
                }
            #endif /* HAVE_ECC */
            #ifdef HAVE_ED25519
                case ED25519k:
                {
                    if (sigCtx->verify == 1) {
                        ret = 0;
                    }
                    else {
                        WOLFSSL_MSG("ED25519 Verify didn't match");
                        ret = ASN_SIG_CONFIRM_E;
                    }
                    break;
                }
            #endif /* HAVE_ED25519 */
            #ifdef HAVE_ED448
                case ED448k:
                {
                    if (sigCtx->verify == 1) {
                        ret = 0;
                    }
                    else {
                        WOLFSSL_MSG("ED448 Verify didn't match");
                        ret = ASN_SIG_CONFIRM_E;
                    }
                    break;
                }
            #endif /* HAVE_ED448 */
                default:
                    break;
            }  /* switch (keyOID) */

            break;
        } /* SIG_STATE_CHECK */

        default:
            break;
    } /* switch (sigCtx->state) */

exit_cs:

#endif /* !NO_ASN_CRYPT */

    (void)keyOID;
    (void)sigOID;

    WOLFSSL_LEAVE("ConfirmSignature", ret);

#ifdef WOLFSSL_ASYNC_CRYPT
    if (ret == WC_PENDING_E)
        return ret;
#endif

    FreeSignatureCtx(sigCtx);

    return ret;
}


#ifndef IGNORE_NAME_CONSTRAINTS

static int MatchBaseName(int type, const char* name, int nameSz,
                         const char* base, int baseSz)
{
    if (base == NULL || baseSz <= 0 || name == NULL || nameSz <= 0 ||
            name[0] == '.' || nameSz < baseSz ||
            (type != ASN_RFC822_TYPE && type != ASN_DNS_TYPE))
        return 0;

    /* If an email type, handle special cases where the base is only
     * a domain, or is an email address itself. */
    if (type == ASN_RFC822_TYPE) {
        const char* p = NULL;
        int count = 0;

        if (base[0] != '.') {
            p = base;
            count = 0;

            /* find the '@' in the base */
            while (*p != '@' && count < baseSz) {
                count++;
                p++;
            }

            /* No '@' in base, reset p to NULL */
            if (count >= baseSz)
                p = NULL;
        }

        if (p == NULL) {
            /* Base isn't an email address, it is a domain name,
             * wind the name forward one character past its '@'. */
            p = name;
            count = 0;
            while (*p != '@' && count < baseSz) {
                count++;
                p++;
            }

            if (count < baseSz && *p == '@') {
                name = p + 1;
                nameSz -= count + 1;
            }
        }
    }

    if ((type == ASN_DNS_TYPE || type == ASN_RFC822_TYPE) && base[0] == '.') {
        int szAdjust = nameSz - baseSz;
        name += szAdjust;
        nameSz -= szAdjust;
    }

    while (nameSz > 0) {
        if (XTOLOWER((unsigned char)*name++) !=
                                               XTOLOWER((unsigned char)*base++))
            return 0;
        nameSz--;
    }

    return 1;
}


static int ConfirmNameConstraints(Signer* signer, DecodedCert* cert)
{
    if (signer == NULL || cert == NULL)
        return 0;

    /* Check against the excluded list */
    if (signer->excludedNames) {
        Base_entry* base = signer->excludedNames;

        while (base != NULL) {
            switch (base->type) {
                case ASN_DNS_TYPE:
                {
                    DNS_entry* name = cert->altNames;
                    while (name != NULL) {
                        if (MatchBaseName(ASN_DNS_TYPE,
                                          name->name, name->len,
                                          base->name, base->nameSz)) {
                            return 0;
                        }
                        name = name->next;
                    }
                    break;
                }
                case ASN_RFC822_TYPE:
                {
                    DNS_entry* name = cert->altEmailNames;
                    while (name != NULL) {
                        if (MatchBaseName(ASN_RFC822_TYPE,
                                          name->name, name->len,
                                          base->name, base->nameSz)) {
                            return 0;
                        }
                        name = name->next;
                    }
                    break;
                }
                case ASN_DIR_TYPE:
                {
                    /* allow permitted dirName smaller than actual subject */
                    if (cert->subjectRawLen >= base->nameSz &&
                        XMEMCMP(cert->subjectRaw, base->name,
                                                        base->nameSz) == 0) {
                        return 0;
                    }
                    #ifndef WOLFSSL_NO_ASN_STRICT
                    /* RFC 5280 section 4.2.1.10
                       "Restrictions of the form directoryName MUST be
                        applied to the subject field .... and to any names
                        of type directoryName in the subjectAltName
                        extension"
                    */
                    if (cert->altDirNames != NULL) {
                        DNS_entry* cur = cert->altDirNames;
                        while (cur != NULL) {
                            if (XMEMCMP(cur->name, base->name, base->nameSz)
                                    == 0) {
                                WOLFSSL_MSG("DIR alt name constraint err");
                                return 0;
                            }
                            cur = cur->next;
                        }
                    }
                    #endif /* !WOLFSSL_NO_ASN_STRICT */
                    break;
                }
                default:
                    break;
            }; /* switch */
            base = base->next;
        }
    }

    /* Check against the permitted list */
    if (signer->permittedNames != NULL) {
        int needDns = 0;
        int matchDns = 0;
        int needEmail = 0;
        int matchEmail = 0;
        int needDir = 0;
        int matchDir = 0;
        Base_entry* base = signer->permittedNames;

        while (base != NULL) {
            switch (base->type) {
                case ASN_DNS_TYPE:
                {
                    DNS_entry* name = cert->altNames;

                    if (name != NULL)
                        needDns = 1;

                    while (name != NULL) {
                        matchDns = MatchBaseName(ASN_DNS_TYPE,
                                          name->name, name->len,
                                          base->name, base->nameSz);
                        name = name->next;
                    }
                    break;
                }
                case ASN_RFC822_TYPE:
                {
                    DNS_entry* name = cert->altEmailNames;

                    if (name != NULL)
                        needEmail = 1;

                    while (name != NULL) {
                        matchEmail = MatchBaseName(ASN_DNS_TYPE,
                                          name->name, name->len,
                                          base->name, base->nameSz);
                        name = name->next;
                    }
                    break;
                }
                case ASN_DIR_TYPE:
                {
                    /* allow permitted dirName smaller than actual subject */
                    needDir = 1;
                    if (cert->subjectRaw != NULL &&
                        cert->subjectRawLen >= base->nameSz &&
                        XMEMCMP(cert->subjectRaw, base->name,
                                                        base->nameSz) == 0) {
                        matchDir = 1;

                        #ifndef WOLFSSL_NO_ASN_STRICT
                        /* RFC 5280 section 4.2.1.10
                           "Restrictions of the form directoryName MUST be
                            applied to the subject field .... and to any names
                            of type directoryName in the subjectAltName
                            extension"
                        */
                        if (cert->altDirNames != NULL) {
                            DNS_entry* cur = cert->altDirNames;
                            while (cur != NULL) {
                                if (XMEMCMP(cur->name, base->name, base->nameSz)
                                        != 0) {
                                    WOLFSSL_MSG("DIR alt name constraint err");
                                    matchDir = 0; /* did not match */
                                }
                                cur = cur->next;
                            }
                        }
                        #endif /* !WOLFSSL_NO_ASN_STRICT */
                    }
                    break;
                }
                default:
                    break;
            } /* switch */
            base = base->next;
        }

        if ((needDns   && !matchDns) ||
            (needEmail && !matchEmail) ||
            (needDir   && !matchDir)) {
            return 0;
        }
    }

    return 1;
}

#endif /* IGNORE_NAME_CONSTRAINTS */

#ifdef WOLFSSL_ASN_TEMPLATE
#ifdef WOLFSSL_SEP
/* ASN.1 template for OtherName of an X.509 certificate.
 * X.509: RFC 5280, 4.2.1.6 - OtherName (without implicit outer SEQUENCE).
 * Only support HW Name where the type is a HW serial number.
 */
static const ASNItem otherNameASN[] = {
/*  0 */    { 0, ASN_OBJECT_ID, 0, 0, 0 },
/*  1 */    { 0, ASN_CONTEXT_SPECIFIC | 0, 1, 0, 0 },
/*  2 */        { 1, ASN_SEQUENCE, 1, 0, 0 },
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  4 */            { 2, ASN_OCTET_STRING, 0, 0, 0 }
};

/* Number of items in ASN.1 template for OtherName of an X.509 certificate. */
#define otherNameASN_Length (sizeof(otherNameASN) / sizeof(ASNItem))

/* Decode data with OtherName format from after implicit SEQUENCE.
 *
 * @param [in, out] cert      Certificate object.
 * @param [in]      input     Buffer containing encoded OtherName.
 * @param [in, out] inOutIdx  On in, the index of the start of the OtherName.
 *                            On out, index after OtherName.
 * @param [in]      maxIdx    Maximum index of data in buffer.
 * @return  0 on success.
 * @return  MEMORY_E on dynamic memory allocation failure.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  ASN_PARSE_E when OID does is not HW Name.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  BUFFER_E when data in buffer is too small.
 */
static int DecodeOtherName(DecodedCert* cert, const byte* input,
                           word32* inOutIdx, word32 maxIdx)
{
    DECL_ASNGETDATA(dataASN, otherNameASN_Length);
    int ret = 0;
    word32 oidLen, serialLen;

    CALLOC_ASNGETDATA(dataASN, otherNameASN_Length, ret, cert->heap);

    if (ret == 0) {
        /* Check the first OID is a recognized Alt Cert Name type. */
        GetASN_OID(&dataASN[0], oidCertAltNameType);
        /* Only support HW serial number. */
        GetASN_OID(&dataASN[3], oidIgnoreType);
        /* Parse OtherName. */
        ret = GetASN_Items(otherNameASN, dataASN, otherNameASN_Length, 1, input,
                           inOutIdx, maxIdx);
    }
    if (ret == 0) {
        /* Ensure expected OID. */
        if (dataASN[0].data.oid.sum != HW_NAME_OID) {
            WOLFSSL_MSG("\tincorrect OID");
            ret = ASN_PARSE_E;
        }
    }

    if (ret == 0) {
        oidLen = dataASN[3].data.oid.length;
        serialLen = dataASN[4].data.ref.length;

        /* Allocate space for HW type OID. */
        cert->hwType = (byte*)XMALLOC(oidLen, cert->heap,
                                      DYNAMIC_TYPE_X509_EXT);
        if (cert->hwType == NULL)
            ret = MEMORY_E;
    }
    if (ret == 0) {
        /* Copy, into cert HW type OID */
        XMEMCPY(cert->hwType, dataASN[3].data.oid.data, oidLen);
        cert->hwTypeSz = oidLen;
        /* TODO: check this is the HW serial number OID - no test data. */

        /* Allocate space for HW serial number. */
        cert->hwSerialNum = (byte*)XMALLOC(serialLen, cert->heap,
                                           DYNAMIC_TYPE_X509_EXT);
        if (cert->hwSerialNum == NULL) {
            WOLFSSL_MSG("\tOut of Memory");
            ret = MEMORY_E;
        }
    }
    if (ret == 0) {
        /* Copy into cert HW serial number. */
        XMEMCPY(cert->hwSerialNum, dataASN[4].data.ref.data, serialLen);
        cert->hwSerialNum[serialLen] = '\0';
        cert->hwSerialNumSz = serialLen;
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
}
#endif /* WOLFSSL_SEP */

/* Decode a GeneralName.
 *
 * @param [in]      input     Buffer containing encoded OtherName.
 * @param [in, out] inOutIdx  On in, the index of the start of the OtherName.
 *                            On out, index after OtherName.
 * @param [in]      len       Length of data in buffer.
 * @param [in]      cert      Decoded certificate object.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int DecodeGeneralName(const byte* input, word32* inOutIdx, byte tag,
                             int len, DecodedCert* cert)
{
    int ret = 0;
    word32 idx = *inOutIdx;

    /* GeneralName choice: dnsName */
    if (tag == (ASN_CONTEXT_SPECIFIC | ASN_DNS_TYPE)) {
        ret = SetDNSEntry(cert, (const char*)(input + idx), len, ASN_DNS_TYPE,
                &cert->altNames);
        if (ret == 0) {
            idx += len;
        }
    }
#ifndef IGNORE_NAME_CONSTRAINTS
    /* GeneralName choice: directoryName */
    else if (tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | ASN_DIR_TYPE)) {
        int strLen;
        word32 idxDir = idx;

        /* Expecing a SEQUENCE using up all data. */
        if (GetASN_Sequence(input, &idxDir, &strLen, idx + len, 1) < 0) {
            WOLFSSL_MSG("\tfail: seq length");
            return ASN_PARSE_E;
        }

        ret = SetDNSEntry(cert, (const char*)(input + idxDir), strLen,
                ASN_DIR_TYPE, &cert->altDirNames);
        if (ret == 0) {
            idx += len;
        }
    }
    /* GeneralName choice: rfc822Name */
    else if (tag == (ASN_CONTEXT_SPECIFIC | ASN_RFC822_TYPE)) {
        ret = SetDNSEntry(cert, (const char*)(input + idx), len,
                ASN_RFC822_TYPE, &cert->altEmailNames);
        if (ret == 0) {
            idx += len;
        }
    }
    /* GeneralName choice: uniformResourceIdentifier */
    else if (tag == (ASN_CONTEXT_SPECIFIC | ASN_URI_TYPE)) {
        WOLFSSL_MSG("\tPutting URI into list but not using");

    #ifndef WOLFSSL_NO_ASN_STRICT
        /* Verify RFC 5280 Sec 4.2.1.6 rule:
            "The name MUST NOT be a relative URI" */
        {
            int i;

            /* skip past scheme (i.e http,ftp,...) finding first ':' char */
            for (i = 0; i < len; i++) {
                if (input[idx + i] == ':') {
                    break;
                }
                if (input[idx + i] == '/') {
                    i = len; /* error, found relative path since '/' was
                              * encountered before ':'. Returning error
                              * value in next if statement. */
                }
            }

            /* test if no ':' char was found and test that the next two
             * chars are // to match the pattern "://" */
            if (i >= len - 2 || (input[idx + i + 1] != '/' ||
                                 input[idx + i + 2] != '/')) {
                WOLFSSL_MSG("\tAlt Name must be absolute URI");
                return ASN_ALT_NAME_E;
            }
        }
    #endif

        ret = SetDNSEntry(cert, (const char*)(input + idx), len, ASN_URI_TYPE,
                &cert->altNames);
        if (ret == 0) {
            idx += len;
        }
    }
    #if defined(WOLFSSL_QT) || defined(OPENSSL_ALL) || \
                                            defined(WOLFSSL_IP_ALT_NAME)
    /* GeneralName choice: iPAddress */
    else if (tag == (ASN_CONTEXT_SPECIFIC | ASN_IP_TYPE)) {
        ret = SetDNSEntry(cert, (const char*)(input + idx), len, ASN_IP_TYPE,
                &cert->altNames);
        if (ret == 0) {
            idx += len;
        }
    }
    #endif /* WOLFSSL_QT || OPENSSL_ALL */
#endif /* IGNORE_NAME_CONSTRAINTS */
#ifdef WOLFSSL_SEP
    /* GeneralName choice: otherName */
    else if (tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | ASN_OTHER_TYPE)) {
        /* TODO: test data for code path */
        ret = DecodeOtherName(cert, input, &idx, idx + len);
    }
#endif
    /* GeneralName choice: dNSName, x400Address, ediPartyName,
     *                     registeredID */
    else {
        WOLFSSL_MSG("\tUnsupported name type, skipping");
        idx += len;
    }

    if (ret == 0) {
        /* Return index of next encoded byte. */
        *inOutIdx = idx;
    }
    return ret;
}

/* ASN.1 choices for GeneralName.
 * X.509: RFC 5280, 4.2.1.6 - GeneralName.
 */
static const byte generalNameChoice[] = {
    ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 0,
    ASN_CONTEXT_SPECIFIC                   | 1,
    ASN_CONTEXT_SPECIFIC                   | 2,
    ASN_CONTEXT_SPECIFIC                   | 3,
    ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 4,
    ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 5,
    ASN_CONTEXT_SPECIFIC                   | 6,
    ASN_CONTEXT_SPECIFIC                   | 7,
    ASN_CONTEXT_SPECIFIC                   | 8,
    0
};

/* ASN.1 template for GeneralName.
 * X.509: RFC 5280, 4.2.1.6 - GeneralName.
 */
static const ASNItem altNameASN[] = {
    { 0, ASN_CONTEXT_SPECIFIC | 0, 0, 1, 0 }
};

/* Numbe of items in ASN.1 template for GeneralName. */
#define altNameASN_Length (sizeof(altNameASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* Decode subject alternative names extension.
 *
 * RFC 5280 4.2.1.6.  Subject Alternative Name
 *
 * @param [in]      input  Buffer holding encoded data.
 * @param [in]      sz     Size of encoded data in bytes.
 * @param [in, out] cert   Decoded certificate object.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int DecodeAltNames(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length = 0;

    WOLFSSL_ENTER("DecodeAltNames");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tBad Sequence");
        return ASN_PARSE_E;
    }

    if (length == 0) {
        /* RFC 5280 4.2.1.6.  Subject Alternative Name
           If the subjectAltName extension is present, the sequence MUST
           contain at least one entry. */
        return ASN_PARSE_E;
    }

#ifdef OPENSSL_ALL
    cert->extSubjAltNameSrc = input;
    cert->extSubjAltNameSz = sz;
#endif

    cert->weOwnAltNames = 1;

    while (length > 0) {
        byte b = input[idx++];

        length--;

        /* Save DNS Type names in the altNames list. */
        /* Save Other Type names in the cert's OidMap */
        if (b == (ASN_CONTEXT_SPECIFIC | ASN_DNS_TYPE)) {
            DNS_entry* dnsEntry;
            int strLen;
            word32 lenStartIdx = idx;

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: str length");
                return ASN_PARSE_E;
            }
            length -= (idx - lenStartIdx);

            dnsEntry = (DNS_entry*)XMALLOC(sizeof(DNS_entry), cert->heap,
                                        DYNAMIC_TYPE_ALTNAME);
            if (dnsEntry == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            dnsEntry->type = ASN_DNS_TYPE;
            dnsEntry->name = (char*)XMALLOC(strLen + 1, cert->heap,
                                         DYNAMIC_TYPE_ALTNAME);
            if (dnsEntry->name == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                XFREE(dnsEntry, cert->heap, DYNAMIC_TYPE_ALTNAME);
                return MEMORY_E;
            }
            dnsEntry->len = strLen;
            XMEMCPY(dnsEntry->name, &input[idx], strLen);
            dnsEntry->name[strLen] = '\0';

            dnsEntry->next = cert->altNames;
            cert->altNames = dnsEntry;

            length -= strLen;
            idx    += strLen;
        }
    #ifndef IGNORE_NAME_CONSTRAINTS
        else if (b == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | ASN_DIR_TYPE)) {
            DNS_entry* dirEntry;
            int strLen;
            word32 lenStartIdx = idx;

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: str length");
                return ASN_PARSE_E;
            }

            if (GetSequence(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: seq length");
                return ASN_PARSE_E;
            }
            length -= (idx - lenStartIdx);

            dirEntry = (DNS_entry*)XMALLOC(sizeof(DNS_entry), cert->heap,
                                        DYNAMIC_TYPE_ALTNAME);
            if (dirEntry == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            dirEntry->type = ASN_DIR_TYPE;
            dirEntry->name = (char*)XMALLOC(strLen + 1, cert->heap,
                                         DYNAMIC_TYPE_ALTNAME);
            if (dirEntry->name == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                XFREE(dirEntry, cert->heap, DYNAMIC_TYPE_ALTNAME);
                return MEMORY_E;
            }
            dirEntry->len = strLen;
            XMEMCPY(dirEntry->name, &input[idx], strLen);
            dirEntry->name[strLen] = '\0';

            dirEntry->next = cert->altDirNames;
            cert->altDirNames = dirEntry;

            length -= strLen;
            idx    += strLen;
        }
        else if (b == (ASN_CONTEXT_SPECIFIC | ASN_RFC822_TYPE)) {
            DNS_entry* emailEntry;
            int strLen;
            word32 lenStartIdx = idx;

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: str length");
                return ASN_PARSE_E;
            }
            length -= (idx - lenStartIdx);

            emailEntry = (DNS_entry*)XMALLOC(sizeof(DNS_entry), cert->heap,
                                        DYNAMIC_TYPE_ALTNAME);
            if (emailEntry == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            emailEntry->type = ASN_RFC822_TYPE;
            emailEntry->name = (char*)XMALLOC(strLen + 1, cert->heap,
                                         DYNAMIC_TYPE_ALTNAME);
            if (emailEntry->name == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                XFREE(emailEntry, cert->heap, DYNAMIC_TYPE_ALTNAME);
                return MEMORY_E;
            }
            emailEntry->len = strLen;
            XMEMCPY(emailEntry->name, &input[idx], strLen);
            emailEntry->name[strLen] = '\0';

            emailEntry->next = cert->altEmailNames;
            cert->altEmailNames = emailEntry;

            length -= strLen;
            idx    += strLen;
        }
        else if (b == (ASN_CONTEXT_SPECIFIC | ASN_URI_TYPE)) {
            DNS_entry* uriEntry;
            int strLen;
            word32 lenStartIdx = idx;

            WOLFSSL_MSG("\tPutting URI into list but not using");
            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: str length");
                return ASN_PARSE_E;
            }
            length -= (idx - lenStartIdx);

            /* check that strLen at index is not past input buffer */
            if (strLen + (int)idx > sz) {
                return BUFFER_E;
            }

        #ifndef WOLFSSL_NO_ASN_STRICT
            /* Verify RFC 5280 Sec 4.2.1.6 rule:
                "The name MUST NOT be a relative URI" */

            {
                int i;

                /* skip past scheme (i.e http,ftp,...) finding first ':' char */
                for (i = 0; i < strLen; i++) {
                    if (input[idx + i] == ':') {
                        break;
                    }
                    if (input[idx + i] == '/') {
                        WOLFSSL_MSG("\tAlt Name must be absolute URI");
                        return ASN_ALT_NAME_E;
                    }
                }

                /* test if no ':' char was found and test that the next two
                 * chars are // to match the pattern "://" */
                if (i >= strLen - 2 || (input[idx + i + 1] != '/' ||
                                        input[idx + i + 2] != '/')) {
                    WOLFSSL_MSG("\tAlt Name must be absolute URI");
                    return ASN_ALT_NAME_E;
                }
            }
        #endif

            uriEntry = (DNS_entry*)XMALLOC(sizeof(DNS_entry), cert->heap,
                                        DYNAMIC_TYPE_ALTNAME);
            if (uriEntry == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            uriEntry->type = ASN_URI_TYPE;
            uriEntry->name = (char*)XMALLOC(strLen + 1, cert->heap,
                                         DYNAMIC_TYPE_ALTNAME);
            if (uriEntry->name == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                XFREE(uriEntry, cert->heap, DYNAMIC_TYPE_ALTNAME);
                return MEMORY_E;
            }
            uriEntry->len = strLen;
            XMEMCPY(uriEntry->name, &input[idx], strLen);
            uriEntry->name[strLen] = '\0';

            uriEntry->next = cert->altNames;
            cert->altNames = uriEntry;

            length -= strLen;
            idx    += strLen;
        }
#if defined(WOLFSSL_QT) || defined(OPENSSL_ALL) || defined(WOLFSSL_IP_ALT_NAME)
        else if (b == (ASN_CONTEXT_SPECIFIC | ASN_IP_TYPE)) {
            DNS_entry* ipAddr;
            int strLen;
            word32 lenStartIdx = idx;
            WOLFSSL_MSG("Decoding Subject Alt. Name: IP Address");

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: str length");
                return ASN_PARSE_E;
            }
            length -= (idx - lenStartIdx);
            /* check that strLen at index is not past input buffer */
            if (strLen + (int)idx > sz) {
                return BUFFER_E;
            }

            ipAddr = (DNS_entry*)XMALLOC(sizeof(DNS_entry), cert->heap,
                                        DYNAMIC_TYPE_ALTNAME);
            if (ipAddr == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            ipAddr->type = ASN_IP_TYPE;
            ipAddr->name = (char*)XMALLOC(strLen + 1, cert->heap,
                                         DYNAMIC_TYPE_ALTNAME);
            if (ipAddr->name == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                XFREE(ipAddr, cert->heap, DYNAMIC_TYPE_ALTNAME);
                return MEMORY_E;
            }
            ipAddr->len = strLen;
            XMEMCPY(ipAddr->name, &input[idx], strLen);
            ipAddr->name[strLen] = '\0';

            ipAddr->next   = cert->altNames;
            cert->altNames = ipAddr;

            length -= strLen;
            idx    += strLen;
        }
#endif /* WOLFSSL_QT || OPENSSL_ALL */
#endif /* IGNORE_NAME_CONSTRAINTS */
#ifdef WOLFSSL_SEP
        else if (b == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | ASN_OTHER_TYPE))
        {
            int strLen;
            word32 lenStartIdx = idx;
            word32 oid = 0;
            int    ret;
            byte   tag;

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: other name length");
                return ASN_PARSE_E;
            }
            /* Consume the rest of this sequence. */
            length -= (strLen + idx - lenStartIdx);

            if (GetObjectId(input, &idx, &oid, oidCertAltNameType, sz) < 0) {
                WOLFSSL_MSG("\tbad OID");
                return ASN_PARSE_E;
            }

            if (oid != HW_NAME_OID) {
                WOLFSSL_MSG("\tincorrect OID");
                return ASN_PARSE_E;
            }

            /* Certiciates issued with this OID in the subject alt name are for
             * verifying signatures created on a module.
             * RFC 4108 Section 5. */
            if (cert->hwType != NULL) {
                WOLFSSL_MSG("\tAlready seen Hardware Module Name");
                return ASN_PARSE_E;
            }

            if (GetASNTag(input, &idx, &tag, sz) < 0) {
                return ASN_PARSE_E;
            }

            if (tag != (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED)) {
                WOLFSSL_MSG("\twrong type");
                return ASN_PARSE_E;
            }

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: str len");
                return ASN_PARSE_E;
            }

            if (GetSequence(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tBad Sequence");
                return ASN_PARSE_E;
            }

            ret = GetASNObjectId(input, &idx, &strLen, sz);
            if (ret != 0) {
                WOLFSSL_MSG("\tbad OID");
                return ret;
            }

            cert->hwType = (byte*)XMALLOC(strLen, cert->heap,
                                          DYNAMIC_TYPE_X509_EXT);
            if (cert->hwType == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            XMEMCPY(cert->hwType, &input[idx], strLen);
            cert->hwTypeSz = strLen;
            idx += strLen;

            ret = GetOctetString(input, &idx, &strLen, sz);
            if (ret < 0)
                return ret;

            cert->hwSerialNum = (byte*)XMALLOC(strLen + 1, cert->heap,
                                               DYNAMIC_TYPE_X509_EXT);
            if (cert->hwSerialNum == NULL) {
                WOLFSSL_MSG("\tOut of Memory");
                return MEMORY_E;
            }

            XMEMCPY(cert->hwSerialNum, &input[idx], strLen);
            cert->hwSerialNum[strLen] = '\0';
            cert->hwSerialNumSz = strLen;
            idx += strLen;
        }
    #endif /* WOLFSSL_SEP */
        else {
            int strLen;
            word32 lenStartIdx = idx;

            WOLFSSL_MSG("\tUnsupported name type, skipping");

            if (GetLength(input, &idx, &strLen, sz) < 0) {
                WOLFSSL_MSG("\tfail: unsupported name length");
                return ASN_PARSE_E;
            }
            length -= (strLen + idx - lenStartIdx);
            idx += strLen;
        }
    }

    return 0;
#else
    word32 idx = 0;
    int length = 0;
    int ret = 0;

    WOLFSSL_ENTER("DecodeAltNames");

    /* Get SEQUENCE and expect all data to be accounted for. */
    if (GetASN_Sequence(input, &idx, &length, sz, 1) != 0) {
        WOLFSSL_MSG("\tBad Sequence");
        ret = ASN_PARSE_E;
    }

    if ((ret == 0) && (length == 0)) {
        /* RFC 5280 4.2.1.6.  Subject Alternative Name
           If the subjectAltName extension is present, the sequence MUST
           contain at least one entry. */
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
    #ifdef OPENSSL_ALL
        cert->extSubjAltNameSrc = input;
        cert->extSubjAltNameSz = sz;
    #endif

        cert->weOwnAltNames = 1;

        if (length + (int)idx != sz) {
            ret = ASN_PARSE_E;
        }
    }

    while ((ret == 0) && ((int)idx < sz)) {
        ASNGetData dataASN[altNameASN_Length];

        /* Clear dynamic data items. */
        XMEMSET(dataASN, 0, sizeof(dataASN));
        /* Parse GeneralName with the choices supported. */
        GetASN_Choice(&dataASN[0], generalNameChoice);
        /* Decode a GeneralName choice. */
        ret = GetASN_Items(altNameASN, dataASN, altNameASN_Length, 0, input,
                           &idx, sz);
        if (ret == 0) {
            ret = DecodeGeneralName(input, &idx, dataASN[0].tag,
                dataASN[0].length, cert);
        }
    }

    return ret;
#endif
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for BasicContraints.
 * X.509: RFC 5280, 4.2.1.9 - BasicConstraints.
 */
static const ASNItem basicConsASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_BOOLEAN, 0, 0, 1 },
/*  2 */        { 1, ASN_INTEGER, 0, 0, 1 }
};

/* Number of items in ASN.1 template for BasicContraints. */
#define basicConsASN_Length (sizeof(basicConsASN) / sizeof(ASNItem))
#endif

/* Decode basic constraints extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.1.9 - BasicConstraints.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  MEMORY_E on dynamic memory allocation failure.
 * @return  ASN_PARSE_E when CA boolean is present and false (default is false).
 * @return  ASN_PARSE_E when CA boolean is not present unless
 *          WOLFSSL_X509_BASICCONS_INT is defined. Only a CA extension.
 * @return  ASN_PARSE_E when path legth more than 7 bits.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 */
static int DecodeBasicCaConstraint(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length = 0;
    int ret;

    WOLFSSL_ENTER("DecodeBasicCaConstraint");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: bad SEQUENCE");
        return ASN_PARSE_E;
    }

    if (length == 0)
        return 0;

    /* If the basic ca constraint is false, this extension may be named, but
     * left empty. So, if the length is 0, just return. */

    ret = GetBoolean(input, &idx, sz);

    /* Removed logic for WOLFSSL_X509_BASICCONS_INT which was mistreating the
     * pathlen value as if it were the CA Boolean value 7/2/2021 - KH.
     * When CA Boolean not asserted use the default value "False" */
    if (ret < 0) {
        WOLFSSL_MSG("\tfail: constraint not valid BOOLEAN, set default FALSE");
        ret = 0;
    }

    cert->isCA = (byte)ret;

    /* If there isn't any more data, return. */
    if (idx >= (word32)sz) {
        return 0;
    }

    ret = GetInteger7Bit(input, &idx, sz);
    if (ret < 0)
        return ret;
    cert->pathLength = (byte)ret;
    cert->pathLengthSet = 1;

    return 0;
#else
    DECL_ASNGETDATA(dataASN, basicConsASN_Length);
    int ret = 0;
    word32 idx = 0;
    byte isCA = 0;

    WOLFSSL_ENTER("DecodeBasicCaConstraints");

    CALLOC_ASNGETDATA(dataASN, basicConsASN_Length, ret, cert->heap);

    if (ret == 0) {
        /* Get the CA boolean and path length when present. */
        GetASN_Boolean(&dataASN[1], &isCA);
        GetASN_Int8Bit(&dataASN[2], &cert->pathLength);

        ret = GetASN_Items(basicConsASN, dataASN, basicConsASN_Length, 1, input,
                           &idx, sz);
    }

    /* Empty SEQUENCE is OK - nothing to store. */
    if ((ret == 0) && (dataASN[0].length != 0)) {
        /* Bad encoding when CA Boolean is false
         * (default when not present). */
        if ((dataASN[1].length != 0) && (!isCA)) {
            ret = ASN_PARSE_E;
        }
        /* Path length must be a 7-bit value. */
        if ((ret == 0) && (cert->pathLength >= (1 << 7))) {
            ret = ASN_PARSE_E;
        }
        /* Store CA boolean and whether a path length was seen. */
        if (ret == 0) {
            /* isCA in certificate is a 1 bit of a byte. */
            cert->isCA = isCA;
            cert->pathLengthSet = (dataASN[2].length > 0);
        }
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
#endif
}


static int DecodePolicyConstraints(const byte* input, int sz, DecodedCert* cert)
{
    word32 idx = 0;
    int length = 0;
    int skipLength = 0;
    int ret;
    byte tag;

    WOLFSSL_ENTER("DecodePolicyConstraints");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: bad SEQUENCE");
        return ASN_PARSE_E;
    }

    if (length == 0)
        return ASN_PARSE_E;

    if (GetASNTag(input, &idx, &tag, sz) < 0) {
        WOLFSSL_MSG("\tfail: bad TAG");
        return ASN_PARSE_E;
    }

    if (tag == (ASN_CONTEXT_SPECIFIC | 0)) {
        /* requireExplicitPolicy */
        cert->extPolicyConstRxpSet = 1;
    }
    else if (tag == (ASN_CONTEXT_SPECIFIC | 1)) {
        /* inhibitPolicyMapping */
        cert->extPolicyConstIpmSet = 1;
    }
    else {
        WOLFSSL_MSG("\tfail: invalid TAG");
        return ASN_PARSE_E;
    }

    ret = GetLength(input, &idx, &skipLength, sz);
    if (ret < 0) {
        WOLFSSL_MSG("\tfail: invalid length");
        return ret;
    }
    if (skipLength > 1) {
        WOLFSSL_MSG("\tfail: skip value too big");
        return BUFFER_E;
    }
    cert->policyConstSkip = input[idx];

    return 0;
}


/* Context-Specific value for: DistributionPoint.distributionPoint
 * From RFC5280 SS4.2.1.13, Distribution Point */
#define DISTRIBUTION_POINT  (ASN_CONTEXT_SPECIFIC | 0)
/* Context-Specific value for: DistributionPoint.DistributionPointName.fullName
 *  From RFC3280 SS4.2.1.13, Distribution Point Name */
#define CRLDP_FULL_NAME     (ASN_CONTEXT_SPECIFIC | 0)
/* Context-Specific value for choice: GeneralName.uniformResourceIdentifier
 * From RFC3280 SS4.2.1.7, GeneralName */
#define GENERALNAME_URI     (ASN_CONTEXT_SPECIFIC | 6)

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for CRL distribution points.
 * X.509: RFC 5280, 4.2.1.13 - CRL Distribution Points.
 */
static const ASNItem crlDistASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* Distribution point name */
/*  2 */            { 2, DISTRIBUTION_POINT, 1, 1, 1 },
                        /* fullName */
/*  3 */                { 3, CRLDP_FULL_NAME, 1, 1, 2 },
/*  4 */                    { 4, GENERALNAME_URI, 0, 0, 0 },
                        /* nameRelativeToCRLIssuer */
/*  5 */                { 3, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 2 },
                    /* reasons: IMPLICIT BIT STRING */
/*  6 */            { 2, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 1 },
                    /* cRLIssuer */
/*  7 */            { 2, ASN_CONTEXT_SPECIFIC | 2, 1, 0, 1 },
};

/* Number of items in ASN.1 template for CRL distribution points. */
#define crlDistASN_Length (sizeof(crlDistASN) / sizeof(ASNItem))
#endif

/* Decode CRL distribution point extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.1.13 - CRL Distribution Points.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  MEMORY_E on dynamic memory allocation failure.
 * @return  ASN_PARSE_E when invalid bits of reason are set.
 * @return  ASN_PARSE_E when BITSTRING value is more than 2 bytes.
 * @return  ASN_PARSE_E when unused bits of BITSTRING is invalid.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 */
static int DecodeCrlDist(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0, localIdx;
    int length = 0;
    byte tag   = 0;

    WOLFSSL_ENTER("DecodeCrlDist");

    /* Unwrap the list of Distribution Points*/
    if (GetSequence(input, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    /* Unwrap a single Distribution Point */
    if (GetSequence(input, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    /* The Distribution Point has three explicit optional members
     *  First check for a DistributionPointName
     */
    localIdx = idx;
    if (GetASNTag(input, &localIdx, &tag, sz) == 0 &&
            tag == (ASN_CONSTRUCTED | DISTRIBUTION_POINT))
    {
        idx++;
        if (GetLength(input, &idx, &length, sz) < 0)
            return ASN_PARSE_E;

        localIdx = idx;
        if (GetASNTag(input, &localIdx, &tag, sz) == 0 &&
                tag == (ASN_CONSTRUCTED | CRLDP_FULL_NAME))
        {
            idx++;
            if (GetLength(input, &idx, &length, sz) < 0)
                return ASN_PARSE_E;

            localIdx = idx;
            if (GetASNTag(input, &localIdx, &tag, sz) == 0 &&
                    tag == GENERALNAME_URI)
            {
                idx++;
                if (GetLength(input, &idx, &length, sz) < 0)
                    return ASN_PARSE_E;

                cert->extCrlInfoSz = length;
                cert->extCrlInfo = input + idx;
                idx += length;
            }
            else
                /* This isn't a URI, skip it. */
                idx += length;
        }
        else {
            /* This isn't a FULLNAME, skip it. */
            idx += length;
        }
    }

    /* Check for reasonFlags */
    localIdx = idx;
    if (idx < (word32)sz &&
        GetASNTag(input, &localIdx, &tag, sz) == 0 &&
        tag == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1))
    {
        idx++;
        if (GetLength(input, &idx, &length, sz) < 0)
            return ASN_PARSE_E;
        idx += length;
    }

    /* Check for cRLIssuer */
    localIdx = idx;
    if (idx < (word32)sz &&
        GetASNTag(input, &localIdx, &tag, sz) == 0 &&
        tag == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 2))
    {
        idx++;
        if (GetLength(input, &idx, &length, sz) < 0)
            return ASN_PARSE_E;
        idx += length;
    }

    if (idx < (word32)sz)
    {
        WOLFSSL_MSG("\tThere are more CRL Distribution Point records, "
                   "but we only use the first one.");
    }

    return 0;
#else
    DECL_ASNGETDATA(dataASN, crlDistASN_Length);
    word32 idx = 0;
    int ret = 0;
#ifdef CRLDP_VALIDATE_DATA
    word16 reason;
#endif

    WOLFSSL_ENTER("DecodeCrlDist");

    CALLOC_ASNGETDATA(dataASN, crlDistASN_Length, ret, cert->heap);

    if  (ret == 0) {
        /* Get the GeneralName choice */
        GetASN_Choice(&dataASN[4], generalNameChoice);
        /* Parse CRL distribtion point. */
        ret = GetASN_Items(crlDistASN, dataASN, crlDistASN_Length, 0, input,
                           &idx, sz);
    }
    if (ret == 0) {
        /* If the choice was a URI, store it in certificate. */
        if (dataASN[4].tag == GENERALNAME_URI) {
            word32 sz32;
            GetASN_GetConstRef(&dataASN[4], &cert->extCrlInfo, &sz32);
            cert->extCrlInfoSz = sz32;
        }

    #ifdef CRLDP_VALIDATE_DATA
        if (dataASN[6].data.ref.data != NULL) {
             /* TODO: test case */
             /* Validate ReasonFlags. */
             ret = GetASN_BitString_Int16Bit(&dataASN[6], &reason);
             /* First bit (LSB) unused and eight other bits defined. */
             if ((ret == 0) && ((reason >> 9) || (reason & 0x01))) {
                ret = ASN_PARSE_E;
             }
        }
    #endif
    }

    /* Only parsing the first one. */
    if (ret == 0 && idx < (word32)sz) {
        WOLFSSL_MSG("\tThere are more CRL Distribution Point records, "
                    "but we only use the first one.");
    }
    /* TODO: validate other points. */

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for the access description.
 * X.509: RFC 5280, 4.2.2.1 - Authority Information Access.
 */
static const ASNItem accessDescASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* accessMethod */
/*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
                /* accessLocation: GeneralName */
/*  2 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 0 },
};

/* Number of items in ASN.1 template for the access description. */
#define accessDescASN_Length (sizeof(accessDescASN) / sizeof(ASNItem))
#endif

/* Decode authority information access extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.2.1 - Authority Information Access.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  MEMORY_E on dynamic memory allocation failure.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
static int DecodeAuthInfo(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length = 0;
    int count  = 0;
    byte b = 0;
    word32 oid;

    WOLFSSL_ENTER("DecodeAuthInfo");

    /* Unwrap the list of AIAs */
    if (GetSequence(input, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    while ((idx < (word32)sz) && (count < MAX_AIA_SZ)) {
        /* Unwrap a single AIA */
        if (GetSequence(input, &idx, &length, sz) < 0)
            return ASN_PARSE_E;

        oid = 0;
        if (GetObjectId(input, &idx, &oid, oidCertAuthInfoType, sz) < 0) {
            return ASN_PARSE_E;
        }

        /* Only supporting URIs right now. */
        if (GetASNTag(input, &idx, &b, sz) < 0)
            return ASN_PARSE_E;

        if (GetLength(input, &idx, &length, sz) < 0)
            return ASN_PARSE_E;

        /* Set ocsp entry */
        if (b == GENERALNAME_URI && oid == AIA_OCSP_OID)
        {
            cert->extAuthInfoSz = length;
            cert->extAuthInfo = input + idx;
            count++;
        #if !defined(OPENSSL_ALL) || !defined(WOLFSSL_QT)
            break;
        #endif
        }
        #if defined(OPENSSL_ALL) || defined(WOLFSSL_QT)
        /* Set CaIssuers entry */
        else if ((b == GENERALNAME_URI) && oid == AIA_CA_ISSUER_OID)
        {
            cert->extAuthInfoCaIssuerSz = length;
            cert->extAuthInfoCaIssuer = input + idx;
            count++;
        }
        #endif
        idx += length;
    }

    return 0;
#else
    word32 idx = 0;
    int length = 0;
    int count  = 0;
    int ret    = 0;

    WOLFSSL_ENTER("DecodeAuthInfo");

    /* Unwrap the list of AIAs */
    if (GetASN_Sequence(input, &idx, &length, sz, 1) < 0) {
        ret = ASN_PARSE_E;
    }

    while ((ret == 0) && (idx < (word32)sz) && (count < MAX_AIA_SZ)) {
        ASNGetData dataASN[accessDescASN_Length];
        word32 sz32;

        /* Clear dynamic data and retrieve OID and name. */
        XMEMSET(dataASN, 0, sizeof(dataASN));
        GetASN_OID(&dataASN[1], oidCertAuthInfoType);
        GetASN_Choice(&dataASN[2], generalNameChoice);
        /* Parse AccessDescription. */
        ret = GetASN_Items(accessDescASN, dataASN, accessDescASN_Length, 0,
                           input, &idx, sz);
        if (ret == 0) {
            /* Check we have OCSP and URI. */
            if ((dataASN[1].data.oid.sum == AIA_OCSP_OID) &&
                    (dataASN[2].tag == GENERALNAME_URI)) {
                /* Store URI for OCSP lookup. */
                GetASN_GetConstRef(&dataASN[2], &cert->extAuthInfo, &sz32);
                cert->extAuthInfoSz = sz32;
                count++;
            #if !defined(OPENSSL_ALL) || !defined(WOLFSSL_QT)
                break;
            #endif
            }
            #if defined(OPENSSL_ALL) || defined(WOLFSSL_QT)
            /* Check we have CA Issuer and URI. */
            else if ((dataASN[1].data.oid.sum == AIA_CA_ISSUER_OID) &&
                    (dataASN[2].tag == GENERALNAME_URI)) {
                /* Set CaIssuers entry */
                GetASN_GetConstRef(&dataASN[2], &cert->extAuthInfoCaIssuer,
                                   &sz32);
                cert->extAuthInfoCaIssuerSz = sz32;
                count++;
            }
            #endif
            /* Otherwise skip. */
        }
    }

    return ret;
#endif
}


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for AuthorityKeyIdentifier.
 * X.509: RFC 5280, 4.2.1.1 - Authority Key Identifier.
 */
static const ASNItem authKeyIdASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* keyIdentifier */
/*  1 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 1 },
                /* authorityCertIssuer */
/*  2 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 1 },
                /* authorityCertSerialNumber */
/*  3 */        { 1, ASN_CONTEXT_SPECIFIC | 2, 0, 0, 1 },
};

/* Number of items in ASN.1 template for AuthorityKeyIdentifier. */
#define authKeyIdASN_Length (sizeof(authKeyIdASN) / sizeof(ASNItem))
#endif

/* Decode authority information access extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.2.1 - Authority Information Access.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  MEMORY_E on dynamic memory allocation failure.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 */
static int DecodeAuthKeyId(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length = 0;
    byte tag;

    WOLFSSL_ENTER("DecodeAuthKeyId");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: should be a SEQUENCE\n");
        return ASN_PARSE_E;
    }

    if (GetASNTag(input, &idx, &tag, sz) < 0) {
        return ASN_PARSE_E;
    }

    if (tag != (ASN_CONTEXT_SPECIFIC | 0)) {
        WOLFSSL_MSG("\tinfo: OPTIONAL item 0, not available\n");
        cert->extAuthKeyIdSet = 0;
        return 0;
    }

    if (GetLength(input, &idx, &length, sz) <= 0) {
        WOLFSSL_MSG("\tfail: extension data length");
        return ASN_PARSE_E;
    }

#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
    cert->extAuthKeyIdSrc = &input[idx];
    cert->extAuthKeyIdSz = length;
#endif /* OPENSSL_EXTRA */

    return GetHashId(input + idx, length, cert->extAuthKeyId);
#else
    DECL_ASNGETDATA(dataASN, authKeyIdASN_Length);
    int ret = 0;
    word32 idx = 0;

    WOLFSSL_ENTER("DecodeAuthKeyId");

    CALLOC_ASNGETDATA(dataASN, authKeyIdASN_Length, ret, cert->heap);

    if (ret == 0) {
        /* Parse an authority key identifier. */
        ret = GetASN_Items(authKeyIdASN, dataASN, authKeyIdASN_Length, 1, input,
                           &idx, sz);
    }
    if (ret == 0) {
        /* Key id is optional. */
        if (dataASN[1].data.ref.data == NULL) {
            WOLFSSL_MSG("\tinfo: OPTIONAL item 0, not available");
        }
        else {
#ifdef OPENSSL_EXTRA
            /* Store the autority key id. */
            GetASN_GetConstRef(&dataASN[1], &cert->extAuthKeyIdSrc,
                               &cert->extAuthKeyIdSz);
#endif /* OPENSSL_EXTRA */

            /* Get the hash or hash of the hash if wrong size. */
            ret = GetHashId(dataASN[1].data.ref.data,
                        dataASN[1].data.ref.length, cert->extAuthKeyId);
        }
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* Decode subject key id extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.2.1 - Authority Information Access.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  ASN_PARSE_E when the OCTET_STRING tag is not found or length is
 *          invalid.
 * @return  MEMORY_E on dynamic memory allocation failure.
 */
static int DecodeSubjKeyId(const byte* input, int sz, DecodedCert* cert)
{
    word32 idx = 0;
    int length = 0;
    int ret = 0;

    WOLFSSL_ENTER("DecodeSubjKeyId");

    if (sz <= 0) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        ret = GetOctetString(input, &idx, &length, sz);
    }
    if (ret > 0) {
    #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
        cert->extSubjKeyIdSrc = &input[idx];
        cert->extSubjKeyIdSz = length;
    #endif /* OPENSSL_EXTRA */

        /* Get the hash or hash of the hash if wrong size. */
        ret = GetHashId(input + idx, length, cert->extSubjKeyId);
    }

    return ret;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for KeyUsage.
 * X.509: RFC 5280, 4.2.1.3 - Key Usage.
 */
static const ASNItem keyUsageASN[] = {
/*  0 */    { 0, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for KeyUsage. */
#define keyUsageASN_Length (sizeof(keyUsageASN) / sizeof(ASNItem))
#endif

/* Decode key usage extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.2.1 - Authority Information Access.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  MEMORY_E on dynamic memory allocation failure.
 */
static int DecodeKeyUsage(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length;
    int ret;
    WOLFSSL_ENTER("DecodeKeyUsage");

    ret = CheckBitString(input, &idx, &length, sz, 0, NULL);
    if (ret != 0)
        return ret;

    cert->extKeyUsage = (word16)(input[idx]);
    if (length == 2)
        cert->extKeyUsage |= (word16)(input[idx+1] << 8);

    return 0;
#else
    ASNGetData dataASN[keyUsageASN_Length];
    word32 idx = 0;
    WOLFSSL_ENTER("DecodeKeyUsage");

    /* Clear dynamic data and set where to store extended key usage. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_Int16Bit(&dataASN[0], &cert->extKeyUsage);
    /* Parse key usage. */
    return GetASN_Items(keyUsageASN, dataASN, keyUsageASN_Length, 0, input,
                        &idx, sz);
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for KeyPurposeId.
 * X.509: RFC 5280, 4.2.1.12 - Extended Key Usage.
 */
static const ASNItem keyPurposeIdASN[] = {
/*  0 */    { 0, ASN_OBJECT_ID, 0, 0, 0 },
};

/* Number of items in ASN.1 template for KeyPurposeId. */
#define keyPurposeIdASN_Length (sizeof(keyPurposeIdASN) / sizeof(ASNItem))
#endif

/* Decode extended key usage extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.1.12 - Extended Key Usage.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  MEMORY_E on dynamic memory allocation failure.
 */
static int DecodeExtKeyUsage(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0, oid;
    int length, ret;

    WOLFSSL_MSG("DecodeExtKeyUsage");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: should be a SEQUENCE");
        return ASN_PARSE_E;
    }

#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
    cert->extExtKeyUsageSrc = input + idx;
    cert->extExtKeyUsageSz = length;
#endif

    while (idx < (word32)sz) {
        ret = GetObjectId(input, &idx, &oid, oidCertKeyUseType, sz);
        if (ret == ASN_UNKNOWN_OID_E)
            continue;
        else if (ret < 0)
            return ret;

        switch (oid) {
            case EKU_ANY_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_ANY;
                break;
            case EKU_SERVER_AUTH_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_SERVER_AUTH;
                break;
            case EKU_CLIENT_AUTH_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_CLIENT_AUTH;
                break;
            case EKU_CODESIGNING_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_CODESIGN;
                break;
            case EKU_EMAILPROTECT_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_EMAILPROT;
                break;
            case EKU_TIMESTAMP_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_TIMESTAMP;
                break;
            case EKU_OCSP_SIGN_OID:
                cert->extExtKeyUsage |= EXTKEYUSE_OCSP_SIGN;
                break;
            default:
                break;
        }

    #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
        cert->extExtKeyUsageCount++;
    #endif
    }

    return 0;
#else
    word32 idx = 0;
    int length;
    int ret = 0;

    WOLFSSL_MSG("DecodeExtKeyUsage");

    /* Strip SEQUENCE OF and expect to account for all the data. */
    if (GetASN_Sequence(input, &idx, &length, sz, 1) < 0) {
        WOLFSSL_MSG("\tfail: should be a SEQUENCE");
        ret = ASN_PARSE_E;
    }

    if (ret == 0) {
    #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
        /* Keep reference for WOFLSSL_X509. */
        cert->extExtKeyUsageSrc = input + idx;
        cert->extExtKeyUsageSz = length;
    #endif
    }

    /* Check all OIDs. */
    while ((ret == 0) && (idx < (word32)sz)) {
        ASNGetData dataASN[keyPurposeIdASN_Length];

        /* Clear dynamic data items and set OID type expected. */
        XMEMSET(dataASN, 0, sizeof(dataASN));
        GetASN_OID(&dataASN[0], oidCertKeyUseType);
        /* Decode KeyPurposeId. */
        ret = GetASN_Items(keyPurposeIdASN, dataASN, keyPurposeIdASN_Length, 0,
                           input, &idx, sz);
        /* Skip unknown OIDs. */
        if (ret == ASN_UNKNOWN_OID_E) {
            ret = 0;
        }
        else if (ret == 0) {
            /* Store the bit for the OID. */
            switch (dataASN[0].data.oid.sum) {
                case EKU_ANY_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_ANY;
                    break;
                case EKU_SERVER_AUTH_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_SERVER_AUTH;
                    break;
                case EKU_CLIENT_AUTH_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_CLIENT_AUTH;
                    break;
                case EKU_CODESIGNING_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_CODESIGN;
                    break;
                case EKU_EMAILPROTECT_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_EMAILPROT;
                    break;
                case EKU_TIMESTAMP_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_TIMESTAMP;
                    break;
                case EKU_OCSP_SIGN_OID:
                    cert->extExtKeyUsage |= EXTKEYUSE_OCSP_SIGN;
                    break;
            }

        #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
            /* Keep count for WOFLSSL_X509. */
            cert->extExtKeyUsageCount++;
        #endif
        }
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


#ifndef IGNORE_NAME_CONSTRAINTS
#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for GeneralSubtree.
 * X.509: RFC 5280, 4.2.1.10 - Name Constraints.
 */
static const ASNItem subTreeASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* base     GeneralName */
/*  1 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 0 },
                /* minimum  BaseDistance DEFAULT 0*/
/*  2 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 1 },
                /* maximum  BaseDistance OPTIONAL  */
/*  3 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 0, 0, 1 },
};

/* Number of items in ASN.1 template for GeneralSubtree. */
#define subTreeASN_Length (sizeof(subTreeASN) / sizeof(ASNItem))
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
/* Decode the Subtree's GeneralName.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in]      tag    BER tag on GeneralName.
 * @param [in, out] head   Linked list of subtree names.
 * @param [in]      heap   Dynamic memory hint.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_PARSE_E when SEQUENCE is not found as expected.
 */
static int DecodeSubtreeGeneralName(const byte* input, int sz, byte tag,
                                    Base_entry** head, void* heap)
{
    Base_entry* entry;
    word32 nameIdx = 0;
    word32 len = sz;
    int strLen;
    int ret = 0;

    (void)heap;

    /* if constructed has leading sequence */
    if ((tag & ASN_CONSTRUCTED) == ASN_CONSTRUCTED) {
        ret = GetASN_Sequence(input, &nameIdx, &strLen, sz, 0);
        if (ret < 0) {
            ret = ASN_PARSE_E;
        }
        else {
            len = strLen;
            ret = 0;
        }
    }
    if (ret == 0) {
        /* TODO: consider one malloc. */
        /* Allocate Base Entry object. */
        entry = (Base_entry*)XMALLOC(sizeof(Base_entry), heap,
                                     DYNAMIC_TYPE_ALTNAME);
        if (entry == NULL) {
            ret = MEMORY_E;
        }
    }
    if (ret == 0) {
        /* Allocate name. */
        entry->name = (char*)XMALLOC(len, heap, DYNAMIC_TYPE_ALTNAME);
        if (entry->name == NULL) {
            XFREE(entry, heap, DYNAMIC_TYPE_ALTNAME);
            ret = MEMORY_E;
        }
    }
    if (ret == 0) {
        /* Store name, size and tag in object. */
        XMEMCPY(entry->name, &input[nameIdx], len);
        entry->nameSz = len;
        entry->type = tag & ASN_TYPE_MASK;

        /* Put entry at front of linked list. */
        entry->next = *head;
        *head = entry;
    }

    return ret;
}
#endif

/* Decode a subtree of a name contraint in a certificate.
 *
 * X.509: RFC 5280, 4.2.1.10 - Name Contraints.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] head   Linked list of subtree names.
 * @param [in]      heap   Dynamic memory hint.
 * @return  0 on success.
 * @return  MEMORY_E when dynamic memory allocation fails.
 * @return  ASN_PARSE_E when SEQUENCE is not found as expected.
 */
static int DecodeSubtree(const byte* input, int sz, Base_entry** head,
                         void* heap)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int ret = 0;

    (void)heap;

    while (idx < (word32)sz) {
        int seqLength, strLength;
        word32 nameIdx;
        byte b, bType;

        if (GetSequence(input, &idx, &seqLength, sz) < 0) {
            WOLFSSL_MSG("\tfail: should be a SEQUENCE");
            return ASN_PARSE_E;
        }
        nameIdx = idx;
        b = input[nameIdx++];

        if (GetLength(input, &nameIdx, &strLength, sz) <= 0) {
            WOLFSSL_MSG("\tinvalid length");
            return ASN_PARSE_E;
        }

        /* Get type, LSB 4-bits */
        bType = (b & ASN_TYPE_MASK);

        if (bType == ASN_DNS_TYPE || bType == ASN_RFC822_TYPE ||
                                                        bType == ASN_DIR_TYPE) {
            Base_entry* entry;

            /* if constructed has leading sequence */
            if (b & ASN_CONSTRUCTED) {
                if (GetSequence(input, &nameIdx, &strLength, sz) < 0) {
                    WOLFSSL_MSG("\tfail: constructed be a SEQUENCE");
                    return ASN_PARSE_E;
                }
            }

            entry = (Base_entry*)XMALLOC(sizeof(Base_entry), heap,
                                                          DYNAMIC_TYPE_ALTNAME);
            if (entry == NULL) {
                WOLFSSL_MSG("allocate error");
                return MEMORY_E;
            }

            entry->name = (char*)XMALLOC(strLength, heap, DYNAMIC_TYPE_ALTNAME);
            if (entry->name == NULL) {
                WOLFSSL_MSG("allocate error");
                XFREE(entry, heap, DYNAMIC_TYPE_ALTNAME);
                return MEMORY_E;
            }

            XMEMCPY(entry->name, &input[nameIdx], strLength);
            entry->nameSz = strLength;
            entry->type = bType;

            entry->next = *head;
            *head = entry;
        }

        idx += seqLength;
    }

    return ret;
#else
    DECL_ASNGETDATA(dataASN, subTreeASN_Length);
    word32 idx = 0;
    int ret = 0;

    (void)heap;

    ALLOC_ASNGETDATA(dataASN, subTreeASN_Length, ret, heap);

    /* Process all subtrees. */
    while ((ret == 0) && (idx < (word32)sz)) {
        byte min = 0;
        byte max = 0;

        /* Clear dynamic data and set choice for GeneralName and location to
         * store minimum and maximum.
         */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * subTreeASN_Length);
        GetASN_Choice(&dataASN[1], generalNameChoice);
        GetASN_Int8Bit(&dataASN[2], &min);
        GetASN_Int8Bit(&dataASN[3], &max);
        /* Parse GeneralSubtree. */
        ret = GetASN_Items(subTreeASN, dataASN, subTreeASN_Length, 0, input,
                           &idx, sz);
        if (ret == 0) {
            byte t = dataASN[1].tag;

            /* Check GeneralName tag is one of the types we can handle. */
            if (t == (ASN_CONTEXT_SPECIFIC | ASN_DNS_TYPE) ||
                t == (ASN_CONTEXT_SPECIFIC | ASN_RFC822_TYPE) ||
                t == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | ASN_DIR_TYPE)) {
                /* Parse the general name and store a new entry. */
                ret = DecodeSubtreeGeneralName(input +
                    GetASNItem_DataIdx(dataASN[1], input),
                    GetASNItem_EndIdx(dataASN[1], input), t, head, heap);
            }
            /* Skip entry. */
        }
    }

    FREE_ASNGETDATA(dataASN, heap);
    return ret;
#endif
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for NameConstraints.
 * X.509: RFC 5280, 4.2.1.10 - Name Contraints.
 */
static const ASNItem nameConstraintsASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* permittedSubtrees */
/*  1 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 0, 1 },
                /* excludededSubtrees */
/*  2 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 1 },
};

/* Number of items in ASN.1 template for NameConstraints. */
#define nameConstraintsASN_Length (sizeof(nameConstraintsASN) / sizeof(ASNItem))
#endif

/* Decode name constraints extension in a certificate.
 *
 * X.509: RFC 5280, 4.2.1.10 - Name Constraints.
 *
 * @param [in]      input  Buffer holding data.
 * @param [in]      sz     Size of data in buffer.
 * @param [in, out] cert   Certificate object.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  MEMORY_E on dynamic memory allocation failure.
 */
static int DecodeNameConstraints(const byte* input, int sz, DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length = 0;

    WOLFSSL_ENTER("DecodeNameConstraints");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: should be a SEQUENCE");
        return ASN_PARSE_E;
    }

    while (idx < (word32)sz) {
        byte b = input[idx++];
        Base_entry** subtree = NULL;

        if (GetLength(input, &idx, &length, sz) <= 0) {
            WOLFSSL_MSG("\tinvalid length");
            return ASN_PARSE_E;
        }

        if (b == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 0))
            subtree = &cert->permittedNames;
        else if (b == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 1))
            subtree = &cert->excludedNames;
        else {
            WOLFSSL_MSG("\tinvalid subtree");
            return ASN_PARSE_E;
        }

        if (DecodeSubtree(input + idx, length, subtree, cert->heap) < 0) {
            WOLFSSL_MSG("\terror parsing subtree");
            return ASN_PARSE_E;
        }

        idx += length;
    }

    return 0;
#else
    ASNGetData dataASN[nameConstraintsASN_Length];
    word32 idx = 0;
    int    ret = 0;

    /* Clear dynamic data. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    /* Parse NameConstraints. */
    ret = GetASN_Items(nameConstraintsASN, dataASN, nameConstraintsASN_Length,
                       1, input, &idx, sz);
    if (ret == 0) {
        /* If there was a permittedSubtrees then parse it. */
        if (dataASN[1].data.ref.data != NULL) {
            ret = DecodeSubtree(dataASN[1].data.ref.data,
                dataASN[1].data.ref.length, &cert->permittedNames, cert->heap);
        }
    }
    if (ret == 0) {
        /* If there was a excludedSubtrees then parse it. */
        if (dataASN[2].data.ref.data != NULL) {
            ret = DecodeSubtree(dataASN[2].data.ref.data,
                dataASN[2].data.ref.length, &cert->excludedNames, cert->heap);
        }
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* IGNORE_NAME_CONSTRAINTS */

#if (defined(WOLFSSL_CERT_EXT) && !defined(WOLFSSL_SEP)) || \
    defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)

/* Decode ITU-T X.690 OID format to a string representation
 * return string length */
int DecodePolicyOID(char *out, word32 outSz, const byte *in, word32 inSz)
{
    word32 val, inIdx = 0, outIdx = 0;
    int w = 0;

    if (out == NULL || in == NULL || outSz < 4 || inSz < 2)
        return BAD_FUNC_ARG;

    /* The first byte expands into b/40 dot b%40. */
    val = in[inIdx++];

    w = XSNPRINTF(out, outSz, "%u.%u", val / 40, val % 40);
    if (w < 0) {
        w = BUFFER_E;
        goto exit;
    }
    outIdx += w;
    val = 0;

    while (inIdx < inSz && outIdx < outSz) {
        /* extract the next OID digit from in to val */
        /* first bit is used to set if value is coded on 1 or multiple bytes */
        if (in[inIdx] & 0x80) {
            val += in[inIdx] & 0x7F;
            val *= 128;
        }
        else {
            /* write val as text into out */
            val += in[inIdx];
            w = XSNPRINTF(out + outIdx, outSz - outIdx, ".%u", val);
            if (w < 0 || (word32)w > outSz - outIdx) {
                w = BUFFER_E;
                goto exit;
            }
            outIdx += w;
            val = 0;
        }
        inIdx++;
    }
    if (outIdx == outSz)
        outIdx--;
    out[outIdx] = 0;

    w = (int)outIdx;

exit:
    return w;
}
#endif /* WOLFSSL_CERT_EXT && !WOLFSSL_SEP */

#if defined(WOLFSSL_SEP) || defined(WOLFSSL_CERT_EXT) || defined(WOLFSSL_QT)
    #ifdef WOLFSSL_ASN_TEMPLATE
    /* ASN.1 template for PolicyInformation.
     * X.509: RFC 5280, 4.2.1.4 - Certificate Policies.
     */
    static const ASNItem policyInfoASN[] = {
    /*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                    /* policyIdentifier */
    /*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
                    /* policyQualifiers */
    /*  2 */        { 1, ASN_SEQUENCE, 1, 0, 1 },
    };

    /* Number of items in ASN.1 template for PolicyInformation. */
    #define policyInfoASN_Length (sizeof(policyInfoASN) / sizeof(ASNItem))
    #endif

    /* Reference: https://tools.ietf.org/html/rfc5280#section-4.2.1.4 */
    static int DecodeCertPolicy(const byte* input, int sz, DecodedCert* cert)
    {
    #ifndef WOLFSSL_ASN_TEMPLATE
        word32 idx = 0;
        word32 oldIdx;
        int policy_length = 0;
        int ret;
        int total_length = 0;
    #if !defined(WOLFSSL_SEP) && defined(WOLFSSL_CERT_EXT) && \
        !defined(WOLFSSL_DUP_CERTPOL)
        int i;
    #endif

        WOLFSSL_ENTER("DecodeCertPolicy");
        #if defined(WOLFSSL_SEP) || defined(WOLFSSL_CERT_EXT)
        /* Check if cert is null before dereferencing below */
        if (cert == NULL)
            return BAD_FUNC_ARG;
        #endif

    #if defined(WOLFSSL_CERT_EXT)
         cert->extCertPoliciesNb = 0;
    #endif

        if (GetSequence(input, &idx, &total_length, sz) < 0) {
            WOLFSSL_MSG("\tGet CertPolicy total seq failed");
            return ASN_PARSE_E;
        }

        /* Validate total length */
        if (total_length > (sz - (int)idx)) {
            WOLFSSL_MSG("\tCertPolicy length mismatch");
            return ASN_PARSE_E;
        }

        /* Unwrap certificatePolicies */
        do {
            int length = 0;

            if (GetSequence(input, &idx, &policy_length, sz) < 0) {
                WOLFSSL_MSG("\tGet CertPolicy seq failed");
                return ASN_PARSE_E;
            }

            oldIdx = idx;
            ret = GetASNObjectId(input, &idx, &length, sz);
            if (ret != 0)
                return ret;
            policy_length -= idx - oldIdx;

            if (length > 0) {
                /* Verify length won't overrun buffer */
                if (length > (sz - (int)idx)) {
                    WOLFSSL_MSG("\tCertPolicy length exceeds input buffer");
                    return ASN_PARSE_E;
                }

        #if defined(WOLFSSL_SEP)
                cert->deviceType = (byte*)XMALLOC(length, cert->heap,
                                                         DYNAMIC_TYPE_X509_EXT);
                if (cert->deviceType == NULL) {
                    WOLFSSL_MSG("\tCouldn't alloc memory for deviceType");
                    return MEMORY_E;
                }
                cert->deviceTypeSz = length;
                XMEMCPY(cert->deviceType, input + idx, length);
                break;
        #elif defined(WOLFSSL_CERT_EXT)
                /* decode cert policy */
                if (DecodePolicyOID(cert->extCertPolicies[
                                       cert->extCertPoliciesNb], MAX_CERTPOL_SZ,
                                       input + idx, length) <= 0) {
                    WOLFSSL_MSG("\tCouldn't decode CertPolicy");
                    return ASN_PARSE_E;
                }
            #ifndef WOLFSSL_DUP_CERTPOL
                /* From RFC 5280 section 4.2.1.3 "A certificate policy OID MUST
                 * NOT appear more than once in a certificate policies
                 * extension". This is a sanity check for duplicates.
                 * extCertPolicies should only have OID values, additional
                 * qualifiers need to be stored in a separate array. */
                for (i = 0; i < cert->extCertPoliciesNb; i++) {
                    if (XMEMCMP(cert->extCertPolicies[i],
                            cert->extCertPolicies[cert->extCertPoliciesNb],
                            MAX_CERTPOL_SZ) == 0) {
                            WOLFSSL_MSG("Duplicate policy OIDs not allowed");
                            WOLFSSL_MSG("Use WOLFSSL_DUP_CERTPOL if wanted");
                            return CERTPOLICIES_E;
                    }
                }
            #endif /* !WOLFSSL_DUP_CERTPOL */
                cert->extCertPoliciesNb++;
        #else
                WOLFSSL_LEAVE("DecodeCertPolicy : unsupported mode", 0);
                return 0;
        #endif
            }
            idx += policy_length;
        } while((int)idx < total_length
    #if defined(WOLFSSL_CERT_EXT)
            && cert->extCertPoliciesNb < MAX_CERTPOL_NB
    #endif
        );

        WOLFSSL_LEAVE("DecodeCertPolicy", 0);
        return 0;
    #else /* WOLFSSL_ASN_TEMPLATE */
        word32 idx = 0;
        int ret = 0;
        int total_length = 0;
    #if !defined(WOLFSSL_SEP) && defined(WOLFSSL_CERT_EXT) && \
        !defined(WOLFSSL_DUP_CERTPOL)
        int i;
    #endif

        WOLFSSL_ENTER("DecodeCertPolicy");
        #if defined(WOLFSSL_SEP) || defined(WOLFSSL_CERT_EXT)
        /* Check if cert is null before dereferencing below */
        if (cert == NULL)
            ret = BAD_FUNC_ARG;
        #endif

        if (ret == 0) {
        #if defined(WOLFSSL_CERT_EXT)
             cert->extCertPoliciesNb = 0;
        #endif

            /* Strip SEQUENCE OF and check using all data. */
            if (GetASN_Sequence(input, &idx, &total_length, sz, 1) < 0) {
                ret = ASN_PARSE_E;
            }
        }

        /* Unwrap certificatePolicies */
        while ((ret == 0) && ((int)idx < total_length)
        #if defined(WOLFSSL_CERT_EXT)
            && (cert->extCertPoliciesNb < MAX_CERTPOL_NB)
        #endif
               ) {
            ASNGetData dataASN[policyInfoASN_Length];
            byte* data;
            word32 length = 0;

            /* Clear dynamic data and check OID is a cert policy type. */
            XMEMSET(dataASN, 0, sizeof(dataASN));
            GetASN_OID(&dataASN[1], oidCertPolicyType);
            ret = GetASN_Items(policyInfoASN, dataASN, policyInfoASN_Length, 1,
                               input, &idx, sz);
            if (ret == 0) {
                /* Get the OID. */
                GetASN_OIDData(&dataASN[1], &data, &length);
                if (length == 0) {
                    ret = ASN_PARSE_E;
                }
            }
            #if defined(WOLFSSL_SEP)
            /* Store OID in device type. */
            if (ret == 0) {
                cert->deviceType = (byte*)XMALLOC(length, cert->heap,
                                                  DYNAMIC_TYPE_X509_EXT);
                if (cert->deviceType == NULL) {
                    WOLFSSL_MSG("\tCouldn't alloc memory for deviceType");
                    ret = MEMORY_E;
                }
            }
            if (ret == 0) {
                /* Store device type data and length. */
                cert->deviceTypeSz = length;
                XMEMCPY(cert->deviceType, data, length);
                break;
            }
            #elif defined(WOLFSSL_CERT_EXT)
            if (ret == 0) {
                /* Decode cert policy. */
                if (DecodePolicyOID(
                                 cert->extCertPolicies[cert->extCertPoliciesNb],
                                 MAX_CERTPOL_SZ, data, length) <= 0) {
                    WOLFSSL_MSG("\tCouldn't decode CertPolicy");
                    ret = ASN_PARSE_E;
                }
            }
            #ifndef WOLFSSL_DUP_CERTPOL
            /* From RFC 5280 section 4.2.1.3 "A certificate policy OID MUST
             * NOT appear more than once in a certificate policies
             * extension". This is a sanity check for duplicates.
             * extCertPolicies should only have OID values, additional
             * qualifiers need to be stored in a seperate array. */
            for (i = 0; (ret == 0) && (i < cert->extCertPoliciesNb); i++) {
                if (XMEMCMP(cert->extCertPolicies[i],
                            cert->extCertPolicies[cert->extCertPoliciesNb],
                            MAX_CERTPOL_SZ) == 0) {
                    WOLFSSL_MSG("Duplicate policy OIDs not allowed");
                    WOLFSSL_MSG("Use WOLFSSL_DUP_CERTPOL if wanted");
                    ret = CERTPOLICIES_E;
                }
            }
            #endif /* !defined(WOLFSSL_DUP_CERTPOL) */
            if (ret == 0) {
                /* Keep count of policies seen. */
                cert->extCertPoliciesNb++;
            }
            #else
                (void)data;
                WOLFSSL_LEAVE("DecodeCertPolicy : unsupported mode", 0);
                break;
            #endif
        }

        WOLFSSL_LEAVE("DecodeCertPolicy", 0);
        return ret;
    #endif /* WOLFSSL_ASN_TEMPLATE */
    }
#endif /* WOLFSSL_SEP */

/* Macro to check if bit is set, if not sets and return success.
    Otherwise returns failure */
/* Macro required here because bit-field operation */
#ifndef WOLFSSL_NO_ASN_STRICT
    #define VERIFY_AND_SET_OID(bit) \
        if (bit == 0) \
            bit = 1; \
        else \
            return ASN_OBJECT_ID_E;
#else
    /* With no strict defined, the verify is skipped */
#define VERIFY_AND_SET_OID(bit) bit = 1;
#endif

/* Parse extension type specific data based on OID sum.
 *
 * Supported extensions:
 *   Basic Constraints - BASIC_CA_OID
 *   CRL Distribution Points - CRL_DIST_OID
 *   Authority Information Access - AUTH_INFO_OID
 *   Subject Alternative Name - ALT_NAMES_OID
 *   Authority Key Identifer - AUTH_KEY_OID
 *   Subject Key Identifier - SUBJ_KEY_OID
 *   Certificate Policies - CERT_POLICY_OID (conditional parsing)
 *   Key Usage - KEY_USAGE_OID
 *   Extended Key Usage - EXT_KEY_USAGE_OID
 *   Name Constraints - NAME_CONS_OID
 *   Inhibit anyPolicy - INHIBIT_ANY_OID
 *   Netscape Certificate Type - NETSCAPE_CT_OID (able to be excluded)
 *   OCSP no check - OCSP_NOCHECK_OID (when compiling OCSP)
 * Unsupported extensions from RFC 580:
 *   4.2.1.5 - Policy mappings
 *   4.2.1.7 - Issuer Alternative Name
 *   4.2.1.8 - Subject Directory Attributes
 *   4.2.1.11 - Policy Constraints
 *   4.2.1.15 - Freshest CRL
 *   4.2.2.2 - Subject Information Access
 *
 * @param [in]      input     Buffer containing extension type specific data.
 * @param [in]      length    Length of data.
 * @param [in]      oid       OID sum for extension.
 * @param [in]      critical  Whether extension is critical.
 * @param [in, out] cert      Certificate object.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoding is invalid.
 * @return  MEMORY_E on dynamic memory allocation failure.
 * @return  Other -ve value on error.
 */
static int DecodeExtensionType(const byte* input, int length, word32 oid,
                               byte critical, DecodedCert* cert)
{
    int ret = 0;
    word32 idx = 0;

    switch (oid) {
        /* Basic Constraints. */
        case BASIC_CA_OID:
            VERIFY_AND_SET_OID(cert->extBasicConstSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extBasicConstCrit = critical;
            #endif
            if (DecodeBasicCaConstraint(input, length, cert) < 0) {
                ret = ASN_PARSE_E;
            }
            break;

        /* CRL Distribution point. */
        case CRL_DIST_OID:
            VERIFY_AND_SET_OID(cert->extCRLdistSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extCRLdistCrit = critical;
            #endif
            if (DecodeCrlDist(input, length, cert) < 0) {
                ret = ASN_PARSE_E;
            }
            break;

        /* Authority information access. */
        case AUTH_INFO_OID:
            VERIFY_AND_SET_OID(cert->extAuthInfoSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extAuthInfoCrit = critical;
            #endif
            if (DecodeAuthInfo(input, length, cert) < 0) {
                ret = ASN_PARSE_E;
            }
            break;

        /* Subject alternative name. */
        case ALT_NAMES_OID:
            VERIFY_AND_SET_OID(cert->extSubjAltNameSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extSubjAltNameCrit = critical;
            #endif
            ret = DecodeAltNames(input, length, cert);
            break;

        /* Authority Key Identifier. */
        case AUTH_KEY_OID:
            VERIFY_AND_SET_OID(cert->extAuthKeyIdSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extAuthKeyIdCrit = critical;
            #endif
            #ifndef WOLFSSL_ALLOW_CRIT_SKID
                /* This check is added due to RFC 5280 section 4.2.1.1
                 * stating that conforming CA's must mark this extension
                 * as non-critical. When parsing extensions check that
                 * certificate was made in compliance with this. */
                if (critical) {
                    WOLFSSL_MSG("Critical Auth Key ID is not allowed");
                    WOLFSSL_MSG("Use macro WOLFSSL_ALLOW_CRIT_SKID if wanted");
                    ret = ASN_CRIT_EXT_E;
                }
            #endif
            if ((ret == 0) && (DecodeAuthKeyId(input, length, cert) < 0)) {
                ret = ASN_PARSE_E;
            }
            break;

        /* Subject Key Identifier. */
        case SUBJ_KEY_OID:
            VERIFY_AND_SET_OID(cert->extSubjKeyIdSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extSubjKeyIdCrit = critical;
            #endif
            #ifndef WOLFSSL_ALLOW_CRIT_SKID
                /* This check is added due to RFC 5280 section 4.2.1.2
                 * stating that conforming CA's must mark this extension
                 * as non-critical. When parsing extensions check that
                 * certificate was made in compliance with this. */
                if (critical) {
                    WOLFSSL_MSG("Critical Subject Key ID is not allowed");
                    WOLFSSL_MSG("Use macro WOLFSSL_ALLOW_CRIT_SKID if wanted");
                    ret = ASN_CRIT_EXT_E;
                }
            #endif

            if ((ret == 0) && (DecodeSubjKeyId(input, length, cert) < 0)) {
                ret = ASN_PARSE_E;
            }
            break;

        /* Certificate policies. */
        case CERT_POLICY_OID:
            #if defined(WOLFSSL_SEP) || defined(WOLFSSL_QT)
                VERIFY_AND_SET_OID(cert->extCertPolicySet);
                #if defined(OPENSSL_EXTRA) || \
                    defined(OPENSSL_EXTRA_X509_SMALL)
                    cert->extCertPolicyCrit = critical;
                #endif
            #endif
            #if defined(WOLFSSL_SEP) || defined(WOLFSSL_CERT_EXT) || \
                defined(WOLFSSL_QT)
                if (DecodeCertPolicy(input, length, cert) < 0) {
                    ret = ASN_PARSE_E;
                }
            #else
                WOLFSSL_MSG("Certificate Policy extension not supported yet.");
            #endif
            break;

        /* Key usage. */
        case KEY_USAGE_OID:
            VERIFY_AND_SET_OID(cert->extKeyUsageSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extKeyUsageCrit = critical;
            #endif
            if (DecodeKeyUsage(input, length, cert) < 0) {
                ret = ASN_PARSE_E;
            }
            break;

        /* Extended key usage. */
        case EXT_KEY_USAGE_OID:
            VERIFY_AND_SET_OID(cert->extExtKeyUsageSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extExtKeyUsageCrit = critical;
            #endif
            if (DecodeExtKeyUsage(input, length, cert) < 0) {
                ret = ASN_PARSE_E;
            }
            break;

        #ifndef IGNORE_NAME_CONSTRAINTS
        /* Name constraints. */
        case NAME_CONS_OID:
        #ifndef WOLFSSL_NO_ASN_STRICT
            /* Verify RFC 5280 Sec 4.2.1.10 rule:
                "The name constraints extension,
                which MUST be used only in a CA certificate" */
            if (!cert->isCA) {
                WOLFSSL_MSG("Name constraints allowed only for CA certs");
                ret = ASN_NAME_INVALID_E;
            }
        #endif
            VERIFY_AND_SET_OID(cert->extNameConstraintSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extNameConstraintCrit = critical;
            #endif
            if (DecodeNameConstraints(input, length, cert) < 0) {
                ret = ASN_PARSE_E;
            }
            break;
        #endif /* IGNORE_NAME_CONSTRAINTS */

        /* Inhibit anyPolicy. */
        case INHIBIT_ANY_OID:
            VERIFY_AND_SET_OID(cert->inhibitAnyOidSet);
            WOLFSSL_MSG("Inhibit anyPolicy extension not supported yet.");
            break;

   #ifndef IGNORE_NETSCAPE_CERT_TYPE
        /* Netscape's certificate type. */
        case NETSCAPE_CT_OID:
            WOLFSSL_MSG("Netscape certificate type extension not supported "
                        "yet.");
            if (CheckBitString(input, &idx, &length, length, 0, NULL) < 0) {
                ret = ASN_PARSE_E;
            }
            break;
    #endif
    #ifdef HAVE_OCSP
        /* OCSP no check. */
        case OCSP_NOCHECK_OID:
            VERIFY_AND_SET_OID(cert->ocspNoCheckSet);
            ret = GetASNNull(input, &idx, length);
            if (ret != 0) {
                ret = ASN_PARSE_E;
            }
            break;
    #endif
        case POLICY_CONST_OID:
            VERIFY_AND_SET_OID(cert->extPolicyConstSet);
            #if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
                cert->extPolicyConstCrit = critical;
            #endif
            if (DecodePolicyConstraints(&input[idx], length, cert) < 0)
                return ASN_PARSE_E;
            break;
        default:
        #ifndef WOLFSSL_NO_ASN_STRICT
            /* While it is a failure to not support critical extensions,
             * still parse the certificate ignoring the unsupported
             * extension to allow caller to accept it with the verify
             * callback. */
            if (critical)
                ret = ASN_CRIT_EXT_E;
        #endif
            break;
    }

    return ret;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for extensions.
 * X.509: RFC 5280, 4.1 - Basic Certificate Fields.
 */
static const ASNItem certExtHdrASN[] = {
/*  0 */    { 0, ASN_CONTEXT_SPECIFIC | 3, 1, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
};

/* Number of itesm in ASN.1 template for extensions. */
#define certExtHdrASN_Length (sizeof(certExtHdrASN) / sizeof(ASNItem))

/* ASN.1 template for Extension.
 * X.509: RFC 5280, 4.1 - Basic Certificate Fields.
 */
static const ASNItem certExtASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* Extension object id */
/*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
                /* critical - when true, must be parseable. */
/*  2 */        { 1, ASN_BOOLEAN, 0, 0, 1 },
                /* Data for extension - leave index at start of data. */
/*  3 */        { 1, ASN_OCTET_STRING, 0, 1, 0 },
};

/* Number of items in ASN.1 template for Extension. */
#define certExtASN_Length (sizeof(certExtASN) / sizeof(ASNItem))
#endif

/*
 *  Processing the Certificate Extensions. This does not modify the current
 *  index. It is works starting with the recorded extensions pointer.
 */
static int DecodeCertExtensions(DecodedCert* cert)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret = 0;
    word32 idx = 0;
    int sz = cert->extensionsSz;
    const byte* input = cert->extensions;
    int length;
    word32 oid;
    byte critical = 0;
    byte criticalFail = 0;
    byte tag = 0;

    WOLFSSL_ENTER("DecodeCertExtensions");

    if (input == NULL || sz == 0)
        return BAD_FUNC_ARG;

#ifdef WOLFSSL_CERT_REQ
    if (!cert->isCSR)
#endif
    { /* Not included in CSR */
        if (GetASNTag(input, &idx, &tag, sz) < 0) {
            return ASN_PARSE_E;
        }

        if (tag != ASN_EXTENSIONS) {
            WOLFSSL_MSG("\tfail: should be an EXTENSIONS");
            return ASN_PARSE_E;
        }

        if (GetLength(input, &idx, &length, sz) < 0) {
            WOLFSSL_MSG("\tfail: invalid length");
            return ASN_PARSE_E;
        }
    }

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: should be a SEQUENCE (1)");
        return ASN_PARSE_E;
    }

    while (idx < (word32)sz) {
        word32 localIdx;

        if (GetSequence(input, &idx, &length, sz) < 0) {
            WOLFSSL_MSG("\tfail: should be a SEQUENCE");
            return ASN_PARSE_E;
        }

        oid = 0;
        if ((ret = GetObjectId(input, &idx, &oid, oidCertExtType, sz)) < 0) {
            WOLFSSL_MSG("\tfail: OBJECT ID");
            return ret;
        }

        /* check for critical flag */
        critical = 0;
        if ((idx + 1) > (word32)sz) {
            WOLFSSL_MSG("\tfail: malformed buffer");
            return BUFFER_E;
        }

        localIdx = idx;
        if (GetASNTag(input, &localIdx, &tag, sz) == 0) {
            if (tag == ASN_BOOLEAN) {
                ret = GetBoolean(input, &idx, sz);
                if (ret < 0) {
                    WOLFSSL_MSG("\tfail: critical boolean");
                    return ret;
                }

                critical = (byte)ret;
            }
        }

        /* process the extension based on the OID */
        ret = GetOctetString(input, &idx, &length, sz);
        if (ret < 0) {
            WOLFSSL_MSG("\tfail: bad OCTET STRING");
            return ret;
        }

        ret = DecodeExtensionType(input + idx, length, oid, critical, cert);
        if (ret == ASN_CRIT_EXT_E) {
            ret = 0;
            criticalFail = 1;
        }
        if (ret < 0)
            goto end;
        idx += length;
    }

    ret = criticalFail ? ASN_CRIT_EXT_E : 0;
end:
    return ret;
#else
    DECL_ASNGETDATA(dataASN, certExtASN_Length);
    ASNGetData dataExtsASN[certExtHdrASN_Length];
    int ret = 0;
    const byte* input = cert->extensions;
    int sz = cert->extensionsSz;
    word32 idx = 0;
    int criticalRet = 0;
    int offset = 0;

    WOLFSSL_ENTER("DecodeCertExtensions");

    if (input == NULL || sz == 0)
        ret = BAD_FUNC_ARG;

    ALLOC_ASNGETDATA(dataASN, certExtASN_Length, ret, cert->heap);

#ifdef WOLFSSL_CERT_REQ
    if (cert->isCSR) {
        offset = 1;
    }
#endif
    if (ret == 0) {
        /* Clear dynamic data. */
        XMEMSET(dataExtsASN, 0, sizeof(dataExtsASN));
        /* Parse extensions header. */
        ret = GetASN_Items(certExtHdrASN + offset, dataExtsASN + offset,
                           certExtHdrASN_Length - offset, 0, input, &idx, sz);
    }
    /* Parse each extension. */
    while ((ret == 0) && (idx < (word32)sz)) {
        byte critical = 0;

        /* Clear dynamic data. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * certExtASN_Length);
        /* Ensure OID is an extention type. */
        GetASN_OID(&dataASN[1], oidCertExtType);
        /* Set criticality variable. */
        GetASN_Int8Bit(&dataASN[2], &critical);
        /* Parse extension wrapper. */
        ret = GetASN_Items(certExtASN, dataASN, certExtASN_Length, 0, input,
                           &idx, sz);
        if (ret == 0) {
            word32 oid = dataASN[1].data.oid.sum;
            int length = dataASN[3].length;

            /* Decode the extension by type. */
            ret = DecodeExtensionType(input + idx, length, oid, critical, cert);
            /* Move index on to next extension. */
            idx += length;
        }
        /* Don't fail criticality until all other extensions have been checked.
         */
        if (ret == ASN_CRIT_EXT_E) {
            criticalRet = ASN_CRIT_EXT_E;
            ret = 0;
        }
    }

    if (ret == 0) {
        /* Use criticality return. */
        ret = criticalRet;
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
#endif
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN template for an X509 certificate.
 * X.509: RFC 5280, 4.1 - Basic Certificate Fields.
 */
static const ASNItem x509CertASN[] = {
        /* Certificate ::= SEQUENCE */
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
        /* tbsCertificate       TBSCertificate */
        /* TBSCertificate ::= SEQUENCE */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
        /* version         [0]  EXPLICT Version DEFAULT v1 */
/*  2 */            { 2, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
        /* Version ::= INTEGER { v1(0), v2(1), v3(2) */
/*  3 */                { 3, ASN_INTEGER, 0, 0, 0 },
        /* serialNumber         CertificateSerialNumber */
        /* CetificateSerialNumber ::= INTEGER */
/*  4 */            { 2, ASN_INTEGER, 0, 0, 0 },
        /* signature            AlgorithmIdentifier */
        /* AlgorithmIdentifier ::= SEQUENCE */
/*  5 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
        /* Algorithm    OBJECT IDENTIFIER */
/*  6 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
        /* parameters   ANY defined by algorithm OPTIONAL */
/*  7 */                { 3, ASN_TAG_NULL, 0, 0, 1 },
        /* issuer               Name */
/*  8 */            { 2, ASN_SEQUENCE, 1, 0, 0 },
        /* validity             Validity */
        /* Validity ::= SEQUENCE */
/*  9 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
        /* notBefore   Time */
        /* Time :: CHOICE { UTCTime, GeneralizedTime } */
/* 10 */                { 3, ASN_UTC_TIME, 0, 0, 2 },
/* 11 */                { 3, ASN_GENERALIZED_TIME, 0, 0, 2 },
        /* notAfter   Time */
        /* Time :: CHOICE { UTCTime, GeneralizedTime } */
/* 12 */                { 3, ASN_UTC_TIME, 0, 0, 3 },
/* 13 */                { 3, ASN_GENERALIZED_TIME, 0, 0, 3 },
        /* subject              Name */
/* 14 */            { 2, ASN_SEQUENCE, 1, 0, 0 },
        /* subjectPublicKeyInfo SubjectPublicKeyInfo */
/* 15 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
        /* algorithm          AlgorithmIdentifier */
        /* AlgorithmIdentifier ::= SEQUENCE */
/* 16 */                { 3, ASN_SEQUENCE, 1, 1, 0 },
        /* Algorithm    OBJECT IDENTIFIER */
/* 17 */                    { 4, ASN_OBJECT_ID, 0, 0, 0 },
        /* parameters   ANY defined by algorithm OPTIONAL */
/* 18 */                    { 4, ASN_TAG_NULL, 0, 0, 1 },
/* 19 */                    { 4, ASN_OBJECT_ID, 0, 0, 1 },
        /* subjectPublicKey   BIT STRING */
/* 20 */                { 3, ASN_BIT_STRING, 0, 0, 0 },
        /* issuerUniqueID       UniqueIdentfier OPTIONAL */
/* 21 */            { 2, ASN_CONTEXT_SPECIFIC | 1, 0, 0, 1 },
        /* subjectUniqueID      UniqueIdentfier OPTIONAL */
/* 22 */            { 2, ASN_CONTEXT_SPECIFIC | 2, 0, 0, 1 },
        /* extensions           Extensions OPTIONAL */
/* 23 */            { 2, ASN_CONTEXT_SPECIFIC | 3, 1, 0, 1 },
        /* signatureAlgorithm   AlgorithmIdentifier */
        /* AlgorithmIdentifier ::= SEQUENCE */
/* 24 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
        /* Algorithm    OBJECT IDENTIFIER */
/* 25 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
        /* parameters   ANY defined by algorithm OPTIONAL */
/* 26 */            { 2, ASN_TAG_NULL, 0, 0, 1 },
        /* signature            BIT STRING */
/* 27 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN template for an X509 certificate. */
#define x509CertASN_Length (sizeof(x509CertASN) / sizeof(ASNItem))

/* Check the data data.
 *
 * @param [in] dataASN   ASN template dynamic data item.
 * @param [in] dataType  BEFORE or AFTER date.
 * @return  0 on success.
 * @return  ASN_TIME_E when BER tag is nor UTC or GENERALIZED time.
 * @return  ASN_DATE_SZ_E when time data is not supported.
 * @return  ASN_BEFORE_DATE_E when BEFORE date is invalid.
 * @return  ASN_AFTER_DATE_E when AFTER date is invalid.
 */
static int CheckDate(ASNGetData *dataASN, int dateType)
{
    int ret = 0;

    /* Check BER tag is valid. */
    if ((dataASN->tag != ASN_UTC_TIME) &&
            (dataASN->tag != ASN_GENERALIZED_TIME)) {
        ret = ASN_TIME_E;
    }
    /* Check date length is valid. */
    if ((ret == 0) && ((dataASN->length > MAX_DATE_SIZE) ||
                       (dataASN->length < MIN_DATE_SIZE))) {
        ret = ASN_DATE_SZ_E;
    }

#ifndef NO_ASN_TIME
    /* Check date is a valid string and BEFORE or AFTER now. */
    if ((ret == 0) &&
            (!XVALIDATE_DATE(dataASN->data.ref.data, dataASN->tag, dateType))) {
        if (dateType == BEFORE) {
            ret = ASN_BEFORE_DATE_E;
        }
        else {
            ret = ASN_AFTER_DATE_E;
        }
    }
#endif
    (void)dateType;

    return ret;
}

/* Decode a certificate. Internal/non-public API.
 *
 * @param [in]  cert             Certificate object.
 * @param [in]  verify           Whether to verify dates before and after now.
 * @param [out] criticalExt      Critical extension return code.
 * @param [out] badDateRet       Bad date return code.
 * @param [in]  stopAtPubKey     Stop parsing before subkectPublicKeyInfo.
 * @param [in]  stopAfterPubKey  Stop parsing after subkectPublicKeyInfo.
 * @return  0 on success.
 * @return  ASN_CRIT_EXT_E when a critical extension was not recognized.
 * @return  ASN_TIME_E when date BER tag is nor UTC or GENERALIZED time.
 * @return  ASN_DATE_SZ_E when time data is not supported.
 * @return  ASN_BEFORE_DATE_E when BEFORE date is invalid.
 * @return  ASN_AFTER_DATE_E when AFTER date is invalid.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
static int DecodeCertInternal(DecodedCert* cert, int verify, int* criticalExt,
                              int* badDateRet, int stopAtPubKey,
                              int stopAfterPubKey)
{
    DECL_ASNGETDATA(dataASN, x509CertASN_Length);
    int ret = 0;
    int badDate = 0;
    int i;
    byte version;
    word32 idx;
    word32 serialSz;
    int done = 0;

    CALLOC_ASNGETDATA(dataASN, x509CertASN_Length, ret, cert->heap);

    if (ret == 0) {
        version = 0;
        serialSz = EXTERNAL_SERIAL_SIZE;

        /* Get the version and put the serial number into the buffer. */
        GetASN_Int8Bit(&dataASN[3], &version);
        GetASN_Buffer(&dataASN[4], cert->serial, &serialSz);
        /* Check OID types for signature, algorithm, ECC curve and sigAlg. */
        GetASN_OID(&dataASN[6], oidSigType);
        GetASN_OID(&dataASN[17], oidKeyType);
        GetASN_OID(&dataASN[19], oidCurveType);
        GetASN_OID(&dataASN[25], oidSigType);
        /* Parse the X509 certificate. */
        ret = GetASN_Items(x509CertASN, dataASN, x509CertASN_Length, 1,
                           cert->source, &cert->srcIdx, cert->maxIdx);
    }
    /* Check version is valid/supported - can't be negative. */
    if ((ret == 0) && (version > MAX_X509_VERSION)) {
        WOLFSSL_MSG("Unexpected certificate version");
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* Set fields extracted from data. */
        cert->version = version;
        cert->serialSz = serialSz;
        cert->signatureOID = dataASN[6].data.oid.sum;
        cert->keyOID = dataASN[17].data.oid.sum;
        cert->certBegin = dataASN[1].offset;

        /* No bad date error - don't always care. */
        badDate = 0;
        /* Find the item with the BEFORE date and check it. */
        i = (dataASN[10].tag != 0) ? 10 : 11;
        if ((CheckDate(&dataASN[i], BEFORE) < 0) && verify) {
            badDate = ASN_BEFORE_DATE_E;
        }
        /* Store reference to BEFOREdate. */
        cert->beforeDate = GetASNItem_Addr(dataASN[i], cert->source);
        cert->beforeDateLen = GetASNItem_Length(dataASN[i], cert->source);

        /* Find the item with the AFTER date and check it. */
        i = (dataASN[12].tag != 0) ? 12 : 13;
        if ((CheckDate(&dataASN[i], AFTER) < 0) && verify) {
            badDate = ASN_AFTER_DATE_E;
        }
        /* Store reference to AFTER date. */
        cert->afterDate = GetASNItem_Addr(dataASN[i], cert->source);
        cert->afterDateLen = GetASNItem_Length(dataASN[i], cert->source);

        /* Get the issuer name and calculate hash. */
        idx = dataASN[8].offset;
        ret = GetCertName(cert, cert->issuer, cert->issuerHash, ISSUER,
                          cert->source, &idx, dataASN[9].offset);
    }
    if (ret == 0) {
        /* Get the subject name and calculate hash. */
        idx = dataASN[14].offset;
        ret = GetCertName(cert, cert->subject, cert->subjectHash, SUBJECT,
                          cert->source, &idx, dataASN[15].offset);
    }
    if (ret == 0) {
        /* Determine if self signed by comparig issuer and subject hashes. */
        cert->selfSigned = XMEMCMP(cert->issuerHash, cert->subjectHash,
                                   KEYID_SIZE) == 0 ? 1 : 0;

        if (stopAtPubKey) {
            /* Return any bad date error through badDateRed and return offset of
             * subjectPublicKeyInfo.
             */
            if (badDateRet != NULL) {
                *badDateRet = badDate;
            }
            ret = dataASN[15].offset;
            done = 1;
        }
    }

    if ((ret == 0) && (!done)) {
        /* Parse the public key. */
        idx = dataASN[15].offset;
        ret = GetCertKey(cert, cert->source, &idx, dataASN[21].offset);
        if ((ret == 0) && stopAfterPubKey) {
            /* Return any bad date error through badDateRed and return offset
             * after subjectPublicKeyInfo.
             */
            if (badDateRet != NULL) {
                *badDateRet = badDate;
            }
            done = 1;
        }
    }
    if ((ret == 0) && (!done) && (dataASN[23].data.ref.data != NULL)) {
    #ifndef ALLOW_V1_EXTENSIONS
        /* Certificate extensions were only defined in version 2. */
        if (cert->version < 2) {
            WOLFSSL_MSG("\tv1 and v2 certs not allowed extensions");
            ret = ASN_VERSION_E;
        }
    #endif
        if (ret == 0) {
            /* Save references to extension data. */
            cert->extensions    = GetASNItem_Addr(dataASN[23], cert->source);
            cert->extensionsSz  = GetASNItem_Length(dataASN[23], cert->source);
            cert->extensionsIdx = dataASN[23].offset;

            /* Decode the extension data starting at [3]. */
            ret = DecodeCertExtensions(cert);
            if (criticalExt != NULL) {
                if (ret == ASN_CRIT_EXT_E) {
                    /* Return critical extension not recognized. */
                    *criticalExt = ret;
                    ret = 0;
                }
                else {
                    /* No critical extension error. */
                    *criticalExt = 0;
                }
            }
        }
        if (ret == 0) {
            /* Advance past extensions. */
            cert->srcIdx = dataASN[24].offset;
        }
    }

    if ((ret == 0) && (!done)) {
        /* Store the signature information. */
        cert->sigIndex = dataASN[24].offset;
        GetASN_GetConstRef(&dataASN[27], &cert->signature, &cert->sigLength);
        /* Make sure 'signature' and 'signatureAlgorithm' are the same. */
        if (dataASN[25].data.oid.sum != cert->signatureOID) {
            ret = ASN_SIG_OID_E;
        }
        /* NULL tagged item not allowed after ECDSA or EdDSA algorithm OID. */
        if (IsSigAlgoECC(cert->signatureOID) && (dataASN[26].tag != 0)) {
            ret = ASN_PARSE_E;
        }
    }
    if ((ret == 0) && (!done) && (badDate != 0)) {
        /* Parsed whole certificate fine but return any date errors. */
        ret = badDate;
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
}

/* Decode BER/DER data into certificate object.
 *
 * BER/DER data information held in source, srcIdx and maxIdx fields of
 * certificate object.
 *
 * @param [in] cert         Decoded certificate object.
 * @param [in] verify       Whether to find CA and verify certificate.
 * @param [in] criticalExt  Any error for critical extensions not recognized.
 * @return  0 on success.
 * @return  ASN_CRIT_EXT_E when a critical extension was not recognized.
 * @return  ASN_TIME_E when date BER tag is nor UTC or GENERALIZED time.
 * @return  ASN_DATE_SZ_E when time data is not supported.
 * @return  ASN_BEFORE_DATE_E when BEFORE date is invalid.
 * @return  ASN_AFTER_DATE_E when AFTER date is invalid.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_BITSTR_E when the expected BIT_STRING tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
int DecodeCert(DecodedCert* cert, int verify, int* criticalExt)
{
    return DecodeCertInternal(cert, verify, criticalExt, NULL, 0, 0);
}

#ifdef WOLFSSL_CERT_REQ
/* ASN.1 template for certificate request Attribute.
 * PKCS #10: RFC 2986, 4.1 - CertificationRequestInfo
 */
static const ASNItem reqAttrASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* type */
/*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
                /* values */
/*  2 */        { 1, ASN_SET, 1, 0, 0 },
};

/* Number of items in ASN.1 template for certificate request Attribute. */
#define reqAttrASN_Length (sizeof(reqAttrASN) / sizeof(ASNItem))

/* ASN.1 template for a string choice. */
static const ASNItem strAttrASN[] = {
    { 0, 0, 0, 0, 0 },
};

/* Number of items in ASN.1 template for a string choice. */
#define strAttrASN_Length (sizeof(strAttrASN) / sizeof(ASNItem))

/* ASN.1 choices for types for a string in an attribute. */
static const byte strAttrChoice[] = {
    ASN_PRINTABLE_STRING, ASN_IA5_STRING, ASN_UTF8STRING, 0
};

/* Decode a certificate request attribute's value.
 *
 * @param [in]  cert         Certificate request object.
 * @param [out] criticalExt  Critical extension return code.
 * @param [in]  oid          OID decribing which attribute was found.
 * @param [in]  aIdx         Index into certificate source to start parsing.
 * @param [in]  input        Attribute value data.
 * @param [in]  maxIdx       Maximum index to parse to.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 */
static int DecodeCertReqAttrValue(DecodedCert* cert, int* criticalExt,
    word32 oid, word32 aIdx, const byte* input, word32 maxIdx)
{
    int ret = 0;
    word32 idx = 0;
    ASNGetData strDataASN[strAttrASN_Length];

    switch (oid) {
        /* A password by which the entity may request certificate revocation.
         * PKCS#9: RFC 2985, 5.4.1 - Challenge password
         */
        case CHALLENGE_PASSWORD_OID:
            /* Clear dynamic data and specify choices acceptable. */
            XMEMSET(strDataASN, 0, sizeof(strDataASN));
            GetASN_Choice(&strDataASN[0], strAttrChoice);
            /* Parse a string. */
            ret = GetASN_Items(strAttrASN, strDataASN, strAttrASN_Length,
                               1, input, &idx, maxIdx);
            if (ret == 0) {
                /* Store references to password data. */
                cert->cPwd = (char*)strDataASN[0].data.ref.data;
                cert->cPwdLen = strDataASN[0].data.ref.length;
            }
            break;

        /* Requested serial number to issue with.
         * PKCS#9: RFC 2985, 5.2.10 - Serial Number
         * (References: ISO/IEC 9594-6:1997)
         */
        case SERIAL_NUMBER_OID:
            /* Clear dynamic data and specify choices acceptable. */
            XMEMSET(strDataASN, 0, sizeof(strDataASN));
            GetASN_Choice(&strDataASN[0], strAttrChoice);
            /* Parse a string. */
            ret = GetASN_Items(strAttrASN, strDataASN, strAttrASN_Length,
                               1, input, &idx, maxIdx);
            if (ret == 0) {
                /* Store references to serial number. */
                cert->sNum = (char*)strDataASN[0].data.ref.data;
                cert->sNumLen = strDataASN[0].data.ref.length;
                /* Store serial number if small enough. */
                if (cert->sNumLen <= EXTERNAL_SERIAL_SIZE) {
                    XMEMCPY(cert->serial, cert->sNum, cert->sNumLen);
                    cert->serialSz = cert->sNumLen;
                }
            }
            break;

        /* Certificate extensions to be included in generated certificate.
         * PKCS#9: RFC 2985, 5.4.2 - Extension request
         */
        case EXTENSION_REQUEST_OID:
            /* Store references to all extensions. */
            cert->extensions    = input;
            cert->extensionsSz  = maxIdx;
            cert->extensionsIdx = aIdx;

            /* Decode and validate extensions. */
            ret = DecodeCertExtensions(cert);
            if (ret == ASN_CRIT_EXT_E) {
                /* Return critical extension not recognized. */
                *criticalExt = ret;
                ret = 0;
            }
            else {
                /* No critical extension error. */
                *criticalExt = 0;
            }
            break;

        default:
            ret = ASN_PARSE_E;
            break;
    }

    return ret;
}

/* Decode attributes of a BER encoded certificate request.
 *
 * RFC 2986 - PKCS #10: Certification Request Syntax Specification Version 1.7
 *
 * Outer sequence has been removed.
 *
 * @param [in]  cert         Certificate request object.
 * @param [out] criticalExt  Critical extension return code.
 * @param [in]  idx          Index into certificate source to start parsing.
 * @param [in]  maxIdx       Maximum index to parse to.
 * @return  0 on success.
 * @return  ASN_CRIT_EXT_E when a critical extension was not recognized.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 */
static int DecodeCertReqAttributes(DecodedCert* cert, int* criticalExt,
                                   word32 idx, word32 maxIdx)
{
    DECL_ASNGETDATA(dataASN, reqAttrASN_Length);
    int ret = 0;

    WOLFSSL_ENTER("DecodeCertReqAttributes");

    ALLOC_ASNGETDATA(dataASN, reqAttrASN_Length, ret, cert->heap);

    /* Parse each attribute until all data used up. */
    while ((ret == 0) && (idx < maxIdx)) {
        /* Clear dynamic data. */
        XMEMSET(dataASN, 0, sizeof(ASNGetData) * reqAttrASN_Length);
        GetASN_OID(&dataASN[1], oidIgnoreType);

        /* Parse an attribute. */
        ret = GetASN_Items(reqAttrASN, dataASN, reqAttrASN_Length, 0,
                           cert->source, &idx, maxIdx);
        /* idx is now at end of attribute data. */
        if (ret == 0) {
            ret = DecodeCertReqAttrValue(cert, criticalExt,
                dataASN[1].data.oid.sum,
                GetASNItem_DataIdx(dataASN[2], cert->source),
                dataASN[2].data.ref.data, dataASN[2].data.ref.length);
        }
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
}

/* ASN.1 template for a certificate request.
 * PKCS#10: RFC 2986, 4.1 - CertificationRequestInfo
 */
static const ASNItem certReqASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* version              INTEGER { v1(0), v2(1), v3(2) */
/*  2 */            { 2, ASN_INTEGER, 0, 0, 0 },
                    /* subject              Name */
/*  3 */            { 2, ASN_SEQUENCE, 1, 0, 0 },
                    /* subjectPublicKeyInfo SubjectPublicKeyInfo */
/*  4 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                        /* algorithm          AlgorithmIdentifier */
/*  5 */                { 3, ASN_SEQUENCE, 1, 1, 0 },
                            /* Algorithm    OBJECT IDENTIFIER */
/*  6 */                    { 4, ASN_OBJECT_ID, 0, 0, 0 },
                            /* parameters   ANY defined by algorithm OPTIONAL */
/*  7 */                    { 4, ASN_TAG_NULL, 0, 0, 1 },
/*  8 */                    { 4, ASN_OBJECT_ID, 0, 0, 1 },
/*  9 */                    { 4, ASN_SEQUENCE, 1, 0, 1 },
                        /* subjectPublicKey   BIT STRING */
/* 10 */                { 3, ASN_BIT_STRING, 0, 0, 0 },
                    /* attributes       [0] Attributes */
/* 11 */            { 2, ASN_CONTEXT_SPECIFIC | 0, 1, 0, 1 },
                /* signatureAlgorithm   AlgorithmIdentifier */
/* 12 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* Algorithm    OBJECT IDENTIFIER */
/* 13 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
                    /* parameters   ANY defined by algorithm OPTIONAL */
/* 14 */            { 2, ASN_TAG_NULL, 0, 0, 1 },
                /* signature            BIT STRING */
/* 15 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for a certificate request. */
#define certReqASN_Length (sizeof(certReqASN) / sizeof(ASNItem))

/* Parse BER encoded certificate request.
 *
 * RFC 2986 - PKCS #10: Certification Request Syntax Specification Version 1.7
 *
 * @param [in]  cert         Certificate request object.
 * @param [out] criticalExt  Critical extension return code.
 * @return  0 on success.
 * @return  ASN_CRIT_EXT_E when a critical extension was not recognized.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  BUFFER_E when data in buffer is too small.
 * @return  ASN_OBJECT_ID_E when the expected OBJECT_ID tag is not found.
 * @return  ASN_EXPECT_0_E when the INTEGER has the MSB set or NULL has a
 *          non-zero length.
 * @return  ASN_UNKNOWN_OID_E when the OID cannot be verified.
 * @return  MEMORY_E on dynamic memory allocation failure.
 */
static int DecodeCertReq(DecodedCert* cert, int* criticalExt)
{
    DECL_ASNGETDATA(dataASN, certReqASN_Length);
    int ret = 0;
    byte version;
    word32 idx;

    CALLOC_ASNGETDATA(dataASN, certReqASN_Length, ret, cert->heap);

    if (ret == 0) {
        /* Default version is 0. */
        version = 0;

        /* Set version var and OID types to expect. */
        GetASN_Int8Bit(&dataASN[2], &version);
        GetASN_OID(&dataASN[6], oidKeyType);
        GetASN_OID(&dataASN[8], oidCurveType);
        GetASN_OID(&dataASN[13], oidSigType);
        /* Parse a certificate request. */
        ret = GetASN_Items(certReqASN, dataASN, certReqASN_Length, 1,
                           cert->source, &cert->srcIdx, cert->maxIdx);
    }
    /* Check version is valid/supported - can't be negative. */
    if ((ret == 0) && (version > MAX_X509_VERSION)) {
        WOLFSSL_MSG("Unexpected certificate request version");
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* Set fields of certificate request. */
        cert->version = version;
        cert->signatureOID = dataASN[13].data.oid.sum;
        cert->keyOID = dataASN[6].data.oid.sum;
        cert->certBegin = dataASN[1].offset;

        /* Parse the subject name. */
        idx = dataASN[3].offset;
        ret = GetCertName(cert, cert->subject, cert->subjectHash, SUBJECT,
                          cert->source, &idx, dataASN[4].offset);
    }
    if (ret == 0) {
        /* Parse the certificate request Attributes. */
        ret = DecodeCertReqAttributes(cert, criticalExt,
            GetASNItem_DataIdx(dataASN[11], cert->source), dataASN[12].offset);
    }
    if (ret == 0) {
        /* Parse the certificate request's key. */
        idx = dataASN[4].offset;
        ret = GetCertKey(cert, cert->source, &idx, dataASN[11].offset);
    }
    if (ret == 0) {
        /* Store references to signature. */
        cert->sigIndex = dataASN[12].offset;
        GetASN_GetConstRef(&dataASN[15], &cert->signature, &cert->sigLength);
    }

    FREE_ASNGETDATA(dataASN, cert->heap);
    return ret;
}

#endif /* WOLFSSL_CERT_REQ */

#endif

int ParseCert(DecodedCert* cert, int type, int verify, void* cm)
{
    int   ret;
    char* ptr;

    ret = ParseCertRelative(cert, type, verify, cm);
    if (ret < 0)
        return ret;

    if (cert->subjectCNLen > 0) {
        ptr = (char*) XMALLOC(cert->subjectCNLen + 1, cert->heap,
                              DYNAMIC_TYPE_SUBJECT_CN);
        if (ptr == NULL)
            return MEMORY_E;
        XMEMCPY(ptr, cert->subjectCN, cert->subjectCNLen);
        ptr[cert->subjectCNLen] = '\0';
        cert->subjectCN = ptr;
        cert->subjectCNStored = 1;
    }

    if (cert->keyOID == RSAk &&
                          cert->publicKey != NULL  && cert->pubKeySize > 0) {
        ptr = (char*) XMALLOC(cert->pubKeySize, cert->heap,
                              DYNAMIC_TYPE_PUBLIC_KEY);
        if (ptr == NULL)
            return MEMORY_E;
        XMEMCPY(ptr, cert->publicKey, cert->pubKeySize);
        cert->publicKey = (byte *)ptr;
        cert->pubKeyStored = 1;
    }

    return ret;
}

#if !defined(OPENSSL_EXTRA) && !defined(OPENSSL_EXTRA_X509_SMALL) && \
    !defined(GetCA)
/* from SSL proper, for locking can't do find here anymore.
 * brought in from internal.h if built with compat layer.
 * if defined(GetCA), it's a predefined macro and these prototypes
 * would conflict.
 */
#ifdef __cplusplus
    extern "C" {
#endif
    Signer* GetCA(void* signers, byte* hash);
    #ifndef NO_SKID
        Signer* GetCAByName(void* signers, byte* hash);
    #endif
#ifdef __cplusplus
    }
#endif

#endif /* !OPENSSL_EXTRA && !OPENSSL_EXTRA_X509_SMALL && !GetCA */

#if defined(WOLFCRYPT_ONLY)

/* dummy functions, not using wolfSSL so don't need actual ones */
Signer* GetCA(void* signers, byte* hash)
{
    (void)hash;

    return (Signer*)signers;
}

#ifndef NO_SKID
Signer* GetCAByName(void* signers, byte* hash)
{
    (void)hash;

    return (Signer*)signers;
}
#endif /* NO_SKID */

#endif /* WOLFCRYPT_ONLY */

#if defined(WOLFSSL_NO_TRUSTED_CERTS_VERIFY) && !defined(NO_SKID)
static Signer* GetCABySubjectAndPubKey(DecodedCert* cert, void* cm)
{
    Signer* ca = NULL;
    if (cert->extSubjKeyIdSet)
        ca = GetCA(cm, cert->extSubjKeyId);
    if (ca == NULL)
        ca = GetCAByName(cm, cert->subjectHash);
    if (ca) {
        if ((ca->pubKeySize == cert->pubKeySize) &&
               (XMEMCMP(ca->publicKey, cert->publicKey, ca->pubKeySize) == 0)) {
            return ca;
        }
    }
    return NULL;
}
#endif

#if defined(WOLFSSL_SMALL_CERT_VERIFY) || defined(OPENSSL_EXTRA)
#ifdef WOLFSSL_ASN_TEMPLATE
/* Get the Hash of the Authority Key Identifier from the list of extensions.
 *
 * @param [in]  input   Input data.
 * @param [in]  maxIdx  Maximum index for data.
 * @param [out] hash    Hash of AKI.
 * @param [out] set     Whether the hash buffer was set.
 * @param [in]  heap    Dynamic memory allocation hint.
 * @return  0 on success.
 * @return  ASN_PARSE_E when BER encoded data does not match ASN.1 items or
 *          is invalid.
 * @return  MEMORY_E on dynamic memory allocation failure.
 */
static int GetAKIHash(const byte* input, word32 maxIdx, byte* hash, int* set,
                      void* heap)
{
    /* AKI and Certificate Extenion ASN.1 templates are the same length. */
    DECL_ASNGETDATA(dataASN, certExtASN_Length);
    int ret = 0;
    word32 idx = 0;
    int extLen = 0;
    word32 extEndIdx;
    byte* extData;
    word32 extDataSz;
    byte critical;

    ALLOC_ASNGETDATA(dataASN, certExtASN_Length, ret, heap);
    (void)heap;

    /* Parse the outer SEQUENCE and calculate end index of extensions. */
    if ((ret == 0) && (GetASN_Sequence(input, &idx, &extLen, maxIdx, 1) < 0)) {
        ret = ASN_PARSE_E;
    }
    extEndIdx = idx + extLen;

    /* Step through each extension looking for AKI. */
    while ((ret == 0) && (idx < extEndIdx)) {
        /* Clear dynamic data and check for certificate extension type OIDs. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * certExtASN_Length);
        GetASN_OID(&dataASN[1], oidCertExtType);
        /* Set criticality variable. */
        GetASN_Int8Bit(&dataASN[2], &critical);
        /* Parse an extension. */
        ret = GetASN_Items(certExtASN, dataASN, certExtASN_Length, 0, input,
                &idx, extEndIdx);
        if (ret == 0) {
            /* Get reference to extension data and move index on past this
             * extension. */
            GetASN_GetRef(&dataASN[3], &extData, &extDataSz);
            idx += extDataSz;

            /* Check whether we have the AKI extension. */
            if (dataASN[1].data.oid.sum == AUTH_KEY_OID) {
                /* Clear dynamic data. */
                XMEMSET(dataASN, 0, sizeof(*dataASN) * authKeyIdASN_Length);
                /* Start parsing extension data from the start. */
                idx = 0;
                /* Parse AKI extension data. */
                ret = GetASN_Items(authKeyIdASN, dataASN, authKeyIdASN_Length,
                        1, extData, &idx, extDataSz);
                if ((ret == 0) && (dataASN[1].data.ref.data != NULL)) {
                    /* We parsed successfully and have data. */
                    *set = 1;
                    /* Get the hash or hash of the hash if wrong size. */
                    ret = GetHashId(dataASN[1].data.ref.data,
                            dataASN[1].data.ref.length, hash);
                }
                break;
            }
        }
    }

    FREE_ASNGETDATA(dataASN, heap);
    return ret;
}
#endif

/* Only quick step through the certificate to find fields that are then used
 * in certificate signature verification.
 * Must use the signature OID from the signed part of the certificate.
 * Works also on certificate signing requests.
 *
 * This is only for minimizing dynamic memory usage during TLS certificate
 * chain processing.
 * Doesn't support:
 *   OCSP Only: alt lookup using subject and pub key w/o sig check
 */
static int CheckCertSignature_ex(const byte* cert, word32 certSz, void* heap,
        void* cm, const byte* pubKey, word32 pubKeySz, int pubKeyOID, int req)
{
#ifndef WOLFSSL_ASN_TEMPLATE
#ifndef WOLFSSL_SMALL_STACK
    SignatureCtx  sigCtx[1];
#else
    SignatureCtx* sigCtx;
#endif
    byte          hash[KEYID_SIZE];
    Signer*       ca = NULL;
    word32        idx = 0;
    int           len;
    word32        tbsCertIdx = 0;
    word32        sigIndex   = 0;
    word32        signatureOID = 0;
    word32        oid = 0;
    word32        issuerIdx = 0;
    word32        issuerSz  = 0;
#ifndef NO_SKID
    int           extLen = 0;
    word32        extIdx = 0;
    word32        extEndIdx = 0;
    int           extAuthKeyIdSet = 0;
#endif
    int           ret = 0;
    word32        localIdx;
    byte          tag;


    if (cert == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    sigCtx = (SignatureCtx*)XMALLOC(sizeof(*sigCtx), heap, DYNAMIC_TYPE_SIGNATURE);
    if (sigCtx == NULL)
        return MEMORY_E;
#endif
    InitSignatureCtx(sigCtx, heap, INVALID_DEVID);

    /* Certificate SEQUENCE */
    if (GetSequence(cert, &idx, &len, certSz) < 0)
        ret = ASN_PARSE_E;
    if (ret == 0) {
        tbsCertIdx = idx;

        /* TBSCertificate SEQUENCE */
        if (GetSequence(cert, &idx, &len, certSz) < 0)
            ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        sigIndex = len + idx;

        if ((idx + 1) > certSz)
            ret = BUFFER_E;
    }
    if (ret == 0) {
        /* version - optional */
        localIdx = idx;
        if (GetASNTag(cert, &localIdx, &tag, certSz) == 0) {
            if (tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED)) {
                idx++;
                if (GetLength(cert, &idx, &len, certSz) < 0)
                    ret = ASN_PARSE_E;
                idx += len;
            }
        }
    }

    if (ret == 0) {
        /* serialNumber */
        if (GetASNHeader(cert, ASN_INTEGER, &idx, &len, certSz) < 0)
            ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        idx += len;

        /* signature */
        if (!req &&
                GetAlgoId(cert, &idx, &signatureOID, oidSigType, certSz) < 0)
            ret = ASN_PARSE_E;
    }

    if (ret == 0) {
        issuerIdx = idx;
        /* issuer for cert or subject for csr */
        if (GetSequence(cert, &idx, &len, certSz) < 0)
            ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        issuerSz = len + idx - issuerIdx;
    }
#ifndef NO_SKID
    if (!req && ret == 0) {
        idx += len;

        /* validity */
        if (GetSequence(cert, &idx, &len, certSz) < 0)
            ret = ASN_PARSE_E;
    }
    if (!req && ret == 0) {
        idx += len;

        /* subject */
        if (GetSequence(cert, &idx, &len, certSz) < 0)
            ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        idx += len;

        /* subjectPublicKeyInfo */
        if (GetSequence(cert, &idx, &len, certSz) < 0)
            ret = ASN_PARSE_E;
    }
    if (req && ret == 0) {
        idx += len;

        /* attributes */
        if (GetASNHeader_ex(cert,
                ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED, &idx,
                &len, certSz, 1) < 0)
            ret = ASN_PARSE_E;
    }
    if (!req) {
        if (ret == 0) {
            idx += len;

            if ((idx + 1) > certSz)
                ret = BUFFER_E;
        }
        if (ret == 0) {
            /* issuerUniqueID - optional */
            localIdx = idx;
            if (GetASNTag(cert, &localIdx, &tag, certSz) == 0) {
                if (tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 1)) {
                    idx++;
                    if (GetLength(cert, &idx, &len, certSz) < 0)
                        ret = ASN_PARSE_E;
                    idx += len;
                }
            }
        }
        if (ret == 0) {
            if ((idx + 1) > certSz)
                ret = BUFFER_E;
        }
        if (ret == 0) {
            /* subjectUniqueID - optional */
            localIdx = idx;
            if (GetASNTag(cert, &localIdx, &tag, certSz) == 0) {
                if (tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 2)) {
                    idx++;
                    if (GetLength(cert, &idx, &len, certSz) < 0)
                        ret = ASN_PARSE_E;
                    idx += len;
                }
            }
        }

        if (ret == 0) {
            if ((idx + 1) > certSz)
                ret = BUFFER_E;
        }
        /* extensions - optional */
        localIdx = idx;
        if (ret == 0 && GetASNTag(cert, &localIdx, &tag, certSz) == 0 &&
                tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 3)) {
            idx++;
            if (GetLength(cert, &idx, &extLen, certSz) < 0)
                ret = ASN_PARSE_E;
            if (ret == 0) {
                if (GetSequence(cert, &idx, &extLen, certSz) < 0)
                    ret = ASN_PARSE_E;
            }
            if (ret == 0) {
                extEndIdx = idx + extLen;

                /* Check each extension for the ones we want. */
                while (ret == 0 && idx < extEndIdx) {
                    if (GetSequence(cert, &idx, &len, certSz) < 0)
                        ret = ASN_PARSE_E;
                    if (ret == 0) {
                        extIdx = idx;
                        if (GetObjectId(cert, &extIdx, &oid, oidCertExtType,
                                                                  certSz) < 0) {
                            ret = ASN_PARSE_E;
                        }

                        if (ret == 0) {
                            if ((extIdx + 1) > certSz)
                                ret = BUFFER_E;
                        }
                    }

                    if (ret == 0) {
                        localIdx = extIdx;
                        if (GetASNTag(cert, &localIdx, &tag, certSz) == 0 &&
                                tag == ASN_BOOLEAN) {
                            if (GetBoolean(cert, &extIdx, certSz) < 0)
                                ret = ASN_PARSE_E;
                        }
                    }
                    if (ret == 0) {
                        if (GetOctetString(cert, &extIdx, &extLen, certSz) < 0)
                            ret = ASN_PARSE_E;
                    }

                    if (ret == 0) {
                        switch (oid) {
                        case AUTH_KEY_OID:
                            if (GetSequence(cert, &extIdx, &extLen, certSz) < 0)
                                ret = ASN_PARSE_E;

                            if (ret == 0 && (extIdx + 1) >= certSz)
                                ret = BUFFER_E;

                            if (ret == 0 &&
                                    GetASNTag(cert, &extIdx, &tag, certSz) == 0 &&
                                    tag == (ASN_CONTEXT_SPECIFIC | 0)) {
                                if (GetLength(cert, &extIdx, &extLen, certSz) <= 0)
                                    ret = ASN_PARSE_E;
                                if (ret == 0) {
                                    extAuthKeyIdSet = 1;
                                    /* Get the hash or hash of the hash if wrong
                                     * size. */
                                    ret = GetHashId(cert + extIdx, extLen,
                                                    hash);
                                }
                            }
                            break;

                        default:
                            break;
                        }
                    }
                    idx += len;
                }
            }
        }
    }
    else if (ret == 0) {
        idx += len;
    }

    if (ret == 0 && pubKey == NULL) {
        if (extAuthKeyIdSet)
            ca = GetCA(cm, hash);
        if (ca == NULL) {
            ret = CalcHashId(cert + issuerIdx, issuerSz, hash);
            if (ret == 0)
                ca = GetCAByName(cm, hash);
        }
    }
#else
    if (ret == 0 && pubKey == NULL) {
        ret = CalcHashId(cert + issuerIdx, issuerSz, hash);
        if (ret == 0)
            ca = GetCA(cm, hash);
    }
#endif /* !NO_SKID */
    if (ca == NULL && pubKey == NULL)
        ret = ASN_NO_SIGNER_E;

    if (ret == 0) {
        idx = sigIndex;
        /* signatureAlgorithm */
        if (GetAlgoId(cert, &idx, &oid, oidSigType, certSz) < 0)
            ret = ASN_PARSE_E;
        /* In CSR signature data is not present in body */
        if (req)
            signatureOID = oid;
    }
    if (ret == 0) {
        if (oid != signatureOID)
            ret = ASN_SIG_OID_E;
    }
    if (ret == 0) {
        /* signatureValue */
        if (CheckBitString(cert, &idx, &len, certSz, 1, NULL) < 0)
            ret = ASN_PARSE_E;
    }

    if (ret == 0) {
        if (pubKey != NULL) {
            ret = ConfirmSignature(sigCtx, cert + tbsCertIdx,
                               sigIndex - tbsCertIdx,
                               pubKey, pubKeySz, pubKeyOID,
                               cert + idx, len, signatureOID, NULL);
        }
        else {
            ret = ConfirmSignature(sigCtx, cert + tbsCertIdx,
                               sigIndex - tbsCertIdx,
                               ca->publicKey, ca->pubKeySize, ca->keyOID,
                               cert + idx, len, signatureOID, NULL);
        }
        if (ret != 0) {
            WOLFSSL_MSG("Confirm signature failed");
        }
    }

    FreeSignatureCtx(sigCtx);
#ifdef WOLFSSL_SMALL_STACK
    if (sigCtx != NULL)
        XFREE(sigCtx, heap, DYNAMIC_TYPE_SIGNATURE);
#endif
    return ret;
#else /* WOLFSSL_ASN_TEMPLATE */
    /* X509 ASN.1 template longer than Certificate Request template. */
    DECL_ASNGETDATA(dataASN, x509CertASN_Length);
#ifndef WOLFSSL_SMALL_STACK
    SignatureCtx  sigCtx[1];
#else
    SignatureCtx* sigCtx = NULL;
#endif
    byte hash[KEYID_SIZE];
    Signer* ca = NULL;
    int ret = 0;
    word32 idx = 0;
#ifndef NO_SKID
    int extAuthKeyIdSet = 0;
#endif
    const byte* tbs = NULL;
    word32 tbsSz = 0;
    const byte* sig = NULL;
    word32 sigSz = 0;
    word32 sigOID = 0;
    const byte* caName = NULL;
    word32 caNameLen = 0;

    (void)req;
    (void)heap;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }

    ALLOC_ASNGETDATA(dataASN, x509CertASN_Length, ret, heap);
#ifdef WOLFSSL_SMALL_STACK
    if (ret == 0) {
        sigCtx = (SignatureCtx*)XMALLOC(sizeof(*sigCtx), heap,
                                                        DYNAMIC_TYPE_SIGNATURE);
        if (sigCtx == NULL) {
            ret = MEMORY_E;
        }
    }
#endif

    InitSignatureCtx(sigCtx, heap, INVALID_DEVID);

    if ((ret == 0) && (!req)) {
        /* Clear dynamic data for certificate items. */
        XMEMSET(dataASN, 0, sizeof(ASNGetData) * x509CertASN_Length);
        /* Set OID types expected for signature and public key. */
        GetASN_OID(&dataASN[6], oidSigType);
        GetASN_OID(&dataASN[17], oidKeyType);
        GetASN_OID(&dataASN[19], oidCurveType);
        GetASN_OID(&dataASN[25], oidSigType);
        /* Parse certificate. */
        ret = GetASN_Items(x509CertASN, dataASN, x509CertASN_Length, 1, cert,
                           &idx, certSz);

        /* Check signature OIDs match. */
        if ((ret == 0) && dataASN[6].data.oid.sum != dataASN[25].data.oid.sum) {
            ret = ASN_SIG_OID_E;
        }
        /* Store the data for verification in the certificate. */
        if (ret == 0) {
            tbs = GetASNItem_Addr(dataASN[1], cert);
            tbsSz = GetASNItem_Length(dataASN[1], cert);
            caName = GetASNItem_Addr(dataASN[8], cert);
            caNameLen = GetASNItem_Length(dataASN[8], cert);
            sigOID = dataASN[25].data.oid.sum;
            GetASN_GetConstRef(&dataASN[27], &sig, &sigSz);
        }
    }
    else if (ret == 0) {
#ifndef WOLFSSL_CERT_REQ
        ret = NOT_COMPILED_IN;
#else
        /* Clear dynamic data for certificate request items. */
        XMEMSET(dataASN, 0, sizeof(ASNGetData) * certReqASN_Length);
        /* Set OID types expected for signature and public key. */
        GetASN_OID(&dataASN[6], oidKeyType);
        GetASN_OID(&dataASN[8], oidCurveType);
        GetASN_OID(&dataASN[13], oidSigType);
        /* Parse certificate request. */
        ret = GetASN_Items(certReqASN, dataASN, certReqASN_Length, 1, cert,
                           &idx, certSz);
        if (ret == 0) {
            /* Store the data for verification in the certificate. */
            tbs = GetASNItem_Addr(dataASN[1], cert);
            tbsSz = GetASNItem_Length(dataASN[1], cert);
            caName = GetASNItem_Addr(dataASN[3], cert);
            caNameLen = GetASNItem_Length(dataASN[3], cert);
            sigOID = dataASN[13].data.oid.sum;
            GetASN_GetConstRef(&dataASN[15], &sig, &sigSz);
        }
#endif
    }

    /* If no public passed, then find the CA. */
    if ((ret == 0) && (pubKey == NULL)) {
#ifndef NO_SKID
        /* Find the AKI extension in list of extensions and get hash. */
        if ((ret == 0) && (!req) && (dataASN[23].data.ref.data != NULL)) {
            /* TODO: test case */
            ret = GetAKIHash(dataASN[23].data.ref.data,
                             dataASN[23].data.ref.length, hash,
                             &extAuthKeyIdSet, heap);
        }

        /* Get the CA by hash one was found. */
        if (extAuthKeyIdSet) {
            ca = GetCA(cm, hash);
        }
        if (ca == NULL)
#endif
        {
            /* Try hash of issuer name. */
            ret = CalcHashId(caName, caNameLen, hash);
            if (ret == 0) {
                ca = GetCAByName(cm, hash);
            }
        }

        if (ca != NULL) {
            /* Extract public key information. */
            pubKey = ca->publicKey;
            pubKeySz = ca->pubKeySize;
            pubKeyOID = ca->keyOID;
        }
        else {
            /* No public key to verify with. */
            ret = ASN_NO_SIGNER_E;
        }
    }

    if (ret == 0) {
        /* Check signature. */
        ret = ConfirmSignature(sigCtx, tbs, tbsSz, pubKey, pubKeySz, pubKeyOID,
                sig, sigSz, sigOID, NULL);
        if (ret != 0) {
            WOLFSSL_MSG("Confirm signature failed");
        }
    }

    FreeSignatureCtx(sigCtx);
#ifdef WOLFSSL_SMALL_STACK
    if (sigCtx != NULL)
        XFREE(sigCtx, heap, DYNAMIC_TYPE_SIGNATURE);
#endif
    FREE_ASNGETDATA(dataASN, heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef OPENSSL_EXTRA
/* Call CheckCertSignature_ex using a public key buffer for verification
 */
int CheckCertSignaturePubKey(const byte* cert, word32 certSz, void* heap,
        const byte* pubKey, word32 pubKeySz, int pubKeyOID)
{
    return CheckCertSignature_ex(cert, certSz, heap, NULL,
            pubKey, pubKeySz, pubKeyOID, 0);
}
#ifdef WOLFSSL_CERT_REQ
int CheckCSRSignaturePubKey(const byte* cert, word32 certSz, void* heap,
        const byte* pubKey, word32 pubKeySz, int pubKeyOID)
{
    return CheckCertSignature_ex(cert, certSz, heap, NULL,
            pubKey, pubKeySz, pubKeyOID, 1);
}
#endif /* WOLFSSL_CERT_REQ */
#endif /* OPENSSL_EXTRA */
#ifdef WOLFSSL_SMALL_CERT_VERIFY
/* Call CheckCertSignature_ex using a certificate manager (cm)
 */
int CheckCertSignature(const byte* cert, word32 certSz, void* heap, void* cm)
{
    return CheckCertSignature_ex(cert, certSz, heap, cm, NULL, 0, 0, 0);
}
#endif /* WOLFSSL_SMALL_CERT_VERIFY */
#endif /* WOLFSSL_SMALL_CERT_VERIFY || OPENSSL_EXTRA */

int ParseCertRelative(DecodedCert* cert, int type, int verify, void* cm)
{
    int    ret = 0;
    int    checkPathLen = 0;
    int    decrementMaxPathLen = 0;
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 confirmOID = 0;
#ifdef WOLFSSL_CERT_REQ
    int    len = 0;
#endif
#endif
#if defined(WOLFSSL_RENESAS_TSIP)
    int    idx = 0;
#endif
    byte*  tsip_encRsaKeyIdx;

    if (cert == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_CERT_REQ
    if (type == CERTREQ_TYPE)
        cert->isCSR = 1;
#endif

    if (cert->sigCtx.state == SIG_STATE_BEGIN) {
#ifndef WOLFSSL_ASN_TEMPLATE
        cert->badDate = 0;
        cert->criticalExt = 0;
        if ((ret = DecodeToKey(cert, verify)) < 0) {
            if (ret == ASN_BEFORE_DATE_E || ret == ASN_AFTER_DATE_E)
                cert->badDate = ret;
            else
                return ret;
        }

        WOLFSSL_MSG("Parsed Past Key");


#ifdef WOLFSSL_CERT_REQ
        /* Read attributes */
        if (cert->isCSR) {
            if (GetASNHeader_ex(cert->source,
                    ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED, &cert->srcIdx,
                    &len, cert->maxIdx, 1) < 0) {
                WOLFSSL_MSG("GetASNHeader_ex error");
                return ASN_PARSE_E;
            }

            if (len) {
                word32 attrMaxIdx = cert->srcIdx + len;
                word32 oid;
                byte   tag;

                if (attrMaxIdx > cert->maxIdx) {
                    WOLFSSL_MSG("Attribute length greater than CSR length");
                    return ASN_PARSE_E;
                }

                while (cert->srcIdx < attrMaxIdx) {
                    /* Attributes have the structure:
                     * SEQ -> OID -> SET -> ATTRIBUTE */
                    if (GetSequence(cert->source, &cert->srcIdx, &len,
                            attrMaxIdx) < 0) {
                        WOLFSSL_MSG("attr GetSequence error");
                        return ASN_PARSE_E;
                    }
                    if (GetObjectId(cert->source, &cert->srcIdx, &oid,
                            oidCsrAttrType, attrMaxIdx) < 0) {
                        WOLFSSL_MSG("attr GetObjectId error");
                        return ASN_PARSE_E;
                    }
                    if (GetSet(cert->source, &cert->srcIdx, &len,
                            attrMaxIdx) < 0) {
                        WOLFSSL_MSG("attr GetSet error");
                        return ASN_PARSE_E;
                    }
                    switch (oid) {
                    case CHALLENGE_PASSWORD_OID:
                        if (GetHeader(cert->source, &tag,
                                &cert->srcIdx, &len, attrMaxIdx, 1) < 0) {
                            WOLFSSL_MSG("attr GetHeader error");
                            return ASN_PARSE_E;
                        }
                        if (tag != ASN_PRINTABLE_STRING && tag != ASN_UTF8STRING &&
                                tag != ASN_IA5_STRING) {
                            WOLFSSL_MSG("Unsupported attribute value format");
                            return ASN_PARSE_E;
                        }
                        cert->cPwd = (char*)cert->source + cert->srcIdx;
                        cert->cPwdLen = len;
                        cert->srcIdx += len;
                        break;
                    case SERIAL_NUMBER_OID:
                        if (GetHeader(cert->source, &tag,
                                &cert->srcIdx, &len, attrMaxIdx, 1) < 0) {
                            WOLFSSL_MSG("attr GetHeader error");
                            return ASN_PARSE_E;
                        }
                        if (tag != ASN_PRINTABLE_STRING && tag != ASN_UTF8STRING &&
                                tag != ASN_IA5_STRING) {
                            WOLFSSL_MSG("Unsupported attribute value format");
                            return ASN_PARSE_E;
                        }
                        cert->sNum = (char*)cert->source + cert->srcIdx;
                        cert->sNumLen = len;
                        cert->srcIdx += len;
                        if (cert->sNumLen <= EXTERNAL_SERIAL_SIZE) {
                            XMEMCPY(cert->serial, cert->sNum, cert->sNumLen);
                            cert->serialSz = cert->sNumLen;
                        }
                        break;
                    case EXTENSION_REQUEST_OID:
                        /* save extensions */
                        cert->extensions    = &cert->source[cert->srcIdx];
                        cert->extensionsSz  = len;
                        cert->extensionsIdx = cert->srcIdx; /* for potential later use */

                        if ((ret = DecodeCertExtensions(cert)) < 0) {
                            if (ret == ASN_CRIT_EXT_E)
                                cert->criticalExt = ret;
                            else
                                return ret;
                        }
                        cert->srcIdx += len;
                        break;
                    default:
                        WOLFSSL_MSG("Unsupported attribute type");
                        return ASN_PARSE_E;
                    }
                }
            }
        }
#endif

        if (cert->srcIdx < cert->sigIndex) {
        #ifndef ALLOW_V1_EXTENSIONS
            if (cert->version < 2) {
                WOLFSSL_MSG("\tv1 and v2 certs not allowed extensions");
                return ASN_VERSION_E;
            }
        #endif

            /* save extensions */
            cert->extensions    = &cert->source[cert->srcIdx];
            cert->extensionsSz  = cert->sigIndex - cert->srcIdx;
            cert->extensionsIdx = cert->srcIdx;   /* for potential later use */

            if ((ret = DecodeCertExtensions(cert)) < 0) {
                if (ret == ASN_CRIT_EXT_E)
                    cert->criticalExt = ret;
                else
                    return ret;
            }

        #ifdef HAVE_OCSP
            if (verify == VERIFY_OCSP_CERT) {
                /* trust for the lifetime of the responder's cert*/
                if (cert->ocspNoCheckSet)
                    verify = VERIFY;
                else
                    verify = VERIFY_OCSP;
            }
        #endif
            /* advance past extensions */
            cert->srcIdx = cert->sigIndex;
        }

        if ((ret = GetAlgoId(cert->source, &cert->srcIdx,
#ifdef WOLFSSL_CERT_REQ
                !cert->isCSR ? &confirmOID : &cert->signatureOID,
#else
                &confirmOID,
#endif
                oidSigType, cert->maxIdx)) < 0)
            return ret;

        if ((ret = GetSignature(cert)) < 0)
            return ret;

        if (confirmOID != cert->signatureOID
#ifdef WOLFSSL_CERT_REQ
                && !cert->isCSR
#endif
                )
            return ASN_SIG_OID_E;
#else
#ifdef WOLFSSL_CERT_REQ
        if (cert->isCSR) {
            ret = DecodeCertReq(cert, &cert->criticalExt);
            if (ret < 0) {
                return ret;
            }
        }
        else
#endif
        {
            ret = DecodeCert(cert, verify, &cert->criticalExt);
            if (ret == ASN_BEFORE_DATE_E || ret == ASN_AFTER_DATE_E)
                cert->badDate = ret;
            else if (ret < 0)
                return ret;
        }
#endif

    #ifndef NO_SKID
        if (cert->extSubjKeyIdSet == 0 && cert->publicKey != NULL &&
                                                         cert->pubKeySize > 0) {
            ret = CalcHashId(cert->publicKey, cert->pubKeySize,
                                                            cert->extSubjKeyId);
            if (ret != 0)
                return ret;
        }
    #endif /* !NO_SKID */

        if (!cert->selfSigned || (verify != NO_VERIFY && type != CA_TYPE &&
                                                   type != TRUSTED_PEER_TYPE)) {
            cert->ca = NULL;
    #ifndef NO_SKID
            if (cert->extAuthKeyIdSet) {
                cert->ca = GetCA(cm, cert->extAuthKeyId);
            }
            if (cert->ca == NULL && cert->extSubjKeyIdSet
                                 && verify != VERIFY_OCSP) {
                cert->ca = GetCA(cm, cert->extSubjKeyId);
            }
            if (cert->ca != NULL && XMEMCMP(cert->issuerHash,
                                  cert->ca->subjectNameHash, KEYID_SIZE) != 0) {
                cert->ca = NULL;
            }
            if (cert->ca == NULL) {
                cert->ca = GetCAByName(cm, cert->issuerHash);
                /* If AKID is available then this CA doesn't have the public
                 * key required */
                if (cert->ca && cert->extAuthKeyIdSet) {
                    WOLFSSL_MSG("CA SKID doesn't match AKID");
                    cert->ca = NULL;
                }
            }

            /* OCSP Only: alt lookup using subject and pub key w/o sig check */
        #ifdef WOLFSSL_NO_TRUSTED_CERTS_VERIFY
            if (cert->ca == NULL && verify == VERIFY_OCSP) {
                cert->ca = GetCABySubjectAndPubKey(cert, cm);
                if (cert->ca) {
                    ret = 0; /* success */
                    goto exit_pcr;
                }
            }
        #endif /* WOLFSSL_NO_TRUSTED_CERTS_VERIFY */
    #else
            cert->ca = GetCA(cm, cert->issuerHash);
    #endif /* !NO_SKID */

            if (cert->ca) {
                WOLFSSL_MSG("CA found");
            }
        }

        if (cert->selfSigned) {
            cert->maxPathLen = WOLFSSL_MAX_PATH_LEN;
        } else {
            /* RFC 5280 Section 4.2.1.9:
             *
             * load/receive check
             *
             * 1) Is CA boolean set?
             *      No  - SKIP CHECK
             *      Yes - Check key usage
             * 2) Is Key usage extension present?
             *      No  - goto 3
             *      Yes - check keyCertSign assertion
             *     2.a) Is keyCertSign asserted?
             *          No  - goto 4
             *          Yes - goto 3
             * 3) Is pathLen set?
             *      No  - goto 4
             *      Yes - check pathLen against maxPathLen.
             *      3.a) Is pathLen less than maxPathLen?
             *           No - goto 4
             *           Yes - set maxPathLen to pathLen and EXIT
             * 4) Is maxPathLen > 0?
             *      Yes - Reduce by 1
             *      No  - ERROR
             */

            if (cert->ca && cert->pathLengthSet) {
                cert->maxPathLen = cert->pathLength;
                if (cert->isCA) {
                    WOLFSSL_MSG("\tCA boolean set");
                    if (cert->extKeyUsageSet) {
                         WOLFSSL_MSG("\tExtension Key Usage Set");
                         if ((cert->extKeyUsage & KEYUSE_KEY_CERT_SIGN) != 0) {
                            checkPathLen = 1;
                         } else {
                            decrementMaxPathLen = 1;
                         }
                    } else {
                        checkPathLen = 1;
                    } /* !cert->ca check */
                } /* cert is not a CA (assuming entity cert) */

                if (checkPathLen && cert->pathLengthSet) {
                    if (cert->pathLength < cert->ca->maxPathLen) {
                        WOLFSSL_MSG("\tmaxPathLen status: set to pathLength");
                        cert->maxPathLen = cert->pathLength;
                    } else {
                        decrementMaxPathLen = 1;
                    }
                }

                if (decrementMaxPathLen && cert->ca->maxPathLen > 0) {
                    WOLFSSL_MSG("\tmaxPathLen status: reduce by 1");
                    cert->maxPathLen = cert->ca->maxPathLen - 1;
                    if (verify != NO_VERIFY && type != CA_TYPE &&
                                                    type != TRUSTED_PEER_TYPE) {
                        WOLFSSL_MSG("\tmaxPathLen status: OK");
                    }
                } else if (decrementMaxPathLen && cert->ca->maxPathLen == 0) {
                    cert->maxPathLen = 0;
                    if (verify != NO_VERIFY && type != CA_TYPE &&
                                                    type != TRUSTED_PEER_TYPE) {
                        WOLFSSL_MSG("\tNon-entity cert, maxPathLen is 0");
                        WOLFSSL_MSG("\tmaxPathLen status: ERROR");
                        return ASN_PATHLEN_INV_E;
                    }
                }
            } else if (cert->ca && cert->isCA) {
                /* case where cert->pathLength extension is not set */
                if (cert->ca->maxPathLen > 0) {
                    cert->maxPathLen = cert->ca->maxPathLen - 1;
                } else {
                    cert->maxPathLen = 0;
                    if (verify != NO_VERIFY && type != CA_TYPE &&
                                                    type != TRUSTED_PEER_TYPE) {
                        WOLFSSL_MSG("\tNon-entity cert, maxPathLen is 0");
                        WOLFSSL_MSG("\tmaxPathLen status: ERROR");
                        return ASN_PATHLEN_INV_E;
                    }
                }
            }
        }

        #ifdef HAVE_OCSP
        if (verify != NO_VERIFY && type != CA_TYPE &&
                                                type != TRUSTED_PEER_TYPE) {
            if (cert->ca) {
                /* Need the CA's public key hash for OCSP */
                XMEMCPY(cert->issuerKeyHash, cert->ca->subjectKeyHash,
                                                                KEYID_SIZE);
            }
        }
        #endif /* HAVE_OCSP */
    }
#if defined(WOLFSSL_RENESAS_TSIP)
    /* prepare for TSIP TLS cert verification API use */
    if (cert->keyOID == RSAk) {
        /* to call TSIP API, it needs keys position info in bytes */
        if ((ret = RsaPublicKeyDecodeRawIndex(cert->publicKey, (word32*)&idx,
                                   cert->pubKeySize,
                                   &cert->sigCtx.pubkey_n_start,
                                   &cert->sigCtx.pubkey_n_len,
                                   &cert->sigCtx.pubkey_e_start,
                                   &cert->sigCtx.pubkey_e_len)) != 0) {
            WOLFSSL_MSG("Decoding index from cert failed.");
            return ret;
        }
        cert->sigCtx.certBegin = cert->certBegin;
    }
    /* check if we can use TSIP for cert verification */
    /* if the ca is verified as tsip root ca.         */
    /* TSIP can only handle 2048 bits(256 byte) key.  */
    if (cert->ca && tsip_checkCA(cert->ca->cm_idx) != 0 &&
        cert->sigCtx.pubkey_n_len == 256) {

        /* assign memory to encrypted tsip Rsa key index */
        if (!cert->tsip_encRsaKeyIdx)
            cert->tsip_encRsaKeyIdx =
                            (byte*)XMALLOC(TSIP_TLS_ENCPUBKEY_SZ_BY_CERTVRFY,
                             cert->heap, DYNAMIC_TYPE_RSA);
        if (cert->tsip_encRsaKeyIdx == NULL)
                return MEMORY_E;
    } else {
        if (cert->ca) {
            /* TSIP isn't usable */
            if (tsip_checkCA(cert->ca->cm_idx) == 0)
                WOLFSSL_MSG("TSIP isn't usable because the ca isn't verified "
                            "by TSIP.");
            else if (cert->sigCtx.pubkey_n_len != 256)
                WOLFSSL_MSG("TSIP isn't usable because the ca isn't signed by "
                            "RSA 2048.");
            else
                WOLFSSL_MSG("TSIP isn't usable");
        }
        cert->tsip_encRsaKeyIdx = NULL;
    }

    tsip_encRsaKeyIdx = cert->tsip_encRsaKeyIdx;
#else
    tsip_encRsaKeyIdx = NULL;
#endif

    if (verify != NO_VERIFY && type != CA_TYPE && type != TRUSTED_PEER_TYPE) {
        if (cert->ca) {
            if (verify == VERIFY || verify == VERIFY_OCSP ||
                                                 verify == VERIFY_SKIP_DATE) {
                /* try to confirm/verify signature */
                if ((ret = ConfirmSignature(&cert->sigCtx,
                        cert->source + cert->certBegin,
                        cert->sigIndex - cert->certBegin,
                        cert->ca->publicKey, cert->ca->pubKeySize,
                        cert->ca->keyOID, cert->signature,
                        cert->sigLength, cert->signatureOID,
                        tsip_encRsaKeyIdx)) != 0) {
                    if (ret != WC_PENDING_E) {
                        WOLFSSL_MSG("Confirm signature failed");
                    }
                    return ret;
                }
            }
        #ifndef IGNORE_NAME_CONSTRAINTS
            if (verify == VERIFY || verify == VERIFY_OCSP ||
                        verify == VERIFY_NAME || verify == VERIFY_SKIP_DATE) {
                /* check that this cert's name is permitted by the signer's
                 * name constraints */
                if (!ConfirmNameConstraints(cert->ca, cert)) {
                    WOLFSSL_MSG("Confirm name constraint failed");
                    return ASN_NAME_INVALID_E;
                }
            }
        #endif /* IGNORE_NAME_CONSTRAINTS */
        }
        else {
            /* no signer */
            WOLFSSL_MSG("No CA signer to verify with");
            return ASN_NO_SIGNER_E;
        }
    }

#if defined(WOLFSSL_NO_TRUSTED_CERTS_VERIFY) && !defined(NO_SKID)
exit_pcr:
#endif

    if (cert->badDate != 0) {
        if (verify != VERIFY_SKIP_DATE) {
            return cert->badDate;
        }
        WOLFSSL_MSG("Date error: Verify option is skipping");
    }

    if (cert->criticalExt != 0)
        return cert->criticalExt;

    return ret;
}

/* Create and init an new signer */
Signer* MakeSigner(void* heap)
{
    Signer* signer = (Signer*) XMALLOC(sizeof(Signer), heap,
                                       DYNAMIC_TYPE_SIGNER);
    if (signer) {
        XMEMSET(signer, 0, sizeof(Signer));
    }
    (void)heap;

    return signer;
}


/* Free an individual signer.
 *
 * Used by Certificate Manager.
 *
 * @param [in, out] signer  On in, signer object.
 *                          On out, pointer is no longer valid.
 * @param [in]      heap    Dynamic memory hint.
 */
void FreeSigner(Signer* signer, void* heap)
{
    XFREE(signer->name, heap, DYNAMIC_TYPE_SUBJECT_CN);
    XFREE((void*)signer->publicKey, heap, DYNAMIC_TYPE_PUBLIC_KEY);
#ifndef IGNORE_NAME_CONSTRAINTS
    if (signer->permittedNames)
        FreeNameSubtrees(signer->permittedNames, heap);
    if (signer->excludedNames)
        FreeNameSubtrees(signer->excludedNames, heap);
#endif
#ifdef WOLFSSL_SIGNER_DER_CERT
    FreeDer(&signer->derCert);
#endif
    XFREE(signer, heap, DYNAMIC_TYPE_SIGNER);

    (void)heap;
}


/* Free the whole singer table with number of rows.
 *
 * Each table entry is a linked list of signers.
 * Used by Certificate Manager.
 *
 * @param [in, out] table   Array of signer objects.
 * @param [in]      rows    Number of entries in table.
 * @param [in]      heap    Dynamic memory hint.
 */
void FreeSignerTable(Signer** table, int rows, void* heap)
{
    int i;

    for (i = 0; i < rows; i++) {
        Signer* signer = table[i];
        while (signer) {
            Signer* next = signer->next;
            FreeSigner(signer, heap);
            signer = next;
        }
        table[i] = NULL;
    }
}

#ifdef WOLFSSL_TRUST_PEER_CERT
/* Free an individual trusted peer cert.
 *
 * @param [in, out] tp    Trusted peer certificate object.
 * @param [in]      heap  Dynamic memory hint.
 */
void FreeTrustedPeer(TrustedPeerCert* tp, void* heap)
{
    if (tp == NULL) {
        return;
    }

    if (tp->name) {
        XFREE(tp->name, heap, DYNAMIC_TYPE_SUBJECT_CN);
    }

    if (tp->sig) {
        XFREE(tp->sig, heap, DYNAMIC_TYPE_SIGNATURE);
    }
#ifndef IGNORE_NAME_CONSTRAINTS
    if (tp->permittedNames)
        FreeNameSubtrees(tp->permittedNames, heap);
    if (tp->excludedNames)
        FreeNameSubtrees(tp->excludedNames, heap);
#endif
    XFREE(tp, heap, DYNAMIC_TYPE_CERT);

    (void)heap;
}

/* Free the whole Trusted Peer linked list.
 *
 * Each table entry is a linked list of trusted peer certificates.
 * Used by Certificate Manager.
 *
 * @param [in, out] table   Array of trusted peer certificate objects.
 * @param [in]      rows    Number of entries in table.
 * @param [in]      heap    Dynamic memory hint.
 */
void FreeTrustedPeerTable(TrustedPeerCert** table, int rows, void* heap)
{
    int i;

    for (i = 0; i < rows; i++) {
        TrustedPeerCert* tp = table[i];
        while (tp) {
            TrustedPeerCert* next = tp->next;
            FreeTrustedPeer(tp, heap);
            tp = next;
        }
        table[i] = NULL;
    }
}
#endif /* WOLFSSL_TRUST_PEER_CERT */

int SetMyVersion(word32 version, byte* output, int header)
{
    int i = 0;

    if (output == NULL)
        return BAD_FUNC_ARG;

    if (header) {
        output[i++] = ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED;
        output[i++] = 3;
    }
    output[i++] = ASN_INTEGER;
    output[i++] = 0x01;
    output[i++] = (byte)version;

    return i;
}

#if !defined(WOLFSSL_ASN_TEMPLATE) || defined(HAVE_PKCS7)
int SetSerialNumber(const byte* sn, word32 snSz, byte* output,
    word32 outputSz, int maxSnSz)
{
    int i;
    int snSzInt = (int)snSz;

    if (sn == NULL || output == NULL || snSzInt < 0)
        return BAD_FUNC_ARG;

    /* remove leading zeros */
    while (snSzInt > 0 && sn[0] == 0) {
        snSzInt--;
        sn++;
    }
    /* RFC 5280 - 4.1.2.2:
     *   Serial numbers must be a positive value (and not zero) */
    if (snSzInt == 0)
        return BAD_FUNC_ARG;

    if (sn[0] & 0x80)
        maxSnSz--;
    /* truncate if input is too long */
    if (snSzInt > maxSnSz)
        snSzInt = maxSnSz;

    i = SetASNInt(snSzInt, sn[0], NULL);
    /* truncate if input is too long */
    if (snSzInt > (int)outputSz - i)
        snSzInt = (int)outputSz - i;
    /* sanity check number of bytes to copy */
    if (snSzInt <= 0) {
        return BUFFER_E;
    }

    /* write out ASN.1 Integer */
    (void)SetASNInt(snSzInt, sn[0], output);
    XMEMCPY(output + i, sn, snSzInt);

    /* compute final length */
    i += snSzInt;

    return i;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */

#endif /* !NO_CERTS */

#ifndef WOLFSSL_ASN_TEMPLATE
int GetSerialNumber(const byte* input, word32* inOutIdx,
    byte* serial, int* serialSz, word32 maxIdx)
{
    int result = 0;
    int ret;

    WOLFSSL_ENTER("GetSerialNumber");

    if (serial == NULL || input == NULL || serialSz == NULL) {
        return BAD_FUNC_ARG;
    }

    /* First byte is ASN type */
    if ((*inOutIdx+1) > maxIdx) {
        WOLFSSL_MSG("Bad idx first");
        return BUFFER_E;
    }

    ret = GetASNInt(input, inOutIdx, serialSz, maxIdx);
    if (ret != 0)
        return ret;

    if (*serialSz > EXTERNAL_SERIAL_SIZE || *serialSz <= 0) {
        WOLFSSL_MSG("Serial size bad");
        return ASN_PARSE_E;
    }

    /* return serial */
    XMEMCPY(serial, &input[*inOutIdx], (size_t)*serialSz);
    *inOutIdx += *serialSz;

    return result;
}
#endif

#ifndef NO_CERTS

/* TODO: consider moving PEM code out to a different file. */

int AllocDer(DerBuffer** pDer, word32 length, int type, void* heap)
{
    int ret = BAD_FUNC_ARG;
    if (pDer) {
        int dynType = 0;
        DerBuffer* der;

        /* Determine dynamic type */
        switch (type) {
            case CA_TYPE:   dynType = DYNAMIC_TYPE_CA;   break;
            case CERT_TYPE: dynType = DYNAMIC_TYPE_CERT; break;
            case CRL_TYPE:  dynType = DYNAMIC_TYPE_CRL;  break;
            case DSA_TYPE:  dynType = DYNAMIC_TYPE_DSA;  break;
            case ECC_TYPE:  dynType = DYNAMIC_TYPE_ECC;  break;
            case RSA_TYPE:  dynType = DYNAMIC_TYPE_RSA;  break;
            default:        dynType = DYNAMIC_TYPE_KEY;  break;
        }

        /* Setup new buffer */
        *pDer = (DerBuffer*)XMALLOC(sizeof(DerBuffer) + length, heap, dynType);
        if (*pDer == NULL) {
            return MEMORY_E;
        }
        XMEMSET(*pDer, 0, sizeof(DerBuffer) + length);

        der = *pDer;
        der->type = type;
        der->dynType = dynType; /* Cache this for FreeDer */
        der->heap = heap;
        der->buffer = (byte*)der + sizeof(DerBuffer);
        der->length = length;
        ret = 0; /* Success */
    }
    return ret;
}

void FreeDer(DerBuffer** pDer)
{
    if (pDer && *pDer)
    {
        DerBuffer* der = (DerBuffer*)*pDer;

        /* ForceZero private keys */
        if (der->type == PRIVATEKEY_TYPE && der->buffer != NULL) {
            ForceZero(der->buffer, der->length);
        }
        der->buffer = NULL;
        der->length = 0;
        XFREE(der, der->heap, der->dynType);

        *pDer = NULL;
    }
}

int wc_AllocDer(DerBuffer** pDer, word32 length, int type, void* heap)
{
    return AllocDer(pDer, length, type, heap);
}
void wc_FreeDer(DerBuffer** pDer)
{
    FreeDer(pDer);
}


#if defined(WOLFSSL_PEM_TO_DER) || defined(WOLFSSL_DER_TO_PEM)

/* Note: If items added make sure MAX_X509_HEADER_SZ is
    updated to reflect maximum length and pem_struct_min_sz
    to reflect minimum size */
wcchar BEGIN_CERT           = "-----BEGIN CERTIFICATE-----";
wcchar END_CERT             = "-----END CERTIFICATE-----";
#ifdef WOLFSSL_CERT_REQ
    wcchar BEGIN_CERT_REQ   = "-----BEGIN CERTIFICATE REQUEST-----";
    wcchar END_CERT_REQ     = "-----END CERTIFICATE REQUEST-----";
#endif
#ifndef NO_DH
    wcchar BEGIN_DH_PARAM   = "-----BEGIN DH PARAMETERS-----";
    wcchar END_DH_PARAM     = "-----END DH PARAMETERS-----";
#endif
#ifndef NO_DSA
    wcchar BEGIN_DSA_PARAM  = "-----BEGIN DSA PARAMETERS-----";
    wcchar END_DSA_PARAM    = "-----END DSA PARAMETERS-----";
#endif
wcchar BEGIN_X509_CRL       = "-----BEGIN X509 CRL-----";
wcchar END_X509_CRL         = "-----END X509 CRL-----";
wcchar BEGIN_RSA_PRIV       = "-----BEGIN RSA PRIVATE KEY-----";
wcchar END_RSA_PRIV         = "-----END RSA PRIVATE KEY-----";
wcchar BEGIN_RSA_PUB        = "-----BEGIN RSA PUBLIC KEY-----";
wcchar END_RSA_PUB          = "-----END RSA PUBLIC KEY-----";
wcchar BEGIN_PRIV_KEY       = "-----BEGIN PRIVATE KEY-----";
wcchar END_PRIV_KEY         = "-----END PRIVATE KEY-----";
wcchar BEGIN_ENC_PRIV_KEY   = "-----BEGIN ENCRYPTED PRIVATE KEY-----";
wcchar END_ENC_PRIV_KEY     = "-----END ENCRYPTED PRIVATE KEY-----";
#ifdef HAVE_ECC
    wcchar BEGIN_EC_PRIV    = "-----BEGIN EC PRIVATE KEY-----";
    wcchar END_EC_PRIV      = "-----END EC PRIVATE KEY-----";
#endif
#if defined(HAVE_ECC) || defined(HAVE_ED25519) || defined(HAVE_ED448) || \
                                                                !defined(NO_DSA)
    wcchar BEGIN_DSA_PRIV   = "-----BEGIN DSA PRIVATE KEY-----";
    wcchar END_DSA_PRIV     = "-----END DSA PRIVATE KEY-----";
#endif
#ifdef OPENSSL_EXTRA
    const char BEGIN_PRIV_KEY_PREFIX[] = "-----BEGIN";
    const char PRIV_KEY_SUFFIX[] = "PRIVATE KEY-----";
    const char END_PRIV_KEY_PREFIX[]   = "-----END";
#endif
wcchar BEGIN_PUB_KEY        = "-----BEGIN PUBLIC KEY-----";
wcchar END_PUB_KEY          = "-----END PUBLIC KEY-----";
#if defined(HAVE_ED25519) || defined(HAVE_ED448)
    wcchar BEGIN_EDDSA_PRIV = "-----BEGIN EDDSA PRIVATE KEY-----";
    wcchar END_EDDSA_PRIV   = "-----END EDDSA PRIVATE KEY-----";
#endif

const int pem_struct_min_sz = XSTR_SIZEOF("-----BEGIN X509 CRL-----"
                                             "-----END X509 CRL-----");

static WC_INLINE const char* SkipEndOfLineChars(const char* line,
                                                const char* endOfLine)
{
    /* eat end of line characters */
    while (line < endOfLine &&
              (line[0] == '\r' || line[0] == '\n')) {
        line++;
    }
    return line;
}

int wc_PemGetHeaderFooter(int type, const char** header, const char** footer)
{
    int ret = BAD_FUNC_ARG;

    switch (type) {
        case CA_TYPE:       /* same as below */
        case TRUSTED_PEER_TYPE:
        case CERT_TYPE:
            if (header) *header = BEGIN_CERT;
            if (footer) *footer = END_CERT;
            ret = 0;
            break;

        case CRL_TYPE:
            if (header) *header = BEGIN_X509_CRL;
            if (footer) *footer = END_X509_CRL;
            ret = 0;
            break;
    #ifndef NO_DH
        case DH_PARAM_TYPE:
            if (header) *header = BEGIN_DH_PARAM;
            if (footer) *footer = END_DH_PARAM;
            ret = 0;
            break;
    #endif
    #ifndef NO_DSA
        case DSA_PARAM_TYPE:
            if (header) *header = BEGIN_DSA_PARAM;
            if (footer) *footer = END_DSA_PARAM;
            ret = 0;
            break;
    #endif
    #ifdef WOLFSSL_CERT_REQ
        case CERTREQ_TYPE:
            if (header) *header = BEGIN_CERT_REQ;
            if (footer) *footer = END_CERT_REQ;
            ret = 0;
            break;
    #endif
    #ifndef NO_DSA
        case DSA_TYPE:
        case DSA_PRIVATEKEY_TYPE:
            if (header) *header = BEGIN_DSA_PRIV;
            if (footer) *footer = END_DSA_PRIV;
            ret = 0;
            break;
    #endif
    #ifdef HAVE_ECC
        case ECC_TYPE:
        case ECC_PRIVATEKEY_TYPE:
            if (header) *header = BEGIN_EC_PRIV;
            if (footer) *footer = END_EC_PRIV;
            ret = 0;
            break;
    #endif
        case RSA_TYPE:
        case PRIVATEKEY_TYPE:
            if (header) *header = BEGIN_RSA_PRIV;
            if (footer) *footer = END_RSA_PRIV;
            ret = 0;
            break;
    #ifdef HAVE_ED25519
        case ED25519_TYPE:
    #endif
    #ifdef HAVE_ED448
        case ED448_TYPE:
    #endif
    #if defined(HAVE_ED25519) || defined(HAVE_ED448)
        case EDDSA_PRIVATEKEY_TYPE:
            if (header) *header = BEGIN_EDDSA_PRIV;
            if (footer) *footer = END_EDDSA_PRIV;
            ret = 0;
            break;
    #endif
        case PUBLICKEY_TYPE:
        case ECC_PUBLICKEY_TYPE:
            if (header) *header = BEGIN_PUB_KEY;
            if (footer) *footer = END_PUB_KEY;
            ret = 0;
            break;
    #ifndef NO_DH
        case DH_PRIVATEKEY_TYPE:
    #endif
        case PKCS8_PRIVATEKEY_TYPE:
            if (header) *header = BEGIN_PRIV_KEY;
            if (footer) *footer = END_PRIV_KEY;
            ret = 0;
            break;
        case PKCS8_ENC_PRIVATEKEY_TYPE:
            if (header) *header = BEGIN_ENC_PRIV_KEY;
            if (footer) *footer = END_ENC_PRIV_KEY;
            ret = 0;
            break;
        default:
            break;
    }
    return ret;
}

#ifdef WOLFSSL_ENCRYPTED_KEYS

static wcchar kProcTypeHeader = "Proc-Type";
static wcchar kDecInfoHeader = "DEK-Info";

#ifdef WOLFSSL_PEM_TO_DER
#ifndef NO_DES3
    static wcchar kEncTypeDes = "DES-CBC";
    static wcchar kEncTypeDes3 = "DES-EDE3-CBC";
#endif
#if !defined(NO_AES) && defined(HAVE_AES_CBC) && defined(WOLFSSL_AES_128)
    static wcchar kEncTypeAesCbc128 = "AES-128-CBC";
#endif
#if !defined(NO_AES) && defined(HAVE_AES_CBC) && defined(WOLFSSL_AES_192)
    static wcchar kEncTypeAesCbc192 = "AES-192-CBC";
#endif
#if !defined(NO_AES) && defined(HAVE_AES_CBC) && defined(WOLFSSL_AES_256)
    static wcchar kEncTypeAesCbc256 = "AES-256-CBC";
#endif

int wc_EncryptedInfoGet(EncryptedInfo* info, const char* cipherInfo)
{
    int ret = 0;

    if (info == NULL || cipherInfo == NULL)
        return BAD_FUNC_ARG;

    /* determine cipher information */
#ifndef NO_DES3
    if (XSTRNCMP(cipherInfo, kEncTypeDes, XSTRLEN(kEncTypeDes)) == 0) {
        info->cipherType = WC_CIPHER_DES;
        info->keySz = DES_KEY_SIZE;
/* DES_IV_SIZE is incorrectly 16 in FIPS v2. It should be 8, same as the
 * block size. */
#if defined(HAVE_FIPS) && defined(HAVE_FIPS_VERSION) && (HAVE_FIPS_VERSION == 2)
        if (info->ivSz == 0) info->ivSz  = DES_BLOCK_SIZE;
#else
        if (info->ivSz == 0) info->ivSz  = DES_IV_SIZE;
#endif
    }
    else if (XSTRNCMP(cipherInfo, kEncTypeDes3, XSTRLEN(kEncTypeDes3)) == 0) {
        info->cipherType = WC_CIPHER_DES3;
        info->keySz = DES3_KEY_SIZE;
#if defined(HAVE_FIPS) && defined(HAVE_FIPS_VERSION) && (HAVE_FIPS_VERSION == 2)
        if (info->ivSz == 0) info->ivSz  = DES_BLOCK_SIZE;
#else
        if (info->ivSz == 0) info->ivSz  = DES_IV_SIZE;
#endif
    }
    else
#endif /* !NO_DES3 */
#if !defined(NO_AES) && defined(HAVE_AES_CBC) && defined(WOLFSSL_AES_128)
    if (XSTRNCMP(cipherInfo, kEncTypeAesCbc128, XSTRLEN(kEncTypeAesCbc128)) == 0) {
        info->cipherType = WC_CIPHER_AES_CBC;
        info->keySz = AES_128_KEY_SIZE;
        if (info->ivSz == 0) info->ivSz  = AES_IV_SIZE;
    }
    else
#endif
#if !defined(NO_AES) && defined(HAVE_AES_CBC) && defined(WOLFSSL_AES_192)
    if (XSTRNCMP(cipherInfo, kEncTypeAesCbc192, XSTRLEN(kEncTypeAesCbc192)) == 0) {
        info->cipherType = WC_CIPHER_AES_CBC;
        info->keySz = AES_192_KEY_SIZE;
        if (info->ivSz == 0) info->ivSz  = AES_IV_SIZE;
    }
    else
#endif
#if !defined(NO_AES) && defined(HAVE_AES_CBC) && defined(WOLFSSL_AES_256)
    if (XSTRNCMP(cipherInfo, kEncTypeAesCbc256, XSTRLEN(kEncTypeAesCbc256)) == 0) {
        info->cipherType = WC_CIPHER_AES_CBC;
        info->keySz = AES_256_KEY_SIZE;
        if (info->ivSz == 0) info->ivSz  = AES_IV_SIZE;
    }
    else
#endif
    {
        ret = NOT_COMPILED_IN;
    }
    return ret;
}

int wc_EncryptedInfoParse(EncryptedInfo* info, const char** pBuffer,
                          size_t bufSz)
{
    int         err = 0;
    const char* bufferStart;
    const char* bufferEnd;
    char*       line;
    word32      lineSz;
    char*       finish;
    word32      finishSz;
    char*       start = NULL;
    word32      startSz;
    const char* newline = NULL;

    if (info == NULL || pBuffer == NULL || bufSz == 0)
        return BAD_FUNC_ARG;

    bufferStart = *pBuffer;
    bufferEnd = bufferStart + bufSz;

    /* find encrypted info marker */
    line = XSTRNSTR(bufferStart, kProcTypeHeader,
                    min((word32)bufSz, PEM_LINE_LEN));
    if (line != NULL) {
        if (line >= bufferEnd) {
            return BUFFER_E;
        }

        lineSz = (word32)(bufferEnd - line);

        /* find DEC-Info marker */
        start = XSTRNSTR(line, kDecInfoHeader, min(lineSz, PEM_LINE_LEN));

        if (start == NULL)
            return BUFFER_E;

        /* skip dec-info and ": " */
        start += XSTRLEN(kDecInfoHeader);
        if (start >= bufferEnd)
            return BUFFER_E;

        if (start[0] == ':') {
            start++;
            if (start >= bufferEnd)
                return BUFFER_E;
        }
        if (start[0] == ' ')
            start++;

        startSz = (word32)(bufferEnd - start);
        finish = XSTRNSTR(start, ",", min(startSz, PEM_LINE_LEN));

        if ((start != NULL) && (finish != NULL) && (start < finish)) {
            if (finish >= bufferEnd) {
                return BUFFER_E;
            }

            finishSz = (word32)(bufferEnd - finish);
            newline = XSTRNSTR(finish, "\r", min(finishSz, PEM_LINE_LEN));

            /* get cipher name */
            if (NAME_SZ < (finish - start)) /* buffer size of info->name */
                return BUFFER_E;
            if (XMEMCPY(info->name, start, finish - start) == NULL)
                return BUFFER_E;
            info->name[finish - start] = '\0'; /* null term */

            /* populate info */
            err = wc_EncryptedInfoGet(info, info->name);
            if (err != 0)
                return err;

            /* get IV */
            if (finishSz < info->ivSz + 1)
                return BUFFER_E;

            if (newline == NULL) {
                newline = XSTRNSTR(finish, "\n", min(finishSz,
                                                     PEM_LINE_LEN));
            }
            if ((newline != NULL) && (newline > finish)) {
                finish++;
                info->ivSz = (word32)(newline - finish);
                if (info->ivSz > IV_SZ)
                    return BUFFER_E;
                if (XMEMCPY(info->iv, finish, info->ivSz) == NULL)
                    return BUFFER_E;
                info->set = 1;
            }
            else
                return BUFFER_E;
        }
        else
            return BUFFER_E;

        /* eat end of line characters */
        newline = SkipEndOfLineChars(newline, bufferEnd);

        /* return new headerEnd */

        *pBuffer = newline;
    }

    return err;
}
#endif /* WOLFSSL_PEM_TO_DER */

#ifdef WOLFSSL_DER_TO_PEM
static int wc_EncryptedInfoAppend(char* dest, int destSz, char* cipherInfo)
{
    if (cipherInfo != NULL) {
        int cipherInfoStrLen = (int)XSTRLEN((char*)cipherInfo);

        if (cipherInfoStrLen > HEADER_ENCRYPTED_KEY_SIZE - (9+14+10+3))
            cipherInfoStrLen = HEADER_ENCRYPTED_KEY_SIZE - (9+14+10+3);

        if (destSz - (int)XSTRLEN(dest) >= cipherInfoStrLen + (9+14+8+2+2+1)) {
            /* strncat's src length needs to include the NULL */
            XSTRNCAT(dest, kProcTypeHeader, 10);
            XSTRNCAT(dest, ": 4,ENCRYPTED\n", 15);
            XSTRNCAT(dest, kDecInfoHeader, 9);
            XSTRNCAT(dest, ": ", 3);
            XSTRNCAT(dest, cipherInfo, destSz - (int)XSTRLEN(dest) - 1);
            XSTRNCAT(dest, "\n\n", 4);
        }
    }
    return 0;
}
#endif /* WOLFSSL_DER_TO_PEM */
#endif /* WOLFSSL_ENCRYPTED_KEYS */

#ifdef WOLFSSL_DER_TO_PEM

/* Used for compatibility API */
int wc_DerToPem(const byte* der, word32 derSz,
                byte* output, word32 outSz, int type)
{
    return wc_DerToPemEx(der, derSz, output, outSz, NULL, type);
}

/* convert der buffer to pem into output, can't do inplace, der and output
   need to be different */
int wc_DerToPemEx(const byte* der, word32 derSz, byte* output, word32 outSz,
             byte *cipher_info, int type)
{
    const char* headerStr = NULL;
    const char* footerStr = NULL;
#ifdef WOLFSSL_SMALL_STACK
    char* header = NULL;
    char* footer = NULL;
#else
    char header[MAX_X509_HEADER_SZ + HEADER_ENCRYPTED_KEY_SIZE];
    char footer[MAX_X509_HEADER_SZ];
#endif
    int headerLen = MAX_X509_HEADER_SZ + HEADER_ENCRYPTED_KEY_SIZE;
    int footerLen = MAX_X509_HEADER_SZ;
    int i;
    int err;
    int outLen;   /* return length or error */

    (void)cipher_info;

    if (der == output)      /* no in place conversion */
        return BAD_FUNC_ARG;

    err = wc_PemGetHeaderFooter(type, &headerStr, &footerStr);
    if (err != 0)
        return err;

#ifdef WOLFSSL_SMALL_STACK
    header = (char*)XMALLOC(headerLen, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    if (header == NULL)
        return MEMORY_E;

    footer = (char*)XMALLOC(footerLen, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    if (footer == NULL) {
        XFREE(header, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }
#endif

    /* build header and footer based on type */
    XSTRNCPY(header, headerStr, headerLen - 1);
    header[headerLen - 2] = 0;
    XSTRNCPY(footer, footerStr, footerLen - 1);
    footer[footerLen - 2] = 0;

    /* add new line to end */
    XSTRNCAT(header, "\n", 2);
    XSTRNCAT(footer, "\n", 2);

#ifdef WOLFSSL_ENCRYPTED_KEYS
    err = wc_EncryptedInfoAppend(header, headerLen, (char*)cipher_info);
    if (err != 0) {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(header, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return err;
    }
#endif

    headerLen = (int)XSTRLEN(header);
    footerLen = (int)XSTRLEN(footer);

    /* if null output and 0 size passed in then return size needed */
    if (!output && outSz == 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(header, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        outLen = 0;
        if ((err = Base64_Encode(der, derSz, NULL, (word32*)&outLen))
                != LENGTH_ONLY_E) {
            return err;
        }
        return headerLen + footerLen + outLen;
    }

    if (!der || !output) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(header, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return BAD_FUNC_ARG;
    }

    /* don't even try if outSz too short */
    if (outSz < headerLen + footerLen + derSz) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(header, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return BAD_FUNC_ARG;
    }

    /* header */
    XMEMCPY(output, header, headerLen);
    i = headerLen;

#ifdef WOLFSSL_SMALL_STACK
    XFREE(header, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    /* body */
    outLen = outSz - (headerLen + footerLen);  /* input to Base64_Encode */
    if ( (err = Base64_Encode(der, derSz, output + i, (word32*)&outLen)) < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return err;
    }
    i += outLen;

    /* footer */
    if ( (i + footerLen) > (int)outSz) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return BAD_FUNC_ARG;
    }
    XMEMCPY(output + i, footer, footerLen);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(footer, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return outLen + headerLen + footerLen;
}

#endif /* WOLFSSL_DER_TO_PEM */

#ifdef WOLFSSL_PEM_TO_DER

/* Remove PEM header/footer, convert to ASN1, store any encrypted data
   info->consumed tracks of PEM bytes consumed in case multiple parts */
int PemToDer(const unsigned char* buff, long longSz, int type,
              DerBuffer** pDer, void* heap, EncryptedInfo* info, int* keyFormat)
{
    const char* header      = NULL;
    const char* footer      = NULL;
    const char* headerEnd;
    const char* footerEnd;
    const char* consumedEnd;
    const char* bufferEnd   = (const char*)(buff + longSz);
    long        neededSz;
    int         ret         = 0;
    int         sz          = (int)longSz;
    int         encrypted_key = 0;
    DerBuffer*  der;
    word32      algId = 0;
    word32      idx;
#if defined(WOLFSSL_ENCRYPTED_KEYS)
    #if defined(WOLFSSL_ENCRYPTED_KEYS) && !defined(NO_DES3) && \
        !defined(NO_WOLFSSL_SKIP_TRAILING_PAD)
        int     padVal = 0;
    #endif
#endif
#ifdef OPENSSL_EXTRA
    char        beginBuf[PEM_LINE_LEN + 1]; /* add 1 for null terminator */
    char        endBuf[PEM_LINE_LEN + 1];   /* add 1 for null terminator */
#endif

    WOLFSSL_ENTER("PemToDer");

    /* get PEM header and footer based on type */
    ret = wc_PemGetHeaderFooter(type, &header, &footer);
    if (ret != 0)
        return ret;

    /* map header if not found for type */
    for (;;) {
        headerEnd = XSTRNSTR((char*)buff, header, sz);

        if (headerEnd) {
            break;
        }

        if (type == PRIVATEKEY_TYPE) {
            if (header == BEGIN_RSA_PRIV) {
                header = BEGIN_PRIV_KEY;
                footer = END_PRIV_KEY;
            }
            else if (header == BEGIN_PRIV_KEY) {
                header = BEGIN_ENC_PRIV_KEY;
                footer = END_ENC_PRIV_KEY;
            }
#ifdef HAVE_ECC
            else if (header == BEGIN_ENC_PRIV_KEY) {
                header = BEGIN_EC_PRIV;
                footer = END_EC_PRIV;
            }
            else if (header == BEGIN_EC_PRIV) {
                header = BEGIN_DSA_PRIV;
                footer = END_DSA_PRIV;
            }
#endif
#if defined(HAVE_ED25519) || defined(HAVE_ED448)
    #ifdef HAVE_ECC
            else if (header == BEGIN_DSA_PRIV) {
    #else
            else if (header == BEGIN_ENC_PRIV_KEY) {
    #endif
                header = BEGIN_EDDSA_PRIV;
                footer = END_EDDSA_PRIV;
            }
#endif
            else {
                break;
            }
        }
        else if (type == PUBLICKEY_TYPE) {
            if (header == BEGIN_PUB_KEY) {
                header = BEGIN_RSA_PUB;
                footer = END_RSA_PUB;
            }
            else {
                break;
            }
        }
#ifdef HAVE_CRL
        else if ((type == CRL_TYPE) && (header != BEGIN_X509_CRL)) {
            header =  BEGIN_X509_CRL;
            footer = END_X509_CRL;
        }
#endif
        else {
            break;
        }
    }

    if (!headerEnd) {
#ifdef OPENSSL_EXTRA
        if (type == PRIVATEKEY_TYPE) {
            const char* beginEnd;
            int endLen;
            /* see if there is a -----BEGIN * PRIVATE KEY----- header */
            headerEnd = XSTRNSTR((char*)buff, PRIV_KEY_SUFFIX, sz);
            if (headerEnd) {
                beginEnd = headerEnd + XSTR_SIZEOF(PRIV_KEY_SUFFIX);
                if (beginEnd >= (char*)buff + sz) {
                    return BUFFER_E;
                }

                /* back up to BEGIN_PRIV_KEY_PREFIX */
                while (headerEnd > (char*)buff &&
                        XSTRNCMP(headerEnd, BEGIN_PRIV_KEY_PREFIX,
                                XSTR_SIZEOF(BEGIN_PRIV_KEY_PREFIX)) != 0 &&
                        *headerEnd != '\n') {
                    headerEnd--;
                }
                if (headerEnd <= (char*)buff ||
                        XSTRNCMP(headerEnd, BEGIN_PRIV_KEY_PREFIX,
                        XSTR_SIZEOF(BEGIN_PRIV_KEY_PREFIX)) != 0 ||
                        beginEnd - headerEnd > PEM_LINE_LEN) {
                    WOLFSSL_MSG("Couldn't find PEM header");
                    WOLFSSL_ERROR(ASN_NO_PEM_HEADER);
                    return ASN_NO_PEM_HEADER;
                }

                /* headerEnd now points to beginning of header */
                XMEMCPY(beginBuf, headerEnd, beginEnd - headerEnd);
                beginBuf[beginEnd - headerEnd] = '\0';
                /* look for matching footer */
                footer = XSTRNSTR(beginEnd,
                                beginBuf + XSTR_SIZEOF(BEGIN_PRIV_KEY_PREFIX),
                                (unsigned int)((char*)buff + sz - beginEnd));
                if (!footer) {
                    WOLFSSL_MSG("Couldn't find PEM footer");
                    WOLFSSL_ERROR(ASN_NO_PEM_HEADER);
                    return ASN_NO_PEM_HEADER;
                }

                footer -= XSTR_SIZEOF(END_PRIV_KEY_PREFIX);
                if (footer > (char*)buff + sz - XSTR_SIZEOF(END_PRIV_KEY_PREFIX)
                        || XSTRNCMP(footer, END_PRIV_KEY_PREFIX,
                            XSTR_SIZEOF(END_PRIV_KEY_PREFIX)) != 0) {
                    WOLFSSL_MSG("Unexpected footer for PEM");
                    return BUFFER_E;
                }

                endLen = (unsigned int)(beginEnd - headerEnd -
                            (XSTR_SIZEOF(BEGIN_PRIV_KEY_PREFIX) -
                                    XSTR_SIZEOF(END_PRIV_KEY_PREFIX)));
                XMEMCPY(endBuf, footer, endLen);
                endBuf[endLen] = '\0';

                header = beginBuf;
                footer = endBuf;
                headerEnd = beginEnd;
            }
        }

        if (!headerEnd) {
            WOLFSSL_MSG("Couldn't find PEM header");
            WOLFSSL_ERROR(ASN_NO_PEM_HEADER);
            return ASN_NO_PEM_HEADER;
        }
#else
        WOLFSSL_MSG("Couldn't find PEM header");
        return ASN_NO_PEM_HEADER;
#endif
    } else {
        headerEnd += XSTRLEN(header);
    }

    /* eat end of line characters */
    headerEnd = SkipEndOfLineChars(headerEnd, bufferEnd);

    if (type == PRIVATEKEY_TYPE) {
        /* keyFormat is Key_Sum enum */
        if (keyFormat) {
        #ifdef HAVE_ECC
            if (header == BEGIN_EC_PRIV)
                *keyFormat = ECDSAk;
        #endif
        #if !defined(NO_DSA)
            if (header == BEGIN_DSA_PRIV)
                *keyFormat = DSAk;
        #endif
        }
    }

#ifdef WOLFSSL_ENCRYPTED_KEYS
    if (info) {
        ret = wc_EncryptedInfoParse(info, &headerEnd, bufferEnd - headerEnd);
        if (ret < 0)
            return ret;
        if (info->set)
            encrypted_key = 1;
    }
#endif /* WOLFSSL_ENCRYPTED_KEYS */

    /* find footer */
    footerEnd = XSTRNSTR(headerEnd, footer, (unsigned int)((char*)buff +
        sz - headerEnd));
    if (!footerEnd) {
        if (info)
            info->consumed = longSz; /* No more certs if no footer */
        return BUFFER_E;
    }

    consumedEnd = footerEnd + XSTRLEN(footer);

    if (consumedEnd < bufferEnd) { /* handle no end of line on last line */
        /* eat end of line characters */
        consumedEnd = SkipEndOfLineChars(consumedEnd, bufferEnd);
        /* skip possible null term */
        if (consumedEnd < bufferEnd && consumedEnd[0] == '\0')
            consumedEnd++;
    }

    if (info)
        info->consumed = (long)(consumedEnd - (const char*)buff);

    /* set up der buffer */
    neededSz = (long)(footerEnd - headerEnd);
    if (neededSz > sz || neededSz <= 0)
        return BUFFER_E;

    ret = AllocDer(pDer, (word32)neededSz, type, heap);
    if (ret < 0) {
        return ret;
    }
    der = *pDer;

    if (Base64_Decode((byte*)headerEnd, (word32)neededSz,
                      der->buffer, &der->length) < 0) {
        WOLFSSL_ERROR(BUFFER_E);
        return BUFFER_E;
    }

    if ((header == BEGIN_PRIV_KEY
#ifdef OPENSSL_EXTRA
         || header == beginBuf
#endif
#ifdef HAVE_ECC
         || header == BEGIN_EC_PRIV
#endif
        ) && !encrypted_key)
    {
        /* detect pkcs8 key and get alg type */
        /* keep PKCS8 header */
        idx = 0;
        ret = ToTraditionalInline_ex(der->buffer, &idx, der->length, &algId);
        if (ret > 0) {
            if (keyFormat)
                *keyFormat = algId;
        }
        else {
            /* ignore failure here and assume key is not pkcs8 wrapped */
        }
        return 0;
    }


#ifdef WOLFSSL_ENCRYPTED_KEYS
    if (encrypted_key || header == BEGIN_ENC_PRIV_KEY) {
        int   passwordSz = NAME_SZ;
    #ifdef WOLFSSL_SMALL_STACK
        char* password = NULL;
    #else
        char  password[NAME_SZ];
    #endif

        if (!info || !info->passwd_cb) {
            WOLFSSL_MSG("No password callback set");
            return NO_PASSWORD;
        }

    #ifdef WOLFSSL_SMALL_STACK
        password = (char*)XMALLOC(passwordSz, heap, DYNAMIC_TYPE_STRING);
        if (password == NULL)
            return MEMORY_E;
    #endif

        /* get password */
        ret = info->passwd_cb(password, passwordSz, PEM_PASS_READ,
            info->passwd_userdata);
        if (ret >= 0) {
            passwordSz = ret;

            /* convert and adjust length */
            if (header == BEGIN_ENC_PRIV_KEY) {
            #ifndef NO_PWDBASED
                ret = wc_DecryptPKCS8Key(der->buffer, der->length,
                    password, passwordSz);
                if (ret > 0) {
                    /* update length by decrypted content */
                    der->length = ret;
                    idx = 0;
                    /* detect pkcs8 key and get alg type */
                    /* keep PKCS8 header */
                    ret = ToTraditionalInline_ex(der->buffer, &idx, der->length,
                        &algId);
                    if (ret >= 0) {
                        if (keyFormat)
                            *keyFormat = algId;
                        ret = 0;
                    }
                }
            #else
                ret = NOT_COMPILED_IN;
            #endif
            }
            /* decrypt the key */
            else {
                if (passwordSz == 0) {
                    /* The key is encrypted but does not have a password */
                    WOLFSSL_MSG("No password for encrypted key");
                    ret = NO_PASSWORD;
                }
                else {
                    ret = wc_BufferKeyDecrypt(info, der->buffer, der->length,
                        (byte*)password, passwordSz, WC_MD5);

#ifndef NO_WOLFSSL_SKIP_TRAILING_PAD
                #ifndef NO_DES3
                    if (info->cipherType == WC_CIPHER_DES3) {
                        /* Assuming there is padding:
                         *      (der->length > 0 && der->length > DES_BLOCK_SIZE &&
                         *       (der->length % DES_BLOCK_SIZE) != 0)
                         * and assuming the last value signifies the number of
                         * padded bytes IE if last value is 0x08 then there are
                         * 8 bytes of padding:
                         *      padVal = der->buffer[der->length-1];
                         * then strip this padding before proceeding:
                         * der->length -= padVal;
                         */
                        if (der->length > DES_BLOCK_SIZE &&
                            (der->length % DES_BLOCK_SIZE) != 0) {
                            padVal = der->buffer[der->length-1];
                            if (padVal < DES_BLOCK_SIZE) {
                                der->length -= padVal;
                            }
                        }
                    }
                #endif /* !NO_DES3 */
#endif /* !NO_WOLFSSL_SKIP_TRAILING_PAD */
                }
            }
#ifdef OPENSSL_EXTRA
            if (ret) {
                PEMerr(0, PEM_R_BAD_DECRYPT);
            }
#endif
            ForceZero(password, passwordSz);
        }
#ifdef OPENSSL_EXTRA
        else {
            PEMerr(0, PEM_R_BAD_PASSWORD_READ);
        }
#endif

    #ifdef WOLFSSL_SMALL_STACK
        XFREE(password, heap, DYNAMIC_TYPE_STRING);
    #endif
    }
#endif /* WOLFSSL_ENCRYPTED_KEYS */

    return ret;
}

int wc_PemToDer(const unsigned char* buff, long longSz, int type,
              DerBuffer** pDer, void* heap, EncryptedInfo* info, int* keyFormat)
{
    int ret = PemToDer(buff, longSz, type, pDer, heap, info, keyFormat);
#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
    if (ret == 0 && type == PRIVATEKEY_TYPE) {
        DerBuffer* der = *pDer;
        /* if a PKCS8 key header exists remove it */
        ret = ToTraditional(der->buffer, der->length);
        if (ret > 0) {
            der->length = ret;
            ret = 0;
        }
    }
#endif
    return ret;
}


/* our KeyPemToDer password callback, password in userData */
static int KeyPemToDerPassCb(char* passwd, int sz, int rw, void* userdata)
{
    (void)rw;

    if (userdata == NULL)
        return 0;

    XSTRNCPY(passwd, (char*)userdata, sz);
    return min((word32)sz, (word32)XSTRLEN((char*)userdata));
}

/* Return bytes written to buff or < 0 for error */
int wc_KeyPemToDer(const unsigned char* pem, int pemSz,
                        unsigned char* buff, int buffSz, const char* pass)
{
    int ret;
    DerBuffer* der = NULL;
#ifdef WOLFSSL_SMALL_STACK
    EncryptedInfo* info = NULL;
#else
    EncryptedInfo  info[1];
#endif

    WOLFSSL_ENTER("wc_KeyPemToDer");

    if (pem == NULL || buff == NULL || buffSz <= 0) {
        WOLFSSL_MSG("Bad pem der args");
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    info = (EncryptedInfo*)XMALLOC(sizeof(EncryptedInfo), NULL,
                                   DYNAMIC_TYPE_ENCRYPTEDINFO);
    if (info == NULL)
        return MEMORY_E;
#endif

    XMEMSET(info, 0, sizeof(EncryptedInfo));
    info->passwd_cb = KeyPemToDerPassCb;
    info->passwd_userdata = (void*)pass;

    ret = PemToDer(pem, pemSz, PRIVATEKEY_TYPE, &der, NULL, info, NULL);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(info, NULL, DYNAMIC_TYPE_ENCRYPTEDINFO);
#endif

    if (ret < 0 || der == NULL) {
        WOLFSSL_MSG("Bad Pem To Der");
    }
    else {
        if (der->length <= (word32)buffSz) {
            XMEMCPY(buff, der->buffer, der->length);
            ret = der->length;
        }
        else {
            WOLFSSL_MSG("Bad der length");
            ret = BAD_FUNC_ARG;
        }
    }

    FreeDer(&der);
    return ret;
}


/* Return bytes written to buff or < 0 for error */
int wc_CertPemToDer(const unsigned char* pem, int pemSz,
                        unsigned char* buff, int buffSz, int type)
{
    int ret;
    DerBuffer* der = NULL;

    WOLFSSL_ENTER("wc_CertPemToDer");

    if (pem == NULL || buff == NULL || buffSz <= 0) {
        WOLFSSL_MSG("Bad pem der args");
        return BAD_FUNC_ARG;
    }

    if (type != CERT_TYPE && type != CA_TYPE && type != CERTREQ_TYPE) {
        WOLFSSL_MSG("Bad cert type");
        return BAD_FUNC_ARG;
    }


    ret = PemToDer(pem, pemSz, type, &der, NULL, NULL, NULL);
    if (ret < 0 || der == NULL) {
        WOLFSSL_MSG("Bad Pem To Der");
    }
    else {
        if (der->length <= (word32)buffSz) {
            XMEMCPY(buff, der->buffer, der->length);
            ret = der->length;
        }
        else {
            WOLFSSL_MSG("Bad der length");
            ret = BAD_FUNC_ARG;
        }
    }

    FreeDer(&der);
    return ret;
}

#endif /* WOLFSSL_PEM_TO_DER */
#endif /* WOLFSSL_PEM_TO_DER || WOLFSSL_DER_TO_PEM */


#ifdef WOLFSSL_PEM_TO_DER
#if defined(WOLFSSL_CERT_EXT) || defined(WOLFSSL_PUB_PEM_TO_DER)
/* Return bytes written to buff or < 0 for error */
int wc_PubKeyPemToDer(const unsigned char* pem, int pemSz,
                           unsigned char* buff, int buffSz)
{
    int ret;
    DerBuffer* der = NULL;

    WOLFSSL_ENTER("wc_PubKeyPemToDer");

    if (pem == NULL || buff == NULL || buffSz <= 0) {
        WOLFSSL_MSG("Bad pem der args");
        return BAD_FUNC_ARG;
    }

    ret = PemToDer(pem, pemSz, PUBLICKEY_TYPE, &der, NULL, NULL, NULL);
    if (ret < 0 || der == NULL) {
        WOLFSSL_MSG("Bad Pem To Der");
    }
    else {
        if (der->length <= (word32)buffSz) {
            XMEMCPY(buff, der->buffer, der->length);
            ret = der->length;
        }
        else {
            WOLFSSL_MSG("Bad der length");
            ret = BAD_FUNC_ARG;
        }
    }

    FreeDer(&der);
    return ret;
}
#endif /* WOLFSSL_CERT_EXT || WOLFSSL_PUB_PEM_TO_DER */
#endif /* WOLFSSL_PEM_TO_DER */

#ifndef NO_FILESYSTEM

#ifdef WOLFSSL_CERT_GEN
/* load pem cert from file into der buffer, return der size or error */
int wc_PemCertToDer(const char* fileName, unsigned char* derBuf, int derSz)
{
#ifdef WOLFSSL_SMALL_STACK
    byte   staticBuffer[1]; /* force XMALLOC */
#else
    byte   staticBuffer[FILE_BUFFER_SIZE];
#endif
    byte*  fileBuf = staticBuffer;
    int    dynamic = 0;
    int    ret     = 0;
    long   sz      = 0;
    XFILE  file;
    DerBuffer* converted = NULL;

    WOLFSSL_ENTER("wc_PemCertToDer");

    if (fileName == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        file = XFOPEN(fileName, "rb");
        if (file == XBADFILE) {
            ret = BUFFER_E;
        }
    }

    if (ret == 0) {
        if(XFSEEK(file, 0, XSEEK_END) != 0)
            ret = BUFFER_E;
        sz = XFTELL(file);
        XREWIND(file);

        if (sz <= 0) {
            ret = BUFFER_E;
        }
        else if (sz > (long)sizeof(staticBuffer)) {
        #ifdef WOLFSSL_STATIC_MEMORY
            WOLFSSL_MSG("File was larger then static buffer");
            return MEMORY_E;
        #endif
            fileBuf = (byte*)XMALLOC(sz, NULL, DYNAMIC_TYPE_FILE);
            if (fileBuf == NULL)
                ret = MEMORY_E;
            else
                dynamic = 1;
        }

        if (ret == 0) {
            if ((size_t)XFREAD(fileBuf, 1, sz, file) != (size_t)sz) {
                ret = BUFFER_E;
            }
        #ifdef WOLFSSL_PEM_TO_DER
            else {
                ret = PemToDer(fileBuf, sz, CA_TYPE, &converted,  0, NULL,NULL);
            }
        #endif

            if (ret == 0) {
                if (converted->length < (word32)derSz) {
                    XMEMCPY(derBuf, converted->buffer, converted->length);
                    ret = converted->length;
                }
                else
                    ret = BUFFER_E;
            }

            FreeDer(&converted);
        }

        XFCLOSE(file);
        if (dynamic)
            XFREE(fileBuf, NULL, DYNAMIC_TYPE_FILE);
    }

    return ret;
}
#endif /* WOLFSSL_CERT_GEN */

#if defined(WOLFSSL_CERT_EXT) || defined(WOLFSSL_PUB_PEM_TO_DER)
/* load pem public key from file into der buffer, return der size or error */
int wc_PemPubKeyToDer(const char* fileName,
                           unsigned char* derBuf, int derSz)
{
#ifdef WOLFSSL_SMALL_STACK
    byte   staticBuffer[1]; /* force XMALLOC */
#else
    byte   staticBuffer[FILE_BUFFER_SIZE];
#endif
    byte*  fileBuf = staticBuffer;
    int    dynamic = 0;
    int    ret     = 0;
    long   sz      = 0;
    XFILE  file;
    DerBuffer* converted = NULL;

    WOLFSSL_ENTER("wc_PemPubKeyToDer");

    if (fileName == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        file = XFOPEN(fileName, "rb");
        if (file == XBADFILE) {
            ret = BUFFER_E;
        }
    }

    if (ret == 0) {
        if(XFSEEK(file, 0, XSEEK_END) != 0)
            ret = BUFFER_E;
        sz = XFTELL(file);
        XREWIND(file);

        if (sz <= 0) {
            ret = BUFFER_E;
        }
        else if (sz > (long)sizeof(staticBuffer)) {
        #ifdef WOLFSSL_STATIC_MEMORY
            WOLFSSL_MSG("File was larger then static buffer");
            return MEMORY_E;
        #endif
            fileBuf = (byte*)XMALLOC(sz, NULL, DYNAMIC_TYPE_FILE);
            if (fileBuf == NULL)
                ret = MEMORY_E;
            else
                dynamic = 1;
        }
        if (ret == 0) {
            if ((size_t)XFREAD(fileBuf, 1, sz, file) != (size_t)sz) {
                ret = BUFFER_E;
            }
        #ifdef WOLFSSL_PEM_TO_DER
            else {
                ret = PemToDer(fileBuf, sz, PUBLICKEY_TYPE, &converted,
                               0, NULL, NULL);
            }
        #endif

            if (ret == 0) {
                if (converted->length < (word32)derSz) {
                    XMEMCPY(derBuf, converted->buffer, converted->length);
                    ret = converted->length;
                }
                else
                    ret = BUFFER_E;
            }

            FreeDer(&converted);
        }

        XFCLOSE(file);
        if (dynamic)
            XFREE(fileBuf, NULL, DYNAMIC_TYPE_FILE);
    }

    return ret;
}
#endif /* WOLFSSL_CERT_EXT || WOLFSSL_PUB_PEM_TO_DER */

#endif /* !NO_FILESYSTEM */


#if !defined(NO_RSA) && (defined(WOLFSSL_CERT_GEN) || \
    ((defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA)) && !defined(HAVE_USER_RSA)))
/* USER RSA ifdef portions used instead of refactor in consideration for
   possible fips build */
/* Encode a public RSA key to output.
 *
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * PKCS #1: RFC 8017, A.1.1 - RSAPublicKey
 *
 * Encoded data can either be SubjectPublicKeyInfo (with header) or just the key
 * (RSAPublicKey).
 *
 * @param [out] output       Buffer to put encoded data in.
 * @param [in]  key          RSA key object.
 * @param [in]  outLen       Size of the output buffer in bytes.
 * @param [in]  with_header  Whether to include SubjectPublicKeyInfo around key.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when output or key is NULL, or outLen is less than
 *          minimum length (5 bytes).
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
static int SetRsaPublicKey(byte* output, RsaKey* key, int outLen,
                           int with_header)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int  idx, nSz, eSz, seqSz, headSz = 0, bitStringSz = 0, algoSz = 0;
    byte seq[MAX_SEQ_SZ];
    byte headSeq[MAX_SEQ_SZ];
    byte bitString[1 + MAX_LENGTH_SZ + 1];
    byte algo[MAX_ALGO_SZ]; /* 20 bytes */

    if (key == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef HAVE_USER_RSA
    nSz = SetASNIntRSA(key->n, NULL);
#else
    nSz = SetASNIntMP(&key->n, MAX_RSA_INT_SZ, NULL);
#endif
    if (nSz < 0)
        return nSz;

#ifdef HAVE_USER_RSA
    eSz = SetASNIntRSA(key->e, NULL);
#else
    eSz = SetASNIntMP(&key->e, MAX_RSA_INT_SZ, NULL);
#endif
    if (eSz < 0)
        return eSz;
    seqSz = SetSequence(nSz + eSz, seq);

    /* headers */
    if (with_header) {
        algoSz = SetAlgoID(RSAk, algo, oidKeyType, 0);
        bitStringSz = SetBitString(seqSz + nSz + eSz, 0, bitString);
        headSz = SetSequence(nSz + eSz + seqSz + bitStringSz + algoSz, headSeq);
    }

    /* if getting length only */
    if (output == NULL) {
        return headSz + algoSz + bitStringSz + seqSz + nSz + eSz;
    }

    /* check output size */
    if ((headSz + algoSz + bitStringSz + seqSz + nSz + eSz) > outLen) {
        return BUFFER_E;
    }

    /* write output */
    idx = 0;
    if (with_header) {
        /* header size */
        XMEMCPY(output + idx, headSeq, headSz);
        idx += headSz;
        /* algo */
        XMEMCPY(output + idx, algo, algoSz);
        idx += algoSz;
        /* bit string */
        XMEMCPY(output + idx, bitString, bitStringSz);
        idx += bitStringSz;
    }

    /* seq */
    XMEMCPY(output + idx, seq, seqSz);
    idx += seqSz;
    /* n */
#ifdef HAVE_USER_RSA
    nSz = SetASNIntRSA(key->n, output + idx);
#else
    nSz = SetASNIntMP(&key->n, nSz, output + idx);
#endif
    idx += nSz;
    /* e */
#ifdef HAVE_USER_RSA
    eSz = SetASNIntRSA(key->e, output + idx);
#else
    eSz = SetASNIntMP(&key->e, eSz, output + idx);
#endif
    idx += eSz;

    return idx;
#else
    DECL_ASNSETDATA(dataASN, rsaPublicKeyASN_Length);
    int sz;
    int ret = 0;
    int o = 0;

    /* Check parameter validity. */
    if ((key == NULL) || ((output != NULL) && (outLen < MAX_SEQ_SZ))) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNSETDATA(dataASN, rsaPublicKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        if (!with_header) {
            /* Start encoding with items after header. */
            o = 5;
        }
        /* Set OID for RSA key. */
        SetASN_OID(&dataASN[2], RSAk, oidKeyType);
        /* Set public key mp_ints. */
    #ifdef HAVE_USER_RSA
        SetASN_MP(&dataASN[6], key->n);
        SetASN_MP(&dataASN[7], key->e);
    #else
        SetASN_MP(&dataASN[6], &key->n);
        SetASN_MP(&dataASN[7], &key->e);
    #endif
        /* Calculate size of RSA public key. */
        ret = SizeASN_Items(rsaPublicKeyASN + o, dataASN + o,
                            rsaPublicKeyASN_Length - o, &sz);
    }
    /* Check output buffer is big enough for encoding. */
    if ((ret == 0) && (output != NULL) && (sz > outLen)) {
        ret = BUFFER_E;
    }
    if ((ret == 0) && (output != NULL)) {
        /* Encode RSA public key. */
        SetASN_Items(rsaPublicKeyASN + o, dataASN + o,
                     rsaPublicKeyASN_Length - o, output);
    }
    if (ret == 0) {
        /* Return size of encoding. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, key->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#endif /* !NO_RSA && (WOLFSSL_CERT_GEN || (WOLFSSL_KEY_GEN &&
                                           !HAVE_USER_RSA))) */

#if !defined(NO_RSA) && (defined(WOLFSSL_CERT_GEN) || defined(OPENSSL_EXTRA))
/* Calculate size of encoded public RSA key in bytes.
 *
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * PKCS #1: RFC 8017, A.1.1 - RSAPublicKey
 *
 * Encoded data can either be SubjectPublicKeyInfo (with header) or just the key
 * (RSAPublicKey).
 *
 * @param [in]  key          RSA key object.
 * @param [in]  with_header  Whether to include SubjectPublicKeyInfo around key.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
int wc_RsaPublicKeyDerSize(RsaKey* key, int with_header)
{
    return SetRsaPublicKey(NULL, key, 0, with_header);
}

#endif /* !NO_RSA && WOLFSSL_CERT_GEN */

#if (defined(WOLFSSL_KEY_GEN) || defined(OPENSSL_EXTRA)) && \
    !defined(NO_RSA) && !defined(HAVE_USER_RSA)

/* Encode private RSA key in DER format.
 *
 * PKCS #1: RFC 8017, A.1.2 - RSAPrivateKey
 *
 * @param [in]  key     RSA key object.
 * @param [out] output  Buffer to put encoded data in.
 * @param [in]  inLen   Size of buffer in bytes.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key is NULL or not a private key.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
int wc_RsaKeyToDer(RsaKey* key, byte* output, word32 inLen)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret = 0, i, j, outLen = 0, mpSz;
    word32 seqSz = 0, verSz = 0, rawLen, intTotalLen = 0;
    word32 sizes[RSA_INTS];
    byte  seq[MAX_SEQ_SZ];
    byte  ver[MAX_VERSION_SZ];
    byte* tmps[RSA_INTS];

    if (key == NULL)
        return BAD_FUNC_ARG;

    if (key->type != RSA_PRIVATE)
        return BAD_FUNC_ARG;

    for (i = 0; i < RSA_INTS; i++)
        tmps[i] = NULL;

    /* write all big ints from key to DER tmps */
    for (i = 0; i < RSA_INTS; i++) {
        mp_int* keyInt = GetRsaInt(key, (byte)i);

        rawLen = mp_unsigned_bin_size(keyInt) + 1;
        if (output != NULL) {
            tmps[i] = (byte*)XMALLOC(rawLen + MAX_SEQ_SZ, key->heap,
                                 DYNAMIC_TYPE_RSA);
            if (tmps[i] == NULL) {
                ret = MEMORY_E;
                break;
            }
        }

        mpSz = SetASNIntMP(keyInt, MAX_RSA_INT_SZ, tmps[i]);
        if (mpSz < 0) {
            ret = mpSz;
            break;
        }
        intTotalLen += (sizes[i] = mpSz);
    }

    if (ret == 0) {
        /* make headers */
        verSz = SetMyVersion(0, ver, FALSE);
        seqSz = SetSequence(verSz + intTotalLen, seq);

        outLen = seqSz + verSz + intTotalLen;
        if (output != NULL && outLen > (int)inLen)
            ret = BUFFER_E;
    }
    if (ret == 0 && output != NULL) {
        /* write to output */
        XMEMCPY(output, seq, seqSz);
        j = seqSz;
        XMEMCPY(output + j, ver, verSz);
        j += verSz;

        for (i = 0; i < RSA_INTS; i++) {
            XMEMCPY(output + j, tmps[i], sizes[i]);
            j += sizes[i];
        }
    }

    for (i = 0; i < RSA_INTS; i++) {
        if (tmps[i])
            XFREE(tmps[i], key->heap, DYNAMIC_TYPE_RSA);
    }

    if (ret == 0)
        ret = outLen;
    return ret;
#else
    DECL_ASNSETDATA(dataASN, rsaKeyASN_Length);
    byte i;
    int sz;
    int ret = 0;

    if ((key == NULL) || (key->type != RSA_PRIVATE)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNSETDATA(dataASN, rsaKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Set the version. */
        SetASN_Int8Bit(&dataASN[1], 0);
        /* Set all the mp_ints in private key. */
        for (i = 0; i < RSA_INTS; i++) {
            SetASN_MP(&dataASN[2 + i], GetRsaInt(key, i));
        }

        /* Calculate size of RSA private key encoding. */
        ret = SizeASN_Items(rsaKeyASN, dataASN, rsaKeyASN_Length, &sz);
    }
    /* Check output buffer has enough space for encoding. */
    if ((ret == 0) && (output != NULL) && (sz > (int)inLen)) {
        ret = BAD_FUNC_ARG;
    }
    if ((ret == 0) && (output != NULL)) {
        /* Encode RSA private key. */
        SetASN_Items(rsaKeyASN, dataASN, rsaKeyASN_Length, output);
    }

    if (ret == 0) {
        /* Return size of encoding. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, key->heap);
    return ret;
#endif
}


/* Encode public RSA key in DER format.
 *
 * X.509: RFC 5280, 4.1 - SubjectPublicKeyInfo
 * PKCS #1: RFC 8017, A.1.1 - RSAPublicKey
 *
 * @param [in]  key     RSA key object.
 * @param [out] output  Buffer to put encoded data in.
 * @param [in]  inLen   Size of buffer in bytes.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key or output is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
int wc_RsaKeyToPublicDer(RsaKey* key, byte* output, word32 inLen)
{
    return SetRsaPublicKey(output, key, inLen, 1);
}

/* Returns public DER version of the RSA key. If with_header is 0 then only a
 * seq + n + e is returned in ASN.1 DER format */
int wc_RsaKeyToPublicDer_ex(RsaKey* key, byte* output, word32 inLen,
    int with_header)
{
    return SetRsaPublicKey(output, key, inLen, with_header);
}
#endif /* (WOLFSSL_KEY_GEN || OPENSSL_EXTRA) && !NO_RSA && !HAVE_USER_RSA */


#ifdef WOLFSSL_CERT_GEN

/* Initialize and Set Certificate defaults:
   version    = 3 (0x2)
   serial     = 0
   sigType    = SHA_WITH_RSA
   issuer     = blank
   daysValid  = 500
   selfSigned = 1 (true) use subject as issuer
   subject    = blank
*/
int wc_InitCert_ex(Cert* cert, void* heap, int devId)
{
#ifdef WOLFSSL_MULTI_ATTRIB
    int i = 0;
#endif
    if (cert == NULL) {
        return BAD_FUNC_ARG;
    }

    XMEMSET(cert, 0, sizeof(Cert));

    cert->version    = 2;   /* version 3 is hex 2 */
#ifndef NO_SHA
    cert->sigType    = CTC_SHAwRSA;
#elif !defined(NO_SHA256)
    cert->sigType    = CTC_SHA256wRSA;
#else
    cert->sigType    = 0;
#endif
    cert->daysValid  = 500;
    cert->selfSigned = 1;
    cert->keyType    = RSA_KEY;

    cert->issuer.countryEnc = CTC_PRINTABLE;
    cert->issuer.stateEnc = CTC_UTF8;
    cert->issuer.localityEnc = CTC_UTF8;
    cert->issuer.surEnc = CTC_UTF8;
    cert->issuer.orgEnc = CTC_UTF8;
    cert->issuer.unitEnc = CTC_UTF8;
    cert->issuer.commonNameEnc = CTC_UTF8;

    cert->subject.countryEnc = CTC_PRINTABLE;
    cert->subject.stateEnc = CTC_UTF8;
    cert->subject.localityEnc = CTC_UTF8;
    cert->subject.surEnc = CTC_UTF8;
    cert->subject.orgEnc = CTC_UTF8;
    cert->subject.unitEnc = CTC_UTF8;
    cert->subject.commonNameEnc = CTC_UTF8;

#ifdef WOLFSSL_MULTI_ATTRIB
    for (i = 0; i < CTC_MAX_ATTRIB; i++) {
        cert->issuer.name[i].type   = CTC_UTF8;
        cert->subject.name[i].type  = CTC_UTF8;
    }
#endif /* WOLFSSL_MULTI_ATTRIB */

    cert->heap = heap;
    (void)devId; /* future */

    return 0;
}

int wc_InitCert(Cert* cert)
{
    return wc_InitCert_ex(cert, NULL, INVALID_DEVID);
}

/* DER encoded x509 Certificate */
typedef struct DerCert {
    byte size[MAX_LENGTH_SZ];          /* length encoded */
    byte version[MAX_VERSION_SZ];      /* version encoded */
    byte serial[(int)CTC_SERIAL_SIZE + (int)MAX_LENGTH_SZ]; /* serial number encoded */
    byte sigAlgo[MAX_ALGO_SZ];         /* signature algo encoded */
    byte issuer[ASN_NAME_MAX];         /* issuer  encoded */
    byte subject[ASN_NAME_MAX];        /* subject encoded */
    byte validity[MAX_DATE_SIZE*2 + MAX_SEQ_SZ*2];  /* before and after dates */
    byte publicKey[MAX_PUBLIC_KEY_SZ]; /* rsa / ntru public key encoded */
    byte ca[MAX_CA_SZ];                /* basic constraint CA true size */
    byte extensions[MAX_EXTENSIONS_SZ]; /* all extensions */
#ifdef WOLFSSL_CERT_EXT
    byte skid[MAX_KID_SZ];             /* Subject Key Identifier extension */
    byte akid[MAX_KID_SZ];             /* Authority Key Identifier extension */
    byte keyUsage[MAX_KEYUSAGE_SZ];    /* Key Usage extension */
    byte extKeyUsage[MAX_EXTKEYUSAGE_SZ]; /* Extended Key Usage extension */
    byte certPolicies[MAX_CERTPOL_NB*MAX_CERTPOL_SZ]; /* Certificate Policies */
#endif
#ifdef WOLFSSL_CERT_REQ
    byte attrib[MAX_ATTRIB_SZ];        /* Cert req attributes encoded */
#endif
#ifdef WOLFSSL_ALT_NAMES
    byte altNames[CTC_MAX_ALT_SIZE];   /* Alternative Names encoded */
#endif
    int  sizeSz;                       /* encoded size length */
    int  versionSz;                    /* encoded version length */
    int  serialSz;                     /* encoded serial length */
    int  sigAlgoSz;                    /* encoded sig algo length */
    int  issuerSz;                     /* encoded issuer length */
    int  subjectSz;                    /* encoded subject length */
    int  validitySz;                   /* encoded validity length */
    int  publicKeySz;                  /* encoded public key length */
    int  caSz;                         /* encoded CA extension length */
#ifdef WOLFSSL_CERT_EXT
    int  skidSz;                       /* encoded SKID extension length */
    int  akidSz;                       /* encoded SKID extension length */
    int  keyUsageSz;                   /* encoded KeyUsage extension length */
    int  extKeyUsageSz;                /* encoded ExtendedKeyUsage extension length */
    int  certPoliciesSz;               /* encoded CertPolicies extension length*/
#endif
#ifdef WOLFSSL_ALT_NAMES
    int  altNamesSz;                   /* encoded AltNames extension length */
#endif
    int  extensionsSz;                 /* encoded extensions total length */
    int  total;                        /* total encoded lengths */
#ifdef WOLFSSL_CERT_REQ
    int  attribSz;
#endif
} DerCert;


#ifdef WOLFSSL_CERT_REQ
#ifndef WOLFSSL_ASN_TEMPLATE

/* Write a set header to output */
static word32 SetPrintableString(word32 len, byte* output)
{
    output[0] = ASN_PRINTABLE_STRING;
    return SetLength(len, output + 1) + 1;
}

static word32 SetUTF8String(word32 len, byte* output)
{
    output[0] = ASN_UTF8STRING;
    return SetLength(len, output + 1) + 1;
}

#endif
#endif /* WOLFSSL_CERT_REQ */


#ifndef WOLFSSL_CERT_GEN_CACHE
/* wc_SetCert_Free is only public when WOLFSSL_CERT_GEN_CACHE is not defined */
static
#endif
void wc_SetCert_Free(Cert* cert)
{
    if (cert != NULL) {
        cert->der = NULL;
        if (cert->decodedCert) {
            FreeDecodedCert((DecodedCert*)cert->decodedCert);

            XFREE(cert->decodedCert, cert->heap, DYNAMIC_TYPE_DCERT);
            cert->decodedCert = NULL;
        }
    }
}

static int wc_SetCert_LoadDer(Cert* cert, const byte* der, word32 derSz)
{
    int ret;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        /* Allocate DecodedCert struct and Zero */
        cert->decodedCert = (void*)XMALLOC(sizeof(DecodedCert), cert->heap,
            DYNAMIC_TYPE_DCERT);

        if (cert->decodedCert == NULL) {
            ret = MEMORY_E;
        }
        else {
            XMEMSET(cert->decodedCert, 0, sizeof(DecodedCert));

            InitDecodedCert((DecodedCert*)cert->decodedCert, der, derSz,
                    cert->heap);
            ret = ParseCertRelative((DecodedCert*)cert->decodedCert,
                    CERT_TYPE, 0, NULL);
            if (ret >= 0) {
                cert->der = (byte*)der;
            }
            else {
                wc_SetCert_Free(cert);
            }
        }
    }

    return ret;
}

#endif /* WOLFSSL_CERT_GEN */

#ifdef HAVE_ECC
#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for ECC public key (SubjectPublicKeyInfo).
 * RFC 5480, 2 - Subject Public Key Information Fields
 *           2.1.1 - Unrestricted Algorithm Identifier and Parameters
 * X9.62 ECC point format.
 * See ASN.1 template 'eccSpecifiedASN' for specifiedCurve.
 */
static const ASNItem eccPublicKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* AlgorithmIdentifier */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* algorithm */
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
                    /* namedCurve */
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 2 },
                    /* specifiedCurve - explicit parameters */
/*  4 */            { 2, ASN_SEQUENCE, 1, 0, 2 },
                /*  */
/*  5 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for ECC public key. */
#define eccPublicKeyASN_Length (sizeof(eccPublicKeyASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */
#endif /* HAVE_ECC */

#if defined(HAVE_ECC) && defined(HAVE_ECC_KEY_EXPORT)

/* Encode public ECC key in DER format.
 *
 * RFC 5480, 2 - Subject Public Key Information Fields
 *           2.1.1 - Unrestricted Algorithm Identifier and Parameters
 * X9.62 ECC point format.
 * SEC 1 Ver. 2.0, C.2 - Syntax for Elliptic Curve Domain Parameters
 *
 * @param [out] output       Buffer to put encoded data in.
 * @param [in]  key          ECC key object.
 * @param [in]  outLen       Size of buffer in bytes.
 * @param [in]  with_header  Whether to use SubjectPublicKeyInfo format.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key or key's parameters is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
static int SetEccPublicKey(byte* output, ecc_key* key, int outLen,
                           int with_header)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    byte bitString[1 + MAX_LENGTH_SZ + 1];
    int  algoSz;
    int  curveSz;
    int  bitStringSz;
    int  idx;
    word32 pubSz = ECC_BUFSIZE;
#ifdef WOLFSSL_SMALL_STACK
    byte* algo = NULL;
    byte* curve = NULL;
    byte* pub;
#else
    byte algo[MAX_ALGO_SZ];
    byte curve[MAX_ALGO_SZ];
    byte pub[ECC_BUFSIZE];
#endif
    int ret;

    (void)outLen;

#ifdef WOLFSSL_SMALL_STACK
    pub = (byte*)XMALLOC(ECC_BUFSIZE, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (pub == NULL)
        return MEMORY_E;
#endif

#if defined(HAVE_SELFTEST) || defined(HAVE_FIPS)
    /* older version of ecc.c can not handle dp being NULL */
    if (key != NULL && key->dp == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        ret = wc_ecc_export_x963(key, pub, &pubSz);
    }
#else
    ret = wc_ecc_export_x963(key, pub, &pubSz);
#endif
    if (ret != 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(pub, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ret;
    }

    /* headers */
    if (with_header) {
#ifdef WOLFSSL_SMALL_STACK
        curve = (byte*)XMALLOC(MAX_ALGO_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (curve == NULL) {
            XFREE(pub, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            return MEMORY_E;
        }
#endif
        curveSz = SetCurve(key, curve);
        if (curveSz <= 0) {
#ifdef WOLFSSL_SMALL_STACK
            XFREE(curve, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(pub,   key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            return curveSz;
        }

#ifdef WOLFSSL_SMALL_STACK
        algo = (byte*)XMALLOC(MAX_ALGO_SZ, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (algo == NULL) {
            XFREE(curve, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(pub,   key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            return MEMORY_E;
        }
#endif
        algoSz  = SetAlgoID(ECDSAk, algo, oidKeyType, curveSz);

        bitStringSz = SetBitString(pubSz, 0, bitString);

        idx = SetSequence(pubSz + curveSz + bitStringSz + algoSz, output);
        /* algo */
        if (output)
            XMEMCPY(output + idx, algo, algoSz);
        idx += algoSz;
        /* curve */
        if (output)
            XMEMCPY(output + idx, curve, curveSz);
        idx += curveSz;
        /* bit string */
        if (output)
            XMEMCPY(output + idx, bitString, bitStringSz);
        idx += bitStringSz;
    }
    else
        idx = 0;

    /* pub */
    if (output)
        XMEMCPY(output + idx, pub, pubSz);
    idx += pubSz;

#ifdef WOLFSSL_SMALL_STACK
    if (with_header) {
        XFREE(algo,  key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(curve, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
    XFREE(pub,   key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return idx;
#else
    word32 pubSz;
    int sz = 0;
    int ret = 0;

    /* Check key validity. */
    if ((key == NULL) || (key->dp == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    if (ret == 0) {
        /* Calculate the size of the encoded public point. */
        ret = wc_ecc_export_x963(key, NULL, &pubSz);
        /* LENGTH_ONLY_E on success. */
        if (ret == LENGTH_ONLY_E) {
            ret = 0;
        }
    }
    if ((ret == 0) && with_header) {
        /* Including SubjectPublicKeyInfo header. */
        DECL_ASNSETDATA(dataASN, eccPublicKeyASN_Length);

        CALLOC_ASNSETDATA(dataASN, eccPublicKeyASN_Length, ret, key->heap);

        if (ret == 0) {
            /* Set the key type OID. */
            SetASN_OID(&dataASN[2], ECDSAk, oidKeyType);
            /* Set the curve OID. */
            SetASN_Buffer(&dataASN[3], key->dp->oid, key->dp->oidSz);
            /* Don't try to write out explicit parameters. */
            dataASN[4].noOut = 1;
            /* Set size of public point to ensure space is made for it. */
            SetASN_Buffer(&dataASN[5], NULL, pubSz);
            /* Calculate size of ECC public key. */
            ret = SizeASN_Items(eccPublicKeyASN, dataASN,
                                eccPublicKeyASN_Length, &sz);
        }
        /* Check buffer, if passed in, is big enough for encoded data. */
        if ((ret == 0) && (output != NULL) && (sz > outLen)) {
            ret = BUFFER_E;
        }
        if ((ret == 0) && (output != NULL)) {
            /* Encode ECC public key. */
            SetASN_Items(eccPublicKeyASN, dataASN, eccPublicKeyASN_Length,
                         output);
            /* Skip to where public point is to be encoded. */
            output += sz - pubSz;
        }

        FREE_ASNSETDATA(dataASN, key->heap);
    }
    else if ((ret == 0) && (output != NULL) && (pubSz > (word32)outLen)) {
        ret = BUFFER_E;
    }
    else if (ret == 0) {
        /* Total size is the public point size. */
        sz = pubSz;
    }

    if ((ret == 0) && (output != NULL)) {
        /* Encode public point. */
        ret = wc_ecc_export_x963(key, output, &pubSz);
    }
    if (ret == 0) {
        /* Return the size of the encoding. */
        ret = sz;
    }

    return ret;
#endif
}


/* Encode the public part of an ECC key in a DER.
 *
 * Pass NULL for output to get the size of the encoding.
 *
 * @param [in]  key            ECC key object.
 * @param [out] output         Buffer to hold DER encoding.
 * @param [in]  inLen          Size of buffer in bytes.
 * @param [in]  with_AlgCurve  Whether to use SubjectPublicKeyInfo format.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key or key's parameters is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
int wc_EccPublicKeyToDer(ecc_key* key, byte* output, word32 inLen,
                                                              int with_AlgCurve)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 infoSz = 0;
    word32 keySz  = 0;
    int ret;

    if (key == NULL) {
        return BAD_FUNC_ARG;
    }

    if (with_AlgCurve) {
        /* buffer space for algorithm/curve */
        infoSz += MAX_SEQ_SZ;
        infoSz += 2 * MAX_ALGO_SZ;

        /* buffer space for public key sequence */
        infoSz += MAX_SEQ_SZ;
        infoSz += TRAILING_ZERO;
    }

#if defined(HAVE_SELFTEST) || defined(HAVE_FIPS)
    /* older version of ecc.c can not handle dp being NULL */
    if (key->dp == NULL) {
        keySz = 1 + 2 * MAX_ECC_BYTES;
        ret = LENGTH_ONLY_E;
    }
    else {
        ret = wc_ecc_export_x963(key, NULL, &keySz);
    }
#else
    ret = wc_ecc_export_x963(key, NULL, &keySz);
#endif
    if (ret != LENGTH_ONLY_E) {
        WOLFSSL_MSG("Error in getting ECC public key size");
        return ret;
    }

    /* if output null then just return size */
    if (output == NULL) {
        return keySz + infoSz;
    }

    if (inLen < keySz + infoSz) {
        return BUFFER_E;
    }
#endif

    return SetEccPublicKey(output, key, inLen, with_AlgCurve);
}

int wc_EccPublicKeyDerSize(ecc_key* key, int with_AlgCurve)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    return wc_EccPublicKeyToDer(key, NULL, 0, with_AlgCurve);
#else
    return SetEccPublicKey(NULL, key, 0, with_AlgCurve);
#endif
}

#endif /* HAVE_ECC && HAVE_ECC_KEY_EXPORT */

#ifdef WOLFSSL_ASN_TEMPLATE
#if defined(WC_ENABLE_ASYM_KEY_EXPORT) || defined(WC_ENABLE_ASYM_KEY_IMPORT)
/* ASN.1 template for Ed25519 and Ed448 public key (SubkectPublicKeyInfo).
 * RFC 8410, 4 - Subject Public Key Fields
 */
static const ASNItem edPubKeyASN[] = {
            /* SubjectPublicKeyInfo */
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* AlgorithmIdentifier */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* Ed25519/Ed448 OID */
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 1 },
                /* Public key stream */
/*  3 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for Ed25519 and Ed448 public key. */
#define edPubKeyASN_Length (sizeof(edPubKeyASN) / sizeof(ASNItem))
#endif /* WC_ENABLE_ASYM_KEY_EXPORT || WC_ENABLE_ASYM_KEY_IMPORT */
#endif /* WOLFSSL_ASN_TEMPLATE */

#ifdef WC_ENABLE_ASYM_KEY_EXPORT

/* Build ASN.1 formatted public key based on RFC 8410
 *
 * Pass NULL for output to get the size of the encoding.
 *
 * @param [in]  pubKey       public key buffer
 * @param [in]  pubKeyLen    public ket buffer length
 * @param [out] output       Buffer to put encoded data in (optional)
 * @param [in]  outLen       Size of buffer in bytes
 * @param [in]  keyType      is "enum Key_Sum" like ED25519k
 * @param [in]  withHeader   Whether to include SubjectPublicKeyInfo around key.
 * @return  Size of encoded data in bytes on success
 * @return  BAD_FUNC_ARG when key is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
static int SetAsymKeyDerPublic(const byte* pubKey, word32 pubKeyLen,
    byte* output, word32 outLen, int keyType, int withHeader)
{
    int ret = 0;
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0, bitStringSz, algoSz, sz = 0;
#else
    int sz = 0;
    DECL_ASNSETDATA(dataASN, edPubKeyASN_Length);
#endif

    if (pubKey == NULL) {
        return BAD_FUNC_ARG;
    }

#ifndef WOLFSSL_ASN_TEMPLATE
    /* calculate size */
    if (withHeader) {
        algoSz      = SetAlgoID(keyType, NULL, oidKeyType, 0);
        bitStringSz = SetBitString(pubKeyLen, 0, NULL);

        sz  = algoSz + bitStringSz + pubKeyLen;
        sz += SetSequence(outLen, NULL);
    }
    else {
        sz = pubKeyLen;
    }

    /* checkout output size */
    if (output != NULL && sz > outLen) {
        ret = BUFFER_E;
    }

    /* headers */
    if (ret == 0 && output != NULL && withHeader) {
        /* sequence */
        idx = SetSequence(algoSz + bitStringSz + pubKeyLen, output);
        /* algo */
        algoSz = SetAlgoID(keyType, output + idx, oidKeyType, 0);
        idx += algoSz;
        /* bit string */
        bitStringSz = SetBitString(pubKeyLen, 0, output + idx);
        idx += bitStringSz;
    }

    if (ret == 0 && output != NULL) {
        /* pub */
        XMEMCPY(output + idx, pubKey, pubKeyLen);
        idx += pubKeyLen;

        sz = idx;
    }

    if (ret == 0) {
        ret = sz;
    }
#else
    if (withHeader) {
        CALLOC_ASNSETDATA(dataASN, edPubKeyASN_Length, ret, NULL);

        if (ret == 0) {
            /* Set the OID. */
            SetASN_OID(&dataASN[2], keyType, oidKeyType);
            /* Leave space for public point. */
            SetASN_Buffer(&dataASN[3], NULL, pubKeyLen);
            /* Calculate size of public key encoding. */
            ret = SizeASN_Items(edPubKeyASN, dataASN, edPubKeyASN_Length, &sz);
        }
        if ((ret == 0) && (output != NULL) && (sz > (int)outLen)) {
            ret = BUFFER_E;
        }
        if ((ret == 0) && (output != NULL)) {
            /* Encode public key. */
            SetASN_Items(edPubKeyASN, dataASN, edPubKeyASN_Length, output);
            /* Set location to encode public point. */
            output = (byte*)dataASN[3].data.buffer.data;
        }

        FREE_ASNSETDATA(dataASN, NULL);
    }
    else if ((output != NULL) && (pubKeyLen > outLen)) {
        ret = BUFFER_E;
    }
    else if (ret == 0) {
        sz = pubKeyLen;
    }

    if ((ret == 0) && (output != NULL)) {
        /* Put public key into space provided. */
        XMEMCPY(output, pubKey, pubKeyLen);
    }
    if (ret == 0) {
        ret = sz;
    }
#endif /* WOLFSSL_ASN_TEMPLATE */
    return ret;
}
#endif /* WC_ENABLE_ASYM_KEY_EXPORT */

#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_EXPORT)
/* Encode the public part of an Ed25519 key in DER.
 *
 * Pass NULL for output to get the size of the encoding.
 *
 * @param [in]  key       Ed25519 key object.
 * @param [out] output    Buffer to put encoded data in.
 * @param [in]  outLen    Size of buffer in bytes.
 * @param [in]  withAlg   Whether to use SubjectPublicKeyInfo format.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
int wc_Ed25519PublicKeyToDer(ed25519_key* key, byte* output, word32 inLen,
                             int withAlg)
{
    int    ret;
    byte   pubKey[ED25519_PUB_KEY_SIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (key == NULL || output == NULL) {
        return BAD_FUNC_ARG;
    }

    ret = wc_ed25519_export_public(key, pubKey, &pubKeyLen);
    if (ret == 0) {
        ret = SetAsymKeyDerPublic(pubKey, pubKeyLen, output, inLen,
            ED25519k, withAlg);
    }
    return ret;
}
#endif /* HAVE_ED25519 && HAVE_ED25519_KEY_EXPORT */

#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_EXPORT)
/* Encode the public part of an Ed448 key in DER.
 *
 * Pass NULL for output to get the size of the encoding.
 *
 * @param [in]  key       Ed448 key object.
 * @param [out] output    Buffer to put encoded data in.
 * @param [in]  outLen    Size of buffer in bytes.
 * @param [in]  withAlg   Whether to use SubjectPublicKeyInfo format.
 * @return  Size of encoded data in bytes on success.
 * @return  BAD_FUNC_ARG when key is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 */
int wc_Ed448PublicKeyToDer(ed448_key* key, byte* output, word32 inLen,
                           int withAlg)
{
    int    ret;
    byte   pubKey[ED448_PUB_KEY_SIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (key == NULL || output == NULL) {
        return BAD_FUNC_ARG;
    }

    ret = wc_ed448_export_public(key, pubKey, &pubKeyLen);
    if (ret == 0) {
        ret = SetAsymKeyDerPublic(pubKey, pubKeyLen, output, inLen,
            ED448k, withAlg);
    }
    return ret;
}
#endif /* HAVE_ED448 && HAVE_ED448_KEY_EXPORT */


#ifdef WOLFSSL_CERT_GEN

#ifndef NO_ASN_TIME
static WC_INLINE byte itob(int number)
{
    return (byte)number + 0x30;
}


/* write time to output, format */
static void SetTime(struct tm* date, byte* output)
{
    int i = 0;

    output[i++] = itob((date->tm_year % 10000) / 1000);
    output[i++] = itob((date->tm_year % 1000)  /  100);
    output[i++] = itob((date->tm_year % 100)   /   10);
    output[i++] = itob( date->tm_year % 10);

    output[i++] = itob(date->tm_mon / 10);
    output[i++] = itob(date->tm_mon % 10);

    output[i++] = itob(date->tm_mday / 10);
    output[i++] = itob(date->tm_mday % 10);

    output[i++] = itob(date->tm_hour / 10);
    output[i++] = itob(date->tm_hour % 10);

    output[i++] = itob(date->tm_min / 10);
    output[i++] = itob(date->tm_min % 10);

    output[i++] = itob(date->tm_sec / 10);
    output[i++] = itob(date->tm_sec % 10);

    output[i] = 'Z';  /* Zulu profile */
}
#endif

#ifdef WOLFSSL_ALT_NAMES
#ifndef WOLFSSL_ASN_TEMPLATE

/* Copy Dates from cert, return bytes written */
static int CopyValidity(byte* output, Cert* cert)
{
    int seqSz;

    WOLFSSL_ENTER("CopyValidity");

    /* headers and output */
    seqSz = SetSequence(cert->beforeDateSz + cert->afterDateSz, output);
    if (output) {
        XMEMCPY(output + seqSz, cert->beforeDate, cert->beforeDateSz);
        XMEMCPY(output + seqSz + cert->beforeDateSz, cert->afterDate,
                                                     cert->afterDateSz);
    }
    return seqSz + cert->beforeDateSz + cert->afterDateSz;
}

#endif /* !WOLFSSL_ASN_TEMPLATE */
#endif

/* Get Which Name from index */
const char* GetOneCertName(CertName* name, int idx)
{
    switch (idx) {
    case 0:
       return name->country;

    case 1:
       return name->state;

    case 2:
       return name->locality;

    case 3:
       return name->sur;

    case 4:
       return name->org;

    case 5:
       return name->unit;

    case 6:
       return name->commonName;

    case 7:
       return name->serialDev;

    case 8:
#ifdef WOLFSSL_CERT_EXT
       return name->busCat;

    case 9:
#endif
       return name->email;

    default:
       return NULL;
    }
}


/* Get Which Name Encoding from index */
static char GetNameType(CertName* name, int idx)
{
    switch (idx) {
    case 0:
       return name->countryEnc;

    case 1:
       return name->stateEnc;

    case 2:
       return name->localityEnc;

    case 3:
       return name->surEnc;

    case 4:
       return name->orgEnc;

    case 5:
       return name->unitEnc;

    case 6:
       return name->commonNameEnc;

    case 7:
       return name->serialDevEnc;

    case 8:
#ifdef WOLFSSL_CERT_EXT
       return name->busCatEnc;

    case 9:
#endif
        /* FALL THROUGH */
        /* The last index, email name, does not have encoding type.
           The empty case here is to keep track of it for future reference. */
    default:
       return 0;
    }
}


/* Get ASN Name from index */
byte GetCertNameId(int idx)
{
    switch (idx) {
    case 0:
       return ASN_COUNTRY_NAME;

    case 1:
       return ASN_STATE_NAME;

    case 2:
       return ASN_LOCALITY_NAME;

    case 3:
       return ASN_SUR_NAME;

    case 4:
       return ASN_ORG_NAME;

    case 5:
       return ASN_ORGUNIT_NAME;

    case 6:
       return ASN_COMMON_NAME;

    case 7:
       return ASN_SERIAL_NUMBER;

    case 8:
#ifdef WOLFSSL_CERT_EXT
        return ASN_BUS_CAT;

    case 9:
#endif
        return ASN_EMAIL_NAME;

    default:
       return 0;
    }
}


#ifndef WOLFSSL_ASN_TEMPLATE
/*
 Extensions ::= SEQUENCE OF Extension

 Extension ::= SEQUENCE {
 extnId     OBJECT IDENTIFIER,
 critical   BOOLEAN DEFAULT FALSE,
 extnValue  OCTET STRING }
 */

/* encode all extensions, return total bytes written */
static int SetExtensions(byte* out, word32 outSz, int *IdxInOut,
                         const byte* ext, int extSz)
{
    if (out == NULL || IdxInOut == NULL || ext == NULL)
        return BAD_FUNC_ARG;

    if (outSz < (word32)(*IdxInOut+extSz))
        return BUFFER_E;

    XMEMCPY(&out[*IdxInOut], ext, extSz);  /* extensions */
    *IdxInOut += extSz;

    return *IdxInOut;
}

/* encode extensions header, return total bytes written */
static int SetExtensionsHeader(byte* out, word32 outSz, int extSz)
{
    byte sequence[MAX_SEQ_SZ];
    byte len[MAX_LENGTH_SZ];
    int seqSz, lenSz, idx = 0;

    if (out == NULL)
        return BAD_FUNC_ARG;

    if (outSz < 3)
        return BUFFER_E;

    seqSz = SetSequence(extSz, sequence);

    /* encode extensions length provided */
    lenSz = SetLength(extSz+seqSz, len);

    if (outSz < (word32)(lenSz+seqSz+1))
        return BUFFER_E;

    out[idx++] = ASN_EXTENSIONS; /* extensions id */
    XMEMCPY(&out[idx], len, lenSz);  /* length */
    idx += lenSz;

    XMEMCPY(&out[idx], sequence, seqSz);  /* sequence */
    idx += seqSz;

    return idx;
}


/* encode CA basic constraint true, return total bytes written */
static int SetCa(byte* out, word32 outSz)
{
    const byte ca[] = { 0x30, 0x0c, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x04,
                               0x05, 0x30, 0x03, 0x01, 0x01, 0xff };

    if (out == NULL)
        return BAD_FUNC_ARG;

    if (outSz < sizeof(ca))
        return BUFFER_E;

    XMEMCPY(out, ca, sizeof(ca));

    return (int)sizeof(ca);
}
#endif


#ifdef WOLFSSL_CERT_EXT
#ifndef WOLFSSL_ASN_TEMPLATE
/* encode OID and associated value, return total bytes written */
static int SetOidValue(byte* out, word32 outSz, const byte *oid, word32 oidSz,
                       byte *in, word32 inSz)
{
    int idx = 0;

    if (out == NULL || oid == NULL || in == NULL)
        return BAD_FUNC_ARG;

    if (outSz < 3)
        return BUFFER_E;

    /* sequence,  + 1 => byte to put value size */
    idx = SetSequence(inSz + oidSz + 1, out);

    if ((idx + inSz + oidSz + 1) > outSz)
        return BUFFER_E;

    XMEMCPY(out+idx, oid, oidSz);
    idx += oidSz;
    out[idx++] = (byte)inSz;
    XMEMCPY(out+idx, in, inSz);

    return (idx+inSz);
}

/* encode Subject Key Identifier, return total bytes written
 * RFC5280 : non-critical */
static int SetSKID(byte* output, word32 outSz, const byte *input, word32 length)
{
    byte skid_len[1 + MAX_LENGTH_SZ];
    byte skid_enc_len[MAX_LENGTH_SZ];
    int idx = 0, skid_lenSz, skid_enc_lenSz;
    const byte skid_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04 };

    if (output == NULL || input == NULL)
        return BAD_FUNC_ARG;

    /* Octet String header */
    skid_lenSz = SetOctetString(length, skid_len);

    /* length of encoded value */
    skid_enc_lenSz = SetLength(length + skid_lenSz, skid_enc_len);

    if (outSz < 3)
        return BUFFER_E;

    idx = SetSequence(length + sizeof(skid_oid) + skid_lenSz + skid_enc_lenSz,
                      output);

    if ((length + sizeof(skid_oid) + skid_lenSz + skid_enc_lenSz) > outSz)
        return BUFFER_E;

    /* put oid */
    XMEMCPY(output+idx, skid_oid, sizeof(skid_oid));
    idx += sizeof(skid_oid);

    /* put encoded len */
    XMEMCPY(output+idx, skid_enc_len, skid_enc_lenSz);
    idx += skid_enc_lenSz;

    /* put octet header */
    XMEMCPY(output+idx, skid_len, skid_lenSz);
    idx += skid_lenSz;

    /* put value */
    XMEMCPY(output+idx, input, length);
    idx += length;

    return idx;
}

/* encode Authority Key Identifier, return total bytes written
 * RFC5280 : non-critical */
static int SetAKID(byte* output, word32 outSz,
                                         byte *input, word32 length, void* heap)
{
    byte    *enc_val;
    int     ret, enc_valSz;
    const byte akid_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04 };
    const byte akid_cs[] = { 0x80 };

    (void)heap;

    if (output == NULL || input == NULL)
        return BAD_FUNC_ARG;

    enc_valSz = length + 3 + sizeof(akid_cs);
    enc_val = (byte *)XMALLOC(enc_valSz, heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (enc_val == NULL)
        return MEMORY_E;

    /* sequence for ContentSpec & value */
    ret = SetOidValue(enc_val, enc_valSz, akid_cs, sizeof(akid_cs),
                      input, length);
    if (ret > 0) {
        enc_valSz = ret;

        ret = SetOidValue(output, outSz, akid_oid, sizeof(akid_oid),
                          enc_val, enc_valSz);
    }

    XFREE(enc_val, heap, DYNAMIC_TYPE_TMP_BUFFER);
    return ret;
}

/* encode Key Usage, return total bytes written
 * RFC5280 : critical */
static int SetKeyUsage(byte* output, word32 outSz, word16 input)
{
    byte ku[5];
    int  idx;
    const byte keyusage_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x0f,
                                         0x01, 0x01, 0xff, 0x04};
    if (output == NULL)
        return BAD_FUNC_ARG;

    idx = SetBitString16Bit(input, ku);
    return SetOidValue(output, outSz, keyusage_oid, sizeof(keyusage_oid),
                       ku, idx);
}

static int SetOjectIdValue(byte* output, word32 outSz, int* idx,
    const byte* oid, word32 oidSz)
{
    /* verify room */
    if (*idx + 2 + oidSz >= outSz)
        return ASN_PARSE_E;

    *idx += SetObjectId(oidSz, &output[*idx]);
    XMEMCPY(&output[*idx], oid, oidSz);
    *idx += oidSz;

    return 0;
}
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for extended key usage.
 * X.509: RFC 5280, 4.2.12 - Extended Key Usage
 * Dynamic creation of template for encoding.
 */
static const ASNItem ekuASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
/*  1 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
};

/* OIDs corresponding to extended key usage. */
struct {
    const byte* oid;
    word32 oidSz;
} ekuOid[] = {
    { extExtKeyUsageServerAuthOid,   sizeof(extExtKeyUsageServerAuthOid) },
    { extExtKeyUsageClientAuthOid,   sizeof(extExtKeyUsageClientAuthOid) },
    { extExtKeyUsageCodeSigningOid,  sizeof(extExtKeyUsageCodeSigningOid) },
    { extExtKeyUsageEmailProtectOid, sizeof(extExtKeyUsageEmailProtectOid) },
    { extExtKeyUsageTimestampOid,    sizeof(extExtKeyUsageTimestampOid) },
    { extExtKeyUsageOcspSignOid,     sizeof(extExtKeyUsageOcspSignOid) },
};

#define EKU_OID_LO      1
#define EKU_OID_HI      6
#endif /* WOLFSSL_ASN_TEMPLATE */

/* encode Extended Key Usage (RFC 5280 4.2.1.12), return total bytes written */
static int SetExtKeyUsage(Cert* cert, byte* output, word32 outSz, byte input)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int idx = 0, oidListSz = 0, totalSz, ret = 0;
    const byte extkeyusage_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x25 };

    if (output == NULL)
        return BAD_FUNC_ARG;

    /* Skip to OID List */
    totalSz = 2 + sizeof(extkeyusage_oid) + 4;
    idx = totalSz;

    /* Build OID List */
    /* If any set, then just use it */
    if (input & EXTKEYUSE_ANY) {
        ret |= SetOjectIdValue(output, outSz, &idx,
            extExtKeyUsageAnyOid, sizeof(extExtKeyUsageAnyOid));
    }
    else {
        if (input & EXTKEYUSE_SERVER_AUTH)
            ret |= SetOjectIdValue(output, outSz, &idx,
                extExtKeyUsageServerAuthOid, sizeof(extExtKeyUsageServerAuthOid));
        if (input & EXTKEYUSE_CLIENT_AUTH)
            ret |= SetOjectIdValue(output, outSz, &idx,
                extExtKeyUsageClientAuthOid, sizeof(extExtKeyUsageClientAuthOid));
        if (input & EXTKEYUSE_CODESIGN)
            ret |= SetOjectIdValue(output, outSz, &idx,
                extExtKeyUsageCodeSigningOid, sizeof(extExtKeyUsageCodeSigningOid));
        if (input & EXTKEYUSE_EMAILPROT)
            ret |= SetOjectIdValue(output, outSz, &idx,
                extExtKeyUsageEmailProtectOid, sizeof(extExtKeyUsageEmailProtectOid));
        if (input & EXTKEYUSE_TIMESTAMP)
            ret |= SetOjectIdValue(output, outSz, &idx,
                extExtKeyUsageTimestampOid, sizeof(extExtKeyUsageTimestampOid));
        if (input & EXTKEYUSE_OCSP_SIGN)
            ret |= SetOjectIdValue(output, outSz, &idx,
                extExtKeyUsageOcspSignOid, sizeof(extExtKeyUsageOcspSignOid));
    #ifdef WOLFSSL_EKU_OID
        /* iterate through OID values */
        if (input & EXTKEYUSE_USER) {
            int i, sz;
            for (i = 0; i < CTC_MAX_EKU_NB; i++) {
                sz = cert->extKeyUsageOIDSz[i];
                if (sz > 0) {
                    ret |= SetOjectIdValue(output, outSz, &idx,
                        cert->extKeyUsageOID[i], sz);
                }
            }
        }
    #endif /* WOLFSSL_EKU_OID */
    }
    if (ret != 0)
        return ASN_PARSE_E;

    /* Calculate Sizes */
    oidListSz = idx - totalSz;
    totalSz = idx - 2; /* exclude first seq/len (2) */

    /* 1. Seq + Total Len (2) */
    idx = SetSequence(totalSz, output);

    /* 2. Object ID (2) */
    XMEMCPY(&output[idx], extkeyusage_oid, sizeof(extkeyusage_oid));
    idx += sizeof(extkeyusage_oid);

    /* 3. Octet String (2) */
    idx += SetOctetString(totalSz - idx, &output[idx]);

    /* 4. Seq + OidListLen (2) */
    idx += SetSequence(oidListSz, &output[idx]);

    /* 5. Oid List (already set in-place above) */
    idx += oidListSz;

    (void)cert;
    return idx;
#else
    /* TODO: consider calculating size of OBJECT_IDs, setting length into
     * SEQUENCE, encode SEQUENCE, encode OBJECT_IDs into buffer.  */
    ASNSetData* dataASN;
    ASNItem* extKuASN = NULL;
    int asnIdx = 1;
    int cnt = 1 + EKU_OID_HI;
    int i;
    int ret = 0;
    int sz;

#ifdef WOLFSSL_EKU_OID
    cnt += CTC_MAX_EKU_NB;
#endif

    /* Allocate memory for dynamic data items. */
    dataASN = (ASNSetData*)XMALLOC(cnt * sizeof(ASNSetData), cert->heap,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
    if (dataASN == NULL) {
        ret = MEMORY_E;
    }
    if (ret == 0) {
        /* Allocate memory for dynamic ASN.1 template. */
        extKuASN = (ASNItem*)XMALLOC(cnt * sizeof(ASNItem), cert->heap,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
        if (extKuASN == NULL) {
            ret = MEMORY_E;
        }
    }

    if (ret == 0) {
        /* Copy Sequence into dynamic ASN.1 template. */
        XMEMCPY(&extKuASN[0], ekuASN, sizeof(ASNItem));
        /* Clear dynamic data. */
        XMEMSET(dataASN, 0, cnt * sizeof(ASNSetData));

        /* Build up the template and data. */
        /* If 'any' set, then just use it. */
        if ((input & EXTKEYUSE_ANY) == EXTKEYUSE_ANY) {
            /* Set template item. */
            XMEMCPY(&extKuASN[1], &ekuASN[1], sizeof(ASNItem));
            /* Set data item. */
            SetASN_Buffer(&dataASN[asnIdx], extExtKeyUsageAnyOid,
                sizeof(extExtKeyUsageAnyOid));
            asnIdx++;
        }
        else {
            /* Step through the flagged purposes. */
            for (i = EKU_OID_LO; i <= EKU_OID_HI; i++) {
                if ((input & (1 << i)) != 0) {
                    /* Set template item. */
                    XMEMCPY(&extKuASN[asnIdx], &ekuASN[1], sizeof(ASNItem));
                    /* Set data item. */
                    SetASN_Buffer(&dataASN[asnIdx], ekuOid[i - 1].oid,
                        ekuOid[i - 1].oidSz);
                    asnIdx++;
                }
            }
        #ifdef WOLFSSL_EKU_OID
            if (input & EXTKEYUSE_USER) {
                /* Iterate through OID values */
                for (i = 0; i < CTC_MAX_EKU_NB; i++) {
                    int sz = cert->extKeyUsageOIDSz[i];
                    if (sz > 0) {
                        /* Set template item. */
                        XMEMCPY(&extKuASN[asnIdx], &ekuASN[1], sizeof(ASNItem));
                        /* Set data item. */
                        SetASN_Buffer(&dataASN[asnIdx], cert->extKeyUsageOID[i],
                            sz);
                        asnIdx++;
                    }
                }
            }
        #endif /* WOLFSSL_EKU_OID */
            (void)cert;
        }

        /* Calculate size of encoding. */
        ret = SizeASN_Items(extKuASN, dataASN, asnIdx, &sz);
    }
    /* When buffer to write to, ensure it's big enough. */
    if ((ret == 0) && (output != NULL) && (sz > (int)outSz)) {
        ret = BUFFER_E;
    }
    if ((ret == 0) && (output != NULL)) {
        /* Encode extended key usage. */
        SetASN_Items(extKuASN, dataASN, asnIdx, output);
    }
    if (ret == 0) {
        /* Return the encoding size. */
        ret = sz;
    }

    /* Dispose of allocated data. */
    if (extKuASN != NULL) {
        XFREE(extKuASN, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
    if (dataASN != NULL) {
        XFREE(dataASN, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    }

    return ret;
#endif
}

/* encode Certificate Policies, return total bytes written
 * each input value must be ITU-T X.690 formatted : a.b.c...
 * input must be an array of values with a NULL terminated for the latest
 * RFC5280 : non-critical */
static int SetCertificatePolicies(byte *output,
                                  word32 outputSz,
                                  char input[MAX_CERTPOL_NB][MAX_CERTPOL_SZ],
                                  word16 nb_certpol,
                                  void* heap)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    byte    oid[MAX_OID_SZ];
    byte    der_oid[MAX_CERTPOL_NB][MAX_OID_SZ];
    byte    out[MAX_CERTPOL_SZ];
    word32  oidSz;
    word32  outSz;
    word32  i = 0;
    word32  der_oidSz[MAX_CERTPOL_NB];
    int     ret;

    const byte certpol_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x20, 0x04 };
    const byte oid_oid[] = { 0x06 };

    if (output == NULL || input == NULL || nb_certpol > MAX_CERTPOL_NB)
        return BAD_FUNC_ARG;

    for (i = 0; i < nb_certpol; i++) {
        oidSz = sizeof(oid);
        XMEMSET(oid, 0, oidSz);

        ret = EncodePolicyOID(oid, &oidSz, input[i], heap);
        if (ret != 0)
            return ret;

        /* compute sequence value for the oid */
        ret = SetOidValue(der_oid[i], MAX_OID_SZ, oid_oid,
                          sizeof(oid_oid), oid, oidSz);
        if (ret <= 0)
            return ret;
        else
            der_oidSz[i] = (word32)ret;
    }

    /* concatenate oid, keep two byte for sequence/size of the created value */
    for (i = 0, outSz = 2; i < nb_certpol; i++) {
        XMEMCPY(out+outSz, der_oid[i], der_oidSz[i]);
        outSz += der_oidSz[i];
    }

    /* add sequence */
    ret = SetSequence(outSz-2, out);
    if (ret <= 0)
        return ret;

    /* add Policy OID to compute final value */
    return SetOidValue(output, outputSz, certpol_oid, sizeof(certpol_oid),
                      out, outSz);
#else
    int    i;
    int    ret = 0;
    byte   oid[MAX_OID_SZ];
    word32 oidSz;
    word32 sz = 0;
    int    piSz;

    if ((input == NULL) || (nb_certpol > MAX_CERTPOL_NB)) {
        ret = BAD_FUNC_ARG;
    }
    /* Put in policyIdentifier but not policyQualifiers. */
    for (i = 0; (ret == 0) && (i < nb_certpol); i++) {
        ASNSetData dataASN[policyInfoASN_Length];

        oidSz = sizeof(oid);
        XMEMSET(oid, 0, oidSz);
        dataASN[2].noOut = 1;

        ret = EncodePolicyOID(oid, &oidSz, input[i], heap);
        if (ret == 0) {
            XMEMSET(dataASN, 0, sizeof(dataASN));
            SetASN_Buffer(&dataASN[1], oid, oidSz);
            ret = SizeASN_Items(policyInfoASN, dataASN, policyInfoASN_Length,
                                &piSz);
        }
        if ((ret == 0) && (output != NULL) && (sz + piSz > outputSz)) {
            ret = BUFFER_E;
        }
        if (ret == 0) {
            if (output != NULL) {
                SetASN_Items(policyInfoASN, dataASN, policyInfoASN_Length,
                    output);
                output += piSz;
            }
            sz += piSz;
        }
    }

    if (ret == 0) {
        ret = sz;
    }
    return ret;
#endif
}
#endif /* WOLFSSL_CERT_EXT */


#ifdef WOLFSSL_ALT_NAMES

#ifndef WOLFSSL_ASN_TEMPLATE
/* encode Alternative Names, return total bytes written */
static int SetAltNames(byte *output, word32 outSz,
        const byte *input, word32 length)
{
    byte san_len[1 + MAX_LENGTH_SZ];
    int idx = 0, san_lenSz;
    const byte san_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x11 };

    if (output == NULL || input == NULL)
        return BAD_FUNC_ARG;

    if (outSz < length)
        return BUFFER_E;

    /* Octet String header */
    san_lenSz = SetOctetString(length, san_len);

    if (outSz < MAX_SEQ_SZ)
        return BUFFER_E;

    idx = SetSequence(length + sizeof(san_oid) + san_lenSz, output);

    if ((length + sizeof(san_oid) + san_lenSz) > outSz)
        return BUFFER_E;

    /* put oid */
    XMEMCPY(output+idx, san_oid, sizeof(san_oid));
    idx += sizeof(san_oid);

    /* put octet header */
    XMEMCPY(output+idx, san_len, san_lenSz);
    idx += san_lenSz;

    /* put value */
    XMEMCPY(output+idx, input, length);
    idx += length;

    return idx;
}
#endif /* WOLFSSL_ASN_TEMPLATE */


#ifdef WOLFSSL_CERT_GEN

int FlattenAltNames(byte* output, word32 outputSz, const DNS_entry* names)
{
    word32 idx;
    const DNS_entry* curName;
    word32 namesSz = 0;

    if (output == NULL)
        return BAD_FUNC_ARG;

    if (names == NULL)
        return 0;

    curName = names;
    do {
        namesSz += curName->len + 2 +
            ((curName->len < ASN_LONG_LENGTH) ? 0
             : BytePrecision(curName->len));
        curName = curName->next;
    } while (curName != NULL);

    if (outputSz < MAX_SEQ_SZ + namesSz)
        return BUFFER_E;

    idx = SetSequence(namesSz, output);

    curName = names;
    do {
        output[idx] = ASN_CONTEXT_SPECIFIC | curName->type;
        if (curName->type == ASN_DIR_TYPE) {
            output[idx] |= ASN_CONSTRUCTED;
        }
        idx++;
        idx += SetLength(curName->len, output + idx);
        XMEMCPY(output + idx, curName->name, curName->len);
        idx += curName->len;
        curName = curName->next;
    } while (curName != NULL);

    return idx;
}

#endif /* WOLFSSL_CERT_GEN */

#endif /* WOLFSSL_ALT_NAMES */

/* Simple domain name OID size. */
#define DN_OID_SZ     3

/* Encodes one attribute of the name (issuer/subject)
 *
 * name     structure to hold result of encoding
 * nameStr  value to be encoded
 * nameTag  tag of encoding i.e CTC_UTF8
 * type     id of attribute i.e ASN_COMMON_NAME
 * emailTag tag of email i.e CTC_UTF8
 * returns length on success
 */
static int wc_EncodeName_ex(EncodedName* name, const char* nameStr,
                            byte nameTag, byte type, byte emailTag)
{
#if !defined(WOLFSSL_ASN_TEMPLATE)
    word32 idx = 0;
    /* bottom up */
    byte firstLen[1 + MAX_LENGTH_SZ];
    byte secondLen[MAX_LENGTH_SZ];
    byte sequence[MAX_SEQ_SZ];
    byte set[MAX_SET_SZ];

    int strLen;
    int thisLen;
    int firstSz, secondSz, seqSz, setSz;

    if (nameStr == NULL) {
        name->used = 0;
        return 0;
    }
    thisLen = strLen = (int)XSTRLEN(nameStr);
    if (strLen == 0) { /* no user data for this item */
        name->used = 0;
        return 0;
    }

    /* Restrict country code size */
    if (ASN_COUNTRY_NAME == type && strLen != CTC_COUNTRY_SIZE) {
        WOLFSSL_MSG("Country code size error");
        return ASN_COUNTRY_SIZE_E;
    }

    secondSz = SetLength(strLen, secondLen);
    thisLen += secondSz;
    switch (type) {
        case ASN_EMAIL_NAME: /* email */
            thisLen += EMAIL_JOINT_LEN;
            firstSz  = EMAIL_JOINT_LEN;
            break;

        case ASN_DOMAIN_COMPONENT:
            thisLen += PILOT_JOINT_LEN;
            firstSz  = PILOT_JOINT_LEN;
            break;

        default:
            thisLen++;                                 /* str type */
            thisLen += JOINT_LEN;
            firstSz  = JOINT_LEN + 1;
    }
    thisLen++; /* id  type */
    firstSz  = SetObjectId(firstSz, firstLen);
    thisLen += firstSz;

    seqSz = SetSequence(thisLen, sequence);
    thisLen += seqSz;
    setSz = SetSet(thisLen, set);
    thisLen += setSz;

    if (thisLen > (int)sizeof(name->encoded)) {
        return BUFFER_E;
    }

    /* store it */
    idx = 0;
    /* set */
    XMEMCPY(name->encoded, set, setSz);
    idx += setSz;
    /* seq */
    XMEMCPY(name->encoded + idx, sequence, seqSz);
    idx += seqSz;
    /* asn object id */
    XMEMCPY(name->encoded + idx, firstLen, firstSz);
    idx += firstSz;
    switch (type) {
        case ASN_EMAIL_NAME:
        {
            const byte EMAIL_OID[] = {
                0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x01
            };
            /* email joint id */
            XMEMCPY(name->encoded + idx, EMAIL_OID, sizeof(EMAIL_OID));
            idx += (int)sizeof(EMAIL_OID);
            name->encoded[idx++] = emailTag;
            break;
        }

        case ASN_DOMAIN_COMPONENT:
        {
            const byte PILOT_OID[] = {
                0x09, 0x92, 0x26, 0x89, 0x93, 0xF2, 0x2C, 0x64, 0x01
            };

            XMEMCPY(name->encoded + idx, PILOT_OID, sizeof(PILOT_OID));
            idx += (int)sizeof(PILOT_OID);
            /* id type */
            name->encoded[idx++] = type;
            /* str type */
            name->encoded[idx++] = nameTag;
            break;
        }

        default:
            name->encoded[idx++] = 0x55;
            name->encoded[idx++] = 0x04;
            /* id type */
            name->encoded[idx++] = type;
            /* str type */
            name->encoded[idx++] = nameTag;
    }
    /* second length */
    XMEMCPY(name->encoded + idx, secondLen, secondSz);
    idx += secondSz;
    /* str value */
    XMEMCPY(name->encoded + idx, nameStr, strLen);
    idx += strLen;

    name->type = type;
    name->totalLen = idx;
    name->used = 1;

    return idx;
#else
    ASNSetData dataASN[rdnASN_Length];
    ASNItem nameASN[rdnASN_Length];
    byte dnOid[DN_OID_SZ] = { 0x55, 0x04, 0x00 };
    int ret = 0;
    int sz;
    const byte* oid;
    int oidSz;

    /* Validate input parameters. */
    if ((name == NULL) || (nameStr == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    if (ret == 0) {
        /* Clear data to use when encoding. */
        XMEMSET(dataASN, 0, rdnASN_Length * sizeof(ASNSetData));
        /* Copy the RDN encoding template. ASN.1 tag for the name string is set
         * based on type. */
        XMEMCPY(nameASN, rdnASN, rdnASN_Length * sizeof(ASNItem));

        /* Set OID and ASN.1 tag for name depending on type. */
        switch (type) {
            case ASN_EMAIL_NAME:
                /* email OID different to standard types. */
                oid = emailOid;
                oidSz = sizeof(emailOid);
                /* Use email specific type/tag. */
                nameTag = emailTag;
                break;
            case ASN_DOMAIN_COMPONENT:
                /* Domain component OID different to standard types. */
                oid = dcOid;
                oidSz = sizeof(dcOid);
                break;
            default:
                /* Construct OID using type. */
                dnOid[2] = type;
                oid = dnOid;
                oidSz = DN_OID_SZ;
                break;
        }
    }
    if (ret == 0) {
        /* Set OID corresponding to the name type. */
        SetASN_Buffer(&dataASN[2], oid, oidSz);
        /* Set name string. */
        SetASN_Buffer(&dataASN[3], (const byte *)nameStr,
            (word32)XSTRLEN(nameStr));
        /* Set the ASN.1 tag for the name string. */
        nameASN[3].tag = nameTag;

        /* Calculate size of encoded name and indexes of components. */
        ret = SizeASN_Items(nameASN, dataASN, rdnASN_Length, &sz);
    }
    /* Check if name's buffer is big enough. */
    if ((ret == 0) && (sz > (int)sizeof(name->encoded))) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode name into the buffer. */
        SetASN_Items(nameASN, dataASN, rdnASN_Length, name->encoded);
        /* Cache the type and size, and set that it is used. */
        name->type = type;
        name->totalLen = sz;
        name->used = 1;

        /* Return size of encoding. */
        ret = sz;
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* canonical encoding one attribute of the name (issuer/subject)
 * call wc_EncodeName_ex with CTC_UTF8 for email type
 *
 * name     structure to hold result of encoding
 * nameStr  value to be encoded
 * nameType type of encoding i.e CTC_UTF8
 * type     id of attribute i.e ASN_COMMON_NAME
 *
 * returns length on success
 */
int wc_EncodeNameCanonical(EncodedName* name, const char* nameStr,
                           char nameType, byte type)
{
    return wc_EncodeName_ex(name, nameStr, (byte)nameType, type,
        ASN_UTF8STRING);
}

/* Encodes one attribute of the name (issuer/subject)
 * call we_EncodeName_ex with 0x16, IA5String for email type
 * name     structure to hold result of encoding
 * nameStr  value to be encoded
 * nameType type of encoding i.e CTC_UTF8
 * type     id of attribute i.e ASN_COMMON_NAME
 *
 * returns length on success
 */
int wc_EncodeName(EncodedName* name, const char* nameStr, char nameType,
                  byte type)
{
    return wc_EncodeName_ex(name, nameStr, (byte)nameType, type,
        ASN_IA5_STRING);
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* Simple name OID size. */
#define NAME_OID_SZ     3

/* Domain name OIDs. */
static const byte nameOid[NAME_ENTRIES - 1][NAME_OID_SZ] = {
    { 0x55, 0x04, ASN_COUNTRY_NAME },
    { 0x55, 0x04, ASN_STATE_NAME },
    { 0x55, 0x04, ASN_LOCALITY_NAME },
    { 0x55, 0x04, ASN_SUR_NAME },
    { 0x55, 0x04, ASN_ORG_NAME },
    { 0x55, 0x04, ASN_ORGUNIT_NAME },
    { 0x55, 0x04, ASN_COMMON_NAME },
    { 0x55, 0x04, ASN_SERIAL_NUMBER },
#ifdef WOLFSSL_CERT_EXT
    { 0x55, 0x04, ASN_BUS_CAT },
#endif
    /* Email OID is much longer. */
};

static void SetRdnItems(ASNItem* namesASN, ASNSetData* dataASN, const byte* oid,
    int oidSz, byte tag, const byte* data, int sz)
{
    XMEMCPY(namesASN, rdnASN, sizeof(rdnASN));
    SetASN_Buffer(&dataASN[2], oid, oidSz);
    namesASN[3].tag = tag;
    SetASN_Buffer(&dataASN[3], data, sz);
}

#ifdef WOLFSSL_MULTI_ATTRIB
static int FindMultiAttrib(CertName* name, int id, int* idx)
{
    int i;
    for (i = *idx + 1; i < CTC_MAX_ATTRIB; i++) {
        if (name->name[i].sz > 0 && name->name[i].id == id) {
            break;
        }
    }
    if (i == CTC_MAX_ATTRIB) {
        i = -1;
    }
    *idx = i;
    return i >= 0;
}
#endif

/* ASN.1 template for the SEQUENCE around the RDNs.
 * X.509: RFC 5280, 4.1.2.4 - RDNSequence
 */
static const ASNItem nameASN[] = {
    { 0, ASN_SEQUENCE, 1, 1, 0 },
};

/* Number of items in ASN.1 template for the SEQUENCE around the RDNs. */
#define nameASN_Length (sizeof(nameASN) / sizeof(ASNItem))
#endif

/* encode CertName into output, return total bytes written */
int SetNameEx(byte* output, word32 outputSz, CertName* name, void* heap)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int          totalBytes = 0, i, idx;
#ifdef WOLFSSL_SMALL_STACK
    EncodedName* names = NULL;
#else
    EncodedName  names[NAME_ENTRIES];
#endif
#ifdef WOLFSSL_MULTI_ATTRIB
    EncodedName addNames[CTC_MAX_ATTRIB];
    int j, type;
#endif

    if (output == NULL || name == NULL)
        return BAD_FUNC_ARG;

    if (outputSz < 3)
        return BUFFER_E;

#ifdef WOLFSSL_SMALL_STACK
    names = (EncodedName*)XMALLOC(sizeof(EncodedName) * NAME_ENTRIES, NULL,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
    if (names == NULL)
        return MEMORY_E;
#endif

    for (i = 0; i < NAME_ENTRIES; i++) {
        int ret;
        const char* nameStr = GetOneCertName(name, i);

        ret = wc_EncodeName(&names[i], nameStr, GetNameType(name, i),
                          GetCertNameId(i));
        if (ret < 0) {
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            WOLFSSL_MSG("EncodeName failed");
            return BUFFER_E;
        }
        totalBytes += ret;
    }
#ifdef WOLFSSL_MULTI_ATTRIB
    for (i = 0; i < CTC_MAX_ATTRIB; i++) {
        if (name->name[i].sz > 0) {
            int ret;
            ret = wc_EncodeName(&addNames[i], name->name[i].value,
                        name->name[i].type, name->name[i].id);
            if (ret < 0) {
            #ifdef WOLFSSL_SMALL_STACK
                XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
            #endif
                WOLFSSL_MSG("EncodeName on multiple attributes failed\n");
                return BUFFER_E;
            }
            totalBytes += ret;
        }
        else {
            addNames[i].used = 0;
        }
    }
#endif /* WOLFSSL_MULTI_ATTRIB */

    /* header */
    idx = SetSequence(totalBytes, output);
    totalBytes += idx;
    if (totalBytes > ASN_NAME_MAX) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        WOLFSSL_MSG("Total Bytes is greater than ASN_NAME_MAX");
        return BUFFER_E;
    }

    for (i = 0; i < NAME_ENTRIES; i++) {
    #ifdef WOLFSSL_MULTI_ATTRIB
        type = GetCertNameId(i);

        /* list all DC values before OUs */
        if (type == ASN_ORGUNIT_NAME) {
            type = ASN_DOMAIN_COMPONENT;
            for (j = 0; j < CTC_MAX_ATTRIB; j++) {
                if (name->name[j].sz > 0 && type == name->name[j].id) {
                    if (outputSz < (word32)(idx+addNames[j].totalLen)) {
                    #ifdef WOLFSSL_SMALL_STACK
                        XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
                    #endif
                        WOLFSSL_MSG("Not enough space left for DC value");
                        return BUFFER_E;
                    }

                    XMEMCPY(output + idx, addNames[j].encoded,
                            addNames[j].totalLen);
                    idx += addNames[j].totalLen;
                }
            }
            type = ASN_ORGUNIT_NAME;
        }

        /* write all similar types to the buffer */
        for (j = 0; j < CTC_MAX_ATTRIB; j++) {
            if (name->name[j].sz > 0 && type == name->name[j].id) {
                if (outputSz < (word32)(idx+addNames[j].totalLen)) {
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
                #endif
                    return BUFFER_E;
                }

                XMEMCPY(output + idx, addNames[j].encoded,
                        addNames[j].totalLen);
                idx += addNames[j].totalLen;
            }
        }
    #endif /* WOLFSSL_MULTI_ATTRIB */

        if (names[i].used) {
            if (outputSz < (word32)(idx+names[i].totalLen)) {
#ifdef WOLFSSL_SMALL_STACK
                XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
                return BUFFER_E;
            }

            XMEMCPY(output + idx, names[i].encoded, names[i].totalLen);
            idx += names[i].totalLen;
        }
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(names, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif
    (void)heap;

    return totalBytes;
#else
    /* TODO: consider calculating size of entries, putting length into
     * SEQUENCE, encode SEQUENCE, encode entries into buffer.  */
    ASNSetData* dataASN;
    ASNItem*    namesASN;
    int         i;
    int         idx;
    int         ret = 0;
    int         sz;
    int         nameLen[NAME_ENTRIES];
#ifdef WOLFSSL_MULTI_ATTRIB
    int         j;
#endif

    /* Calculate length of name entries and size for allocating. */
    idx = nameASN_Length;
    for (i = 0; i < NAME_ENTRIES; i++) {
        /* Keep name length to identify component is to be encoded. */
        nameLen[i] = (int)XSTRLEN(GetOneCertName(name, i));
        if (nameLen[i] > 0) {
            idx += rdnASN_Length;
        }
    }
    #ifdef WOLFSSL_MULTI_ATTRIB
    /* Count the extra attributes too. */
    for (i = 0; i < CTC_MAX_ATTRIB; i++) {
        if (name->name[i].sz > 0)
            idx += rdnASN_Length;
    }
    #endif

    /* Allocate dynamic data items. */
    dataASN = (ASNSetData*)XMALLOC(idx * sizeof(ASNSetData), heap,
                                   DYNAMIC_TYPE_TMP_BUFFER);
    if (dataASN == NULL) {
        ret = MEMORY_E;
    }
    if (ret == 0) {
        /* Allocate dynamic ASN.1 template items. */
        namesASN = (ASNItem*)XMALLOC(idx * sizeof(ASNItem), heap,
                                     DYNAMIC_TYPE_TMP_BUFFER);
        if (namesASN == NULL) {
            ret = MEMORY_E;
        }
    }

    if (ret == 0) {
        /* Clear the dynamic data. */
        XMEMSET(dataASN, 0, idx * sizeof(ASNSetData));
        /* Copy in the outer sequence. */
        XMEMCPY(namesASN, nameASN, sizeof(nameASN));

        idx = nameASN_Length;
        for (i = 0; i < NAME_ENTRIES; i++) {
            int email = (i == (NAME_ENTRIES - 1));

        #ifdef WOLFSSL_MULTI_ATTRIB
            int type = GetCertNameId(i);

            if (type == ASN_ORGUNIT_NAME) {
                j = -1;
                /* Put DomainComponents before OrgUnitName. */
                while (FindMultiAttrib(name, ASN_DOMAIN_COMPONENT, &j)) {
                    /* Copy data into dynamic vars. */
                    SetRdnItems(namesASN + idx, dataASN + idx, dcOid,
                        sizeof(dcOid), name->name[j].type,
                        (byte*)name->name[j].value, name->name[j].sz);
                    idx += rdnASN_Length;
                }
            }
        #endif

            if (nameLen[i] > 0) {
                /* Write out first instance of attribute type. */
                if (email) {
                    /* Copy email data into dynamic vars. */
                    SetRdnItems(namesASN + idx, dataASN + idx, emailOid,
                        sizeof(emailOid), ASN_IA5_STRING,
                        (const byte*)GetOneCertName(name, i), nameLen[i]);
                }
                else {
                    /* Copy name data into dynamic vars. */
                    SetRdnItems(namesASN + idx, dataASN + idx, nameOid[i],
                        NAME_OID_SZ, GetNameType(name, i),
                        (const byte*)GetOneCertName(name, i), nameLen[i]);
                }
                idx += rdnASN_Length;
            }

        #ifdef WOLFSSL_MULTI_ATTRIB
            j = -1;
            /* Write all other attributes of this type. */
            while (FindMultiAttrib(name, type, &j)) {
                /* Copy data into dynamic vars. */
                SetRdnItems(namesASN + idx, dataASN + idx, nameOid[type],
                    NAME_OID_SZ, name->name[j].type,
                    (byte*)name->name[j].value, name->name[j].sz);
                idx += rdnASN_Length;
            }
        #endif
        }

        /* Calculate size of encoding. */
        ret = SizeASN_Items(namesASN, dataASN, idx, &sz);
    }
    /* Check buffer size if passed in. */
    if ((ret == 0) && (output != NULL) && (sz > (int)outputSz)) {
        ret = BUFFER_E;
    }
    if ((ret == 0) && (output != NULL)) {
        /* Encode Name. */
        SetASN_Items(namesASN, dataASN, idx, output);
    }
    if (ret == 0) {
        /* Return the encoding size. */
        ret = sz;
    }

    XFREE(namesASN, heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(dataASN, heap, DYNAMIC_TYPE_TMP_BUFFER);
    (void)heap;
    return ret;
#endif
}
int SetName(byte* output, word32 outputSz, CertName* name)
{
    return SetNameEx(output, outputSz, name, NULL);
}

#ifdef WOLFSSL_ASN_TEMPLATE
static int EncodePublicKey(int keyType, byte* output, int outLen,
                           RsaKey* rsaKey, ecc_key* eccKey,
                           ed25519_key* ed25519Key, ed448_key* ed448Key,
                           DsaKey* dsaKey, const byte* ntruKey, word16 ntruSz)
{
    int ret = 0;
#ifdef HAVE_NTRU
    word32 rc;
    word16 encodedSz;
#endif

    (void)outLen;
    (void)rsaKey;
    (void)eccKey;
    (void)ed25519Key;
    (void)ed448Key;
    (void)dsaKey;
    (void)ntruKey;
    (void)ntruSz;

    switch (keyType) {
    #ifndef NO_RSA
        case RSA_KEY:
            ret = SetRsaPublicKey(output, rsaKey, outLen, 1);
            if (ret <= 0) {
                ret = PUBLIC_KEY_E;
            }
            break;
    #endif
    #ifdef HAVE_ECC
        case ECC_KEY:
            ret = SetEccPublicKey(output, eccKey, outLen, 1);
            if (ret <= 0) {
                ret = PUBLIC_KEY_E;
            }
            break;
    #endif /* HAVE_ECC */
    #ifdef HAVE_ED25519
        case ED25519_KEY:
            ret = wc_Ed25519PublicKeyToDer(ed25519Key, output, outLen, 1);
            if (ret <= 0) {
                ret = PUBLIC_KEY_E;
            }
            break;
    #endif
    #ifdef HAVE_ED448
        case ED448_KEY:
            ret = wc_Ed448PublicKeyToDer(ed448Key, output, outLen, 1);
            if (ret <= 0) {
                ret = PUBLIC_KEY_E;
            }
            break;
    #endif
    #ifdef HAVE_NTRU
        case NTRU_KEY:
            rc = ntru_crypto_ntru_encrypt_publicKey2SubjectPublicKeyInfo(ntruSz,
                                                   ntruKey, &encodedSz, output);
            if (rc != NTRU_OK) {
                ret = PUBLIC_KEY_E;
            }
            if (ret == 0) {
                ret = encodedSz;
            }
            break;
    #endif /* HAVE_NTRU */
        default:
            ret = PUBLIC_KEY_E;
            break;
    }

    return ret;
}

/* ASN.1 template for certificate extensions.
 * X.509: RFC 5280, 4.1 - Basic Certificate Fields.
 * All extensions supported for encoding are described.
 */
static const ASNItem certExtsASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* Basic Constraints Extension - 4.2.1.9 */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  2 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  3 */            { 2, ASN_OCTET_STRING, 0, 1, 0 },
/*  4 */                { 3, ASN_SEQUENCE, 1, 1, 0 },
                            /* cA */
/*  5 */                    { 4, ASN_BOOLEAN, 0, 0, 0 },
                            /* pathLenConstraint */
/*  6 */                    { 4, ASN_INTEGER, 0, 0, 1 },
                /* Subject Alternative Name - 4.2.1.6  */
/*  7 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  8 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
                    /*  */
/*  9 */            { 2, ASN_OCTET_STRING, 0, 0, 0 },
#ifdef WOLFSSL_CERT_EXT
                /* Subject Key Identifier - 4.2.1.2 */
/* 10 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/* 11 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/* 12 */            { 2, ASN_OCTET_STRING, 0, 1, 0 },
/* 13 */                { 3, ASN_OCTET_STRING, 0, 0, 0 },
                /* Authority Key Identifier - 4.2.1.1 */
/* 14 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/* 15 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/* 16 */            { 2, ASN_OCTET_STRING, 0, 1, 0 },
/* 17 */                { 3, ASN_SEQUENCE, 1, 1, 0 },
/* 18 */                    { 4, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 0 },
                /* Key Usage - 4.2.1.3 */
/* 19 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/* 20 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/* 21 */            { 2, ASN_BOOLEAN, 0, 0, 0 },
/* 22 */            { 2, ASN_OCTET_STRING, 0, 1, 0 },
/* 23 */                { 3, ASN_BIT_STRING, 0, 0, 0 },
                /* Extended Key Usage - 4,2,1,12 */
/* 24 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/* 25 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/* 26 */            { 2, ASN_OCTET_STRING, 0, 0, 0 },
                /* Certificate Policies - 4.2.1.4 */
/* 27 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/* 28 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/* 29 */            { 2, ASN_OCTET_STRING, 0, 1, 0 },
/* 30 */                { 3, ASN_SEQUENCE, 0, 0, 0 },
#endif
};

/* Number of items in ASN.1 template for certificate extensions. */
#define certExtsASN_Length (sizeof(certExtsASN) / sizeof(ASNItem))

static int EncodeExtensions(Cert* cert, byte* output, word32 maxSz,
                            int forRequest)
{
    DECL_ASNSETDATA(dataASN, certExtsASN_Length);
    int sz;
    int ret = 0;
    static const byte bcOID[]   = { 0x55, 0x1d, 0x13 };
#ifdef WOLFSSL_ALT_NAMES
    static const byte sanOID[]  = { 0x55, 0x1d, 0x11 };
#endif
#ifdef WOLFSSL_CERT_EXT
    static const byte skidOID[] = { 0x55, 0x1d, 0x0e };
    static const byte akidOID[] = { 0x55, 0x1d, 0x23 };
    static const byte kuOID[]   = { 0x55, 0x1d, 0x0f };
    static const byte ekuOID[]  = { 0x55, 0x1d, 0x25 };
    static const byte cpOID[]   = { 0x55, 0x1d, 0x20 };
#endif

    (void)forRequest;

    CALLOC_ASNSETDATA(dataASN, certExtsASN_Length, ret, cert->heap);

    if (ret == 0) {
        if (cert->isCA) {
            /* Set Basic Constraints to be a Certificate Authority. */
            SetASN_Boolean(&dataASN[5], 1);
            SetASN_Buffer(&dataASN[2], bcOID, sizeof(bcOID));
            /* TODO: consider adding path length field in Cert. */
            dataASN[6].noOut = 1;
        }
        else {
            /* Don't write out Basic Constraints extension items. */
            SetASNItem_NoOut(dataASN, 1, 6);
        }
    #ifdef WOLFSSL_ALT_NAMES
        if (!forRequest && cert->altNamesSz > 0) {
            /* Set Subject Alternative Name OID and data. */
            SetASN_Buffer(&dataASN[8], sanOID, sizeof(sanOID));
            SetASN_Buffer(&dataASN[9], cert->altNames, cert->altNamesSz);
        }
        else
    #endif
        {
            /* Don't write out Subject Alternative Name extension items. */
            SetASNItem_NoOut(dataASN, 7, 9);
        }
    #ifdef WOLFSSL_CERT_EXT
        if (cert->skidSz > 0) {
            /* Set Subject Key Identifier OID and data. */
            SetASN_Buffer(&dataASN[11], skidOID, sizeof(skidOID));
            SetASN_Buffer(&dataASN[13], cert->skid, cert->skidSz);
        }
        else {
            /* Don't write out Subject Key Identifier extension items. */
            SetASNItem_NoOut(dataASN, 10, 13);
        }
        if (cert->akidSz > 0) {
            /* Set Authority Key Identifier OID and data. */
            SetASN_Buffer(&dataASN[15], akidOID, sizeof(akidOID));
            SetASN_Buffer(&dataASN[18], cert->akid, cert->akidSz);
        }
        else {
            /* Don't write out Authority Key Identifier extension items. */
            SetASNItem_NoOut(dataASN, 14, 18);
        }
        if (cert->keyUsage != 0) {
            /* Set Key Usage OID, critical and value. */
            SetASN_Buffer(&dataASN[20], kuOID, sizeof(kuOID));
            SetASN_Boolean(&dataASN[21], 1);
            SetASN_Int16Bit(&dataASN[23], cert->keyUsage);
        }
        else {
            /* Don't write out Key Usage extension items. */
            SetASNItem_NoOut(dataASN, 19, 23);
        }
        if (cert->extKeyUsage != 0) {
            /* Calculate size of Extended Key Usage data. */
            sz = SetExtKeyUsage(cert, NULL, 0, cert->extKeyUsage);
            if (sz <= 0) {
                ret = KEYUSAGE_E;
            }
            /* Set Extended Key Usage OID and data. */
            SetASN_Buffer(&dataASN[25], ekuOID, sizeof(ekuOID));
            SetASN_Buffer(&dataASN[26], NULL, sz);
        }
        else {
            /* Don't write out Extended Key Usage extension items. */
            SetASNItem_NoOut(dataASN, 24, 26);
        }

        if ((!forRequest) && (cert->certPoliciesNb > 0)) {
            /* Calculate size of certificate policies. */
            sz = SetCertificatePolicies(NULL, 0, cert->certPolicies,
                    cert->certPoliciesNb, cert->heap);
            if (sz > 0) {
                /* Set Certificate Policies OID. */
                SetASN_Buffer(&dataASN[28], cpOID, sizeof(cpOID));
                /* Make space for data. */
                SetASN_Buffer(&dataASN[30], NULL, sz);
            }
            else {
                ret = CERTPOLICIES_E;
            }
        }
        else {
            /* Don't write out Certificate Policies extension items. */
            SetASNItem_NoOut(dataASN, 27, 30);
        }
    #endif
    }

    if (ret == 0) {
        /* Calculate size of encoded extensions. */
        ret = SizeASN_Items(certExtsASN, dataASN, certExtsASN_Length, &sz);
    }
    if (ret == 0) {
        /* Only SEQUENCE - don't encode extensions. */
        if (sz == 2) {
            sz = 0;
        }
        /* Check buffer is big enough. */
        else if ((output != NULL) && (sz > (int)maxSz)) {
            ret = BUFFER_E;
        }
    }

    if ((ret == 0) && (output != NULL) && (sz > 0)) {
        /* Encode certificate extensions into buffer. */
        SetASN_Items(certExtsASN, dataASN, certExtsASN_Length, output);

    #ifdef WOLFSSL_CERT_EXT
        if (cert->keyUsage != 0){
            /* Encode Extended Key Usage into space provided. */
            if (SetExtKeyUsage(cert, (byte*)dataASN[26].data.buffer.data,
                dataASN[26].data.buffer.length, cert->extKeyUsage) <= 0) {
                ret = KEYUSAGE_E;
            }
        }
        if ((!forRequest) && (cert->certPoliciesNb > 0)) {
            /* Encode Certificate Policies into space provided. */
            if (SetCertificatePolicies((byte*)dataASN[30].data.buffer.data,
                    dataASN[30].data.buffer.length, cert->certPolicies,
                    cert->certPoliciesNb, cert->heap) <= 0) {
                ret = CERTPOLICIES_E;
            }
        }
    #endif
    }
    if (ret == 0) {
        /* Return the encoding size. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, cert->heap);
    return ret;
}
#endif /* WOLFSSL_ASN_TEMPLATE */

#ifndef WOLFSSL_ASN_TEMPLATE
/* Set Date validity from now until now + daysValid
 * return size in bytes written to output, 0 on error */
static int SetValidity(byte* output, int daysValid)
{
#ifndef NO_ASN_TIME
    byte before[MAX_DATE_SIZE];
    byte  after[MAX_DATE_SIZE];

    int beforeSz;
    int afterSz;
    int seqSz;

    time_t now;
    time_t then;
    struct tm* tmpTime;
    struct tm* expandedTime;
    struct tm localTime;

#if defined(NEED_TMP_TIME)
    /* for use with gmtime_r */
    struct tm tmpTimeStorage;
    tmpTime = &tmpTimeStorage;
#else
    tmpTime = NULL;
#endif
    (void)tmpTime;

    now = XTIME(0);

    /* before now */
    before[0] = ASN_GENERALIZED_TIME;
    beforeSz = SetLength(ASN_GEN_TIME_SZ, before + 1) + 1;  /* gen tag */

    /* subtract 1 day of seconds for more compliance */
    then = now - 86400;
    expandedTime = XGMTIME(&then, tmpTime);
    if (expandedTime == NULL) {
        WOLFSSL_MSG("XGMTIME failed");
        return 0;   /* error */
    }
    localTime = *expandedTime;

    /* adjust */
    localTime.tm_year += 1900;
    localTime.tm_mon +=    1;

    SetTime(&localTime, before + beforeSz);
    beforeSz += ASN_GEN_TIME_SZ;

    after[0] = ASN_GENERALIZED_TIME;
    afterSz  = SetLength(ASN_GEN_TIME_SZ, after + 1) + 1;  /* gen tag */

    /* add daysValid of seconds */
    then = now + (daysValid * (time_t)86400);
    expandedTime = XGMTIME(&then, tmpTime);
    if (expandedTime == NULL) {
        WOLFSSL_MSG("XGMTIME failed");
        return 0;   /* error */
    }
    localTime = *expandedTime;

    /* adjust */
    localTime.tm_year += 1900;
    localTime.tm_mon  +=    1;

    SetTime(&localTime, after + afterSz);
    afterSz += ASN_GEN_TIME_SZ;

    /* headers and output */
    seqSz = SetSequence(beforeSz + afterSz, output);
    XMEMCPY(output + seqSz, before, beforeSz);
    XMEMCPY(output + seqSz + beforeSz, after, afterSz);

    return seqSz + beforeSz + afterSz;
#else
    (void)output;
    (void)daysValid;
    return NOT_COMPILED_IN;
#endif
}
#else
static int SetValidity(byte* before, byte* after, int daysValid)
{
    int ret = 0;
    time_t now;
    time_t then;
    struct tm* tmpTime;
    struct tm* expandedTime;
    struct tm localTime;
#if defined(NEED_TMP_TIME)
    /* for use with gmtime_r */
    struct tm tmpTimeStorage;
    tmpTime = &tmpTimeStorage;
#else
    tmpTime = NULL;
#endif
    (void)tmpTime;

    now = XTIME(0);

    /* subtract 1 day of seconds for more compliance */
    then = now - 86400;
    expandedTime = XGMTIME(&then, tmpTime);
    if (expandedTime == NULL) {
        WOLFSSL_MSG("XGMTIME failed");
        ret = DATE_E;
    }
    if (ret == 0) {
        localTime = *expandedTime;

        /* adjust */
        localTime.tm_year += 1900;
        localTime.tm_mon +=    1;

        SetTime(&localTime, before);

        /* add daysValid of seconds */
        then = now + (daysValid * (time_t)86400);
        expandedTime = XGMTIME(&then, tmpTime);
        if (expandedTime == NULL) {
            WOLFSSL_MSG("XGMTIME failed");
            ret = DATE_E;
        }
    }
    if (ret == 0) {
        localTime = *expandedTime;

        /* adjust */
        localTime.tm_year += 1900;
        localTime.tm_mon  +=    1;

        SetTime(&localTime, after);
    }

    return ret;
}
#endif /* WOLFSSL_ASN_TEMPLATE */


#ifndef WOLFSSL_ASN_TEMPLATE
/* encode info from cert into DER encoded format */
static int EncodeCert(Cert* cert, DerCert* der, RsaKey* rsaKey, ecc_key* eccKey,
                      WC_RNG* rng, const byte* ntruKey, word16 ntruSz, DsaKey* dsaKey,
                      ed25519_key* ed25519Key, ed448_key* ed448Key)
{
    int ret;

    if (cert == NULL || der == NULL || rng == NULL)
        return BAD_FUNC_ARG;

    /* make sure at least one key type is provided */
    if (rsaKey == NULL && eccKey == NULL && ed25519Key == NULL &&
            dsaKey == NULL && ed448Key == NULL && ntruKey == NULL) {
        return PUBLIC_KEY_E;
    }

    /* init */
    XMEMSET(der, 0, sizeof(DerCert));

    /* version */
    der->versionSz = SetMyVersion(cert->version, der->version, TRUE);

    /* serial number (must be positive) */
    if (cert->serialSz == 0) {
        /* generate random serial */
        cert->serialSz = CTC_GEN_SERIAL_SZ;
        ret = wc_RNG_GenerateBlock(rng, cert->serial, cert->serialSz);
        if (ret != 0)
            return ret;
        /* Clear the top bit to avoid a negative value */
        cert->serial[0] &= 0x7f;
    }
    der->serialSz = SetSerialNumber(cert->serial, cert->serialSz, der->serial,
        sizeof(der->serial), CTC_SERIAL_SIZE);
    if (der->serialSz < 0)
        return der->serialSz;

    /* signature algo */
    der->sigAlgoSz = SetAlgoID(cert->sigType, der->sigAlgo, oidSigType, 0);
    if (der->sigAlgoSz <= 0)
        return ALGO_ID_E;

    /* public key */
#ifndef NO_RSA
    if (cert->keyType == RSA_KEY) {
        if (rsaKey == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = SetRsaPublicKey(der->publicKey, rsaKey,
                                           sizeof(der->publicKey), 1);
    }
#endif

#ifdef HAVE_ECC
    if (cert->keyType == ECC_KEY) {
        if (eccKey == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = SetEccPublicKey(der->publicKey, eccKey,
                                           sizeof(der->publicKey), 1);
    }
#endif

#if !defined(NO_DSA) && !defined(HAVE_SELFTEST)
    if (cert->keyType == DSA_KEY) {
        if (dsaKey == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = wc_SetDsaPublicKey(der->publicKey, dsaKey,
                                              sizeof(der->publicKey), 1);
    }
#endif

#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_EXPORT)
    if (cert->keyType == ED25519_KEY) {
        if (ed25519Key == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = wc_Ed25519PublicKeyToDer(ed25519Key, der->publicKey,
            (word32)sizeof(der->publicKey), 1);
    }
#endif

#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_EXPORT)
    if (cert->keyType == ED448_KEY) {
        if (ed448Key == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = wc_Ed448PublicKeyToDer(ed448Key, der->publicKey,
            (word32)sizeof(der->publicKey), 1);
    }
#endif

#ifdef HAVE_NTRU
    if (cert->keyType == NTRU_KEY) {
        word32 rc;
        word16 encodedSz;

        if (ntruKey == NULL)
            return PUBLIC_KEY_E;

        rc  = ntru_crypto_ntru_encrypt_publicKey2SubjectPublicKeyInfo(ntruSz,
                                                   ntruKey, &encodedSz, NULL);
        if (rc != NTRU_OK)
            return PUBLIC_KEY_E;
        if (encodedSz > MAX_PUBLIC_KEY_SZ)
            return PUBLIC_KEY_E;

        rc  = ntru_crypto_ntru_encrypt_publicKey2SubjectPublicKeyInfo(ntruSz,
                                         ntruKey, &encodedSz, der->publicKey);
        if (rc != NTRU_OK)
            return PUBLIC_KEY_E;

        der->publicKeySz = encodedSz;
    }
#else
    (void)ntruSz;
#endif /* HAVE_NTRU */

    if (der->publicKeySz <= 0)
        return PUBLIC_KEY_E;

    der->validitySz = 0;
#ifdef WOLFSSL_ALT_NAMES
    /* date validity copy ? */
    if (cert->beforeDateSz && cert->afterDateSz) {
        der->validitySz = CopyValidity(der->validity, cert);
        if (der->validitySz <= 0)
            return DATE_E;
    }
#endif

    /* date validity */
    if (der->validitySz == 0) {
        der->validitySz = SetValidity(der->validity, cert->daysValid);
        if (der->validitySz <= 0)
            return DATE_E;
    }

    /* subject name */
#if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
    if (XSTRLEN((const char*)cert->sbjRaw) > 0) {
        /* Use the raw subject */
        int idx;

        der->subjectSz = min(sizeof(der->subject),
                (word32)XSTRLEN((const char*)cert->sbjRaw));
        /* header */
        idx = SetSequence(der->subjectSz, der->subject);
        if (der->subjectSz + idx > (int)sizeof(der->subject)) {
            return SUBJECT_E;
        }

        XMEMCPY((char*)der->subject + idx, (const char*)cert->sbjRaw,
                der->subjectSz);
        der->subjectSz += idx;
    }
    else
#endif
    {
        /* Use the name structure */
        der->subjectSz = SetNameEx(der->subject, sizeof(der->subject),
                &cert->subject, cert->heap);
    }
    if (der->subjectSz <= 0)
        return SUBJECT_E;

    /* issuer name */
#if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
    if (XSTRLEN((const char*)cert->issRaw) > 0) {
        /* Use the raw issuer */
        int idx;

        der->issuerSz = min(sizeof(der->issuer),
                (word32)XSTRLEN((const char*)cert->issRaw));

        /* header */
        idx = SetSequence(der->issuerSz, der->issuer);
        if (der->issuerSz + idx > (int)sizeof(der->issuer)) {
            return ISSUER_E;
        }

        XMEMCPY((char*)der->issuer + idx, (const char*)cert->issRaw,
                der->issuerSz);
        der->issuerSz += idx;
    }
    else
#endif
    {
        /* Use the name structure */
        der->issuerSz = SetNameEx(der->issuer, sizeof(der->issuer),
                cert->selfSigned ? &cert->subject : &cert->issuer, cert->heap);
    }
    if (der->issuerSz <= 0)
        return ISSUER_E;

    /* set the extensions */
    der->extensionsSz = 0;

    /* CA */
    if (cert->isCA) {
        der->caSz = SetCa(der->ca, sizeof(der->ca));
        if (der->caSz <= 0)
            return CA_TRUE_E;

        der->extensionsSz += der->caSz;
    }
    else
        der->caSz = 0;

#ifdef WOLFSSL_ALT_NAMES
    /* Alternative Name */
    if (cert->altNamesSz) {
        der->altNamesSz = SetAltNames(der->altNames, sizeof(der->altNames),
                                      cert->altNames, cert->altNamesSz);
        if (der->altNamesSz <= 0)
            return ALT_NAME_E;

        der->extensionsSz += der->altNamesSz;
    }
    else
        der->altNamesSz = 0;
#endif

#ifdef WOLFSSL_CERT_EXT
    /* SKID */
    if (cert->skidSz) {
        /* check the provided SKID size */
        if (cert->skidSz > (int)min(CTC_MAX_SKID_SIZE, sizeof(der->skid)))
            return SKID_E;

        /* Note: different skid buffers sizes for der (MAX_KID_SZ) and
            cert (CTC_MAX_SKID_SIZE). */
        der->skidSz = SetSKID(der->skid, sizeof(der->skid),
                              cert->skid, cert->skidSz);
        if (der->skidSz <= 0)
            return SKID_E;

        der->extensionsSz += der->skidSz;
    }
    else
        der->skidSz = 0;

    /* AKID */
    if (cert->akidSz) {
        /* check the provided AKID size */
        if (cert->akidSz > (int)min(CTC_MAX_AKID_SIZE, sizeof(der->akid)))
            return AKID_E;

        der->akidSz = SetAKID(der->akid, sizeof(der->akid),
                              cert->akid, cert->akidSz, cert->heap);
        if (der->akidSz <= 0)
            return AKID_E;

        der->extensionsSz += der->akidSz;
    }
    else
        der->akidSz = 0;

    /* Key Usage */
    if (cert->keyUsage != 0){
        der->keyUsageSz = SetKeyUsage(der->keyUsage, sizeof(der->keyUsage),
                                      cert->keyUsage);
        if (der->keyUsageSz <= 0)
            return KEYUSAGE_E;

        der->extensionsSz += der->keyUsageSz;
    }
    else
        der->keyUsageSz = 0;

    /* Extended Key Usage */
    if (cert->extKeyUsage != 0){
        der->extKeyUsageSz = SetExtKeyUsage(cert, der->extKeyUsage,
                                sizeof(der->extKeyUsage), cert->extKeyUsage);
        if (der->extKeyUsageSz <= 0)
            return EXTKEYUSAGE_E;

        der->extensionsSz += der->extKeyUsageSz;
    }
    else
        der->extKeyUsageSz = 0;

    /* Certificate Policies */
    if (cert->certPoliciesNb != 0) {
        der->certPoliciesSz = SetCertificatePolicies(der->certPolicies,
                                                     sizeof(der->certPolicies),
                                                     cert->certPolicies,
                                                     cert->certPoliciesNb,
                                                     cert->heap);
        if (der->certPoliciesSz <= 0)
            return CERTPOLICIES_E;

        der->extensionsSz += der->certPoliciesSz;
    }
    else
        der->certPoliciesSz = 0;
#endif /* WOLFSSL_CERT_EXT */

    /* put extensions */
    if (der->extensionsSz > 0) {

        /* put the start of extensions sequence (ID, Size) */
        der->extensionsSz = SetExtensionsHeader(der->extensions,
                                                sizeof(der->extensions),
                                                der->extensionsSz);
        if (der->extensionsSz <= 0)
            return EXTENSIONS_E;

        /* put CA */
        if (der->caSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->ca, der->caSz);
            if (ret == 0)
                return EXTENSIONS_E;
        }

#ifdef WOLFSSL_ALT_NAMES
        /* put Alternative Names */
        if (der->altNamesSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->altNames, der->altNamesSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }
#endif

#ifdef WOLFSSL_CERT_EXT
        /* put SKID */
        if (der->skidSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->skid, der->skidSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put AKID */
        if (der->akidSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->akid, der->akidSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put KeyUsage */
        if (der->keyUsageSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->keyUsage, der->keyUsageSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put ExtendedKeyUsage */
        if (der->extKeyUsageSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->extKeyUsage, der->extKeyUsageSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put Certificate Policies */
        if (der->certPoliciesSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->certPolicies, der->certPoliciesSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }
#endif /* WOLFSSL_CERT_EXT */
    }

    der->total = der->versionSz + der->serialSz + der->sigAlgoSz +
        der->publicKeySz + der->validitySz + der->subjectSz + der->issuerSz +
        der->extensionsSz;

    return 0;
}


/* write DER encoded cert to buffer, size already checked */
static int WriteCertBody(DerCert* der, byte* buf)
{
    int idx;

    /* signed part header */
    idx = SetSequence(der->total, buf);
    /* version */
    XMEMCPY(buf + idx, der->version, der->versionSz);
    idx += der->versionSz;
    /* serial */
    XMEMCPY(buf + idx, der->serial, der->serialSz);
    idx += der->serialSz;
    /* sig algo */
    XMEMCPY(buf + idx, der->sigAlgo, der->sigAlgoSz);
    idx += der->sigAlgoSz;
    /* issuer */
    XMEMCPY(buf + idx, der->issuer, der->issuerSz);
    idx += der->issuerSz;
    /* validity */
    XMEMCPY(buf + idx, der->validity, der->validitySz);
    idx += der->validitySz;
    /* subject */
    XMEMCPY(buf + idx, der->subject, der->subjectSz);
    idx += der->subjectSz;
    /* public key */
    XMEMCPY(buf + idx, der->publicKey, der->publicKeySz);
    idx += der->publicKeySz;
    if (der->extensionsSz) {
        /* extensions */
        XMEMCPY(buf + idx, der->extensions, min(der->extensionsSz,
                                                   (int)sizeof(der->extensions)));
        idx += der->extensionsSz;
    }

    return idx;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */


/* Make RSA signature from buffer (sz), write to sig (sigSz) */
static int MakeSignature(CertSignCtx* certSignCtx, const byte* buf, int sz,
    byte* sig, int sigSz, RsaKey* rsaKey, ecc_key* eccKey,
    ed25519_key* ed25519Key, ed448_key* ed448Key, WC_RNG* rng, int sigAlgoType,
    void* heap)
{
    int digestSz = 0, typeH = 0, ret = 0;

    (void)digestSz;
    (void)typeH;
    (void)buf;
    (void)sz;
    (void)sig;
    (void)sigSz;
    (void)rsaKey;
    (void)eccKey;
    (void)ed25519Key;
    (void)ed448Key;
    (void)rng;
    (void)heap;

    switch (certSignCtx->state) {
    case CERTSIGN_STATE_BEGIN:
    case CERTSIGN_STATE_DIGEST:

        certSignCtx->state = CERTSIGN_STATE_DIGEST;
        certSignCtx->digest = (byte*)XMALLOC(WC_MAX_DIGEST_SIZE, heap,
            DYNAMIC_TYPE_TMP_BUFFER);
        if (certSignCtx->digest == NULL) {
            ret = MEMORY_E; goto exit_ms;
        }

        ret = HashForSignature(buf, sz, sigAlgoType, certSignCtx->digest,
                               &typeH, &digestSz, 0);
        /* set next state, since WC_PENDING_E rentry for these are not "call again" */
        certSignCtx->state = CERTSIGN_STATE_ENCODE;
        if (ret != 0) {
            goto exit_ms;
        }
        FALL_THROUGH;

    case CERTSIGN_STATE_ENCODE:
    #ifndef NO_RSA
        if (rsaKey) {
            certSignCtx->encSig = (byte*)XMALLOC(MAX_DER_DIGEST_SZ, heap,
                DYNAMIC_TYPE_TMP_BUFFER);
            if (certSignCtx->encSig == NULL) {
                ret = MEMORY_E; goto exit_ms;
            }

            /* signature */
            certSignCtx->encSigSz = wc_EncodeSignature(certSignCtx->encSig,
                                          certSignCtx->digest, digestSz, typeH);
        }
    #endif /* !NO_RSA */
        FALL_THROUGH;

    case CERTSIGN_STATE_DO:
        certSignCtx->state = CERTSIGN_STATE_DO;
        ret = ALGO_ID_E; /* default to error */

    #ifndef NO_RSA
        if (rsaKey) {
            /* signature */
            ret = wc_RsaSSL_Sign(certSignCtx->encSig, certSignCtx->encSigSz,
                                 sig, sigSz, rsaKey, rng);
        }
    #endif /* !NO_RSA */

    #ifdef HAVE_ECC
        if (!rsaKey && eccKey) {
            word32 outSz = sigSz;

            ret = wc_ecc_sign_hash(certSignCtx->digest, digestSz,
                                   sig, &outSz, rng, eccKey);
            if (ret == 0)
                ret = outSz;
        }
    #endif /* HAVE_ECC */

    #if defined(HAVE_ED25519) && defined(HAVE_ED25519_SIGN)
        if (!rsaKey && !eccKey && ed25519Key) {
            word32 outSz = sigSz;

            ret = wc_ed25519_sign_msg(buf, sz, sig, &outSz, ed25519Key);
            if (ret == 0)
                ret = outSz;
        }
    #endif /* HAVE_ED25519 && HAVE_ED25519_SIGN */

    #if defined(HAVE_ED448) && defined(HAVE_ED448_SIGN)
        if (!rsaKey && !eccKey && !ed25519Key && ed448Key) {
            word32 outSz = sigSz;

            ret = wc_ed448_sign_msg(buf, sz, sig, &outSz, ed448Key, NULL, 0);
            if (ret == 0)
                ret = outSz;
        }
    #endif /* HAVE_ED448 && HAVE_ED448_SIGN */
        break;
    }

exit_ms:

#ifdef WOLFSSL_ASYNC_CRYPT
    if (ret == WC_PENDING_E) {
        return ret;
    }
#endif

#ifndef NO_RSA
    if (rsaKey) {
        XFREE(certSignCtx->encSig, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
#endif /* !NO_RSA */

    XFREE(certSignCtx->digest, heap, DYNAMIC_TYPE_TMP_BUFFER);
    certSignCtx->digest = NULL;

    /* reset state */
    certSignCtx->state = CERTSIGN_STATE_BEGIN;

    return ret;
}


#ifdef WOLFSSL_ASN_TEMPLATE
/* Generate a random integer value of at most len bytes.
 *
 * Most-significant bit will not be set when maximum size.
 * Random value may be smaller than maximum size in bytes.
 *
 * @param [in]  rng  Random number generator.
 * @param [out] out  Buffer to hold integer value.
 * @param [in]  len  Maximum number of bytes of integer.
 * @return  0 on success.
 * @return  -ve when random number generation failed.
 */
static int GenerateInteger(WC_RNG* rng, byte* out, int len)
{
    int ret;

    /* Generate random number. */
    ret = wc_RNG_GenerateBlock(rng, out, len);
    if (ret == 0) {
        int i;

        /* Clear the top bit to make positive. */
        out[0] &= 0x7f;

        /* Find first non-zero byte. One zero byte is valid though. */
        for (i = 0; i < len - 1; i++) {
            if (out[i] != 0) {
                break;
            }
        }
        if (i != 0) {
            /* Remove leading zeros. */
            XMEMMOVE(out, out + i, len - i);
        }
    }

    return ret;
}

/* ASN.1 template for a Certificate.
 * X.509: RFC 5280, 4.1 - Basic Certificate Fields.
 */
static const ASNItem sigASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* tbsCertificate */
/*  1 */        { 1, ASN_SEQUENCE, 1, 0, 0 },
                /* signatureAlgorithm */
/*  2 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  4 */            { 2, ASN_TAG_NULL, 0, 0, 0 },
                /* signatureValue */
/*  5 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for a Certificate. */
#define sigASN_Length (sizeof(sigASN) / sizeof(ASNItem))
#endif

/* add signature to end of buffer, size of buffer assumed checked, return
   new length */
int AddSignature(byte* buf, int bodySz, const byte* sig, int sigSz,
                        int sigAlgoType)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    byte seq[MAX_SEQ_SZ];
    int  idx = bodySz, seqSz;

    /* algo */
    idx += SetAlgoID(sigAlgoType, buf ? buf + idx : NULL, oidSigType, 0);
    /* bit string */
    idx += SetBitString(sigSz, 0, buf ? buf + idx : NULL);
    /* signature */
    if (buf)
        XMEMCPY(buf + idx, sig, sigSz);
    idx += sigSz;

    /* make room for overall header */
    seqSz = SetSequence(idx, seq);
    if (buf) {
        XMEMMOVE(buf + seqSz, buf, idx);
        XMEMCPY(buf, seq, seqSz);
    }

    return idx + seqSz;
#else
    DECL_ASNSETDATA(dataASN, sigASN_Length);
    word32 seqSz;
    int sz;
    int ret = 0;

    CALLOC_ASNSETDATA(dataASN, sigASN_Length, ret, NULL);

    /* In place, put body between SEQUENCE and signature. */
    if (ret == 0) {
        /* Set sigature OID and signature data. */
        SetASN_OID(&dataASN[3], sigAlgoType, oidSigType);
        if (IsSigAlgoECC(sigAlgoType)) {
            /* ECDSA and EdDSA doesn't have NULL tagged item. */
            dataASN[4].noOut = 1;
        }
        SetASN_Buffer(&dataASN[5], sig, sigSz);
        /* Calcuate size of signature data. */
        ret = SizeASN_Items(&sigASN[2], &dataASN[2], sigASN_Length - 2, &sz);
    }
    if (ret == 0) {
        /* Calculate size of outer sequence by calculating size of the encoded
         * length and adding 1 for tag. */
        seqSz = SizeASNHeader(bodySz + sz);
        if (buf != NULL) {
            /* Move body to after sequence. */
            XMEMMOVE(buf + seqSz, buf, bodySz);
        }
        /* Leave space for body in encoding. */
        SetASN_ReplaceBuffer(&dataASN[1], NULL, bodySz);

        /* Calculate overall size and put in offsets and lengths. */
        ret = SizeASN_Items(sigASN, dataASN, sigASN_Length, &sz);
    }
    if ((ret == 0) && (buf != NULL)) {
        /* Write SEQUENCE and signature around body. */
        SetASN_Items(sigASN, dataASN, sigASN_Length, buf);
    }

    if (ret == 0) {
        /* Return the encoding size. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, NULL);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


/* Make an x509 Certificate v3 any key type from cert input, write to buffer */
static int MakeAnyCert(Cert* cert, byte* derBuffer, word32 derSz,
                       RsaKey* rsaKey, ecc_key* eccKey, WC_RNG* rng,
                       DsaKey* dsaKey, const byte* ntruKey, word16 ntruSz,
                       ed25519_key* ed25519Key, ed448_key* ed448Key)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
#ifdef WOLFSSL_SMALL_STACK
    DerCert* der;
#else
    DerCert der[1];
#endif

    if (derBuffer == NULL)
        return BAD_FUNC_ARG;

    if (eccKey)
        cert->keyType = ECC_KEY;
    else if (rsaKey)
        cert->keyType = RSA_KEY;
    else if (dsaKey)
        cert->keyType = DSA_KEY;
    else if (ed25519Key)
        cert->keyType = ED25519_KEY;
    else if (ed448Key)
        cert->keyType = ED448_KEY;
    else if (ntruKey)
        cert->keyType = NTRU_KEY;
    else
        return BAD_FUNC_ARG;

#ifdef WOLFSSL_SMALL_STACK
    der = (DerCert*)XMALLOC(sizeof(DerCert), cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (der == NULL)
        return MEMORY_E;
#endif

    ret = EncodeCert(cert, der, rsaKey, eccKey, rng, ntruKey, ntruSz, dsaKey,
                     ed25519Key, ed448Key);
    if (ret == 0) {
        if (der->total + MAX_SEQ_SZ * 2 > (int)derSz)
            ret = BUFFER_E;
        else
            ret = cert->bodySz = WriteCertBody(der, derBuffer);
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(der, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
#else
    /* TODO: issRaw and sbjRaw should be NUL terminated. */
    DECL_ASNSETDATA(dataASN, x509CertASN_Length);
    word32 publicKeySz = 0;
    word32 issuerSz = 0;
    word32 subjectSz = 0;
    word32 extSz = 0;
    int sz;
    int ret = 0;
    word32 issRawLen = 0;
    word32 sbjRawLen = 0;

    CALLOC_ASNSETDATA(dataASN, x509CertASN_Length, ret, cert->heap);

    if (ret == 0) {
        /* Set key type into certificate object based on key passed in. */
        if (rsaKey) {
            cert->keyType = RSA_KEY;
        }
        else if (eccKey) {
            cert->keyType = ECC_KEY;
        }
        else if (dsaKey) {
            cert->keyType = DSA_KEY;
        }
        else if (ed25519Key) {
            cert->keyType = ED25519_KEY;
        }
        else if (ed448Key) {
            cert->keyType = ED448_KEY;
        }
        else if (ntruKey) {
            cert->keyType = NTRU_KEY;
        }
        else {
            ret = BAD_FUNC_ARG;
        }
    }
    if ((ret == 0) && (cert->serialSz == 0)) {
        /* Generate random serial number. */
        cert->serialSz = CTC_GEN_SERIAL_SZ;
        ret = GenerateInteger(rng, cert->serial, CTC_GEN_SERIAL_SZ);
    }
    if (ret == 0) {
        /* Determine issuer name size. */
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA) || \
        defined(WOLFSSL_CERT_REQ)
        issRawLen = (word32)XSTRLEN((const char*)cert->issRaw);
        if (issRawLen > 0) {
            issuerSz = min(sizeof(cert->issRaw), issRawLen);
        }
        else
    #endif
        {
            /* Calcuate issuer name encoding size. */
            issuerSz = SetNameEx(NULL, ASN_NAME_MAX, &cert->issuer, cert->heap);
            ret = issuerSz;
        }
    }
    if (ret >= 0) {
        /* Determine subject name size. */
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA) || \
        defined(WOLFSSL_CERT_REQ)
        sbjRawLen = (word32)XSTRLEN((const char*)cert->sbjRaw);
        if (sbjRawLen > 0) {
            subjectSz = min(sizeof(cert->sbjRaw), sbjRawLen);
        }
        else
    #endif
        {
            /* Calcuate subject name encoding size. */
            subjectSz = SetNameEx(NULL, ASN_NAME_MAX, &cert->subject, cert->heap);
            ret = subjectSz;
        }
    }
    if (ret >= 0) {
        /* Calcuate public key encoding size. */
        ret = publicKeySz = EncodePublicKey(cert->keyType, NULL, 0, rsaKey,
            eccKey, ed25519Key, ed448Key, dsaKey, ntruKey, ntruSz);
    }
    if (ret >= 0) {
        /* Calcuate extensions encoding size - may be 0. */
        ret = extSz = EncodeExtensions(cert, NULL, 0, 0);
    }
    if (ret >= 0) {
        /* Don't write out outer sequence - only doing body. */
        dataASN[0].noOut = 1;
        /* Set version, serial number and signature OID */
        SetASN_Int8Bit(&dataASN[3], cert->version);
        SetASN_Buffer(&dataASN[4], cert->serial, cert->serialSz);
        SetASN_OID(&dataASN[6], cert->sigType, oidSigType);
        if (IsSigAlgoECC(cert->sigType)) {
            /* No NULL tagged item with ECDSA and EdDSA signature OIDs. */
            dataASN[7].noOut = 1;
        }
        if (issRawLen > 0) {
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA) || \
        defined(WOLFSSL_CERT_REQ)
            /* Put in encoded issuer name. */
            SetASN_Buffer(&dataASN[8], cert->issRaw, issuerSz);
    #endif
        }
        else {
            /* Leave space for issuer name. */
            SetASN_ReplaceBuffer(&dataASN[8], NULL, issuerSz);
        }

#ifdef WOLFSSL_ALT_NAMES
        if (cert->beforeDateSz && cert->afterDateSz) {
            if (cert->beforeDate[0] == ASN_UTC_TIME) {
                /* Make space for before date data. */
                SetASN_Buffer(&dataASN[10], cert->beforeDate + 2,
                    ASN_UTC_TIME_SIZE - 1);
                /* Don't put out Generalized Time before data. */
                dataASN[11].noOut = 1;
            }
            else {
                /* Don't put out UTC before data. */
                dataASN[10].noOut = 1;
                /* Make space for before date data. */
                SetASN_Buffer(&dataASN[11], cert->beforeDate + 2,
                    ASN_GEN_TIME_SZ);
            }
            if (cert->afterDate[0] == ASN_UTC_TIME) {
                /* Make space for after date data. */
                SetASN_Buffer(&dataASN[12], cert->afterDate + 2,
                    ASN_UTC_TIME_SIZE - 1);
                /* Don't put out UTC Generalized Time after data. */
                dataASN[13].noOut = 1;
            }
            else {
                /* Don't put out UTC after data. */
                dataASN[12].noOut = 1;
                /* Make space for after date data. */
                SetASN_Buffer(&dataASN[13], cert->afterDate + 2,
                    ASN_GEN_TIME_SZ);
            }
        }
        else
#endif
        {
            /* Don't put out UTC before data. */
            dataASN[10].noOut = 1;
            /* Make space for before date data. */
            SetASN_Buffer(&dataASN[11], NULL, ASN_GEN_TIME_SZ);
            /* Don't put out UTC after data. */
            dataASN[12].noOut = 1;
            /* Make space for after date data. */
            SetASN_Buffer(&dataASN[13], NULL, ASN_GEN_TIME_SZ);
        }
        if (sbjRawLen > 0) {
            /* Put in encoded subject name. */
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA) || \
        defined(WOLFSSL_CERT_REQ)
            SetASN_Buffer(&dataASN[14], cert->sbjRaw, subjectSz);
    #endif
        }
        else {
            /* Leave space for subject name. */
            SetASN_ReplaceBuffer(&dataASN[14], NULL, subjectSz);
        }
        /* Leave space for public key. */
        SetASN_ReplaceBuffer(&dataASN[15], NULL, publicKeySz);
        /* Replacement buffer instead of algorithm identifier items. */
        SetASNItem_NoOut(dataASN, 16, 20);
        /* issuerUniqueID and subjectUniqueID not supported. */
        dataASN[21].noOut = dataASN[22].noOut = 1;
        /* Leave space for extensions if any set into certificate object. */
        if (extSz > 0) {
            SetASN_Buffer(&dataASN[23], NULL, extSz);
        }
        else {
            dataASN[23].noOut = 1;
        }
        /* No signature - added later. */
        SetASNItem_NoOut(dataASN, 24, 27);

        /* Calculate encoded certificate body size. */
        ret = SizeASN_Items(x509CertASN, dataASN, x509CertASN_Length, &sz);
    }
    /* Check buffer is big enough for encoded data. */
    if ((ret == 0) && (sz > (int)derSz)) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode certificate body into buffer. */
        SetASN_Items(x509CertASN, dataASN, x509CertASN_Length, derBuffer);

        if (issRawLen == 0) {
            /* Encode issuer name into buffer. */
            ret = SetNameEx((byte*)dataASN[8].data.buffer.data,
                dataASN[8].data.buffer.length, &cert->issuer, cert->heap);
        }
    }
    if ((ret >= 0) && (sbjRawLen == 0)) {
        /* Encode subject name into buffer. */
        ret = SetNameEx((byte*)dataASN[14].data.buffer.data,
            dataASN[14].data.buffer.length, &cert->subject, cert->heap);
    }
    if (ret >= 0) {
#ifdef WOLFSSL_ALT_NAMES
        if (cert->beforeDateSz == 0 || cert->afterDateSz == 0)
#endif
        {
            /* Encode validity into buffer. */
            ret = SetValidity((byte*)dataASN[11].data.buffer.data,
                (byte*)dataASN[13].data.buffer.data, cert->daysValid);
        }
    }
    if (ret >= 0) {
        /* Encode public key into buffer. */
        ret = EncodePublicKey(cert->keyType,
            (byte*)dataASN[15].data.buffer.data, dataASN[15].data.buffer.length,
            rsaKey, eccKey, ed25519Key, ed448Key, dsaKey, ntruKey, ntruSz);
    }
    if ((ret >= 0) && (!dataASN[23].noOut)) {
        /* Encode extensions into buffer. */
        ret = EncodeExtensions(cert, (byte*)dataASN[23].data.buffer.data,
            dataASN[23].data.buffer.length, 0);
    }
    if (ret >= 0) {
        /* Store encoded certifcate body size. */
        cert->bodySz = sz;
        /* Return the encoding size. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, cert->heap);
    return ret;
#endif
}


/* Make an x509 Certificate v3 RSA or ECC from cert input, write to buffer */
int wc_MakeCert_ex(Cert* cert, byte* derBuffer, word32 derSz, int keyType,
                   void* key, WC_RNG* rng)
{
    RsaKey*      rsaKey = NULL;
    DsaKey*      dsaKey = NULL;
    ecc_key*     eccKey = NULL;
    ed25519_key* ed25519Key = NULL;
    ed448_key*   ed448Key = NULL;

    if (keyType == RSA_TYPE)
        rsaKey = (RsaKey*)key;
    else if (keyType == DSA_TYPE)
        dsaKey = (DsaKey*)key;
    else if (keyType == ECC_TYPE)
        eccKey = (ecc_key*)key;
    else if (keyType == ED25519_TYPE)
        ed25519Key = (ed25519_key*)key;
    else if (keyType == ED448_TYPE)
        ed448Key = (ed448_key*)key;

    return MakeAnyCert(cert, derBuffer, derSz, rsaKey, eccKey, rng, dsaKey,
                       NULL, 0, ed25519Key, ed448Key);
}
/* Make an x509 Certificate v3 RSA or ECC from cert input, write to buffer */
int wc_MakeCert(Cert* cert, byte* derBuffer, word32 derSz, RsaKey* rsaKey,
             ecc_key* eccKey, WC_RNG* rng)
{
    return MakeAnyCert(cert, derBuffer, derSz, rsaKey, eccKey, rng, NULL, NULL, 0,
                       NULL, NULL);
}


#ifdef HAVE_NTRU

int wc_MakeNtruCert(Cert* cert, byte* derBuffer, word32 derSz,
                  const byte* ntruKey, word16 keySz, WC_RNG* rng)
{
    return MakeAnyCert(cert, derBuffer, derSz, NULL, NULL, rng, NULL,
            ntruKey, keySz, NULL, NULL);
}

#endif /* HAVE_NTRU */


#ifdef WOLFSSL_CERT_REQ

#ifndef WOLFSSL_ASN_TEMPLATE
static int SetReqAttrib(byte* output, char* pw, int pwPrintableString,
                        int extSz)
{
    const byte erOid[] =
        { ASN_OBJECT_ID, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
                         0x09, 0x0e };

    int sz      = 0; /* overall size */
    int cpSz    = 0; /* Challenge Password section size */
    int cpSeqSz = 0;
    int cpSetSz = 0;
    int cpStrSz = 0;
    int pwSz    = 0;
    int erSz    = 0; /* Extension Request section size */
    int erSeqSz = 0;
    int erSetSz = 0;
    byte cpSeq[MAX_SEQ_SZ];
    byte cpSet[MAX_SET_SZ];
    byte cpStr[MAX_PRSTR_SZ];
    byte erSeq[MAX_SEQ_SZ];
    byte erSet[MAX_SET_SZ];

    output[0] = ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED;
    sz++;

    if (pw && pw[0]) {
        pwSz = (int)XSTRLEN(pw);
        if (pwPrintableString) {
            cpStrSz = SetPrintableString(pwSz, cpStr);
        } else {
            cpStrSz = SetUTF8String(pwSz, cpStr);
        }
        cpSetSz = SetSet(cpStrSz + pwSz, cpSet);
        /* +2 for tag and length parts of the TLV triplet */
        cpSeqSz = SetSequence(2 + sizeof(attrChallengePasswordOid) + cpSetSz +
                cpStrSz + pwSz, cpSeq);
        cpSz = cpSeqSz + 2 + sizeof(attrChallengePasswordOid) + cpSetSz +
                cpStrSz + pwSz;
    }

    if (extSz) {
        erSetSz = SetSet(extSz, erSet);
        erSeqSz = SetSequence(erSetSz + sizeof(erOid) + extSz, erSeq);
        erSz = extSz + erSetSz + erSeqSz + sizeof(erOid);
    }

    /* Put the pieces together. */
    sz += SetLength(cpSz + erSz, &output[sz]);

    if (cpSz) {
        XMEMCPY(&output[sz], cpSeq, cpSeqSz);
        sz += cpSeqSz;
        sz += SetObjectId(sizeof(attrChallengePasswordOid), output + sz);
        XMEMCPY(&output[sz], attrChallengePasswordOid,
                sizeof(attrChallengePasswordOid));
        sz += sizeof(attrChallengePasswordOid);
        XMEMCPY(&output[sz], cpSet, cpSetSz);
        sz += cpSetSz;
        XMEMCPY(&output[sz], cpStr, cpStrSz);
        sz += cpStrSz;
        XMEMCPY(&output[sz], pw, pwSz);
        sz += pwSz;
    }

    if (erSz) {
        XMEMCPY(&output[sz], erSeq, erSeqSz);
        sz += erSeqSz;
        XMEMCPY(&output[sz], erOid, sizeof(erOid));
        sz += sizeof(erOid);
        XMEMCPY(&output[sz], erSet, erSetSz);
        sz += erSetSz;
        /* The actual extension data will be tacked onto the output later. */
    }

    return sz;
}


/* encode info from cert into DER encoded format */
static int EncodeCertReq(Cert* cert, DerCert* der, RsaKey* rsaKey,
                         DsaKey* dsaKey, ecc_key* eccKey,
                         ed25519_key* ed25519Key, ed448_key* ed448Key)
{
    (void)eccKey;
    (void)ed25519Key;
    (void)ed448Key;

    if (cert == NULL || der == NULL)
        return BAD_FUNC_ARG;

    if (rsaKey == NULL && eccKey == NULL && ed25519Key == NULL &&
            dsaKey == NULL && ed448Key == NULL) {
            return PUBLIC_KEY_E;
    }

    /* init */
    XMEMSET(der, 0, sizeof(DerCert));

    /* version */
    der->versionSz = SetMyVersion(cert->version, der->version, FALSE);

    /* subject name */
#if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
    if (XSTRLEN((const char*)cert->sbjRaw) > 0) {
        /* Use the raw subject */
        int idx;

        der->subjectSz = min(sizeof(der->subject),
                (word32)XSTRLEN((const char*)cert->sbjRaw));
        /* header */
        idx = SetSequence(der->subjectSz, der->subject);
        if (der->subjectSz + idx > (int)sizeof(der->subject)) {
            return SUBJECT_E;
        }

        XMEMCPY((char*)der->subject + idx, (const char*)cert->sbjRaw,
                der->subjectSz);
        der->subjectSz += idx;
    }
    else
#endif
    {
        der->subjectSz = SetNameEx(der->subject, sizeof(der->subject),
                &cert->subject, cert->heap);
    }
    if (der->subjectSz <= 0)
        return SUBJECT_E;

    /* public key */
#ifndef NO_RSA
    if (cert->keyType == RSA_KEY) {
        if (rsaKey == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = SetRsaPublicKey(der->publicKey, rsaKey,
                                           sizeof(der->publicKey), 1);
    }
#endif

#if !defined(NO_DSA) && !defined(HAVE_SELFTEST)
    if (cert->keyType == DSA_KEY) {
        if (dsaKey == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = wc_SetDsaPublicKey(der->publicKey, dsaKey,
                                           sizeof(der->publicKey), 1);
    }
#endif

#ifdef HAVE_ECC
    if (cert->keyType == ECC_KEY) {
        if (eccKey == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = SetEccPublicKey(der->publicKey, eccKey,
                                           sizeof(der->publicKey), 1);
    }
#endif

#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_EXPORT)
    if (cert->keyType == ED25519_KEY) {
        if (ed25519Key == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = wc_Ed25519PublicKeyToDer(ed25519Key, der->publicKey,
            (word32)sizeof(der->publicKey), 1);
    }
#endif

#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_EXPORT)
    if (cert->keyType == ED448_KEY) {
        if (ed448Key == NULL)
            return PUBLIC_KEY_E;
        der->publicKeySz = wc_Ed448PublicKeyToDer(ed448Key, der->publicKey,
            (word32)sizeof(der->publicKey), 1);
    }
#endif
    if (der->publicKeySz <= 0)
        return PUBLIC_KEY_E;

    /* set the extensions */
    der->extensionsSz = 0;

    /* CA */
    if (cert->isCA) {
        der->caSz = SetCa(der->ca, sizeof(der->ca));
        if (der->caSz <= 0)
            return CA_TRUE_E;

        der->extensionsSz += der->caSz;
    }
    else
        der->caSz = 0;

#ifdef WOLFSSL_ALT_NAMES
    /* Alternative Name */
    if (cert->altNamesSz) {
        der->altNamesSz = SetAltNames(der->altNames, sizeof(der->altNames),
                                      cert->altNames, cert->altNamesSz);
        if (der->altNamesSz <= 0)
            return ALT_NAME_E;

        der->extensionsSz += der->altNamesSz;
    }
    else
        der->altNamesSz = 0;
#endif

#ifdef WOLFSSL_CERT_EXT
    /* SKID */
    if (cert->skidSz) {
        /* check the provided SKID size */
        if (cert->skidSz > (int)min(CTC_MAX_SKID_SIZE, sizeof(der->skid)))
            return SKID_E;

        der->skidSz = SetSKID(der->skid, sizeof(der->skid),
                              cert->skid, cert->skidSz);
        if (der->skidSz <= 0)
            return SKID_E;

        der->extensionsSz += der->skidSz;
    }
    else
        der->skidSz = 0;

    /* Key Usage */
    if (cert->keyUsage != 0){
        der->keyUsageSz = SetKeyUsage(der->keyUsage, sizeof(der->keyUsage),
                                      cert->keyUsage);
        if (der->keyUsageSz <= 0)
            return KEYUSAGE_E;

        der->extensionsSz += der->keyUsageSz;
    }
    else
        der->keyUsageSz = 0;

    /* Extended Key Usage */
    if (cert->extKeyUsage != 0){
        der->extKeyUsageSz = SetExtKeyUsage(cert, der->extKeyUsage,
                                sizeof(der->extKeyUsage), cert->extKeyUsage);
        if (der->extKeyUsageSz <= 0)
            return EXTKEYUSAGE_E;

        der->extensionsSz += der->extKeyUsageSz;
    }
    else
        der->extKeyUsageSz = 0;

#endif /* WOLFSSL_CERT_EXT */

    /* put extensions */
    if (der->extensionsSz > 0) {
        int ret;

        /* put the start of sequence (ID, Size) */
        der->extensionsSz = SetSequence(der->extensionsSz, der->extensions);
        if (der->extensionsSz <= 0)
            return EXTENSIONS_E;

        /* put CA */
        if (der->caSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->ca, der->caSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

#ifdef WOLFSSL_ALT_NAMES
        /* put Alternative Names */
        if (der->altNamesSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->altNames, der->altNamesSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }
#endif

#ifdef WOLFSSL_CERT_EXT
        /* put SKID */
        if (der->skidSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->skid, der->skidSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put AKID */
        if (der->akidSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->akid, der->akidSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put KeyUsage */
        if (der->keyUsageSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->keyUsage, der->keyUsageSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

        /* put ExtendedKeyUsage */
        if (der->extKeyUsageSz) {
            ret = SetExtensions(der->extensions, sizeof(der->extensions),
                                &der->extensionsSz,
                                der->extKeyUsage, der->extKeyUsageSz);
            if (ret <= 0)
                return EXTENSIONS_E;
        }

#endif /* WOLFSSL_CERT_EXT */
    }

    der->attribSz = SetReqAttrib(der->attrib, cert->challengePw,
                                 cert->challengePwPrintableString,
                                 der->extensionsSz);
    if (der->attribSz <= 0)
        return REQ_ATTRIBUTE_E;

    der->total = der->versionSz + der->subjectSz + der->publicKeySz +
        der->extensionsSz + der->attribSz;

    return 0;
}


/* write DER encoded cert req to buffer, size already checked */
static int WriteCertReqBody(DerCert* der, byte* buf)
{
    int idx;

    /* signed part header */
    idx = SetSequence(der->total, buf);
    /* version */
    if (buf)
        XMEMCPY(buf + idx, der->version, der->versionSz);
    idx += der->versionSz;
    /* subject */
    if (buf)
        XMEMCPY(buf + idx, der->subject, der->subjectSz);
    idx += der->subjectSz;
    /* public key */
    if (buf)
        XMEMCPY(buf + idx, der->publicKey, der->publicKeySz);
    idx += der->publicKeySz;
    /* attributes */
    if (buf)
        XMEMCPY(buf + idx, der->attrib, der->attribSz);
    idx += der->attribSz;
    /* extensions */
    if (der->extensionsSz) {
        if (buf)
            XMEMCPY(buf + idx, der->extensions, min(der->extensionsSz,
                                               (int)sizeof(der->extensions)));
        idx += der->extensionsSz;
    }

    return idx;
}
#endif

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for Certificate Request body.
 * PKCS #10: RFC 2986, 4.1 - CertificationRequestInfo
 */
static const ASNItem certReqBodyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* version */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* subject */
/*  2 */        { 1, ASN_SEQUENCE, 1, 0, 0 },
                /* subjectPKInfo */
/*  3 */        { 1, ASN_SEQUENCE, 1, 0, 0 },
                /*  attributes*/
/*  4 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
                    /* Challenge Password Attribute */
/*  5 */            { 2, ASN_SEQUENCE, 1, 1, 1 },
/*  6 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
/*  7 */                { 3, ASN_SET, 1, 1, 0 },
/*  8 */                    { 4, ASN_PRINTABLE_STRING, 0, 0, 0 },
/*  9 */                    { 4, ASN_UTF8STRING, 0, 0, 0 },
                    /* Extensions Attribute */
/* 10 */            { 2, ASN_SEQUENCE, 1, 1, 1 },
/* 11 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
/* 12 */                { 3, ASN_SET, 1, 1, 0 },
/* 13 */                    { 4, ASN_SEQUENCE, 1, 0, 0 },
};

/* Number of items in ASN.1 template for Certificate Request body. */
#define certReqBodyASN_Length (sizeof(certReqBodyASN) / sizeof(ASNItem))
#endif

static int MakeCertReq(Cert* cert, byte* derBuffer, word32 derSz,
                   RsaKey* rsaKey, DsaKey* dsaKey, ecc_key* eccKey,
                   ed25519_key* ed25519Key, ed448_key* ed448Key)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
#ifdef WOLFSSL_SMALL_STACK
    DerCert* der;
#else
    DerCert der[1];
#endif

    if (eccKey)
        cert->keyType = ECC_KEY;
    else if (rsaKey)
        cert->keyType = RSA_KEY;
    else if (dsaKey)
        cert->keyType = DSA_KEY;
    else if (ed25519Key)
        cert->keyType = ED25519_KEY;
    else if (ed448Key)
        cert->keyType = ED448_KEY;
    else
        return BAD_FUNC_ARG;

#ifdef WOLFSSL_SMALL_STACK
    der = (DerCert*)XMALLOC(sizeof(DerCert), cert->heap,
                                                    DYNAMIC_TYPE_TMP_BUFFER);
    if (der == NULL)
        return MEMORY_E;
#endif

    ret = EncodeCertReq(cert, der, rsaKey, dsaKey, eccKey, ed25519Key, ed448Key);

    if (ret == 0) {
        if (der->total + MAX_SEQ_SZ * 2 > (int)derSz)
            ret = BUFFER_E;
        else
            ret = cert->bodySz = WriteCertReqBody(der, derBuffer);
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(der, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
#else
    DECL_ASNSETDATA(dataASN, certReqBodyASN_Length);
    word32 publicKeySz, subjectSz, extSz;
    int sz;
    int ret = 0;
#if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
    word32 sbjRawSz;
#endif
    /* Challenge Password OID. */
    static const byte cpOid[] =
        { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x07 };
    /* Extension Requested OID. */
    static const byte erOid[] =
        { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x0e };

    CALLOC_ASNSETDATA(dataASN, certReqBodyASN_Length, ret, cert->heap);

    if (ret == 0) {
        /* Set key type into certificate object based on key passed in. */
        if (rsaKey != NULL) {
            cert->keyType = RSA_KEY;
        }
        else if (eccKey != NULL) {
            cert->keyType = ECC_KEY;
        }
        else if (dsaKey != NULL) {
            cert->keyType = DSA_KEY;
        }
        else if (ed25519Key != NULL) {
            cert->keyType = ED25519_KEY;
        }
        else if (ed448Key != NULL) {
            cert->keyType = ED448_KEY;
        }
        else {
            ret = BAD_FUNC_ARG;
        }
    }
    if (ret == 0) {
        /* Determine subject name size. */
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
        sbjRawSz = (word32)XSTRLEN((const char*)cert->sbjRaw);
        if (sbjRawSz > 0) {
            subjectSz = min(sizeof(cert->sbjRaw), sbjRawSz);
        }
        else
    #endif
        {
            subjectSz = SetNameEx(NULL, ASN_NAME_MAX, &cert->subject, cert->heap);
            ret = subjectSz;
        }
    }
    if (ret >= 0) {
        /* Determine encode public key size. */
         ret = publicKeySz = EncodePublicKey(cert->keyType, NULL, 0, rsaKey,
             eccKey, ed25519Key, ed448Key, dsaKey, NULL, 0);
    }
    if (ret >= 0) {
        /* Determine encode extensions size. */
        ret = extSz = EncodeExtensions(cert, NULL, 0, 1);
    }
    if (ret >= 0) {
        /* Set version. */
        SetASN_Int8Bit(&dataASN[1], cert->version);
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
        if (sbjRawSz > 0) {
            /* Put in encoded subject name. */
            SetASN_Buffer(&dataASN[2], cert->sbjRaw, subjectSz);
        }
        else
    #endif
        {
            /* Leave space for subject name. */
            SetASN_ReplaceBuffer(&dataASN[2], NULL, subjectSz);
        }
        /* Leave space for public key. */
        SetASN_ReplaceBuffer(&dataASN[3], NULL, publicKeySz);
        if (cert->challengePw[0] != '\0') {
            /* Add challenge password attribute. */
            /* Set challenge password OID. */
            SetASN_Buffer(&dataASN[6], cpOid, sizeof(cpOid));
            /* Enable the ASN template item with the appropriate tag. */
            if (cert->challengePwPrintableString) {
                /* PRINTABLE_STRING - set buffer */
                SetASN_Buffer(&dataASN[8], (byte*)cert->challengePw,
                              (word32)XSTRLEN(cert->challengePw));
                /* UTF8STRING - don't encode */
                dataASN[9].noOut = 1;
            }
            else {
                /* PRINTABLE_STRING - don't encode */
                dataASN[8].noOut = 1;
                /* UTF8STRING - set buffer */
                SetASN_Buffer(&dataASN[9], (byte*)cert->challengePw,
                              (word32)XSTRLEN(cert->challengePw));
            }
        }
        else {
            /* Leave out challenge password attribute items. */
            SetASNItem_NoOut(dataASN, 5, 9);
        }
        if (extSz > 0) {
            /* Set extension attribute OID. */
            SetASN_Buffer(&dataASN[11], erOid, sizeof(erOid));
            /* Leave space for data. */
            SetASN_Buffer(&dataASN[13], NULL, extSz);
        }
        else {
            /* Leave out extension attribute items. */
            SetASNItem_NoOut(dataASN, 10, 13);
        }

        /* Calculate size of encoded certificate request body. */
        ret = SizeASN_Items(certReqBodyASN, dataASN, certReqBodyASN_Length,
                            &sz);
    }
    /* Check buffer is big enough for encoded data. */
    if ((ret == 0) && (sz > (int)derSz)) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode certificate request body into buffer. */
        SetASN_Items(certReqBodyASN, dataASN, certReqBodyASN_Length, derBuffer);

        /* Put in generated data */
    #if defined(WOLFSSL_CERT_EXT) || defined(OPENSSL_EXTRA)
        if (sbjRawSz == 0)
    #endif
        {
            /* Encode subject name into space in buffer. */
            ret = SetNameEx((byte*)dataASN[2].data.buffer.data,
                    dataASN[3].data.buffer.length, &cert->subject, cert->heap);
        }
    }
    if (ret >= 0) {
        /* Encode public key into space in buffer. */
        ret = EncodePublicKey(cert->keyType, (byte*)dataASN[3].data.buffer.data,
            dataASN[3].data.buffer.length, rsaKey, eccKey, ed25519Key, ed448Key,
            dsaKey, NULL, 0);
    }
    if ((ret >= 0) && (!dataASN[13].noOut)) {
        /* Encode extensions into space in buffer. */
        ret = EncodeExtensions(cert, (byte*)dataASN[13].data.buffer.data,
            dataASN[13].data.buffer.length, 1);
    }
    if (ret >= 0) {
        /* Store encoded certifcate request body size. */
        cert->bodySz = sz;
        /* Return the encoding size. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, cert->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

int wc_MakeCertReq_ex(Cert* cert, byte* derBuffer, word32 derSz, int keyType,
                      void* key)
{
    RsaKey*      rsaKey = NULL;
    DsaKey*      dsaKey = NULL;
    ecc_key*     eccKey = NULL;
    ed25519_key* ed25519Key = NULL;
    ed448_key*   ed448Key = NULL;

    if (keyType == RSA_TYPE)
        rsaKey = (RsaKey*)key;
    else if (keyType == DSA_TYPE)
        dsaKey = (DsaKey*)key;
    else if (keyType == ECC_TYPE)
        eccKey = (ecc_key*)key;
    else if (keyType == ED25519_TYPE)
        ed25519Key = (ed25519_key*)key;
    else if (keyType == ED448_TYPE)
        ed448Key = (ed448_key*)key;

    return MakeCertReq(cert, derBuffer, derSz, rsaKey, dsaKey, eccKey, ed25519Key,
                       ed448Key);
}

int wc_MakeCertReq(Cert* cert, byte* derBuffer, word32 derSz,
                   RsaKey* rsaKey, ecc_key* eccKey)
{
    return MakeCertReq(cert, derBuffer, derSz, rsaKey, NULL, eccKey, NULL, NULL);
}
#endif /* WOLFSSL_CERT_REQ */


static int SignCert(int requestSz, int sType, byte* buf, word32 buffSz,
                    RsaKey* rsaKey, ecc_key* eccKey, ed25519_key* ed25519Key,
                    ed448_key* ed448Key, WC_RNG* rng)
{
    int sigSz = 0;
    void* heap = NULL;
    CertSignCtx* certSignCtx;
#ifndef WOLFSSL_ASYNC_CRYPT
    CertSignCtx  certSignCtx_lcl;

    certSignCtx = &certSignCtx_lcl;
    XMEMSET(certSignCtx, 0, sizeof(CertSignCtx));
#else
    certSignCtx = NULL;
#endif

    if (requestSz < 0)
        return requestSz;

    /* locate ctx */
    if (rsaKey) {
    #ifndef NO_RSA
    #ifdef WOLFSSL_ASYNC_CRYPT
        certSignCtx = &rsaKey->certSignCtx;
    #endif
        heap = rsaKey->heap;
    #else
        return NOT_COMPILED_IN;
    #endif /* NO_RSA */
    }
    else if (eccKey) {
    #ifdef HAVE_ECC
    #ifdef WOLFSSL_ASYNC_CRYPT
        certSignCtx = &eccKey->certSignCtx;
    #endif
        heap = eccKey->heap;
    #else
        return NOT_COMPILED_IN;
    #endif /* HAVE_ECC */
    }

#ifdef WOLFSSL_ASYNC_CRYPT
    if (certSignCtx == NULL) {
        return BAD_FUNC_ARG;
    }
#endif

    if (certSignCtx->sig == NULL) {
        certSignCtx->sig = (byte*)XMALLOC(MAX_ENCODED_SIG_SZ, heap,
            DYNAMIC_TYPE_TMP_BUFFER);
        if (certSignCtx->sig == NULL)
            return MEMORY_E;
    }

    sigSz = MakeSignature(certSignCtx, buf, requestSz, certSignCtx->sig,
        MAX_ENCODED_SIG_SZ, rsaKey, eccKey, ed25519Key, ed448Key, rng, sType,
        heap);
#ifdef WOLFSSL_ASYNC_CRYPT
    if (sigSz == WC_PENDING_E) {
        /* Not free'ing certSignCtx->sig here because it could still be in use
         * with async operations. */
        return sigSz;
    }
#endif

    if (sigSz >= 0) {
        if (requestSz + MAX_SEQ_SZ * 2 + sigSz > (int)buffSz)
            sigSz = BUFFER_E;
        else
            sigSz = AddSignature(buf, requestSz, certSignCtx->sig, sigSz,
                                 sType);
    }

    XFREE(certSignCtx->sig, heap, DYNAMIC_TYPE_TMP_BUFFER);
    certSignCtx->sig = NULL;

    return sigSz;
}

int wc_SignCert_ex(int requestSz, int sType, byte* buf, word32 buffSz,
                   int keyType, void* key, WC_RNG* rng)
{
    RsaKey*      rsaKey = NULL;
    ecc_key*     eccKey = NULL;
    ed25519_key* ed25519Key = NULL;
    ed448_key*   ed448Key = NULL;

    if (keyType == RSA_TYPE)
        rsaKey = (RsaKey*)key;
    else if (keyType == ECC_TYPE)
        eccKey = (ecc_key*)key;
    else if (keyType == ED25519_TYPE)
        ed25519Key = (ed25519_key*)key;
    else if (keyType == ED448_TYPE)
        ed448Key = (ed448_key*)key;

    return SignCert(requestSz, sType, buf, buffSz, rsaKey, eccKey, ed25519Key,
                    ed448Key, rng);
}

int wc_SignCert(int requestSz, int sType, byte* buf, word32 buffSz,
                RsaKey* rsaKey, ecc_key* eccKey, WC_RNG* rng)
{
    return SignCert(requestSz, sType, buf, buffSz, rsaKey, eccKey, NULL, NULL,
                    rng);
}

int wc_MakeSelfCert(Cert* cert, byte* buf, word32 buffSz,
                    RsaKey* key, WC_RNG* rng)
{
    int ret;

    ret = wc_MakeCert(cert, buf, buffSz, key, NULL, rng);
    if (ret < 0)
        return ret;

    return wc_SignCert(cert->bodySz, cert->sigType,
                       buf, buffSz, key, NULL, rng);
}


#ifdef WOLFSSL_CERT_EXT

/* Get raw subject from cert, which may contain OIDs not parsed by Decode.
   The raw subject pointer will only be valid while "cert" is valid. */
int wc_GetSubjectRaw(byte **subjectRaw, Cert *cert)
{
    int rc = BAD_FUNC_ARG;
    if ((subjectRaw != NULL) && (cert != NULL)) {
        *subjectRaw = cert->sbjRaw;
        rc = 0;
    }
    return rc;
}

/* Set KID from public key */
static int SetKeyIdFromPublicKey(Cert *cert, RsaKey *rsakey, ecc_key *eckey,
                                 byte *ntruKey, word16 ntruKeySz,
                                 ed25519_key* ed25519Key, ed448_key* ed448Key,
                                 int kid_type)
{
    byte *buf;
    int   bufferSz, ret;

    if (cert == NULL ||
        (rsakey == NULL && eckey == NULL && ntruKey == NULL &&
                                      ed25519Key == NULL && ed448Key == NULL) ||
        (kid_type != SKID_TYPE && kid_type != AKID_TYPE))
        return BAD_FUNC_ARG;

    buf = (byte *)XMALLOC(MAX_PUBLIC_KEY_SZ, cert->heap,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
    if (buf == NULL)
        return MEMORY_E;

    /* Public Key */
    bufferSz = -1;
#ifndef NO_RSA
    /* RSA public key */
    if (rsakey != NULL)
        bufferSz = SetRsaPublicKey(buf, rsakey, MAX_PUBLIC_KEY_SZ, 0);
#endif
#ifdef HAVE_ECC
    /* ECC public key */
    if (eckey != NULL)
        bufferSz = SetEccPublicKey(buf, eckey, MAX_PUBLIC_KEY_SZ, 0);
#endif
#ifdef HAVE_NTRU
    /* NTRU public key */
    if (ntruKey != NULL) {
        bufferSz = MAX_PUBLIC_KEY_SZ;
        ret = ntru_crypto_ntru_encrypt_publicKey2SubjectPublicKeyInfo(
                        ntruKeySz, ntruKey, (word16 *)(&bufferSz), buf);
        if (ret != NTRU_OK)
            bufferSz = -1;
    }
#else
    (void)ntruKeySz;
#endif
#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_EXPORT)
    /* ED25519 public key */
    if (ed25519Key != NULL) {
        bufferSz = wc_Ed25519PublicKeyToDer(ed25519Key, buf, MAX_PUBLIC_KEY_SZ, 0);
    }
#endif
#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_EXPORT)
    /* ED448 public key */
    if (ed448Key != NULL) {
        bufferSz = wc_Ed448PublicKeyToDer(ed448Key, buf, MAX_PUBLIC_KEY_SZ, 0);
    }
#endif

    if (bufferSz <= 0) {
        XFREE(buf, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return PUBLIC_KEY_E;
    }

    /* Compute SKID by hashing public key */
    if (kid_type == SKID_TYPE) {
        ret = CalcHashId(buf, bufferSz, cert->skid);
        cert->skidSz = KEYID_SIZE;
    }
    else if (kid_type == AKID_TYPE) {
        ret = CalcHashId(buf, bufferSz, cert->akid);
        cert->akidSz = KEYID_SIZE;
    }
    else
        ret = BAD_FUNC_ARG;

    XFREE(buf, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    return ret;
}

int wc_SetSubjectKeyIdFromPublicKey_ex(Cert *cert, int keyType, void* key)
{
    RsaKey*      rsaKey = NULL;
    ecc_key*     eccKey = NULL;
    ed25519_key* ed25519Key = NULL;
    ed448_key*   ed448Key = NULL;

    if (keyType == RSA_TYPE)
        rsaKey = (RsaKey*)key;
    else if (keyType == ECC_TYPE)
        eccKey = (ecc_key*)key;
    else if (keyType == ED25519_TYPE)
        ed25519Key = (ed25519_key*)key;
    else if (keyType == ED448_TYPE)
        ed448Key = (ed448_key*)key;

    return SetKeyIdFromPublicKey(cert, rsaKey, eccKey, NULL, 0, ed25519Key,
                                 ed448Key, SKID_TYPE);
}

/* Set SKID from RSA or ECC public key */
int wc_SetSubjectKeyIdFromPublicKey(Cert *cert, RsaKey *rsakey, ecc_key *eckey)
{
    return SetKeyIdFromPublicKey(cert, rsakey, eckey, NULL, 0, NULL, NULL,
                                 SKID_TYPE);
}

#ifdef HAVE_NTRU
/* Set SKID from NTRU public key */
int wc_SetSubjectKeyIdFromNtruPublicKey(Cert *cert,
                                        byte *ntruKey, word16 ntruKeySz)
{
    return SetKeyIdFromPublicKey(cert, NULL,NULL,ntruKey, ntruKeySz, NULL, NULL,
                                 SKID_TYPE);
}
#endif

int wc_SetAuthKeyIdFromPublicKey_ex(Cert *cert, int keyType, void* key)
{
    RsaKey*      rsaKey = NULL;
    ecc_key*     eccKey = NULL;
    ed25519_key* ed25519Key = NULL;
    ed448_key*   ed448Key = NULL;

    if (keyType == RSA_TYPE)
        rsaKey = (RsaKey*)key;
    else if (keyType == ECC_TYPE)
        eccKey = (ecc_key*)key;
    else if (keyType == ED25519_TYPE)
        ed25519Key = (ed25519_key*)key;
    else if (keyType == ED448_TYPE)
        ed448Key = (ed448_key*)key;

    return SetKeyIdFromPublicKey(cert, rsaKey, eccKey, NULL, 0, ed25519Key,
                                 ed448Key, AKID_TYPE);
}

/* Set SKID from RSA or ECC public key */
int wc_SetAuthKeyIdFromPublicKey(Cert *cert, RsaKey *rsakey, ecc_key *eckey)
{
    return SetKeyIdFromPublicKey(cert, rsakey, eckey, NULL, 0, NULL, NULL,
                                 AKID_TYPE);
}


#if !defined(NO_FILESYSTEM) && !defined(NO_ASN_CRYPT)

/* Set SKID from public key file in PEM */
int wc_SetSubjectKeyId(Cert *cert, const char* file)
{
    int     ret, derSz;
    byte*   der;
    word32  idx;
    RsaKey  *rsakey = NULL;
    ecc_key *eckey = NULL;

    if (cert == NULL || file == NULL)
        return BAD_FUNC_ARG;

    der = (byte*)XMALLOC(MAX_PUBLIC_KEY_SZ, cert->heap, DYNAMIC_TYPE_CERT);
    if (der == NULL) {
        WOLFSSL_MSG("wc_SetSubjectKeyId memory Problem");
        return MEMORY_E;
    }
    derSz = MAX_PUBLIC_KEY_SZ;

    XMEMSET(der, 0, derSz);
    derSz = wc_PemPubKeyToDer(file, der, derSz);
    if (derSz <= 0) {
        XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
        return derSz;
    }

    /* Load PubKey in internal structure */
#ifndef NO_RSA
    rsakey = (RsaKey*) XMALLOC(sizeof(RsaKey), cert->heap, DYNAMIC_TYPE_RSA);
    if (rsakey == NULL) {
        XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
        return MEMORY_E;
    }

    if (wc_InitRsaKey(rsakey, cert->heap) != 0) {
        WOLFSSL_MSG("wc_InitRsaKey failure");
        XFREE(rsakey, cert->heap, DYNAMIC_TYPE_RSA);
        XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
        return MEMORY_E;
    }

    idx = 0;
    ret = wc_RsaPublicKeyDecode(der, &idx, rsakey, derSz);
    if (ret != 0)
#endif
    {
#ifndef NO_RSA
        WOLFSSL_MSG("wc_RsaPublicKeyDecode failed");
        wc_FreeRsaKey(rsakey);
        XFREE(rsakey, cert->heap, DYNAMIC_TYPE_RSA);
        rsakey = NULL;
#endif
#ifdef HAVE_ECC
        /* Check to load ecc public key */
        eckey = (ecc_key*) XMALLOC(sizeof(ecc_key), cert->heap,
                                                              DYNAMIC_TYPE_ECC);
        if (eckey == NULL) {
            XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
            return MEMORY_E;
        }

        if (wc_ecc_init(eckey) != 0) {
            WOLFSSL_MSG("wc_ecc_init failure");
            wc_ecc_free(eckey);
            XFREE(eckey, cert->heap, DYNAMIC_TYPE_ECC);
            XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
            return MEMORY_E;
        }

        idx = 0;
        ret = wc_EccPublicKeyDecode(der, &idx, eckey, derSz);
        if (ret != 0) {
            WOLFSSL_MSG("wc_EccPublicKeyDecode failed");
            XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
            wc_ecc_free(eckey);
            XFREE(eckey, cert->heap, DYNAMIC_TYPE_ECC);
            return PUBLIC_KEY_E;
        }
#else
        XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
        return PUBLIC_KEY_E;
#endif /* HAVE_ECC */
    }

    XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);

    ret = wc_SetSubjectKeyIdFromPublicKey(cert, rsakey, eckey);

#ifndef NO_RSA
    wc_FreeRsaKey(rsakey);
    XFREE(rsakey, cert->heap, DYNAMIC_TYPE_RSA);
#endif
#ifdef HAVE_ECC
    wc_ecc_free(eckey);
    XFREE(eckey, cert->heap, DYNAMIC_TYPE_ECC);
#endif
    return ret;
}

#endif /* !NO_FILESYSTEM && !NO_ASN_CRYPT */

static int SetAuthKeyIdFromDcert(Cert* cert, DecodedCert* decoded)
{
    int ret = 0;

    /* Subject Key Id not found !! */
    if (decoded->extSubjKeyIdSet == 0) {
        ret = ASN_NO_SKID;
    }

    /* SKID invalid size */
    else if (sizeof(cert->akid) < sizeof(decoded->extSubjKeyId)) {
        ret = MEMORY_E;
    }

    else {
        /* Put the SKID of CA to AKID of certificate */
        XMEMCPY(cert->akid, decoded->extSubjKeyId, KEYID_SIZE);
        cert->akidSz = KEYID_SIZE;
    }

    return ret;
}

/* Set AKID from certificate contains in buffer (DER encoded) */
int wc_SetAuthKeyIdFromCert(Cert *cert, const byte *der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            ret = SetAuthKeyIdFromDcert(cert, (DecodedCert*)cert->decodedCert);
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
        }
    }

    return ret;
}


#ifndef NO_FILESYSTEM

/* Set AKID from certificate file in PEM */
int wc_SetAuthKeyId(Cert *cert, const char* file)
{
    int         ret;
    int         derSz;
    byte*       der;

    if (cert == NULL || file == NULL)
        return BAD_FUNC_ARG;

    der = (byte*)XMALLOC(EIGHTK_BUF, cert->heap, DYNAMIC_TYPE_CERT);
    if (der == NULL) {
        WOLFSSL_MSG("wc_SetAuthKeyId OOF Problem");
        return MEMORY_E;
    }

    derSz = wc_PemCertToDer(file, der, EIGHTK_BUF);
    if (derSz <= 0)
    {
        XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);
        return derSz;
    }

    ret = wc_SetAuthKeyIdFromCert(cert, der, derSz);
    XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);

    return ret;
}

#endif /* !NO_FILESYSTEM */

/* Set KeyUsage from human readable string */
int wc_SetKeyUsage(Cert *cert, const char *value)
{
    int ret = 0;
    char *token, *str, *ptr;
    word32 len;

    if (cert == NULL || value == NULL)
        return BAD_FUNC_ARG;

    cert->keyUsage = 0;

    /* duplicate string (including terminator) */
    len = (word32)XSTRLEN(value);
    str = (char*)XMALLOC(len+1, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (str == NULL)
        return MEMORY_E;
    XMEMCPY(str, value, len+1);

    /* parse value, and set corresponding Key Usage value */
    if ((token = XSTRTOK(str, ",", &ptr)) == NULL) {
        XFREE(str, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return KEYUSAGE_E;
    }
    while (token != NULL)
    {
        len = (word32)XSTRLEN(token);

        if (!XSTRNCASECMP(token, "digitalSignature", len))
            cert->keyUsage |= KEYUSE_DIGITAL_SIG;
        else if (!XSTRNCASECMP(token, "nonRepudiation", len) ||
                 !XSTRNCASECMP(token, "contentCommitment", len))
            cert->keyUsage |= KEYUSE_CONTENT_COMMIT;
        else if (!XSTRNCASECMP(token, "keyEncipherment", len))
            cert->keyUsage |= KEYUSE_KEY_ENCIPHER;
        else if (!XSTRNCASECMP(token, "dataEncipherment", len))
            cert->keyUsage |= KEYUSE_DATA_ENCIPHER;
        else if (!XSTRNCASECMP(token, "keyAgreement", len))
            cert->keyUsage |= KEYUSE_KEY_AGREE;
        else if (!XSTRNCASECMP(token, "keyCertSign", len))
            cert->keyUsage |= KEYUSE_KEY_CERT_SIGN;
        else if (!XSTRNCASECMP(token, "cRLSign", len))
            cert->keyUsage |= KEYUSE_CRL_SIGN;
        else if (!XSTRNCASECMP(token, "encipherOnly", len))
            cert->keyUsage |= KEYUSE_ENCIPHER_ONLY;
        else if (!XSTRNCASECMP(token, "decipherOnly", len))
            cert->keyUsage |= KEYUSE_DECIPHER_ONLY;
        else {
            ret = KEYUSAGE_E;
            break;
        }

        token = XSTRTOK(NULL, ",", &ptr);
    }

    XFREE(str, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    return ret;
}

/* Set ExtendedKeyUsage from human readable string */
int wc_SetExtKeyUsage(Cert *cert, const char *value)
{
    int ret = 0;
    char *token, *str, *ptr;
    word32 len;

    if (cert == NULL || value == NULL)
        return BAD_FUNC_ARG;

    cert->extKeyUsage = 0;

    /* duplicate string (including terminator) */
    len = (word32)XSTRLEN(value);
    str = (char*)XMALLOC(len+1, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (str == NULL)
        return MEMORY_E;
    XMEMCPY(str, value, len+1);

    /* parse value, and set corresponding Key Usage value */
    if ((token = XSTRTOK(str, ",", &ptr)) == NULL) {
        XFREE(str, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return EXTKEYUSAGE_E;
    }

    while (token != NULL)
    {
        len = (word32)XSTRLEN(token);

        if (!XSTRNCASECMP(token, "any", len))
            cert->extKeyUsage |= EXTKEYUSE_ANY;
        else if (!XSTRNCASECMP(token, "serverAuth", len))
            cert->extKeyUsage |= EXTKEYUSE_SERVER_AUTH;
        else if (!XSTRNCASECMP(token, "clientAuth", len))
            cert->extKeyUsage |= EXTKEYUSE_CLIENT_AUTH;
        else if (!XSTRNCASECMP(token, "codeSigning", len))
            cert->extKeyUsage |= EXTKEYUSE_CODESIGN;
        else if (!XSTRNCASECMP(token, "emailProtection", len))
            cert->extKeyUsage |= EXTKEYUSE_EMAILPROT;
        else if (!XSTRNCASECMP(token, "timeStamping", len))
            cert->extKeyUsage |= EXTKEYUSE_TIMESTAMP;
        else if (!XSTRNCASECMP(token, "OCSPSigning", len))
            cert->extKeyUsage |= EXTKEYUSE_OCSP_SIGN;
        else {
            ret = EXTKEYUSAGE_E;
            break;
        }

        token = XSTRTOK(NULL, ",", &ptr);
    }

    XFREE(str, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
    return ret;
}

#ifdef WOLFSSL_EKU_OID
/*
 * cert structure to set EKU oid in
 * oid  the oid in byte representation
 * sz   size of oid buffer
 * idx  index of array to place oid
 *
 * returns 0 on success
 */
int wc_SetExtKeyUsageOID(Cert *cert, const char *in, word32 sz, byte idx,
        void* heap)
{
    byte oid[MAX_OID_SZ];
    word32 oidSz = MAX_OID_SZ;

    if (idx >= CTC_MAX_EKU_NB || sz >= CTC_MAX_EKU_OID_SZ) {
        WOLFSSL_MSG("Either idx or sz was too large");
        return BAD_FUNC_ARG;
    }

    if (EncodePolicyOID(oid, &oidSz, in, heap) != 0) {
        return BUFFER_E;
    }

    XMEMCPY(cert->extKeyUsageOID[idx], oid, oidSz);
    cert->extKeyUsageOIDSz[idx] = oidSz;
    cert->extKeyUsage |= EXTKEYUSE_USER;

    return 0;
}
#endif /* WOLFSSL_EKU_OID */
#endif /* WOLFSSL_CERT_EXT */


#ifdef WOLFSSL_ALT_NAMES

static int SetAltNamesFromDcert(Cert* cert, DecodedCert* decoded)
{
    int ret = 0;

    cert->altNamesSz = 0;
    if (decoded->altNames) {
        ret = FlattenAltNames(cert->altNames,
            sizeof(cert->altNames), decoded->altNames);
        if (ret >= 0) {
            cert->altNamesSz = ret;
            ret = 0;
        }
    }

    return ret;
}

#ifndef NO_FILESYSTEM

/* Set Alt Names from der cert, return 0 on success */
static int SetAltNamesFromCert(Cert* cert, const byte* der, int derSz)
{
    int ret;
#ifdef WOLFSSL_SMALL_STACK
    DecodedCert* decoded;
#else
    DecodedCert decoded[1];
#endif

    if (derSz < 0)
        return derSz;

#ifdef WOLFSSL_SMALL_STACK
    decoded = (DecodedCert*)XMALLOC(sizeof(DecodedCert), cert->heap,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
    if (decoded == NULL)
        return MEMORY_E;
#endif

    InitDecodedCert(decoded, der, derSz, NULL);
    ret = ParseCertRelative(decoded, CA_TYPE, NO_VERIFY, 0);

    if (ret < 0) {
        WOLFSSL_MSG("ParseCertRelative error");
    }
    else {
        ret = SetAltNamesFromDcert(cert, decoded);
    }

    FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
    XFREE(decoded, cert->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret < 0 ? ret : 0;
}

#endif

static int SetDatesFromDcert(Cert* cert, DecodedCert* decoded)
{
    int ret = 0;

    if (decoded->beforeDate == NULL || decoded->afterDate == NULL) {
        WOLFSSL_MSG("Couldn't extract dates");
        ret = -1;
    }
    else if (decoded->beforeDateLen > MAX_DATE_SIZE ||
                                        decoded->afterDateLen > MAX_DATE_SIZE) {
        WOLFSSL_MSG("Bad date size");
        ret = -1;
    }
    else {
        XMEMCPY(cert->beforeDate, decoded->beforeDate, decoded->beforeDateLen);
        XMEMCPY(cert->afterDate,  decoded->afterDate,  decoded->afterDateLen);

        cert->beforeDateSz = decoded->beforeDateLen;
        cert->afterDateSz  = decoded->afterDateLen;
    }

    return ret;
}

#endif /* WOLFSSL_ALT_NAMES */

static void SetNameFromDcert(CertName* cn, DecodedCert* decoded)
{
    int sz;

    if (decoded->subjectCN) {
        sz = (decoded->subjectCNLen < CTC_NAME_SIZE) ? decoded->subjectCNLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->commonName, decoded->subjectCN, sz);
        cn->commonName[sz] = '\0';
        cn->commonNameEnc = decoded->subjectCNEnc;
    }
    if (decoded->subjectC) {
        sz = (decoded->subjectCLen < CTC_NAME_SIZE) ? decoded->subjectCLen
                                                    : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->country, decoded->subjectC, sz);
        cn->country[sz] = '\0';
        cn->countryEnc = decoded->subjectCEnc;
    }
    if (decoded->subjectST) {
        sz = (decoded->subjectSTLen < CTC_NAME_SIZE) ? decoded->subjectSTLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->state, decoded->subjectST, sz);
        cn->state[sz] = '\0';
        cn->stateEnc = decoded->subjectSTEnc;
    }
    if (decoded->subjectL) {
        sz = (decoded->subjectLLen < CTC_NAME_SIZE) ? decoded->subjectLLen
                                                    : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->locality, decoded->subjectL, sz);
        cn->locality[sz] = '\0';
        cn->localityEnc = decoded->subjectLEnc;
    }
    if (decoded->subjectO) {
        sz = (decoded->subjectOLen < CTC_NAME_SIZE) ? decoded->subjectOLen
                                                    : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->org, decoded->subjectO, sz);
        cn->org[sz] = '\0';
        cn->orgEnc = decoded->subjectOEnc;
    }
    if (decoded->subjectOU) {
        sz = (decoded->subjectOULen < CTC_NAME_SIZE) ? decoded->subjectOULen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->unit, decoded->subjectOU, sz);
        cn->unit[sz] = '\0';
        cn->unitEnc = decoded->subjectOUEnc;
    }
    if (decoded->subjectSN) {
        sz = (decoded->subjectSNLen < CTC_NAME_SIZE) ? decoded->subjectSNLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->sur, decoded->subjectSN, sz);
        cn->sur[sz] = '\0';
        cn->surEnc = decoded->subjectSNEnc;
    }
    if (decoded->subjectSND) {
        sz = (decoded->subjectSNDLen < CTC_NAME_SIZE) ? decoded->subjectSNDLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->serialDev, decoded->subjectSND, sz);
        cn->serialDev[sz] = '\0';
        cn->serialDevEnc = decoded->subjectSNDEnc;
    }
#ifdef WOLFSSL_CERT_EXT
    if (decoded->subjectBC) {
        sz = (decoded->subjectBCLen < CTC_NAME_SIZE) ? decoded->subjectBCLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->busCat, decoded->subjectBC, sz);
        cn->busCat[sz] = '\0';
        cn->busCatEnc = decoded->subjectBCEnc;
    }
    if (decoded->subjectJC) {
        sz = (decoded->subjectJCLen < CTC_NAME_SIZE) ? decoded->subjectJCLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->joiC, decoded->subjectJC, sz);
        cn->joiC[sz] = '\0';
        cn->joiCEnc = decoded->subjectJCEnc;
    }
    if (decoded->subjectJS) {
        sz = (decoded->subjectJSLen < CTC_NAME_SIZE) ? decoded->subjectJSLen
                                                     : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->joiSt, decoded->subjectJS, sz);
        cn->joiSt[sz] = '\0';
        cn->joiStEnc = decoded->subjectJSEnc;
    }
#endif
    if (decoded->subjectEmail) {
        sz = (decoded->subjectEmailLen < CTC_NAME_SIZE)
           ?  decoded->subjectEmailLen : CTC_NAME_SIZE - 1;
        XSTRNCPY(cn->email, decoded->subjectEmail, sz);
        cn->email[sz] = '\0';
    }
}

#ifndef NO_FILESYSTEM

/* Set cn name from der buffer, return 0 on success */
static int SetNameFromCert(CertName* cn, const byte* der, int derSz)
{
    int ret;
#ifdef WOLFSSL_SMALL_STACK
    DecodedCert* decoded;
#else
    DecodedCert decoded[1];
#endif

    if (derSz < 0)
        return derSz;

#ifdef WOLFSSL_SMALL_STACK
    decoded = (DecodedCert*)XMALLOC(sizeof(DecodedCert), NULL,
                                                       DYNAMIC_TYPE_TMP_BUFFER);
    if (decoded == NULL)
        return MEMORY_E;
#endif

    InitDecodedCert(decoded, der, derSz, NULL);
    ret = ParseCertRelative(decoded, CA_TYPE, NO_VERIFY, 0);

    if (ret < 0) {
        WOLFSSL_MSG("ParseCertRelative error");
    }
    else {
        SetNameFromDcert(cn, decoded);
    }

    FreeDecodedCert(decoded);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(decoded, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret < 0 ? ret : 0;
}

/* Set cert issuer from issuerFile in PEM */
int wc_SetIssuer(Cert* cert, const char* issuerFile)
{
    int         ret;
    int         derSz;
    byte*       der;

    if (cert == NULL) {
        return BAD_FUNC_ARG;
    }

    der = (byte*)XMALLOC(EIGHTK_BUF, cert->heap, DYNAMIC_TYPE_CERT);
    if (der == NULL) {
        WOLFSSL_MSG("wc_SetIssuer OOF Problem");
        return MEMORY_E;
    }
    derSz = wc_PemCertToDer(issuerFile, der, EIGHTK_BUF);
    cert->selfSigned = 0;
    ret = SetNameFromCert(&cert->issuer, der, derSz);
    XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);

    return ret;
}


/* Set cert subject from subjectFile in PEM */
int wc_SetSubject(Cert* cert, const char* subjectFile)
{
    int         ret;
    int         derSz;
    byte*       der;

    if (cert == NULL) {
        return BAD_FUNC_ARG;
    }

    der = (byte*)XMALLOC(EIGHTK_BUF, cert->heap, DYNAMIC_TYPE_CERT);
    if (der == NULL) {
        WOLFSSL_MSG("wc_SetSubject OOF Problem");
        return MEMORY_E;
    }

    derSz = wc_PemCertToDer(subjectFile, der, EIGHTK_BUF);
    ret = SetNameFromCert(&cert->subject, der, derSz);
    XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);

    return ret;
}

#ifdef WOLFSSL_ALT_NAMES

/* Set alt names from file in PEM */
int wc_SetAltNames(Cert* cert, const char* file)
{
    int         ret;
    int         derSz;
    byte*       der;

    if (cert == NULL) {
        return BAD_FUNC_ARG;
    }

    der = (byte*)XMALLOC(EIGHTK_BUF, cert->heap, DYNAMIC_TYPE_CERT);
    if (der == NULL) {
        WOLFSSL_MSG("wc_SetAltNames OOF Problem");
        return MEMORY_E;
    }
    derSz = wc_PemCertToDer(file, der, EIGHTK_BUF);
    ret = SetAltNamesFromCert(cert, der, derSz);
    XFREE(der, cert->heap, DYNAMIC_TYPE_CERT);

    return ret;
}

#endif /* WOLFSSL_ALT_NAMES */

#endif /* !NO_FILESYSTEM */

/* Set cert issuer from DER buffer */
int wc_SetIssuerBuffer(Cert* cert, const byte* der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        cert->selfSigned = 0;

        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            SetNameFromDcert(&cert->issuer, (DecodedCert*)cert->decodedCert);
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
        }
    }

    return ret;
}

/* Set cert subject from DER buffer */
int wc_SetSubjectBuffer(Cert* cert, const byte* der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            SetNameFromDcert(&cert->subject, (DecodedCert*)cert->decodedCert);
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
        }
    }

    return ret;
}
#ifdef WOLFSSL_CERT_EXT
/* Set cert raw subject from DER buffer */
int wc_SetSubjectRaw(Cert* cert, const byte* der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            if ((((DecodedCert*)cert->decodedCert)->subjectRaw) &&
                (((DecodedCert*)cert->decodedCert)->subjectRawLen <=
                        (int)sizeof(CertName))) {
                XMEMCPY(cert->sbjRaw,
                        ((DecodedCert*)cert->decodedCert)->subjectRaw,
                        ((DecodedCert*)cert->decodedCert)->subjectRawLen);
            }
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
        }
    }

    return ret;
}

/* Set cert raw issuer from DER buffer */
int wc_SetIssuerRaw(Cert* cert, const byte* der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
        ret = BAD_FUNC_ARG;
    }
    else {
        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            if ((((DecodedCert*)cert->decodedCert)->subjectRaw) &&
                (((DecodedCert*)cert->decodedCert)->subjectRawLen <=
                        (int)sizeof(CertName))) {
                /* Copy the subject to the issuer field */
                XMEMCPY(cert->issRaw,
                        ((DecodedCert*)cert->decodedCert)->subjectRaw,
                        ((DecodedCert*)cert->decodedCert)->subjectRawLen);
            }
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
        }
    }
    return ret;
}
#endif

#ifdef WOLFSSL_ALT_NAMES

/* Set cert alt names from DER buffer */
int wc_SetAltNamesBuffer(Cert* cert, const byte* der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
       ret = BAD_FUNC_ARG;
    }
    else {
        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            ret = SetAltNamesFromDcert(cert, (DecodedCert*)cert->decodedCert);
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
       }
    }

    return(ret);
}

/* Set cert dates from DER buffer */
int wc_SetDatesBuffer(Cert* cert, const byte* der, int derSz)
{
    int ret = 0;

    if (cert == NULL) {
     ret = BAD_FUNC_ARG;
    }
    else {
        /* Check if decodedCert is cached */
        if (cert->der != der) {
            /* Allocate cache for the decoded cert */
            ret = wc_SetCert_LoadDer(cert, der, derSz);
        }

        if (ret >= 0) {
            ret = SetDatesFromDcert(cert, (DecodedCert*)cert->decodedCert);
#ifndef WOLFSSL_CERT_GEN_CACHE
            wc_SetCert_Free(cert);
#endif
        }
    }

    return(ret);
}

#endif /* WOLFSSL_ALT_NAMES */

#endif /* WOLFSSL_CERT_GEN */

#if (defined(WOLFSSL_CERT_GEN) && defined(WOLFSSL_CERT_EXT)) \
        || defined(OPENSSL_EXTRA)
/* Encode OID string representation to ITU-T X.690 format */
int EncodePolicyOID(byte *out, word32 *outSz, const char *in, void* heap)
{
    word32 val, idx = 0, nb_val;
    char *token, *str, *ptr;
    word32 len;

    (void)heap;

    if (out == NULL || outSz == NULL || *outSz < 2 || in == NULL)
        return BAD_FUNC_ARG;

    /* duplicate string (including terminator) */
    len = (word32)XSTRLEN(in);
    str = (char *)XMALLOC(len+1, heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (str == NULL)
        return MEMORY_E;
    XMEMCPY(str, in, len+1);

    nb_val = 0;

    /* parse value, and set corresponding Policy OID value */
    token = XSTRTOK(str, ".", &ptr);
    while (token != NULL)
    {
        val = (word32)XATOI(token);

        if (nb_val == 0) {
            if (val > 2) {
                XFREE(str, heap, DYNAMIC_TYPE_TMP_BUFFER);
                return ASN_OBJECT_ID_E;
            }

            out[idx] = (byte)(40 * val);
        }
        else if (nb_val == 1) {
            if (val > 127) {
                XFREE(str, heap, DYNAMIC_TYPE_TMP_BUFFER);
                return ASN_OBJECT_ID_E;
            }

            if (idx > *outSz) {
                XFREE(str, heap, DYNAMIC_TYPE_TMP_BUFFER);
                return BUFFER_E;
            }

            out[idx++] += (byte)val;
        }
        else {
            word32  tb = 0, x;
            int     i = 0;
            byte    oid[MAX_OID_SZ];

            while (val >= 128) {
                x = val % 128;
                val /= 128;
                oid[i++] = (byte) (((tb++) ? 0x80 : 0) | x);
            }

            if ((idx+(word32)i) >= *outSz) {
                XFREE(str, heap, DYNAMIC_TYPE_TMP_BUFFER);
                return BUFFER_E;
            }

            oid[i] = (byte) (((tb++) ? 0x80 : 0) | val);

            /* push value in the right order */
            while (i >= 0)
                out[idx++] = oid[i--];
        }

        token = XSTRTOK(NULL, ".", &ptr);
        nb_val++;
    }

    *outSz = idx;

    XFREE(str, heap, DYNAMIC_TYPE_TMP_BUFFER);
    return 0;
}
#endif /* WOLFSSL_CERT_EXT || OPENSSL_EXTRA */

#endif /* !NO_CERTS */

#if !defined(NO_DH) && (defined(WOLFSSL_QT) || defined(OPENSSL_ALL))
/* Helper function for wolfSSL_i2d_DHparams */
int StoreDHparams(byte* out, word32* outLen, mp_int* p, mp_int* g)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    word32 total;

    WOLFSSL_ENTER("StoreDHparams");

    if (out == NULL) {
        WOLFSSL_MSG("Null buffer error");
        return BUFFER_E;
    }

    /* determine size */
    /* integer - g */
    idx = SetASNIntMP(g, -1, NULL);
    /* integer - p */
    idx += SetASNIntMP(p, -1, NULL);
    total = idx;
     /* sequence */
    idx += SetSequence(idx, NULL);

    /* make sure output fits in buffer */
    if (idx > *outLen) {
        return BUFFER_E;
    }

    /* write DH parameters */
    /* sequence - for P and G only */
    idx = SetSequence(total, out);
    /* integer - p */
    idx += SetASNIntMP(p, -1, out + idx);
    /* integer - g */
    idx += SetASNIntMP(g, -1, out + idx);
    *outLen = idx;

    return 0;
#else
    ASNSetData dataASN[dhParamASN_Length];
    int ret = 0;
    int sz;

    WOLFSSL_ENTER("StoreDHparams");
    if (out == NULL) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        XMEMSET(dataASN, 0, sizeof(dataASN));
        /* Set mp_int containing p and g. */
        SetASN_MP(&dataASN[1], p);
        SetASN_MP(&dataASN[2], g);
        /* privateValueLength not encoded. */
        dataASN[3].noOut = 1;

        /* Calculate the size of the DH parameters. */
        ret = SizeASN_Items(dhParamASN, dataASN, dhParamASN_Length, &sz);
    }
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && ((int)*outLen < sz)) {
        ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode the DH parameters into buffer. */
        SetASN_Items(dhParamASN, dataASN, dhParamASN_Length, out);
        /* Set the actual encoding size. */
        *outLen = sz;
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif /* !NO_DH && (WOLFSSL_QT || OPENSSL_ALL) */

#if defined(HAVE_ECC) || !defined(NO_DSA)

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for DSA signature.
 * RFC 5912, 6 - DSA-Sig-Value
 */
static const ASNItem dsaSigASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* r */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* s */
/*  2 */        { 1, ASN_INTEGER, 0, 0, 0 },
};

#define dsaSigASN_Length (sizeof(dsaSigASN) / sizeof(ASNItem))
#endif

/* Der Encode r & s ints into out, outLen is (in/out) size */
int StoreECC_DSA_Sig(byte* out, word32* outLen, mp_int* r, mp_int* s)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int    rSz;                           /* encoding size */
    int    sSz;
    word32 headerSz = 4;   /* 2*ASN_TAG + 2*LEN(ENUM) */

    /* If the leading bit on the INTEGER is a 1, add a leading zero */
    int rLeadingZero = mp_leading_bit(r);
    int sLeadingZero = mp_leading_bit(s);
    int rLen = mp_unsigned_bin_size(r);   /* big int size */
    int sLen = mp_unsigned_bin_size(s);

    if (*outLen < (rLen + rLeadingZero + sLen + sLeadingZero +
                   headerSz + 2))  /* SEQ_TAG + LEN(ENUM) */
        return BUFFER_E;

    idx = SetSequence(rLen + rLeadingZero + sLen+sLeadingZero + headerSz, out);

    /* store r */
    rSz = SetASNIntMP(r, *outLen - idx, &out[idx]);
    if (rSz < 0)
        return rSz;
    idx += rSz;

    /* store s */
    sSz = SetASNIntMP(s, *outLen - idx, &out[idx]);
    if (sSz < 0)
        return sSz;
    idx += sSz;

    *outLen = idx;

    return 0;
#else
    ASNSetData dataASN[dsaSigASN_Length];
    int ret;
    int sz;

    /* Clear dynamic data and set mp_ints r and s */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    SetASN_MP(&dataASN[1], r);
    SetASN_MP(&dataASN[2], s);

    /* Calculate size of encoding. */
    ret = SizeASN_Items(dsaSigASN, dataASN, dsaSigASN_Length, &sz);
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && ((int)*outLen < sz)) {
       ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode DSA signature into buffer. */
        SetASN_Items(dsaSigASN, dataASN, dsaSigASN_Length, out);
        /* Set the actual encoding size. */
        *outLen = sz;
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifndef WOLFSSL_ASN_TEMPLATE
/* determine if leading bit is set */
static int is_leading_bit_set(const byte* input, word32 sz)
{
    byte c = 0;
    if (sz > 0)
        c = input[0];
    return (c & 0x80) != 0;
}
static int trim_leading_zeros(const byte** input, word32 sz)
{
    int i, leadingZeroCount = 0;
    const byte* tmp = *input;
    for (i=0; i<(int)sz; i++) {
        if (tmp[i] != 0)
            break;
        leadingZeroCount++;
    }
    /* catch all zero case */
    if (sz > 0 && leadingZeroCount == (int)sz) {
        leadingZeroCount--;
    }
    *input += leadingZeroCount;
    sz -= leadingZeroCount;
    return sz;
}
#endif

/* Der Encode r & s ints into out, outLen is (in/out) size */
/* All input/outputs are assumed to be big-endian */
int StoreECC_DSA_Sig_Bin(byte* out, word32* outLen, const byte* r, word32 rLen,
    const byte* s, word32 sLen)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
    word32 idx;
    word32 headerSz = 4;   /* 2*ASN_TAG + 2*LEN(ENUM) */
    int rAddLeadZero, sAddLeadZero;

    if ((out == NULL) || (outLen == NULL) || (r == NULL) || (s == NULL))
        return BAD_FUNC_ARG;

    /* Trim leading zeros */
    rLen = trim_leading_zeros(&r, rLen);
    sLen = trim_leading_zeros(&s, sLen);
    /* If the leading bit on the INTEGER is a 1, add a leading zero */
    /* Add leading zero if MSB is set */
    rAddLeadZero = is_leading_bit_set(r, rLen);
    sAddLeadZero = is_leading_bit_set(s, sLen);

    if (*outLen < (rLen + rAddLeadZero + sLen + sAddLeadZero +
                   headerSz + 2))  /* SEQ_TAG + LEN(ENUM) */
        return BUFFER_E;

    idx = SetSequence(rLen+rAddLeadZero + sLen+sAddLeadZero + headerSz, out);

    /* store r */
    ret = SetASNInt(rLen, rAddLeadZero ? 0x80 : 0x00, &out[idx]);
    if (ret < 0)
        return ret;
    idx += ret;
    XMEMCPY(&out[idx], r, rLen);
    idx += rLen;

    /* store s */
    ret = SetASNInt(sLen, sAddLeadZero ? 0x80 : 0x00, &out[idx]);
    if (ret < 0)
        return ret;
    idx += ret;
    XMEMCPY(&out[idx], s, sLen);
    idx += sLen;

    *outLen = idx;

    return 0;
#else
    ASNSetData dataASN[dsaSigASN_Length];
    int ret;
    int sz;

    /* Clear dynamic data and set buffers for r and s */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    SetASN_Buffer(&dataASN[1], r, rLen);
    SetASN_Buffer(&dataASN[2], s, sLen);

    /* Calculate size of encoding. */
    ret = SizeASN_Items(dsaSigASN, dataASN, dsaSigASN_Length, &sz);
    /* Check buffer is big enough for encoding. */
    if ((ret == 0) && ((int)*outLen < sz)) {
       ret = BUFFER_E;
    }
    if (ret == 0) {
        /* Encode DSA signature into buffer. */
        SetASN_Items(dsaSigASN, dataASN, dsaSigASN_Length, out);
        /* Set the actual encoding size. */
        *outLen = sz;
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

/* Der Decode ECC-DSA Signature with R/S as unsigned bin */
/* All input/outputs are assumed to be big-endian */
int DecodeECC_DSA_Sig_Bin(const byte* sig, word32 sigLen, byte* r, word32* rLen,
    byte* s, word32* sLen)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    ret;
    word32 idx = 0;
    int    len = 0;

    if (GetSequence(sig, &idx, &len, sigLen) < 0) {
        return ASN_ECC_KEY_E;
    }

#ifndef NO_STRICT_ECDSA_LEN
    /* enable strict length checking for signature */
    if (sigLen != idx + (word32)len) {
        return ASN_ECC_KEY_E;
    }
#else
    /* allow extra signature bytes at end */
    if ((word32)len > (sigLen - idx)) {
        return ASN_ECC_KEY_E;
    }
#endif

    ret = GetASNInt(sig, &idx, &len, sigLen);
    if (ret != 0)
        return ret;
    if (rLen)
        *rLen = len;
    if (r)
        XMEMCPY(r, (byte*)sig + idx, len);
    idx += len;

    ret = GetASNInt(sig, &idx, &len, sigLen);
    if (ret != 0)
        return ret;
    if (sLen)
        *sLen = len;
    if (s)
        XMEMCPY(s, (byte*)sig + idx, len);

#ifndef NO_STRICT_ECDSA_LEN
    /* sanity check that the index has been advanced all the way to the end of
     * the buffer */
    if (idx + len != sigLen) {
        ret = ASN_ECC_KEY_E;
    }
#endif

    return ret;
#else
    ASNGetData dataASN[dsaSigASN_Length];
    word32 idx = 0;

    /* Clear dynamic data and set buffers to put r and s into. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_Buffer(&dataASN[1], r, rLen);
    GetASN_Buffer(&dataASN[2], s, sLen);

    /* Decode the DSA signature. */
    return GetASN_Items(dsaSigASN, dataASN, dsaSigASN_Length, 1, sig, &idx,
                        sigLen);
#endif /* WOLFSSL_ASN_TEMPLATE */
}

int DecodeECC_DSA_Sig(const byte* sig, word32 sigLen, mp_int* r, mp_int* s)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int    len = 0;

    if (GetSequence(sig, &idx, &len, sigLen) < 0) {
        return ASN_ECC_KEY_E;
    }

#ifndef NO_STRICT_ECDSA_LEN
    /* enable strict length checking for signature */
    if (sigLen != idx + (word32)len) {
        return ASN_ECC_KEY_E;
    }
#else
    /* allow extra signature bytes at end */
    if ((word32)len > (sigLen - idx)) {
        return ASN_ECC_KEY_E;
    }
#endif

    if (GetInt(r, sig, &idx, sigLen) < 0) {
        return ASN_ECC_KEY_E;
    }

    if (GetInt(s, sig, &idx, sigLen) < 0) {
        mp_clear(r);
        return ASN_ECC_KEY_E;
    }

#ifndef NO_STRICT_ECDSA_LEN
    /* sanity check that the index has been advanced all the way to the end of
     * the buffer */
    if (idx != sigLen) {
        mp_clear(r);
        mp_clear(s);
        return ASN_ECC_KEY_E;
    }
#endif

    return 0;
#else
    ASNGetData dataASN[dsaSigASN_Length];
    word32 idx = 0;

    /* Clear dynamic data and set mp_ints to put r and s into. */
    XMEMSET(dataASN, 0, sizeof(dataASN));
    GetASN_MP(&dataASN[1], r);
    GetASN_MP(&dataASN[2], s);

    /* Decode the DSA signature. */
    return GetASN_Items(dsaSigASN, dataASN, dsaSigASN_Length, 1, sig, &idx,
                        sigLen);
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif


#ifdef WOLFSSL_ASN_TEMPLATE
#ifdef WOLFSSL_CUSTOM_CURVES
/* Convert data to hex string.
 *
 * Big-endian byte array is converted to big-endian hexadecimal string.
 *
 * @param [in]  input  Buffer containing data.
 * @param [in]  inSz   Size of data in buffer.
 * @param [out] out    Buffer to hold hex string.
 */
static void DataToHexString(const byte* input, word32 inSz, char* out)
{
    static const char hexChar[] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    word32 i;

    /* Converting a byte of data at a time to two hex characters. */
    for (i = 0; i < inSz; i++) {
        out[i*2 + 0] = hexChar[input[i] >> 4];
        out[i*2 + 1] = hexChar[input[i] & 0xf];
    }
    /* NUL terminate string. */
    out[i * 2] = '\0';
}

/* Convert data to hex string and place in allocated buffer.
 *
 * Big-endian byte array is converted to big-endian hexadecimal string.
 *
 * @param [in]  input     Buffer containing data.
 * @param [in]  inSz      Size of data in buffer.
 * @param [out] out       Allocated buffer holding hex string.
 * @param [in]  heap      Dynamic memory allocation hint.
 * @param [in]  heapType  Type of heap to use.
 * @return  0 on succcess.
 * @return  MEMORY_E when dynamic memory allocation fails.
 */
static int DataToHexStringAlloc(const byte* input, word32 inSz, char** out,
                                void* heap, int heapType)
{
    int ret = 0;
    char* str;

    /* Allocate for 2 string characters ber byte plus NUL. */
    str = (char*)XMALLOC(inSz * 2 + 1, heap, heapType);
    if (str == NULL) {
        ret = MEMORY_E;
    }
    else {
        /* Convert to hex string. */
        DataToHexString(input, inSz, str);
        *out = str;
    }

    (void)heap;
    (void)heapType;

    return ret;
}

/* ASN.1 template for SpecifiedECDomain.
 * SEC 1 Ver. 2.0, C.2 - Syntax for Elliptic Curve Domain Parameters
 * NOTE: characteristic-two-field not supported. */
static const ASNItem eccSpecifiedASN[] = {
            /* version */
/*  0 */    { 0, ASN_INTEGER, 0, 0, 0 },
            /* fieldID */
/*  1 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* prime-field or characteristic-two-field */
/*  2 */        { 1, ASN_OBJECT_ID, 0, 0, 0 },
                /* Prime-p */
/*  3 */        { 1, ASN_INTEGER, 0, 0, 0 },
            /* fieldID */
/*  4 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* a */
/*  5 */        { 1, ASN_OCTET_STRING, 0, 0, 0 },
                /* b */
/*  6 */        { 1, ASN_OCTET_STRING, 0, 0, 0 },
                /* seed */
/*  7 */        { 1, ASN_BIT_STRING, 0, 0, 1 },
            /* base */
/*  8 */    { 0, ASN_OCTET_STRING, 0, 0, 0 },
            /* order */
/*  9 */    { 0, ASN_INTEGER, 0, 0, 0 },
            /* cofactor */
/* 10 */    { 0, ASN_INTEGER, 0, 0, 1 },
            /* hash */
/* 11 */    { 0, ASN_SEQUENCE, 0, 0, 1 },
};

/* Number of items in ASN.1 template for SpecifiedECDomain. */
#define eccSpecifiedASN_Length (sizeof(eccSpecifiedASN) / sizeof(ASNItem))

/* OID indicating the prime field is explicity defined. */
static const byte primeFieldOID[] = {
    0x2a, 0x86, 0x48, 0xce, 0x3d, 0x01, 0x01
};
static const char ecSetCustomName[] = "Custom";

/* Explicit EC parameter values. */
static int EccSpecifiedECDomainDecode(const byte* input, word32 inSz,
                                      ecc_key* key)
{
    DECL_ASNGETDATA(dataASN, eccSpecifiedASN_Length);
    int ret = 0;
    ecc_set_type* curve;
    word32 idx = 0;
    byte version;
    byte cofactor;
    const byte *base;
    word32 baseLen;

    /* Allocate a new parameter set. */
    curve = (ecc_set_type*)XMALLOC(sizeof(*curve), key->heap,
                                                       DYNAMIC_TYPE_ECC_BUFFER);
    if (curve == NULL)
        ret = MEMORY_E;

    CALLOC_ASNGETDATA(dataASN, eccSpecifiedASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Clear out parameters and set fields to indicate it is custom. */
        XMEMSET(curve, 0, sizeof(*curve));
        /* Set name to be: "Custom" */
    #ifndef WOLFSSL_ECC_CURVE_STATIC
        curve->name = ecSetCustomName;
    #else
        XMEMCPY((void*)curve->name, ecSetCustomName, sizeof(ecSetCustomName));
    #endif
        curve->id = ECC_CURVE_CUSTOM;

        /* Get version, must have prime field OID and get co-factor. */
        GetASN_Int8Bit(&dataASN[0], &version);
        GetASN_ExpBuffer(&dataASN[2], primeFieldOID, sizeof(primeFieldOID));
        GetASN_Int8Bit(&dataASN[10], &cofactor);
        /* Decode the explicit parameters. */
        ret = GetASN_Items(eccSpecifiedASN, dataASN, eccSpecifiedASN_Length, 1,
                           input, &idx, inSz);
    }
    /* Version must be 1 or 2 for supporting explicit parameters. */
    if ((ret == 0) && (version < 1 || version > 3)) {
        ret = ASN_PARSE_E;
    }
    /* Only version 2 and above can have a seed. */
    if ((ret == 0) && (dataASN[7].tag != 0) && (version < 2)) {
        ret = ASN_PARSE_E;
    }
    /* Only version 2 and above can have a hash algorithm. */
    if ((ret == 0) && (dataASN[11].tag != 0) && (version < 2)) {
        ret = ASN_PARSE_E;
    }
    if ((ret == 0) && (dataASN[10].tag != 0)) {
        /* Store optional co-factor. */
        curve->cofactor = cofactor;
    }
    if (ret == 0) {
        /* Length of the prime in bytes is the curve size. */
        curve->size = (int)dataASN[3].data.ref.length;
        /* Base point: 0x04 <x> <y> (must be uncompressed). */
        GetASN_GetConstRef(&dataASN[8], &base, &baseLen);
        if ((baseLen < (word32)curve->size * 2 + 1) || (base[0] != 0x4)) {
            ret = ASN_PARSE_E;
        }
    }
    /* Put the curve parameters into the set.
     * Convert the big-endian number byte array to a big-endian string.
     */
    #ifndef WOLFSSL_ECC_CURVE_STATIC
    /* Allocate buffer to put hex strings into. */
    if (ret == 0) {
        /* Base X-ordinate */
        ret = DataToHexStringAlloc(base + 1, curve->size,
                                   (char**)&curve->Gx, key->heap,
                                   DYNAMIC_TYPE_ECC_BUFFER);
    }
    if (ret == 0) {
        /* Base Y-ordinate */
        ret = DataToHexStringAlloc(base + 1 + curve->size, curve->size,
                                   (char**)&curve->Gy, key->heap,
                                   DYNAMIC_TYPE_ECC_BUFFER);
    }
    if (ret == 0) {
        /* Prime */
        ret = DataToHexStringAlloc(dataASN[3].data.ref.data,
                                   dataASN[3].data.ref.length,
                                   (char**)&curve->prime, key->heap,
                                   DYNAMIC_TYPE_ECC_BUFFER);
    }
    if (ret == 0) {
        /* Parameter A */
        ret = DataToHexStringAlloc(dataASN[5].data.ref.data,
                                   dataASN[5].data.ref.length,
                                   (char**)&curve->Af, key->heap,
                                   DYNAMIC_TYPE_ECC_BUFFER);
    }
    if (ret == 0) {
        /* Parameter B */
        ret = DataToHexStringAlloc(dataASN[6].data.ref.data,
                                   dataASN[6].data.ref.length,
                                   (char**)&curve->Bf, key->heap,
                                   DYNAMIC_TYPE_ECC_BUFFER);
    }
    if (ret == 0) {
        /* Order of curve */
        ret = DataToHexStringAlloc(dataASN[9].data.ref.data,
                                   dataASN[9].data.ref.length,
                                   (char**)&curve->order, key->heap,
                                   DYNAMIC_TYPE_ECC_BUFFER);
    }
    #else
    if (ret == 0) {
        /* Base X-ordinate */
        DataToHexString(base + 1, curve->size, curve->Gx);
        /* Base Y-ordinate */
        DataToHexString(base + 1 + curve->size, curve->size, curve->Gy);
        /* Prime */
        DataToHexString(dataASN[3].data.ref.data, dataASN[3].data.ref.length,
                        curve->prime);
        /* Parameter A */
        DataToHexString(dataASN[5].data.ref.data, dataASN[5].data.ref.length,
                        curve->Af);
        /* Parameter B */
        DataToHexString(dataASN[6].data.ref.data, dataASN[6].data.ref.length,
                        curve->Bf);
        /* Order of curve */
        DataToHexString(dataASN[9].data.ref.data, dataASN[9].data.ref.length,
                        curve->order);
    }
    #endif /* WOLFSSL_ECC_CURVE_STATIC */

    /* Store parameter set in key. */
    if ((ret == 0) && (wc_ecc_set_custom_curve(key, curve) < 0)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* The parameter set was allocated.. */
        key->deallocSet = 1;
    }

    if ((ret != 0) && (curve != NULL)) {
        /* Failed to set parameters so free paramter set. */
        wc_ecc_free_curve(curve, key->heap);
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
}
#endif /* WOLFSSL_CUSTOM_CURVES */
#endif /* WOLFSSL_ASN_TEMPLATE */

#ifdef HAVE_ECC

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for ECC private key.
 * SEC.1 Ver 2.0, C.4 - Syntax for Elliptic Curve Private Keys
 */
static const ASNItem eccKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* version */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* privateKey */
/*  2 */        { 1, ASN_OCTET_STRING, 0, 0, 0 },
                /* parameters */
/*  3 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
                    /* named */
/*  4 */            { 2, ASN_OBJECT_ID, 0, 0, 2 },
                    /* specified */
/*  5 */            { 2, ASN_SEQUENCE, 1, 0, 2 },
                /* publicKey */
/*  6 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 1, 1 },
                    /* Uncompressed point - X9.62. */
/*  7 */            { 2, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for ECC private key. */
#define eccKeyASN_Length (sizeof(eccKeyASN) / sizeof(ASNItem))
#endif

int wc_EccPrivateKeyDecode(const byte* input, word32* inOutIdx, ecc_key* key,
                        word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 oidSum;
    int    version, length;
    int    privSz, pubSz = 0;
    byte   b;
    int    ret = 0;
    int    curve_id = ECC_CURVE_DEF;
#ifdef WOLFSSL_SMALL_STACK
    byte* priv;
    byte* pub = NULL;
#else
    byte priv[ECC_MAXSIZE+1];
    byte pub[2*(ECC_MAXSIZE+1)]; /* public key has two parts plus header */
#endif
    word32 algId = 0;
    byte* pubData = NULL;

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0)
        return BAD_FUNC_ARG;

    /* if has pkcs8 header skip it */
    if (ToTraditionalInline_ex(input, inOutIdx, inSz, &algId) < 0) {
        /* ignore error, did not have pkcs8 header */
    }

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    if (GetMyVersion(input, inOutIdx, &version, inSz) < 0)
        return ASN_PARSE_E;

    if (*inOutIdx >= inSz)
        return ASN_PARSE_E;

    b = input[*inOutIdx];
    *inOutIdx += 1;

    /* priv type */
    if (b != 4 && b != 6 && b != 7)
        return ASN_PARSE_E;

    if (GetLength(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;
    privSz = length;

    if (privSz > ECC_MAXSIZE)
        return BUFFER_E;

#ifdef WOLFSSL_SMALL_STACK
    priv = (byte*)XMALLOC(privSz, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (priv == NULL)
        return MEMORY_E;
#endif

    /* priv key */
    XMEMCPY(priv, &input[*inOutIdx], privSz);
    *inOutIdx += length;

    if ((*inOutIdx + 1) < inSz) {
        /* prefix 0, may have */
        b = input[*inOutIdx];
        if (b == ECC_PREFIX_0) {
            *inOutIdx += 1;

            if (GetLength(input, inOutIdx, &length, inSz) <= 0)
                ret = ASN_PARSE_E;
            else {
                ret = GetObjectId(input, inOutIdx, &oidSum, oidIgnoreType,
                                  inSz);
                if (ret == 0) {
                    if ((ret = CheckCurve(oidSum)) < 0)
                        ret = ECC_CURVE_OID_E;
                    else {
                        curve_id = ret;
                        ret = 0;
                    }
                }
            }
        }
    }

    if (ret == 0 && (*inOutIdx + 1) < inSz) {
        /* prefix 1 */
        b = input[*inOutIdx];
        *inOutIdx += 1;

        if (b != ECC_PREFIX_1) {
            ret = ASN_ECC_KEY_E;
        }
        else if (GetLength(input, inOutIdx, &length, inSz) <= 0) {
            ret = ASN_PARSE_E;
        }
        else {
            /* key header */
            ret = CheckBitString(input, inOutIdx, &length, inSz, 0, NULL);
            if (ret == 0) {
                /* pub key */
                pubSz = length;
                if (pubSz > 2*(ECC_MAXSIZE+1))
                    ret = BUFFER_E;
                else {
            #ifdef WOLFSSL_SMALL_STACK
                    pub = (byte*)XMALLOC(pubSz, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
                    if (pub == NULL)
                        ret = MEMORY_E;
                    else
            #endif
                    {
                        XMEMCPY(pub, &input[*inOutIdx], pubSz);
                        *inOutIdx += length;
                        pubData = pub;
                    }
                }
            }
        }
    }

    if (ret == 0) {
        ret = wc_ecc_import_private_key_ex(priv, privSz, pubData, pubSz, key,
                                                                      curve_id);
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(priv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(pub,  key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
#else
    DECL_ASNGETDATA(dataASN, eccKeyASN_Length);
    byte version;
    int ret = 0;
    int curve_id = ECC_CURVE_DEF;
#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
    word32 algId = 0;
#endif

    /* Validate parameters. */
    if ((input == NULL) || (inOutIdx == NULL) || (key == NULL) || (inSz == 0)) {
        ret = BAD_FUNC_ARG;
    }

#if defined(HAVE_PKCS8) || defined(HAVE_PKCS12)
    /* if has pkcs8 header skip it */
    if (ToTraditionalInline_ex(input, inOutIdx, inSz, &algId) < 0) {
        /* ignore error, did not have pkcs8 header */
    }
#endif

    CALLOC_ASNGETDATA(dataASN, eccKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Get the version and set the expected OID type. */
        GetASN_Int8Bit(&dataASN[1], &version);
        GetASN_OID(&dataASN[4], oidCurveType);
        /* Decode the private ECC key. */
        ret = GetASN_Items(eccKeyASN, dataASN, eccKeyASN_Length, 1, input,
                           inOutIdx, inSz);
    }
    /* Only version 1 supported. */
    if ((ret == 0) && (version != 1)) {
        ret = ASN_PARSE_E;
    }
    /* Curve Parameters are optional. */
    if ((ret == 0) && (dataASN[3].tag != 0)) {
        if (dataASN[4].tag != 0) {
            /* Named curve - check and get id. */
            curve_id = CheckCurve(dataASN[4].data.oid.sum);
            if (curve_id < 0) {
                ret = ECC_CURVE_OID_E;
            }
        }
        else {
    #ifdef WOLFSSL_CUSTOM_CURVES
            /* Parse explicit parameters. */
            ret = EccSpecifiedECDomainDecode(dataASN[5].data.ref.data,
                    dataASN[5].data.ref.length, key);
    #else
            /* Explicit parameters not supported in build configuration. */
            ret = ASN_PARSE_E;
    #endif
        }
    }
    if (ret == 0) {
        /* Import private key value and public point (may be NULL). */
        ret = wc_ecc_import_private_key_ex(dataASN[2].data.ref.data,
                dataASN[2].data.ref.length, dataASN[7].data.ref.data,
                dataASN[7].data.ref.length, key, curve_id);
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
#endif
}


#ifdef WOLFSSL_CUSTOM_CURVES
#ifndef WOLFSSL_ASN_TEMPLATE
/* returns 0 on success */
static int ASNToHexString(const byte* input, word32* inOutIdx, char** out,
                          word32 inSz, void* heap, int heapType)
{
    int len;
    int i;
    char* str;
    word32 localIdx;
    byte   tag;

    if (*inOutIdx >= inSz) {
        return BUFFER_E;
    }

    localIdx = *inOutIdx;
    if (GetASNTag(input, &localIdx, &tag, inSz) == 0 && tag == ASN_INTEGER) {
        if (GetASNInt(input, inOutIdx, &len, inSz) < 0)
            return ASN_PARSE_E;
    }
    else {
        if (GetOctetString(input, inOutIdx, &len, inSz) < 0)
            return ASN_PARSE_E;
    }

    str = (char*)XMALLOC(len * 2 + 1, heap, heapType);
    if (str == NULL) {
        return MEMORY_E;
    }

    for (i=0; i<len; i++)
        ByteToHexStr(input[*inOutIdx + i], str + i*2);
    str[len*2] = '\0';

    *inOutIdx += len;
    *out = str;

    (void)heap;
    (void)heapType;

    return 0;
}

static int EccKeyParamCopy(char** dst, char* src)
{
    int ret = 0;
#ifdef WOLFSSL_ECC_CURVE_STATIC
    word32 length;
#endif

    if (dst == NULL || src == NULL)
        return BAD_FUNC_ARG;

#ifndef WOLFSSL_ECC_CURVE_STATIC
    *dst = src;
#else
    length = (int)XSTRLEN(src) + 1;
    if (length > MAX_ECC_STRING) {
        WOLFSSL_MSG("ECC Param too large for buffer");
        ret = BUFFER_E;
    }
    else {
        XSTRNCPY(*dst, src, MAX_ECC_STRING);
    }
    XFREE(src, key->heap, DYNAMIC_TYPE_ECC_BUFFER);
#endif

    return ret;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */
#endif /* WOLFSSL_CUSTOM_CURVES */

int wc_EccPublicKeyDecode(const byte* input, word32* inOutIdx,
                          ecc_key* key, word32 inSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    ret;
    int    version, length;
    int    curve_id = ECC_CURVE_DEF;
    word32 oidSum, localIdx;
    byte   tag, isPrivFormat = 0;

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0)
        return BAD_FUNC_ARG;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    /* Check if ECC private key is being used and skip private portion */
    if (GetMyVersion(input, inOutIdx, &version, inSz) >= 0) {
        isPrivFormat = 1;

        /* Type private key */
        if (*inOutIdx >= inSz)
            return ASN_PARSE_E;
        tag = input[*inOutIdx];
        *inOutIdx += 1;
        if (tag != 4 && tag != 6 && tag != 7)
            return ASN_PARSE_E;

        /* Skip Private Key */
        if (GetLength(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;
        if (length > ECC_MAXSIZE)
            return BUFFER_E;
        *inOutIdx += length;

        /* Private Curve Header */
        if (*inOutIdx >= inSz)
            return ASN_PARSE_E;
        tag = input[*inOutIdx];
        *inOutIdx += 1;
        if (tag != ECC_PREFIX_0)
            return ASN_ECC_KEY_E;
        if (GetLength(input, inOutIdx, &length, inSz) <= 0)
            return ASN_PARSE_E;
    }
    /* Standard ECC public key */
    else {
        if (GetSequence(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        ret = SkipObjectId(input, inOutIdx, inSz);
        if (ret != 0)
            return ret;
    }

    if (*inOutIdx >= inSz) {
        return BUFFER_E;
    }

    localIdx = *inOutIdx;
    if (GetASNTag(input, &localIdx, &tag, inSz) == 0 &&
            tag == (ASN_SEQUENCE | ASN_CONSTRUCTED)) {
#ifdef WOLFSSL_CUSTOM_CURVES
        ecc_set_type* curve;
        int len;
        char* point = NULL;

        ret = 0;

        curve = (ecc_set_type*)XMALLOC(sizeof(*curve), key->heap,
                                                       DYNAMIC_TYPE_ECC_BUFFER);
        if (curve == NULL)
            ret = MEMORY_E;

        if (ret == 0) {
            static const char customName[] = "Custom";
            XMEMSET(curve, 0, sizeof(*curve));
        #ifndef WOLFSSL_ECC_CURVE_STATIC
            curve->name = customName;
        #else
            XMEMCPY((void*)curve->name, customName, sizeof(customName));
        #endif
            curve->id = ECC_CURVE_CUSTOM;

            if (GetSequence(input, inOutIdx, &length, inSz) < 0)
                ret = ASN_PARSE_E;
        }

        if (ret == 0) {
            GetInteger7Bit(input, inOutIdx, inSz);
            if (GetSequence(input, inOutIdx, &length, inSz) < 0)
                ret = ASN_PARSE_E;
        }
        if (ret == 0) {
            char* p = NULL;
            SkipObjectId(input, inOutIdx, inSz);
            ret = ASNToHexString(input, inOutIdx, &p, inSz,
                                            key->heap, DYNAMIC_TYPE_ECC_BUFFER);
            if (ret == 0)
                ret = EccKeyParamCopy((char**)&curve->prime, p);
        }
        if (ret == 0) {
            curve->size = (int)XSTRLEN(curve->prime) / 2;

            if (GetSequence(input, inOutIdx, &length, inSz) < 0)
                ret = ASN_PARSE_E;
        }
        if (ret == 0) {
            char* af = NULL;
            ret = ASNToHexString(input, inOutIdx, &af, inSz,
                                            key->heap, DYNAMIC_TYPE_ECC_BUFFER);
            if (ret == 0)
                ret = EccKeyParamCopy((char**)&curve->Af, af);
        }
        if (ret == 0) {
            char* bf = NULL;
            ret = ASNToHexString(input, inOutIdx, &bf, inSz,
                                            key->heap, DYNAMIC_TYPE_ECC_BUFFER);
            if (ret == 0)
                ret = EccKeyParamCopy((char**)&curve->Bf, bf);
        }
        if (ret == 0) {
            localIdx = *inOutIdx;
            if (*inOutIdx < inSz && GetASNTag(input, &localIdx, &tag, inSz)
                    == 0 && tag == ASN_BIT_STRING) {
                len = 0;
                ret = GetASNHeader(input, ASN_BIT_STRING, inOutIdx, &len, inSz);
                if (ret > 0)
                    ret = 0; /* reset on success */
                *inOutIdx += len;
            }
        }
        if (ret == 0) {
            ret = ASNToHexString(input, inOutIdx, (char**)&point, inSz,
                                            key->heap, DYNAMIC_TYPE_ECC_BUFFER);

            /* sanity check that point buffer is not smaller than the expected
             * size to hold ( 0 4 || Gx || Gy )
             * where Gx and Gy are each the size of curve->size * 2 */
            if (ret == 0 && (int)XSTRLEN(point) < (curve->size * 4) + 2) {
                XFREE(point, key->heap, DYNAMIC_TYPE_ECC_BUFFER);
                ret = BUFFER_E;
            }
        }
        if (ret == 0) {
        #ifndef WOLFSSL_ECC_CURVE_STATIC
            curve->Gx = (const char*)XMALLOC(curve->size * 2 + 2, key->heap,
                                                       DYNAMIC_TYPE_ECC_BUFFER);
            curve->Gy = (const char*)XMALLOC(curve->size * 2 + 2, key->heap,
                                                       DYNAMIC_TYPE_ECC_BUFFER);
            if (curve->Gx == NULL || curve->Gy == NULL) {
                XFREE(point, key->heap, DYNAMIC_TYPE_ECC_BUFFER);
                ret = MEMORY_E;
            }
        #else
            if (curve->size * 2 + 2 > MAX_ECC_STRING) {
                WOLFSSL_MSG("curve size is too large to fit in buffer");
                ret = BUFFER_E;
            }
        #endif
        }
        if (ret == 0) {
            char* o = NULL;

            XMEMCPY((char*)curve->Gx, point + 2, curve->size * 2);
            XMEMCPY((char*)curve->Gy, point + curve->size * 2 + 2,
                                                               curve->size * 2);
            ((char*)curve->Gx)[curve->size * 2] = '\0';
            ((char*)curve->Gy)[curve->size * 2] = '\0';
            XFREE(point, key->heap, DYNAMIC_TYPE_ECC_BUFFER);
            ret = ASNToHexString(input, inOutIdx, &o, inSz,
                                            key->heap, DYNAMIC_TYPE_ECC_BUFFER);
            if (ret == 0)
                ret = EccKeyParamCopy((char**)&curve->order, o);
        }
        if (ret == 0) {
            curve->cofactor = GetInteger7Bit(input, inOutIdx, inSz);

        #ifndef WOLFSSL_ECC_CURVE_STATIC
            curve->oid = NULL;
        #else
            XMEMSET((void*)curve->oid, 0, sizeof(curve->oid));
        #endif
            curve->oidSz = 0;
            curve->oidSum = 0;

            if (wc_ecc_set_custom_curve(key, curve) < 0) {
                ret = ASN_PARSE_E;
            }
        #ifdef WOLFSSL_CUSTOM_CURVES
            key->deallocSet = 1;
        #endif
            curve = NULL;
        }
        if (curve != NULL)
            wc_ecc_free_curve(curve, key->heap);

        if (ret < 0)
            return ret;
#else
        return ASN_PARSE_E;
#endif /* WOLFSSL_CUSTOM_CURVES */
    }
    else {
        /* ecc params information */
        ret = GetObjectId(input, inOutIdx, &oidSum, oidIgnoreType, inSz);
        if (ret != 0)
            return ret;

        /* get curve id */
        if ((ret = CheckCurve(oidSum)) < 0)
            return ECC_CURVE_OID_E;
        else {
            curve_id = ret;
        }
    }

    if (isPrivFormat) {
        /* Public Curve Header - skip */
        if (*inOutIdx >= inSz)
            return ASN_PARSE_E;
        tag = input[*inOutIdx];
        *inOutIdx += 1;
        if (tag != ECC_PREFIX_1)
            return ASN_ECC_KEY_E;
        if (GetLength(input, inOutIdx, &length, inSz) <= 0)
            return ASN_PARSE_E;
    }

    /* key header */
    ret = CheckBitString(input, inOutIdx, &length, inSz, 1, NULL);
    if (ret != 0)
        return ret;

    /* This is the raw point data compressed or uncompressed. */
    if (wc_ecc_import_x963_ex(input + *inOutIdx, length, key,
                                                            curve_id) != 0) {
        return ASN_ECC_KEY_E;
    }

    *inOutIdx += length;

    return 0;
#else
    /* eccKeyASN is longer than eccPublicKeyASN. */
    DECL_ASNGETDATA(dataASN, eccKeyASN_Length);
    int ret = 0;
    int curve_id = ECC_CURVE_DEF;
    int oidIdx = 3;
#ifdef WOLFSSL_CUSTOM_CURVES
    int specIdx = 4;
#endif
    int pubIdx = 5;

    if ((input == NULL) || (inOutIdx == NULL) || (key == NULL) || (inSz == 0)) {
        ret = BAD_FUNC_ARG;
    }

    ALLOC_ASNGETDATA(dataASN, eccKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Clear dynamic data for ECC public key. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * eccPublicKeyASN_Length);
        /* Set required ECDSA OID and ignore the curve OID type. */
        GetASN_ExpBuffer(&dataASN[2], keyEcdsaOid, sizeof(keyEcdsaOid));
        GetASN_OID(&dataASN[oidIdx], oidIgnoreType);
        /* Decode the public ECC key. */
        ret = GetASN_Items(eccPublicKeyASN, dataASN, eccPublicKeyASN_Length, 1,
                           input, inOutIdx, inSz);
        if (ret != 0) {
            oidIdx = 4;
        #ifdef WOLFSSL_CUSTOM_CURVES
            specIdx = 5;
        #endif
            pubIdx = 7;

            /* Clear dynamic data for ECC private key. */
            XMEMSET(dataASN, 0, sizeof(*dataASN) * eccKeyASN_Length);
            /* Check named curve OID type. */
            GetASN_OID(&dataASN[oidIdx], oidIgnoreType);
            /* Try private key format .*/
            ret = GetASN_Items(eccKeyASN, dataASN, eccKeyASN_Length, 1, input,
                               inOutIdx, inSz);
            if (ret != 0) {
                ret = ASN_PARSE_E;
            }
        }
    }

    if (ret == 0) {
        if (dataASN[oidIdx].tag != 0) {
            /* Named curve - check and get id. */
            curve_id = CheckCurve(dataASN[oidIdx].data.oid.sum);
            if (curve_id < 0) {
                ret = ASN_OBJECT_ID_E;
            }
        }
        else {
        #ifdef WOLFSSL_CUSTOM_CURVES
            /* Parse explicit parameters. */
            ret = EccSpecifiedECDomainDecode(dataASN[specIdx].data.ref.data,
                                         dataASN[specIdx].data.ref.length, key);
        #else
            /* Explicit parameters not supported in build configuration. */
            ret = ASN_PARSE_E;
        #endif
        }
    }
    if (ret == 0) {
        /* Import public point. */
        ret = wc_ecc_import_x963_ex(dataASN[pubIdx].data.ref.data,
                dataASN[pubIdx].data.ref.length, key, curve_id);
        if (ret != 0) {
            ret = ASN_ECC_KEY_E;
        }
    }

    FREE_ASNGETDATA(dataASN, key->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#if defined(HAVE_ECC_KEY_EXPORT) && !defined(NO_ASN_CRYPT)
/* build DER formatted ECC key, include optional public key if requested,
 * return length on success, negative on error */
static int wc_BuildEccKeyDer(ecc_key* key, byte* output, word32 *inLen,
                             int pubIn, int curveIn)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    byte   curve[MAX_ALGO_SZ+2];
    byte   ver[MAX_VERSION_SZ];
    byte   seq[MAX_SEQ_SZ];
    int    ret, totalSz, curveSz, verSz;
    int    privHdrSz  = ASN_ECC_HEADER_SZ;
    int    pubHdrSz   = ASN_ECC_CONTEXT_SZ + ASN_ECC_HEADER_SZ;
#ifdef WOLFSSL_NO_MALLOC
    byte   prv[MAX_ECC_BYTES + ASN_ECC_HEADER_SZ + MAX_SEQ_SZ];
    byte   pub[(MAX_ECC_BYTES * 2) + 1 + ASN_ECC_CONTEXT_SZ +
                              ASN_ECC_HEADER_SZ + MAX_SEQ_SZ];
#else
    byte   *prv = NULL, *pub = NULL;
#endif

    word32 idx = 0, prvidx = 0, pubidx = 0, curveidx = 0;
    word32 seqSz, privSz, pubSz = ECC_BUFSIZE;

    if (key == NULL || (output == NULL && inLen == NULL))
        return BAD_FUNC_ARG;

    if (curveIn) {
        /* curve */
        curve[curveidx++] = ECC_PREFIX_0;
        curveidx++ /* to put the size after computation */;
        curveSz = SetCurve(key, curve+curveidx);
        if (curveSz < 0)
            return curveSz;
        /* set computed size */
        curve[1] = (byte)curveSz;
        curveidx += curveSz;
    }

    /* private */
    privSz = key->dp->size;

#ifdef WOLFSSL_QNX_CAAM
    /* check if is a black key, and add MAC size if so */
    if (key->blackKey > 0) {
        privSz = privSz + WC_CAAM_MAC_SZ;
    }
#endif

#ifndef WOLFSSL_NO_MALLOC
    prv = (byte*)XMALLOC(privSz + privHdrSz + MAX_SEQ_SZ,
                         key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (prv == NULL) {
        return MEMORY_E;
    }
#else
    if (sizeof(prv) < privSz + privHdrSz + MAX_SEQ_SZ) {
        return BUFFER_E;
    }
#endif
    if (privSz < ASN_LONG_LENGTH) {
        prvidx += SetOctetString8Bit(privSz, &prv[prvidx]);
    }
    else {
        prvidx += SetOctetString(privSz, &prv[prvidx]);
    }
    ret = wc_ecc_export_private_only(key, prv + prvidx, &privSz);
    if (ret < 0) {
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;
    }
    prvidx += privSz;

    /* pubIn */
    if (pubIn) {
        ret = wc_ecc_export_x963(key, NULL, &pubSz);
        if (ret != LENGTH_ONLY_E) {
        #ifndef WOLFSSL_NO_MALLOC
            XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            return ret;
        }

    #ifndef WOLFSSL_NO_MALLOC
        pub = (byte*)XMALLOC(pubSz + pubHdrSz + MAX_SEQ_SZ,
                             key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (pub == NULL) {
            XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            return MEMORY_E;
        }
    #else
        if (sizeof(pub) < pubSz + pubHdrSz + MAX_SEQ_SZ) {
            return BUFFER_E;
        }
    #endif

        pub[pubidx++] = ECC_PREFIX_1;
        if (pubSz > 128) /* leading zero + extra size byte */
            pubidx += SetLength(pubSz + ASN_ECC_CONTEXT_SZ + 2, pub+pubidx);
        else /* leading zero */
            pubidx += SetLength(pubSz + ASN_ECC_CONTEXT_SZ + 1, pub+pubidx);

        /* SetBitString adds leading zero */
        pubidx += SetBitString(pubSz, 0, pub + pubidx);
        ret = wc_ecc_export_x963(key, pub + pubidx, &pubSz);
        if (ret != 0) {
        #ifndef WOLFSSL_NO_MALLOC
            XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(pub, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            return ret;
        }
        pubidx += pubSz;
    }

    /* make headers */
    verSz = SetMyVersion(1, ver, FALSE);
    seqSz = SetSequence(verSz + prvidx + pubidx + curveidx, seq);

    totalSz = prvidx + pubidx + curveidx + verSz + seqSz;
    if (output == NULL) {
        *inLen = totalSz;
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (pub) {
            XFREE(pub, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        }
    #endif
        return LENGTH_ONLY_E;
    }
    if (inLen != NULL && totalSz > (int)*inLen) {
        #ifndef WOLFSSL_NO_MALLOC
        XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (pubIn) {
            XFREE(pub, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
        }
        #endif
        return BAD_FUNC_ARG;
    }

    /* write out */
    /* seq */
    XMEMCPY(output + idx, seq, seqSz);
    idx = seqSz;

    /* ver */
    XMEMCPY(output + idx, ver, verSz);
    idx += verSz;

    /* private */
    XMEMCPY(output + idx, prv, prvidx);
    idx += prvidx;
#ifndef WOLFSSL_NO_MALLOC
    XFREE(prv, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    /* curve */
    XMEMCPY(output + idx, curve, curveidx);
    idx += curveidx;

    /* pubIn */
    if (pubIn) {
        XMEMCPY(output + idx, pub, pubidx);
        /* idx += pubidx;  not used after write, if more data remove comment */
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(pub, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
    }

    return totalSz;
#else
    DECL_ASNSETDATA(dataASN, eccKeyASN_Length);
    word32 privSz, pubSz;
    int sz = 0;
    int ret = 0;

    /* Check validity of parameters. */
    if ((key == NULL) || ((output == NULL) && (inLen == NULL))) {
        ret = BAD_FUNC_ARG;
    }

    /* Check key has parameters when encoding curve. */
    if ((ret == 0) && curveIn && (key->dp == NULL)) {
        ret = BAD_FUNC_ARG;
    }

    CALLOC_ASNSETDATA(dataASN, eccKeyASN_Length, ret, key->heap);

    if (ret == 0) {
        /* Private key size is the curve size. */
        privSz = key->dp->size;
        if (pubIn) {
            /* Get the length of the public key. */
            ret = wc_ecc_export_x963(key, NULL, &pubSz);
            if (ret == LENGTH_ONLY_E)
                ret = 0;
        }
    }
    if (ret == 0) {
        /* Version: 1 */
        SetASN_Int8Bit(&dataASN[1], 1);
        /* Leave space for private key. */
        SetASN_Buffer(&dataASN[2], NULL, privSz);
        if (curveIn) {
            /* Curve OID */
            SetASN_Buffer(&dataASN[4], key->dp->oid, key->dp->oidSz);
        }
        else {
             dataASN[3].noOut = 1;
             dataASN[4].noOut = 1;
        }
        /* TODO: add support for SpecifiedECDomain curve. */
        dataASN[5].noOut = 1;
        if (pubIn) {
            /* Leave space for public key. */
            SetASN_Buffer(&dataASN[7], NULL, pubSz);
        }
        else {
            /* Don't write out public key. */
            dataASN[6].noOut = dataASN[7].noOut = 1;
        }
        /* Calculate size of the private key encoding. */
        ret = SizeASN_Items(eccKeyASN, dataASN, eccKeyASN_Length, &sz);
    }
    /* Return the size if no buffer. */
    if ((ret == 0) && (output == NULL)) {
        *inLen = sz;
        ret = LENGTH_ONLY_E;
    }
    /* Check the buffer is big enough. */
    if ((ret == 0) && (inLen != NULL) && (sz > (int)*inLen)) {
        ret = BAD_FUNC_ARG;
    }
    if ((ret == 0) && (output != NULL)) {
        /* Encode the private key. */
        SetASN_Items(eccKeyASN, dataASN, eccKeyASN_Length, output);

        /* Export the private value into the buffer. */
        ret = wc_ecc_export_private_only(key,
                (byte*)dataASN[2].data.buffer.data, &privSz);
        if ((ret == 0) && pubIn) {
            /* Export the public point into the buffer. */
            ret = wc_ecc_export_x963(key, (byte*)dataASN[7].data.buffer.data,
                    &pubSz);
        }
    }
    if (ret == 0) {
        /* Return the encoding size. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, key->heap);
    return ret;
#endif
}

/* Write a Private ecc key, including public to DER format,
 * length on success else < 0 */
int wc_EccKeyToDer(ecc_key* key, byte* output, word32 inLen)
{
    return wc_BuildEccKeyDer(key, output, &inLen, 1, 1);
}

/* Write only private ecc key to DER format,
 * length on success else < 0 */
int wc_EccKeyDerSize(ecc_key* key, int pub)
{
    word32 sz = 0;
    int ret;

    ret = wc_BuildEccKeyDer(key, NULL, &sz, pub, 1);

    if (ret != LENGTH_ONLY_E) {
        return ret;
    }
    return sz;
 }

/* Write only private ecc key to DER format,
 * length on success else < 0 */
int wc_EccPrivateKeyToDer(ecc_key* key, byte* output, word32 inLen)
{
    return wc_BuildEccKeyDer(key, output, &inLen, 0, 1);
}



#ifdef HAVE_PKCS8

/* Write only private ecc key or both private and public parts to unencrypted
 * PKCS#8 format.
 *
 * If output is NULL, places required PKCS#8 buffer size in outLen and
 * returns LENGTH_ONLY_E.
 *
 * return length on success else < 0 */
static int eccToPKCS8(ecc_key* key, byte* output, word32* outLen,
        int includePublic)
{
    int ret, tmpDerSz;
    int algoID = 0;
    word32 oidSz = 0;
    word32 pkcs8Sz = 0;
    const byte* curveOID = NULL;
#ifdef WOLFSSL_NO_MALLOC
    byte  tmpDer[ECC_BUFSIZE];
#else
    byte* tmpDer = NULL;
#endif
    word32 sz = ECC_BUFSIZE;

    if (key == NULL || key->dp == NULL || outLen == NULL)
        return BAD_FUNC_ARG;

    /* set algoID, get curve OID */
    algoID = ECDSAk;
    ret = wc_ecc_get_oid(key->dp->oidSum, &curveOID, &oidSz);
    if (ret < 0)
        return ret;

#ifndef WOLFSSL_NO_MALLOC
    /* temp buffer for plain DER key */
    tmpDer = (byte*)XMALLOC(ECC_BUFSIZE, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (tmpDer == NULL)
        return MEMORY_E;
#endif
    XMEMSET(tmpDer, 0, ECC_BUFSIZE);

    ret = wc_BuildEccKeyDer(key, tmpDer, &sz, includePublic, 0);
    if (ret < 0) {
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(tmpDer, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;
    }
    tmpDerSz = ret;

    /* get pkcs8 expected output size */
    ret = wc_CreatePKCS8Key(NULL, &pkcs8Sz, tmpDer, tmpDerSz, algoID,
                            curveOID, oidSz);
    if (ret != LENGTH_ONLY_E) {
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(tmpDer, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;
    }

    if (output == NULL) {
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(tmpDer, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        *outLen = pkcs8Sz;
        return LENGTH_ONLY_E;

    }
    else if (*outLen < pkcs8Sz) {
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(tmpDer, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        WOLFSSL_MSG("Input buffer too small for ECC PKCS#8 key");
        return BUFFER_E;
    }

    ret = wc_CreatePKCS8Key(output, &pkcs8Sz, tmpDer, tmpDerSz,
                            algoID, curveOID, oidSz);
    if (ret < 0) {
    #ifndef WOLFSSL_NO_MALLOC
        XFREE(tmpDer, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;
    }

#ifndef WOLFSSL_NO_MALLOC
    XFREE(tmpDer, key->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    *outLen = ret;
    return ret;
}

/* Write only private ecc key to unencrypted PKCS#8 format.
 *
 * return length on success else < 0 */
int wc_EccPrivateKeyToPKCS8(ecc_key* key, byte* output, word32* outLen)
{
    return eccToPKCS8(key, output, outLen, 0);
}

/* Write both private and public ecc keys to unencrypted PKCS#8 format.
 *
 * return length on success else < 0 */
int wc_EccKeyToPKCS8(ecc_key* key, byte* output,
                     word32* outLen)
{
    return eccToPKCS8(key, output, outLen, 1);
}
#endif /* HAVE_PKCS8 */
#endif /* HAVE_ECC_KEY_EXPORT && !NO_ASN_CRYPT */
#endif /* HAVE_ECC */

#ifdef WC_ENABLE_ASYM_KEY_IMPORT
#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for Ed25519 and Ed448 private key.
 * RFC 8410, 7 - Private Key Format (but public value is EXPLICIT OCTET_STRING)
 */
static const ASNItem edKeyASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* Version */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* privateKeyAlgorithm */
/*  2 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 1 },
                /* privateKey */
/*  4 */        { 1, ASN_OCTET_STRING, 0, 1, 0 },
                    /* CurvePrivateKey */
/*  5 */            { 2, ASN_OCTET_STRING, 0, 0, 0 },
                /* attributes */
/*  6 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
                /* publicKey */
/*  7 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 1, 1 },
                    /* Public value */
/*  8 */            { 2, ASN_OCTET_STRING, 0, 0, 0 }
};

/* Number of items in ASN.1 template for Ed25519 and Ed448 private key. */
#define edKeyASN_Length (sizeof(edKeyASN) / sizeof(ASNItem))
#endif

static int DecodeAsymKey(const byte* input, word32* inOutIdx, word32 inSz,
    byte* privKey, word32* privKeyLen,
    byte* pubKey, word32* pubKeyLen, int keyType)
{
    int ret = 0;
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 oid;
    int version, length, endKeyIdx, privSz, pubSz;
    const byte* priv;
    const byte* pub;
#else
    DECL_ASNGETDATA(dataASN, edKeyASN_Length);
#endif    

    if (input == NULL || inOutIdx == NULL || inSz == 0 ||
        privKey == NULL || privKeyLen == NULL) {
        return BAD_FUNC_ARG;
    }

#ifndef WOLFSSL_ASN_TEMPLATE
    if (GetSequence(input, inOutIdx, &length, inSz) >= 0) {
        endKeyIdx = *inOutIdx + length;

        if (GetMyVersion(input, inOutIdx, &version, inSz) < 0)
            return ASN_PARSE_E;
        if (version != 0) {
            WOLFSSL_MSG("Unrecognized version of ED25519 private key");
            return ASN_PARSE_E;
        }

        if (GetAlgoId(input, inOutIdx, &oid, oidKeyType, inSz) < 0)
            return ASN_PARSE_E;
        if (oid != (word32)keyType)
            return ASN_PARSE_E;

        if (GetOctetString(input, inOutIdx, &length, inSz) < 0)
            return ASN_PARSE_E;

        if (GetOctetString(input, inOutIdx, &privSz, inSz) < 0)
            return ASN_PARSE_E;

        priv = input + *inOutIdx;
        *inOutIdx += privSz;
    }
    else {
        if (GetOctetString(input, inOutIdx, &privSz, inSz) < 0)
            return ASN_PARSE_E;

        priv = input + *inOutIdx;
        *inOutIdx += privSz;
        endKeyIdx = *inOutIdx;
    }

    if (endKeyIdx == (int)*inOutIdx) {
        *privKeyLen = privSz;
        XMEMCPY(privKey, priv, *privKeyLen);
        if (pubKeyLen != NULL)
            *pubKeyLen = 0;
    }
    else {
        if (GetASNHeader(input, ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 1,
                         inOutIdx, &length, inSz) < 0) {
            return ASN_PARSE_E;
        }
        if (GetOctetString(input, inOutIdx, &pubSz, inSz) < 0) {
            return ASN_PARSE_E;
        }
        pub = input + *inOutIdx;
        *inOutIdx += pubSz;

        *privKeyLen = privSz;
        XMEMCPY(privKey, priv, *privKeyLen);
        if (pubKeyLen != NULL)
            *pubKeyLen = pubSz;
        if (pubKey != NULL && pubKeyLen != NULL)
            XMEMCPY(pubKey, pub, *pubKeyLen);
    }
    if (ret == 0 && endKeyIdx != (int)*inOutIdx)
        return ASN_PARSE_E;
#else
    CALLOC_ASNGETDATA(dataASN, edKeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Require OID. */
        word32 oidSz;
        const byte* oid = OidFromId(keyType, oidKeyType, &oidSz);
        GetASN_ExpBuffer(&dataASN[3], oid, oidSz);
        /* Parse full private key. */
        ret = GetASN_Items(edKeyASN, dataASN, edKeyASN_Length, 1, input,
                inOutIdx, inSz);
        if (ret != 0) {
            /* Parse just the OCTET_STRING. */
            ret = GetASN_Items(&edKeyASN[5], &dataASN[5], 1, 0, input, inOutIdx,
                    inSz);
            if (ret != 0) {
                ret = ASN_PARSE_E;
            }
        }
    }
    /* Check the private value length is correct. */
    if ((ret == 0) && dataASN[5].data.ref.length > *privKeyLen) {
        ret = ASN_PARSE_E;
    }
    if ((ret == 0) && dataASN[7].tag == 0) {
        *privKeyLen = dataASN[5].data.ref.length;
        XMEMCPY(privKey, dataASN[5].data.ref.data, *privKeyLen);
        if (pubKeyLen != NULL)
            *pubKeyLen = 0;
    }
    else if ((ret == 0) &&
             (dataASN[8].data.ref.length > *pubKeyLen)) {
        ret = ASN_PARSE_E;
    }
    else if (ret == 0) {
        /* Import private and public value. */
        *privKeyLen = dataASN[5].data.ref.length;
        XMEMCPY(privKey, dataASN[5].data.ref.data, *privKeyLen);
        if (pubKeyLen != NULL)
            *pubKeyLen = dataASN[8].data.ref.length;
        if (pubKey != NULL && pubKeyLen != NULL)
            XMEMCPY(pubKey, dataASN[8].data.ref.data, *pubKeyLen);
    }

    FREE_ASNGETDATA(dataASN, NULL);
#endif /* WOLFSSL_ASN_TEMPLATE */
    return ret;
}

static int DecodeAsymKeyPublic(const byte* input, word32* inOutIdx, word32 inSz,
    byte* pubKey, word32* pubKeyLen, int keyType)
{
    int ret = 0;
#ifndef WOLFSSL_ASN_TEMPLATE
    int length;
    word32 oid;
#else
    DECL_ASNGETDATA(dataASN, edPubKeyASN_Length);
#endif

    if (input == NULL || inSz == 0 || inOutIdx == NULL || 
        pubKey == NULL || pubKeyLen == NULL) {
        return BAD_FUNC_ARG;
    }

#ifndef WOLFSSL_ASN_TEMPLATE
    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    if (GetSequence(input, inOutIdx, &length, inSz) < 0)
        return ASN_PARSE_E;

    if (GetObjectId(input, inOutIdx, &oid, oidKeyType, inSz) < 0)
        return ASN_PARSE_E;
    if (oid != (word32)keyType)
        return ASN_PARSE_E;

    /* key header */
    ret = CheckBitString(input, inOutIdx, NULL, inSz, 1, NULL);
    if (ret != 0)
        return ret;

    /* check that the value found is not too large for pubKey buffer */
    if (inSz - *inOutIdx > *pubKeyLen)
        return ASN_PARSE_E;

    /* This is the raw point data compressed or uncompressed. */
    *pubKeyLen = inSz - *inOutIdx;
    XMEMCPY(pubKey, input + *inOutIdx, *pubKeyLen);
#else
    CALLOC_ASNGETDATA(dataASN, edPubKeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Require OID. */
        word32 oidSz;
        const byte* oid = OidFromId(keyType, oidKeyType, &oidSz);
        GetASN_ExpBuffer(&dataASN[2], oid, oidSz);
        /* Decode Ed25519 private key. */
        ret = GetASN_Items(edPubKeyASN, dataASN, edPubKeyASN_Length, 1, input,
                inOutIdx, inSz);
        if (ret != 0) {
            ret = ASN_PARSE_E;
        }
    }
    /* Check the public value length is correct. */
    if ((ret == 0) && (dataASN[3].data.ref.length > *pubKeyLen)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        *pubKeyLen = dataASN[3].data.ref.length;
        XMEMCPY(pubKey, dataASN[3].data.ref.data, *pubKeyLen);
    }

    FREE_ASNGETDATA(dataASN, NULL);
#endif /* WOLFSSL_ASN_TEMPLATE */
    return ret;
}
#endif /* WC_ENABLE_ASYM_KEY_IMPORT */

#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_IMPORT)
int wc_Ed25519PrivateKeyDecode(const byte* input, word32* inOutIdx,
                               ed25519_key* key, word32 inSz)
{
    int ret;
    byte privKey[ED25519_KEY_SIZE], pubKey[ED25519_PUB_KEY_SIZE];
    word32 privKeyLen = (word32)sizeof(privKey);
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKey(input, inOutIdx, inSz, privKey, &privKeyLen, 
        pubKey, &pubKeyLen, ED25519k);
    if (ret == 0) {
        if (pubKeyLen == 0) {
            ret = wc_ed25519_import_private_only(privKey, privKeyLen, key);
        }
        else {
            ret = wc_ed25519_import_private_key(privKey, privKeyLen,
                pubKey, pubKeyLen, key);
        }
    }
    return ret;
}

int wc_Ed25519PublicKeyDecode(const byte* input, word32* inOutIdx,
                              ed25519_key* key, word32 inSz)
{
    int ret;
    byte pubKey[ED25519_PUB_KEY_SIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKeyPublic(input, inOutIdx, inSz,
        pubKey, &pubKeyLen, ED25519k);
    if (ret == 0) {
        ret = wc_ed25519_import_public(pubKey, pubKeyLen, key);
    }
    return ret;
}
#endif /* HAVE_ED25519 && HAVE_ED25519_KEY_IMPORT */

#if defined(HAVE_CURVE25519) && defined(HAVE_CURVE25519_KEY_IMPORT)
int wc_Curve25519PrivateKeyDecode(const byte* input, word32* inOutIdx,
                               curve25519_key* key, word32 inSz)
{
    int ret;
    byte privKey[CURVE25519_KEYSIZE];
    word32 privKeyLen = CURVE25519_KEYSIZE;

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKey(input, inOutIdx, inSz, privKey, &privKeyLen, 
        NULL, NULL, X25519k);
    if (ret == 0) {
        ret = wc_curve25519_import_private(privKey, privKeyLen, key);
    }
    return ret;
}

int wc_Curve25519PublicKeyDecode(const byte* input, word32* inOutIdx,
                              curve25519_key* key, word32 inSz)
{
    int ret;
    byte pubKey[CURVE25519_KEYSIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKeyPublic(input, inOutIdx, inSz,
        pubKey, &pubKeyLen, X25519k);
    if (ret == 0) {
        ret = wc_curve25519_import_public(pubKey, pubKeyLen, key);
    }
    return ret;
}
#endif /* HAVE_CURVE25519 && HAVE_ED25519_KEY_IMPORT */


#ifdef WC_ENABLE_ASYM_KEY_EXPORT

/* Build ASN.1 formatted key based on RFC 5958 (Asymmetric Key Packages)
 *
 * Pass NULL for output to get the size of the encoding.
 *
 * @param [in]  privKey      private key buffer
 * @param [in]  privKeyLen   private ket buffer length
 * @param [in]  pubKey       public key buffer (optional)
 * @param [in]  pubKeyLen    public ket buffer length
 * @param [out] output       Buffer to put encoded data in (optional)
 * @param [in]  outLen       Size of buffer in bytes
 * @param [in]  keyType      is "enum Key_Sum" like ED25519k
 * @return  Size of encoded data in bytes on success
 * @return  BAD_FUNC_ARG when key is NULL.
 * @return  MEMORY_E when dynamic memory allocation failed.
 * @return  LENGTH_ONLY_E return length only.
 */
static int SetAsymKeyDer(const byte* privKey, word32 privKeyLen,
    const byte* pubKey, word32 pubKeyLen,
    byte* output, word32 outLen, int keyType)
{
    int ret = 0;
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0, seqSz, verSz, algoSz, privSz, pubSz = 0, sz;
#else
    DECL_ASNSETDATA(dataASN, edKeyASN_Length);
    int sz;
#endif

    /* Validate parameters. */
    if (privKey == NULL || outLen == 0) {
        return BAD_FUNC_ARG;
    }

#ifndef WOLFSSL_ASN_TEMPLATE
    /* calculate size */
    if (pubKey) {
        pubSz = 2 + 2 + pubKeyLen;
    }
    privSz = 2 + 2 + privKeyLen;
    algoSz = SetAlgoID(keyType, NULL, oidKeyType, 0);
    verSz  = 3; /* version is 3 bytes (enum + id + version(byte)) */
    seqSz  = SetSequence(verSz + algoSz + privSz + pubSz, NULL);
    sz = seqSz + verSz + algoSz + privSz + pubSz;

    /* checkout output size */
    if (ret == 0 && output != NULL && sz > outLen) {
        ret = BAD_FUNC_ARG;
    }

    if (ret == 0 && output != NULL) {
        /* write out */
        /* seq */
        seqSz  = SetSequence(verSz + algoSz + privSz + pubSz, output);
        idx = seqSz;
        /* ver */
        SetMyVersion(0, output + idx, FALSE);
        idx += verSz;
        /* algo */
        algoSz = SetAlgoID(keyType, output + idx, oidKeyType, 0);
        idx += algoSz;
        /* privKey */
        idx += SetOctetString(2 + privKeyLen, output + idx);
        idx += SetOctetString(privKeyLen, output + idx);
        XMEMCPY(output + idx, privKey, privKeyLen);
        idx += privKeyLen;
        /* pubKey */
        if (pubKey) {
            idx += SetExplicit(1, 2 + pubKeyLen, output + idx);
            idx += SetOctetString(pubKeyLen, output + idx);
            XMEMCPY(output + idx, pubKey, pubKeyLen);
            idx += pubKeyLen;
        }

        ret = idx;
    }
#else

    CALLOC_ASNSETDATA(dataASN, edKeyASN_Length, ret, NULL);

    if (ret == 0) {
        /* Set version = 0 */
        SetASN_Int8Bit(&dataASN[1], 0);
        /* Set OID. */
        SetASN_OID(&dataASN[3], keyType, oidKeyType);
        /* Leave space for private key. */
        SetASN_Buffer(&dataASN[5], NULL, privKeyLen);
        /* Don't write out attributes. */
        dataASN[6].noOut = 1;
        if (pubKey) {
            /* Leave space for public key. */
            SetASN_Buffer(&dataASN[8], NULL, pubKeyLen);
        }
        else {
            /* Don't put out public part. */
            dataASN[7].noOut = dataASN[8].noOut = 1;
        }

        /* Calculate the size of encoding. */
        ret = SizeASN_Items(edKeyASN, dataASN, edKeyASN_Length, &sz);
    }

    /* Check buffer is big enough. */
    if ((ret == 0) && (output != NULL) && (sz > (int)outLen)) {
        ret = BAD_FUNC_ARG;
    }
    if (ret == 0 && output != NULL) {
        /* Encode private key. */
        SetASN_Items(edKeyASN, dataASN, edKeyASN_Length, output);

        /* Put private value into space provided. */
        XMEMCPY((byte*)dataASN[5].data.buffer.data, privKey, privKeyLen);

        if (pubKey != NULL) {
            /* Put public value into space provided. */
            XMEMCPY((byte*)dataASN[8].data.buffer.data, pubKey, pubKeyLen);
        }
    }

    if (ret == 0) {
        /* Return size of encoding. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, NULL);
#endif
    return ret;
}
#endif /* WC_ENABLE_ASYM_KEY_EXPORT */

#if defined(HAVE_ED25519) && defined(HAVE_ED25519_KEY_EXPORT)
/* Write a Private ED25519 key, including public to DER format,
 * length on success else < 0 */
int wc_Ed25519KeyToDer(ed25519_key* key, byte* output, word32 inLen)
{
    if (key == NULL) {
        return BAD_FUNC_ARG;
    }
    return SetAsymKeyDer(key->k, ED25519_KEY_SIZE,
        key->p, ED25519_PUB_KEY_SIZE, output, inLen, ED25519k);
}

/* Write only private ED25519 key to DER format,
 * length on success else < 0 */
int wc_Ed25519PrivateKeyToDer(ed25519_key* key, byte* output, word32 inLen)
{
    if (key == NULL) {
        return BAD_FUNC_ARG;
    }
    return SetAsymKeyDer(key->k, ED25519_KEY_SIZE,
        NULL, 0, output, inLen, ED25519k);
}
#endif /* HAVE_ED25519 && HAVE_ED25519_KEY_EXPORT */

#if defined(HAVE_CURVE25519) && defined(HAVE_CURVE25519_KEY_EXPORT)
/* Write only private Curve25519 key to DER format,
 * length on success else < 0 */
int wc_Curve25519PrivateKeyToDer(curve25519_key* key, byte* output, word32 inLen)
{
    int    ret;
    byte   privKey[CURVE25519_KEYSIZE];
    word32 privKeyLen = CURVE25519_KEYSIZE;

    if (key == NULL) {
        return BAD_FUNC_ARG;
    }

    ret = wc_curve25519_export_private_raw(key, privKey, &privKeyLen);
    if (ret == 0) {
        ret = SetAsymKeyDer(privKey, privKeyLen, NULL, 0, output, inLen,
            X25519k);
    }
    return ret;
}

/* Write a public Curve25519 key to DER format,
 * length on success else < 0 */
int wc_Curve25519PublicKeyToDer(curve25519_key* key, byte* output, word32 inLen,
                             int withAlg)
{
    int    ret;
    byte   pubKey[CURVE25519_KEYSIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (key == NULL || output == NULL) {
        return BAD_FUNC_ARG;
    }

    ret = wc_curve25519_export_public(key, pubKey, &pubKeyLen);
    if (ret == 0) {
        ret = SetAsymKeyDerPublic(pubKey, pubKeyLen, output, inLen,
            X25519k, withAlg);
    }
    return ret;
}
#endif /* HAVE_CURVE25519 && HAVE_CURVE25519_KEY_EXPORT */

#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_IMPORT)
int wc_Ed448PrivateKeyDecode(const byte* input, word32* inOutIdx,
                               ed448_key* key, word32 inSz)
{
    int ret;
    byte privKey[ED448_KEY_SIZE], pubKey[ED448_PUB_KEY_SIZE];
    word32 privKeyLen = (word32)sizeof(privKey);
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKey(input, inOutIdx, inSz, privKey, &privKeyLen, 
        pubKey, &pubKeyLen, ED448k);
    if (ret == 0) {
        if (pubKeyLen == 0) {
            ret = wc_ed448_import_private_only(privKey, privKeyLen, key);
        }
        else {
            ret = wc_ed448_import_private_key(privKey, privKeyLen,
                pubKey, pubKeyLen, key);
        }
    }
    return ret;
}

int wc_Ed448PublicKeyDecode(const byte* input, word32* inOutIdx,
                              ed448_key* key, word32 inSz)
{
    int ret;
    byte pubKey[ED448_PUB_KEY_SIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKeyPublic(input, inOutIdx, inSz,
        pubKey, &pubKeyLen, ED448k);
    if (ret == 0) {
        ret = wc_ed448_import_public(pubKey, pubKeyLen, key);
    }
    return ret;
}
#endif /* HAVE_ED448 && HAVE_ED448_KEY_IMPORT */

#if defined(HAVE_CURVE448) && defined(HAVE_CURVE448_KEY_IMPORT)
int wc_Curve448PrivateKeyDecode(const byte* input, word32* inOutIdx,
                               curve448_key* key, word32 inSz)
{
    int ret;
    byte privKey[CURVE448_KEY_SIZE];
    word32 privKeyLen = CURVE448_KEY_SIZE;

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKey(input, inOutIdx, inSz, privKey, &privKeyLen, 
        NULL, NULL, X448k);
    if (ret == 0) {
        ret = wc_curve448_import_private(privKey, privKeyLen, key);
    }
    return ret;
}

int wc_Curve448PublicKeyDecode(const byte* input, word32* inOutIdx,
                              curve448_key* key, word32 inSz)
{
    int ret;
    byte pubKey[CURVE448_PUB_KEY_SIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (input == NULL || inOutIdx == NULL || key == NULL || inSz == 0) {
        return BAD_FUNC_ARG;
    }

    ret = DecodeAsymKeyPublic(input, inOutIdx, inSz,
        pubKey, &pubKeyLen, X448k);
    if (ret == 0) {
        ret = wc_curve448_import_public(pubKey, pubKeyLen, key);
    }
    return ret;
}
#endif /* HAVE_CURVE448 && HAVE_ED448_KEY_IMPORT */

#if defined(HAVE_ED448) && defined(HAVE_ED448_KEY_EXPORT)
/* Write a Private ecc key, including public to DER format,
 * length on success else < 0 */
int wc_Ed448KeyToDer(ed448_key* key, byte* output, word32 inLen)
{
    if (key == NULL) {
        return BAD_FUNC_ARG;
    }
    return SetAsymKeyDer(key->k, ED448_KEY_SIZE,
        key->p, ED448_KEY_SIZE, output, inLen, ED448k);
}

/* Write only private ecc key to DER format,
 * length on success else < 0 */
int wc_Ed448PrivateKeyToDer(ed448_key* key, byte* output, word32 inLen)
{
    if (key == NULL) {
        return BAD_FUNC_ARG;
    }
    return SetAsymKeyDer(key->k, ED448_KEY_SIZE,
        NULL, 0, output, inLen, ED448k);
}

#endif /* HAVE_ED448 && HAVE_ED448_KEY_EXPORT */

#if defined(HAVE_CURVE448) && defined(HAVE_CURVE448_KEY_EXPORT)
/* Write private Curve448 key to DER format,
 * length on success else < 0 */
int wc_Curve448PrivateKeyToDer(curve448_key* key, byte* output, word32 inLen)
{
    int    ret;
    byte   privKey[CURVE448_KEY_SIZE];
    word32 privKeyLen = CURVE448_KEY_SIZE;

    if (key == NULL) {
        return BAD_FUNC_ARG;
    }

    ret = wc_curve448_export_private_raw(key, privKey, &privKeyLen);
    if (ret == 0) {
        ret = SetAsymKeyDer(privKey, privKeyLen, NULL, 0, output, inLen,
            X448k);
    }
    return ret;
}
/* Write a public Curve448 key to DER format,
 * length on success else < 0 */
int wc_Curve448PublicKeyToDer(curve448_key* key, byte* output, word32 inLen,
                             int withAlg)
{
    int    ret;
    byte   pubKey[CURVE448_PUB_KEY_SIZE];
    word32 pubKeyLen = (word32)sizeof(pubKey);

    if (key == NULL || output == NULL) {
        return BAD_FUNC_ARG;
    }

    ret = wc_curve448_export_public(key, pubKey, &pubKeyLen);
    if (ret == 0) {
        ret = SetAsymKeyDerPublic(pubKey, pubKeyLen, output, inLen,
            X448k, withAlg);
    }
    return ret;
}
#endif /* HAVE_CURVE448 && HAVE_CURVE448_KEY_EXPORT */


#if defined(HAVE_OCSP) || defined(HAVE_CRL)

/* Get raw Date only, no processing, 0 on success */
static int GetBasicDate(const byte* source, word32* idx, byte* date,
                        byte* format, int maxIdx)
{
    int    ret, length;
    const byte *datePtr = NULL;

    WOLFSSL_ENTER("GetBasicDate");

    ret = GetDateInfo(source, idx, &datePtr, format, &length, maxIdx);
    if (ret < 0)
        return ret;

    XMEMCPY(date, datePtr, length);

    return 0;
}

#endif /* HAVE_OCSP || HAVE_CRL */


#ifdef HAVE_OCSP

#ifndef WOLFSSL_ASN_TEMPLATE
static int GetEnumerated(const byte* input, word32* inOutIdx, int *value,
        int sz)
{
    word32 idx = *inOutIdx;
    word32 len;
    byte   tag;

    WOLFSSL_ENTER("GetEnumerated");

    *value = 0;

    if (GetASNTag(input, &idx, &tag, sz) < 0)
        return ASN_PARSE_E;

    if (tag != ASN_ENUMERATED)
        return ASN_PARSE_E;

    if ((int)idx >= sz)
        return BUFFER_E;

    len = input[idx++];
    if (len > 4 || (int)(len + idx) > sz)
        return ASN_PARSE_E;

    while (len--) {
        *value  = *value << 8 | input[idx++];
    }

    *inOutIdx = idx;

    return *value;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for OCSP single response.
 * RFC 6960, 4.2.1 - ASN.1 Specification of the OCSP Response
 */
static const ASNItem singleResponseASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* certId */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* hashAlgorithm */
/*  2 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
/*  3 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
/*  4 */                { 3, ASN_TAG_NULL, 0, 0, 1 },
                    /* issuerNameHash */
/*  5 */            { 2, ASN_OCTET_STRING, 0, 0, 0 },
                    /* issuerKeyHash */
/*  6 */            { 2, ASN_OCTET_STRING, 0, 0, 0 },
                    /* serialNumber */
/*  7 */            { 2, ASN_INTEGER, 0, 0, 0 },
                /* certStatus - CHOICE */
                /* good              [0] IMPLICIT NULL */
/*  8 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 0, 0, 2 },
                /* revoked           [1] IMPLICIT RevokedInfo */
/*  9 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 1, 2 },
                    /* revocationTime */
/* 10 */            { 2, ASN_GENERALIZED_TIME, 0, 0, 0 },
                    /* revocationReason  [0] EXPLICIT CRLReason OPTIONAL */
/* 11 */            { 2, ASN_CONTEXT_SPECIFIC | 0, 0, 1, 1 },
                        /* crlReason */
/* 12 */                { 3, ASN_ENUMERATED, 0, 0, 0 },
                /* unknown           [2] IMPLICIT UnknownInfo ::= NULL */
/* 13 */        { 1, ASN_CONTEXT_SPECIFIC | 2, 0, 0, 2 },

                /* thisUpdate */
/* 14 */        { 1, ASN_GENERALIZED_TIME, 0, 0, 0 },
                /* nextUpdate */
/* 15 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
/* 16 */            { 2, ASN_GENERALIZED_TIME, 0, 0, 0 },
                /* singleExtensions */
/* 17 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 1 },
};

/* Number of items in ASN.1 template for OCSP single response. */
#define singleResponseASN_Length (sizeof(singleResponseASN) / sizeof(ASNItem))
#endif

static int DecodeSingleResponse(byte* source, word32* ioIndex, word32 size,
                                int wrapperSz, OcspEntry* single)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *ioIndex, prevIndex, oid, localIdx, certIdIdx;
    int length;
    int ret;
    byte tag;

    WOLFSSL_ENTER("DecodeSingleResponse");

    prevIndex = idx;

    /* Wrapper around the Single Response */
    if (GetSequence(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;

    /* Wrapper around the CertID */
    certIdIdx = idx;
    if (GetSequence(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;
    single->rawCertId = source + certIdIdx;
    /* Hash algorithm */
    ret = GetAlgoId(source, &idx, &oid, oidIgnoreType, size);
    if (ret < 0)
        return ret;
    single->hashAlgoOID = oid;
    /* Save reference to the hash of CN */
    ret = GetOctetString(source, &idx, &length, size);
    if (ret < 0)
        return ret;
    if (length > (int)sizeof(single->issuerHash))
        return BUFFER_E;
    XMEMCPY(single->issuerHash, source + idx, length);
    idx += length;
    /* Save reference to the hash of the issuer public key */
    ret = GetOctetString(source, &idx, &length, size);
    if (ret < 0)
        return ret;
    if (length > (int)sizeof(single->issuerKeyHash))
        return BUFFER_E;
    XMEMCPY(single->issuerKeyHash, source + idx, length);
    idx += length;

    /* Get serial number */
    if (GetSerialNumber(source, &idx, single->status->serial, &single->status->serialSz, size) < 0)
        return ASN_PARSE_E;
    single->rawCertIdSize = idx - certIdIdx;

    if (idx >= size)
        return BUFFER_E;

    /* CertStatus */
    switch (source[idx++])
    {
        case (ASN_CONTEXT_SPECIFIC | CERT_GOOD):
            single->status->status = CERT_GOOD;
            idx++;
            break;
        case (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | CERT_REVOKED):
            single->status->status = CERT_REVOKED;
            if (GetLength(source, &idx, &length, size) < 0)
                return ASN_PARSE_E;
            idx += length;
            break;
        case (ASN_CONTEXT_SPECIFIC | CERT_UNKNOWN):
            single->status->status = CERT_UNKNOWN;
            idx++;
            break;
        default:
            return ASN_PARSE_E;
    }

#if defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX) || defined(WOLFSSL_HAPROXY)
    single->status->thisDateAsn = source + idx;
    localIdx = 0;
    if (GetDateInfo(single->status->thisDateAsn, &localIdx, NULL,
                    (byte*)&single->status->thisDateParsed.type,
                    &single->status->thisDateParsed.length, size) < 0)
        return ASN_PARSE_E;
    XMEMCPY(single->status->thisDateParsed.data,
            single->status->thisDateAsn + localIdx - single->status->thisDateParsed.length,
            single->status->thisDateParsed.length);
#endif
    if (GetBasicDate(source, &idx, single->status->thisDate,
                                                &single->status->thisDateFormat, size) < 0)
        return ASN_PARSE_E;

#ifndef NO_ASN_TIME
#ifndef WOLFSSL_NO_OCSP_DATE_CHECK
    if (!XVALIDATE_DATE(single->status->thisDate, single->status->thisDateFormat, BEFORE))
        return ASN_BEFORE_DATE_E;
#endif
#endif

    /* The following items are optional. Only check for them if there is more
     * unprocessed data in the singleResponse wrapper. */
    localIdx = idx;
    if (((int)(idx - prevIndex) < wrapperSz) &&
        GetASNTag(source, &localIdx, &tag, size) == 0 &&
        tag == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
    {
        idx++;
        if (GetLength(source, &idx, &length, size) < 0)
            return ASN_PARSE_E;
#if defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX) || defined(WOLFSSL_HAPROXY)
        single->status->nextDateAsn = source + idx;
        localIdx = 0;
        if (GetDateInfo(single->status->nextDateAsn, &localIdx, NULL,
                        (byte*)&single->status->nextDateParsed.type,
                        &single->status->nextDateParsed.length, size) < 0)
            return ASN_PARSE_E;
        XMEMCPY(single->status->nextDateParsed.data,
                single->status->nextDateAsn + localIdx - single->status->nextDateParsed.length,
                single->status->nextDateParsed.length);
#endif
        if (GetBasicDate(source, &idx, single->status->nextDate,
                                                &single->status->nextDateFormat, size) < 0)
            return ASN_PARSE_E;

#ifndef NO_ASN_TIME
#ifndef WOLFSSL_NO_OCSP_DATE_CHECK
        if (!XVALIDATE_DATE(single->status->nextDate, single->status->nextDateFormat, AFTER))
            return ASN_AFTER_DATE_E;
#endif
#endif
    }

    /* Skip the optional extensions in singleResponse. */
    localIdx = idx;
    if (((int)(idx - prevIndex) < wrapperSz) &&
        GetASNTag(source, &localIdx, &tag, size) == 0 &&
        tag == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1))
    {
        idx++;
        if (GetLength(source, &idx, &length, size) < 0)
            return ASN_PARSE_E;
        idx += length;
    }

    *ioIndex = idx;

    return 0;
#else
    DECL_ASNGETDATA(dataASN, singleResponseASN_Length);
    int ret = 0;
    CertStatus* cs = NULL;
    word32 serialSz;
    word32 issuerHashLen;
    word32 issuerKeyHashLen;
    word32 thisDateLen;
    word32 nextDateLen;
#if defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX) || \
    defined(WOLFSSL_HAPROXY) || defined(HAVE_LIGHTY)
    WOLFSSL_ASN1_TIME *at;
#endif

    (void)wrapperSz;

    WOLFSSL_ENTER("DecodeSingleResponse");

    CALLOC_ASNGETDATA(dataASN, singleResponseASN_Length, ret, NULL);

    if (ret == 0) {
        /* Certificate Status field. */
        cs = single->status;

        /* Set maximum lengths for data. */
        issuerHashLen    = OCSP_DIGEST_SIZE;
        issuerKeyHashLen = OCSP_DIGEST_SIZE;
        serialSz         = EXTERNAL_SERIAL_SIZE;
        thisDateLen      = MAX_DATE_SIZE;
        nextDateLen      = MAX_DATE_SIZE;

        /* Set OID type, buffers to hold data and variables to hold size. */
        GetASN_OID(&dataASN[3], oidHashType);
        GetASN_Buffer(&dataASN[5], single->issuerHash, &issuerHashLen);
        GetASN_Buffer(&dataASN[6], single->issuerKeyHash, &issuerKeyHashLen);
        GetASN_Buffer(&dataASN[7], cs->serial, &serialSz);
        GetASN_Buffer(&dataASN[14], cs->thisDate, &thisDateLen);
        GetASN_Buffer(&dataASN[16], cs->nextDate, &nextDateLen);
        /* TODO: decode revoked time and reason. */
        /* Decode OCSP single response. */
        ret = GetASN_Items(singleResponseASN, dataASN, singleResponseASN_Length,
                1, source, ioIndex, size);
    }
    /* Validate the issuer hash length is the size required. */
    if ((ret == 0) && (issuerHashLen != OCSP_DIGEST_SIZE)) {
        ret = ASN_PARSE_E;
    }
    /* Validate the issuer key hash length is the size required. */
    if ((ret == 0) && (issuerKeyHashLen != OCSP_DIGEST_SIZE)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* Store serial size. */
        cs->serialSz = serialSz;

        /* Determine status by which item was found. */
        if (dataASN[8].tag != 0) {
            cs->status = CERT_GOOD;
        }
        if (dataASN[9].tag != 0) {
            cs->status = CERT_REVOKED;
        }
        if (dataASN[13].tag != 0) {
            cs->status = CERT_UNKNOWN;
        }

        /* Store the thisDate format - only one possible. */
        cs->thisDateFormat = ASN_GENERALIZED_TIME;
    #if !defined(NO_ASN_TIME) && !defined(WOLFSSL_NO_OCSP_DATE_CHECK)
        /* Check date is a valid string and BEFORE now. */
        if (!XVALIDATE_DATE(cs->thisDate, ASN_GENERALIZED_TIME, BEFORE)) {
            ret = ASN_BEFORE_DATE_E;
        }
    }
    if (ret == 0) {
    #endif
    #if defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX) || \
        defined(WOLFSSL_HAPROXY) || defined(HAVE_LIGHTY)
        /* Store ASN.1 version of thisDate. */
        cs->thisDateAsn = GetASNItem_Addr(dataASN[14], source);
        at = &cs->thisDateParsed;
        at->type = ASN_GENERALIZED_TIME;
        XMEMCPY(at->data, cs->thisDate, thisDateLen);
        at->length = thisDateLen;
    #endif
    }
    if ((ret == 0) && (dataASN[16].tag != 0)) {
        /* Store the nextDate format - only one possible. */
        cs->nextDateFormat = ASN_GENERALIZED_TIME;
    #if !defined(NO_ASN_TIME) && !defined(WOLFSSL_NO_OCSP_DATE_CHECK)
        /* Check date is a valid string and AFTER now. */
        if (!XVALIDATE_DATE(cs->nextDate, ASN_GENERALIZED_TIME, AFTER)) {
            ret = ASN_AFTER_DATE_E;
        }
    }
    if ((ret == 0) && (dataASN[16].tag != 0)) {
    #endif
    #if defined(OPENSSL_ALL) || defined(WOLFSSL_NGINX) || \
        defined(WOLFSSL_HAPROXY) || defined(HAVE_LIGHTY)
        /* Store ASN.1 version of thisDate. */
        cs->nextDateAsn = GetASNItem_Addr(dataASN[16], source);
        at = &cs->nextDateParsed;
        at->type = ASN_GENERALIZED_TIME;
        XMEMCPY(at->data, cs->nextDate, nextDateLen);
        at->length = nextDateLen;
    #endif
    }
    if (ret == 0) {
        /* OcspEntry now used. */
        single->used = 1;
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for OCSP response extension header.
 * RFC 6960, 4.2.1 - ASN.1 Specification of the OCSP Response
 */
static const ASNItem respExtHdrASN[] = {
            /* responseExtensions */
/*  0 */    { 0, ASN_CONTEXT_SPECIFIC | 1, 1, 1, 0 },
                /* extensions */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
};

/* Number of items in ASN.1 template for OCSP response extension header. */
#define respExtHdrASN_Length (sizeof(respExtHdrASN) / sizeof(ASNItem))
#endif

static int DecodeOcspRespExtensions(byte* source, word32* ioIndex,
                                    OcspResponse* resp, word32 sz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *ioIndex;
    int length;
    int ext_bound; /* boundary index for the sequence of extensions */
    word32 oid;
    int ret;
    byte tag;

    WOLFSSL_ENTER("DecodeOcspRespExtensions");

    if ((idx + 1) > sz)
        return BUFFER_E;

    if (GetASNTag(source, &idx, &tag, sz) < 0)
        return ASN_PARSE_E;

    if (tag != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1))
        return ASN_PARSE_E;

    if (GetLength(source, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    if (GetSequence(source, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    ext_bound = idx + length;

    while (idx < (word32)ext_bound) {
        word32 localIdx;

        if (GetSequence(source, &idx, &length, sz) < 0) {
            WOLFSSL_MSG("\tfail: should be a SEQUENCE");
            return ASN_PARSE_E;
        }

        oid = 0;
        if (GetObjectId(source, &idx, &oid, oidOcspType, sz) < 0) {
            WOLFSSL_MSG("\tfail: OBJECT ID");
            return ASN_PARSE_E;
        }

        /* check for critical flag */
        if ((idx + 1) > (word32)sz) {
            WOLFSSL_MSG("\tfail: malformed buffer");
            return BUFFER_E;
        }

        localIdx = idx;
        if (GetASNTag(source, &localIdx, &tag, sz) == 0 && tag == ASN_BOOLEAN) {
            WOLFSSL_MSG("\tfound optional critical flag, moving past");
            ret = GetBoolean(source, &idx, sz);
            if (ret < 0)
                return ret;
        }

        ret = GetOctetString(source, &idx, &length, sz);
        if (ret < 0)
            return ret;

        if (oid == OCSP_NONCE_OID) {
            /* get data inside extra OCTET_STRING */
            ret = GetOctetString(source, &idx, &length, sz);
            if (ret < 0)
                return ret;

            resp->nonce = source + idx;
            resp->nonceSz = length;
        }

        idx += length;
    }

    *ioIndex = idx;
    return 0;
#else
    /* certExtASN_Length is greater than respExtHdrASN_Length */
    DECL_ASNGETDATA(dataASN, certExtASN_Length);
    int ret = 0;
    word32 idx = *ioIndex;
    word32 maxIdx = 0;

    WOLFSSL_ENTER("DecodeOcspRespExtensions");

    ALLOC_ASNGETDATA(dataASN, certExtASN_Length, ret, resp->heap);

    /* Check for header and move past. */
    XMEMSET(dataASN, 0, sizeof(*dataASN) * respExtHdrASN_Length);
    ret = GetASN_Items(respExtHdrASN, dataASN, respExtHdrASN_Length, 0,
        source, &idx, sz);
    if (ret == 0) {
        /* Keep end extensions index for total length check. */
        maxIdx = idx + dataASN[1].length;
    }

    /* Step through all extensions. */
    while ((ret == 0) && (idx < maxIdx)) {
        /* Clear dynamic data, set OID type to expect. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * certExtASN_Length);
        GetASN_OID(&dataASN[1], oidOcspType);
        /* TODO: check criticality. */
        /* Decode OCSP response extension. */
        ret = GetASN_Items(certExtASN, dataASN, certExtASN_Length, 0,
                           source, &idx, sz);
        if (ret == 0) {
            word32 oid = dataASN[1].data.oid.sum;
            int length = dataASN[3].length;

            if (oid == OCSP_NONCE_OID) {
                /* Extract nonce data. */
                ret = GetOctetString(source, &idx, &length, sz);
                if (ret >= 0) {
                    ret = 0;
                    /* get data inside extra OCTET_STRING */
                    resp->nonce = source + idx;
                    resp->nonceSz = length;
                }
            }
            /* Ignore all other extension types. */

            /* Skip over rest of extension. */
            idx += length;
        }
    }

    /* Return index after extensions. */
    *ioIndex = idx;

    FREE_ASNGETDATA(dataASN, resp->heap);
    return ret;
#endif
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for OCSP ResponseData.
 * RFC 6960, 4.2.1 - ASN.1 Specification of the OCSP Response
 */
static const ASNItem ocspRespDataASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* version DEFAULT v1 */
/*  1 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
/*  2 */            { 2, ASN_INTEGER, 1, 0, 0 },
                /* byName */
/*  3 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 2 },
                /* byKey */
/*  4 */        { 1, ASN_CONTEXT_SPECIFIC | 2, 1, 0, 2 },
                /* producedAt */
/*  5 */        { 1, ASN_GENERALIZED_TIME, 0, 0, 0, },
                /* responses */
/*  6 */        { 1, ASN_SEQUENCE, 1, 0, 0 },
                /* responseExtensions */
/*  7 */        { 1, ASN_CONTEXT_SPECIFIC | 1, 1, 0, 1 }
};

/* Number of items in ASN.1 template for OCSP ResponseData. */
#define ocspRespDataASN_Length (sizeof(ocspRespDataASN) / sizeof(ASNItem))
#endif

static int DecodeResponseData(byte* source, word32* ioIndex,
                              OcspResponse* resp, word32 size)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = *ioIndex, prev_idx, localIdx;
    int length;
    int version;
    int ret;
    byte tag;
    int wrapperSz;
    OcspEntry* single;

    WOLFSSL_ENTER("DecodeResponseData");

    resp->response = source + idx;
    prev_idx = idx;
    if (GetSequence(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;
    resp->responseSz = length + idx - prev_idx;

    /* Get version. It is an EXPLICIT[0] DEFAULT(0) value. If this
     * item isn't an EXPLICIT[0], then set version to zero and move
     * onto the next item.
     */
    localIdx = idx;
    if (GetASNTag(source, &localIdx, &tag, size) == 0 &&
            tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED))
    {
        idx += 2; /* Eat the value and length */
        if (GetMyVersion(source, &idx, &version, size) < 0)
            return ASN_PARSE_E;
    } else
        version = 0;

    localIdx = idx;
    if (GetASNTag(source, &localIdx, &tag, size) == 0 &&
        ( tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 1) ||
          tag == (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 2) ))
    {
        idx++; /* advance past ASN tag */
        if (GetLength(source, &idx, &length, size) < 0)
            return ASN_PARSE_E;
        idx += length;
    }
    else
        return ASN_PARSE_E;

    /* save pointer to the producedAt time */
    if (GetBasicDate(source, &idx, resp->producedDate,
                                        &resp->producedDateFormat, size) < 0)
        return ASN_PARSE_E;

    /* Outer wrapper of the SEQUENCE OF Single Responses. */
    if (GetSequence(source, &idx, &wrapperSz, size) < 0)
        return ASN_PARSE_E;

    localIdx = idx;
    single = resp->single;
    while (idx - localIdx < (word32)wrapperSz) {
        ret = DecodeSingleResponse(source, &idx, size, wrapperSz, single);
        if (ret < 0)
            return ret; /* ASN_PARSE_E, ASN_BEFORE_DATE_E, ASN_AFTER_DATE_E */
        if (idx - localIdx < (word32)wrapperSz) {
            single->next = (OcspEntry*)XMALLOC(sizeof(OcspEntry), resp->heap,
                DYNAMIC_TYPE_OCSP_ENTRY);
            if (single->next == NULL) {
                return MEMORY_E;
            }
            XMEMSET(single->next, 0, sizeof(OcspEntry));

            single->next->status = (CertStatus*)XMALLOC(sizeof(CertStatus),
                resp->heap, DYNAMIC_TYPE_OCSP_STATUS);
            if (single->next->status == NULL) {
                XFREE(single->next, resp->heap, DYNAMIC_TYPE_OCSP_ENTRY);
                single->next = NULL;
                return MEMORY_E;
            }
            XMEMSET(single->next->status, 0, sizeof(CertStatus));

            single->next->isDynamic = 1;

            single = single->next;
        }
    }

    /*
     * Check the length of the ResponseData against the current index to
     * see if there are extensions, they are optional.
     */
    if (idx - prev_idx < resp->responseSz)
        if (DecodeOcspRespExtensions(source, &idx, resp, size) < 0)
            return ASN_PARSE_E;

    *ioIndex = idx;
    return 0;
#else
    DECL_ASNGETDATA(dataASN, ocspRespDataASN_Length);
    int ret = 0;
    byte version;
    word32 dateSz, idx = *ioIndex;
    OcspEntry* single;

    WOLFSSL_ENTER("DecodeResponseData");

    CALLOC_ASNGETDATA(dataASN, ocspRespDataASN_Length, ret, resp->heap);

    if (ret == 0) {
        resp->response = source + idx;
        /* Default, not present, is v1 = 0. */
        version = 0;
        /* Max size of date supported. */
        dateSz = MAX_DATE_SIZE;

        /* Set the where to put version an produced date. */
        GetASN_Int8Bit(&dataASN[2], &version);
        GetASN_Buffer(&dataASN[5], resp->producedDate, &dateSz);
        /* Decode the ResponseData. */
        ret = GetASN_Items(ocspRespDataASN, dataASN, ocspRespDataASN_Length,
                1, source, ioIndex, size);
    }
    /* Only support v1 == 0 */
    if ((ret == 0) && (version != 0)) {
        ret = ASN_PARSE_E;
    }
    /* Ensure date is a minimal size. */
    if ((ret == 0) && (dateSz < MIN_DATE_SIZE)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* TODO: use byName/byKey fields. */
        /* Store size of response. */
        resp->responseSz = *ioIndex - idx;
        /* Store date format/tag. */
        resp->producedDateFormat = dataASN[5].tag;

        /* Get the index of the responses SEQUENCE. */
        idx = GetASNItem_DataIdx(dataASN[6], source);
        /* Start with the pre-existing OcspEntry. */
        single = resp->single;
    }
    while ((ret == 0) && (idx < dataASN[7].offset)) {
        /* Allocate and use a new OCSP entry if this is used. */
        if (single->used) {
            single->next = (OcspEntry*)XMALLOC(sizeof(OcspEntry), resp->heap,
                    DYNAMIC_TYPE_OCSP_ENTRY);
            if (single->next == NULL) {
                ret = MEMORY_E;
            }
            else {
                XMEMSET(single->next, 0, sizeof(OcspEntry));

                single->next->status = (CertStatus*)XMALLOC(sizeof(CertStatus),
                    resp->heap, DYNAMIC_TYPE_OCSP_STATUS);
                if (single->next->status == NULL) {
                    XFREE(single->next, resp->heap, DYNAMIC_TYPE_OCSP_ENTRY);
                    single->next = NULL;
                    return MEMORY_E;
                }
                XMEMSET(single->next->status, 0, sizeof(CertStatus));

                /* Entry to be freed. */
                single->isDynamic = 1;
                /* used will be 0 (false) */

                single = single->next;
            }
        }
        /* Decode SingleResponse into OcspEntry. */
        ret = DecodeSingleResponse(source, &idx, dataASN[7].offset,
                dataASN[6].length, single);
        /* single->used set on successful decode. */
    }

    /* Check if there were extensions. */
    if ((ret == 0) && (dataASN[7].data.buffer.data != NULL)) {
        /* Get index of [1] */
        idx = dataASN[7].offset;
        /* Decode the response extensions. */
        if (DecodeOcspRespExtensions(source, &idx, resp, *ioIndex) < 0) {
            ret = ASN_PARSE_E;
        }
    }

    FREE_ASNGETDATA(dataASN, resp->heap);
    return ret;
#endif
}


#ifndef WOLFSSL_ASN_TEMPLATE
#ifndef WOLFSSL_NO_OCSP_OPTIONAL_CERTS

static int DecodeCerts(byte* source,
                            word32* ioIndex, OcspResponse* resp, word32 size)
{
    word32 idx = *ioIndex;
    byte tag;

    WOLFSSL_ENTER("DecodeCerts");

    if (GetASNTag(source, &idx, &tag, size) < 0)
        return ASN_PARSE_E;

    if (tag == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC))
    {
        int length;

        if (GetLength(source, &idx, &length, size) < 0)
            return ASN_PARSE_E;

        if (GetSequence(source, &idx, &length, size) < 0)
            return ASN_PARSE_E;

        resp->cert = source + idx;
        resp->certSz = length;

        idx += length;
    }
    *ioIndex = idx;
    return 0;
}

#endif /* WOLFSSL_NO_OCSP_OPTIONAL_CERTS */
#endif /* !WOLFSSL_ASN_TEMPLATE */

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for BasicOCSPResponse.
 * RFC 6960, 4.2.1 - ASN.1 Specification of the OCSP Response
 */
static const ASNItem ocspBasicRespASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* tbsResponseData */
/*  1 */        { 1, ASN_SEQUENCE, 1, 0, 0, },
                /* signatureAlgorithm */
/*  2 */        { 1, ASN_SEQUENCE, 1, 1, 0, },
/*  3 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/*  4 */            { 2, ASN_TAG_NULL, 0, 0, 1 },
                /* signature */
/*  5 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
                /* certs */
/*  6 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
/*  7 */            { 2, ASN_SEQUENCE, 1, 0, 0, },
};

/* Number of items in ASN.1 template for BasicOCSPResponse. */
#define ocspBasicRespASN_Length (sizeof(ocspBasicRespASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

static int DecodeBasicOcspResponse(byte* source, word32* ioIndex,
            OcspResponse* resp, word32 size, void* cm, void* heap, int noVerify)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    length;
    word32 idx = *ioIndex;
    word32 end_index;
    int    ret;
    int    sigLength;

    WOLFSSL_ENTER("DecodeBasicOcspResponse");
    (void)heap;

    if (GetSequence(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;

    if (idx + length > size)
        return ASN_INPUT_E;
    end_index = idx + length;

    if ((ret = DecodeResponseData(source, &idx, resp, size)) < 0)
        return ret; /* ASN_PARSE_E, ASN_BEFORE_DATE_E, ASN_AFTER_DATE_E */

    /* Get the signature algorithm */
    if (GetAlgoId(source, &idx, &resp->sigOID, oidSigType, size) < 0)
        return ASN_PARSE_E;

    ret = CheckBitString(source, &idx, &sigLength, size, 1, NULL);
    if (ret != 0)
        return ret;

    resp->sigSz = sigLength;
    resp->sig = source + idx;
    idx += sigLength;

    /*
     * Check the length of the BasicOcspResponse against the current index to
     * see if there are certificates, they are optional.
     */
#ifndef WOLFSSL_NO_OCSP_OPTIONAL_CERTS
    if (idx < end_index)
    {
        DecodedCert cert;

        if (DecodeCerts(source, &idx, resp, size) < 0)
            return ASN_PARSE_E;

        InitDecodedCert(&cert, resp->cert, resp->certSz, heap);

        /* Don't verify if we don't have access to Cert Manager. */
        ret = ParseCertRelative(&cert, CERT_TYPE,
                                noVerify ? NO_VERIFY : VERIFY_OCSP_CERT, cm);
        if (ret < 0) {
            WOLFSSL_MSG("\tOCSP Responder certificate parsing failed");
            FreeDecodedCert(&cert);
            return ret;
        }

#ifndef WOLFSSL_NO_OCSP_ISSUER_CHECK
        if ((cert.extExtKeyUsage & EXTKEYUSE_OCSP_SIGN) == 0) {
            if (XMEMCMP(cert.subjectHash,
                        resp->single->issuerHash, OCSP_DIGEST_SIZE) == 0) {
                WOLFSSL_MSG("\tOCSP Response signed by issuer");
            }
            else {
                WOLFSSL_MSG("\tOCSP Responder key usage check failed");
    #ifdef OPENSSL_EXTRA
                resp->verifyError = OCSP_BAD_ISSUER;
    #else
                FreeDecodedCert(&cert);
                return BAD_OCSP_RESPONDER;
    #endif
            }
        }
#endif

        /* ConfirmSignature is blocking here */
        ret = ConfirmSignature(&cert.sigCtx,
            resp->response, resp->responseSz,
            cert.publicKey, cert.pubKeySize, cert.keyOID,
            resp->sig, resp->sigSz, resp->sigOID, NULL);

        FreeDecodedCert(&cert);

        if (ret != 0) {
            WOLFSSL_MSG("\tOCSP Confirm signature failed");
            return ASN_OCSP_CONFIRM_E;
        }
    }
    else
#endif /* WOLFSSL_NO_OCSP_OPTIONAL_CERTS */
    {
        Signer* ca;
        int sigValid = -1;

        #ifndef NO_SKID
            ca = GetCA(cm, resp->single->issuerKeyHash);
        #else
            ca = GetCA(cm, resp->single->issuerHash);
        #endif

        if (ca) {
            SignatureCtx sigCtx;
            InitSignatureCtx(&sigCtx, heap, INVALID_DEVID);

            /* ConfirmSignature is blocking here */
            sigValid = ConfirmSignature(&sigCtx, resp->response,
                resp->responseSz, ca->publicKey, ca->pubKeySize, ca->keyOID,
                                resp->sig, resp->sigSz, resp->sigOID, NULL);
        }
        if (ca == NULL || sigValid != 0) {
            WOLFSSL_MSG("\tOCSP Confirm signature failed");
            return ASN_OCSP_CONFIRM_E;
        }

        (void)noVerify;
    }

    *ioIndex = idx;
    return 0;
#else
    DECL_ASNGETDATA(dataASN, ocspBasicRespASN_Length);
    int ret = 0;
    word32 idx = *ioIndex;
#ifndef WOLFSSL_NO_OCSP_OPTIONAL_CERTS
    #ifdef WOLFSSL_SMALL_STACK
        DecodedCert* cert = NULL;
    #else
        DecodedCert cert[1];
    #endif
    int certInit = 0;
#endif

    WOLFSSL_ENTER("DecodeBasicOcspResponse");
    (void)heap;

    CALLOC_ASNGETDATA(dataASN, ocspBasicRespASN_Length, ret, heap);

    if (ret == 0) {
        /* Set expecting signature OID. */
        GetASN_OID(&dataASN[3], oidSigType);
        /* Decode BasicOCSPResponse. */
        ret = GetASN_Items(ocspBasicRespASN, dataASN, ocspBasicRespASN_Length,
                1, source, &idx, size);
    }
    if (ret == 0) {
        word32 dataIdx = 0;
        /* Decode the response data. */
        if (DecodeResponseData(GetASNItem_Addr(dataASN[1], source), &dataIdx,
                resp, GetASNItem_Length(dataASN[1], source)) < 0) {
            ret = ASN_PARSE_E;
        }
    }
    if (ret == 0) {
        /* Get the signature OID and signature. */
        resp->sigOID = dataASN[3].data.oid.sum;
        GetASN_GetRef(&dataASN[5], &resp->sig, &resp->sigSz);
    }
#ifndef WOLFSSL_NO_OCSP_OPTIONAL_CERTS
    if ((ret == 0) && (dataASN[7].data.ref.data != NULL)) {
        /* TODO: support more than one certificate. */
        /* Store reference to certificate BER data. */
        GetASN_GetRef(&dataASN[7], &resp->cert, &resp->certSz);

        /* Allocate a certificate object to decode cert into. */
    #ifdef WOLFSSL_SMALL_STACK
        cert = (DecodedCert*)XMALLOC(sizeof(DecodedCert), heap,
                DYNAMIC_TYPE_TMP_BUFFER);
        if (cert == NULL) {
            ret = MEMORY_E;
        }
    }
    if ((ret == 0) && (dataASN[7].data.ref.data != NULL)) {
    #endif
        /* Initialize the crtificate object. */
        InitDecodedCert(cert, resp->cert, resp->certSz, heap);
        certInit = 1;
        /* Parse the certificate and don't verify if we don't have access to
         * Cert Manager. */
        ret = ParseCertRelative(cert, CERT_TYPE, noVerify ? NO_VERIFY : VERIFY,
                cm);
        if (ret < 0) {
            WOLFSSL_MSG("\tOCSP Responder certificate parsing failed");
        }
    }
    if ((ret == 0) && (dataASN[7].data.ref.data != NULL)) {
        /* TODO: ConfirmSignature is blocking here */
        /* Check the signature of the response. */
        ret = ConfirmSignature(&cert->sigCtx, resp->response, resp->responseSz,
            cert->publicKey, cert->pubKeySize, cert->keyOID, resp->sig,
            resp->sigSz, resp->sigOID, NULL);
        if (ret != 0) {
            WOLFSSL_MSG("\tOCSP Confirm signature failed");
            ret = ASN_OCSP_CONFIRM_E;
        }
    }
    if ((ret == 0) && (dataASN[7].data.ref.data == NULL))
#else
    if (ret == 0)
#endif /* WOLFSSL_NO_OCSP_OPTIONAL_CERTS */
    {
        Signer* ca;
        int sigValid = -1;

        /* Resonse didn't have a certificate - lookup CA. */
    #ifndef NO_SKID
        ca = GetCA(cm, resp->single->issuerKeyHash);
    #else
        ca = GetCA(cm, resp->single->issuerHash);
    #endif
        if (ca) {
            SignatureCtx sigCtx;
            /* Initialize he signature context. */
            InitSignatureCtx(&sigCtx, heap, INVALID_DEVID);

            /* TODO: ConfirmSignature is blocking here */
            /* Check the signature of the response CA public key. */
            sigValid = ConfirmSignature(&sigCtx, resp->response,
                resp->responseSz, ca->publicKey, ca->pubKeySize, ca->keyOID,
                resp->sig, resp->sigSz, resp->sigOID, NULL);
        }
        if ((ca == NULL) || (sigValid != 0)) {
            /* Didn't find certificate or signature verificate failed. */
            WOLFSSL_MSG("\tOCSP Confirm signature failed");
            ret = ASN_OCSP_CONFIRM_E;
        }
    }

    if (ret == 0) {
        /* Update the position to after response data. */
        *ioIndex = idx;
    }

#ifndef WOLFSSL_NO_OCSP_OPTIONAL_CERTS
    if (certInit) {
        FreeDecodedCert(cert);
    }
    #ifdef WOLFSSL_SMALL_STACK
    if (cert != NULL) {
        /* Dispose of certificate object. */
        XFREE(cert, heap, DYNAMIC_TYPE_TMP_BUFFER);
    }
    #endif
#endif
    FREE_ASNGETDATA(dataASN, heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


void InitOcspResponse(OcspResponse* resp, OcspEntry* single, CertStatus* status,
                      byte* source, word32 inSz, void* heap)
{
    WOLFSSL_ENTER("InitOcspResponse");

    XMEMSET(status, 0, sizeof(CertStatus));
    XMEMSET(single,  0, sizeof(OcspEntry));
    XMEMSET(resp,   0, sizeof(OcspResponse));

    single->status       = status;
    resp->responseStatus = -1;
    resp->single         = single;
    resp->source         = source;
    resp->maxIdx         = inSz;
    resp->heap           = heap;
}

void FreeOcspResponse(OcspResponse* resp)
{
    OcspEntry *single, *next;
    for (single = resp->single; single; single = next) {
        next = single->next;
        if (single->isDynamic) {
            XFREE(single->status, resp->heap, DYNAMIC_TYPE_OCSP_STATUS);
            XFREE(single, resp->heap, DYNAMIC_TYPE_OCSP_ENTRY);
        }
    }
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for OCSPResponse.
 * RFC 6960, 4.2.1 - ASN.1 Specification of the OCSP Response
 */
static const ASNItem ocspResponseASN[] = {
            /* OCSPResponse ::= SEQUENCE */
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* responseStatus      OCSPResponseStatus */
/*  1 */        { 1, ASN_ENUMERATED, 0, 0, 0, },
                /* responseBytes   [0] EXPLICIT ResponseBytes OPTIONAL */
/*  2 */        { 1, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
                    /* ResponseBytes ::= SEQUENCE */
/*  3 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                       /* responseType   OBJECT IDENTIFIER */
/*  4 */               { 3, ASN_OBJECT_ID, 0, 0, 0 },
                       /* response       OCTET STRING */
/*  5 */               { 3, ASN_OCTET_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for OCSPResponse. */
#define ocspResponseASN_Length (sizeof(ocspResponseASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

int OcspResponseDecode(OcspResponse* resp, void* cm, void* heap, int noVerify)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int ret;
    int length = 0;
    word32 idx = 0;
    byte* source = resp->source;
    word32 size = resp->maxIdx;
    word32 oid;
    byte   tag;

    WOLFSSL_ENTER("OcspResponseDecode");

    /* peel the outer SEQUENCE wrapper */
    if (GetSequence(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;

    /* First get the responseStatus, an ENUMERATED */
    if (GetEnumerated(source, &idx, &resp->responseStatus, size) < 0)
        return ASN_PARSE_E;

    if (resp->responseStatus != OCSP_SUCCESSFUL)
        return 0;

    /* Next is an EXPLICIT record called ResponseBytes, OPTIONAL */
    if (idx >= size)
        return ASN_INPUT_E;
    if (GetASNTag(source, &idx, &tag, size) < 0)
        return ASN_PARSE_E;
    if (tag != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC))
        return ASN_PARSE_E;
    if (GetLength(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;

    /* Get the responseBytes SEQUENCE */
    if (GetSequence(source, &idx, &length, size) < 0)
        return ASN_PARSE_E;

    /* Check ObjectID for the resposeBytes */
    if (GetObjectId(source, &idx, &oid, oidOcspType, size) < 0)
        return ASN_PARSE_E;
    if (oid != OCSP_BASIC_OID)
        return ASN_PARSE_E;
    ret = GetOctetString(source, &idx, &length, size);
    if (ret < 0)
        return ret;

    ret = DecodeBasicOcspResponse(source, &idx, resp, size, cm, heap, noVerify);
    if (ret < 0)
        return ret;

    return 0;
#else
    DECL_ASNGETDATA(dataASN, ocspResponseASN_Length);
    int ret = 0;
    word32 idx = 0, size = resp->maxIdx;
    byte* source = resp->source;
    byte status;
    byte* basic;
    word32 basicSz;

    WOLFSSL_ENTER("OcspResponseDecode");

    CALLOC_ASNGETDATA(dataASN, ocspResponseASN_Length, ret, resp->heap);

    if (ret == 0) {
        /* Set variable to put status in and expect OCSP OID. */
        GetASN_Int8Bit(&dataASN[1], &status);
        GetASN_OID(&dataASN[4], oidOcspType);
        /* Decode OCSPResponse (and ResponseBytes). */
        ret = GetASN_Items(ocspResponseASN, dataASN, ocspResponseASN_Length, 1,
            source, &idx, size);
    }
    if (ret == 0) {
        /* Get response. */
        resp->responseStatus = status;
        if (dataASN[4].data.oid.sum == OCSP_BASIC_OID) {
            /* Get reference to BasicOCSPResponse. */
            GetASN_GetRef(&dataASN[5], &basic, &basicSz);
            idx = 0;
            /* Decode BasicOCSPResponse. */
            ret = DecodeBasicOcspResponse(basic, &idx, resp, basicSz, cm, heap,
                noVerify);
        }
        /* Only support BasicOCSPResponse. */
        else {
            ret = ASN_PARSE_E;
        }
    }

    FREE_ASNGETDATA(dataASN, resp->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for OCSP nonce extension.
 * RFC 6960, 4.4.1 - Nonce
 * X.509: RFC 5280, 4.1 - Basic Certificate Fields. (Extension)
 */
static const ASNItem ocspNonceExtASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* Extension */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                   /* extnId */
/*  2 */           {2, ASN_OBJECT_ID, 0, 0, 0 },
                   /* critcal not encoded. */
                   /* extnValue */
/*  3 */           {2, ASN_OCTET_STRING, 0, 1, 0 },
                       /* nonce */
/*  4 */               {3, ASN_OCTET_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for OCSP nonce extension. */
#define ocspNonceExtASN_Length (sizeof(ocspNonceExtASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

word32 EncodeOcspRequestExtensions(OcspRequest* req, byte* output, word32 size)
{
    const byte NonceObjId[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07,
                                       0x30, 0x01, 0x02 };
#ifndef WOLFSSL_ASN_TEMPLATE
    byte seqArray[5][MAX_SEQ_SZ];
    word32 seqSz[5], totalSz = (word32)sizeof(NonceObjId);

    WOLFSSL_ENTER("SetOcspReqExtensions");

    if (!req || !output || !req->nonceSz)
        return 0;

    totalSz += req->nonceSz;
    totalSz += seqSz[0] = SetOctetString(req->nonceSz, seqArray[0]);
    totalSz += seqSz[1] = SetOctetString(req->nonceSz + seqSz[0], seqArray[1]);
    totalSz += seqSz[2] = SetObjectId(sizeof(NonceObjId), seqArray[2]);
    totalSz += seqSz[3] = SetSequence(totalSz, seqArray[3]);
    totalSz += seqSz[4] = SetSequence(totalSz, seqArray[4]);

    if (totalSz > size)
        return 0;

    totalSz = 0;

    XMEMCPY(output + totalSz, seqArray[4], seqSz[4]);
    totalSz += seqSz[4];

    XMEMCPY(output + totalSz, seqArray[3], seqSz[3]);
    totalSz += seqSz[3];

    XMEMCPY(output + totalSz, seqArray[2], seqSz[2]);
    totalSz += seqSz[2];

    XMEMCPY(output + totalSz, NonceObjId, sizeof(NonceObjId));
    totalSz += (word32)sizeof(NonceObjId);

    XMEMCPY(output + totalSz, seqArray[1], seqSz[1]);
    totalSz += seqSz[1];

    XMEMCPY(output + totalSz, seqArray[0], seqSz[0]);
    totalSz += seqSz[0];

    XMEMCPY(output + totalSz, req->nonce, req->nonceSz);
    totalSz += req->nonceSz;

    return totalSz;
#else
    int ret = 0;

    WOLFSSL_ENTER("SetOcspReqExtensions");

    /* Check request has nonce to write in extension. */
    if (req != NULL && req->nonceSz != 0) {
        DECL_ASNSETDATA(dataASN, ocspNonceExtASN_Length);
        int sz;

        CALLOC_ASNSETDATA(dataASN, ocspNonceExtASN_Length, ret, req->heap);

        /* Set nonce extension OID and nonce. */
        SetASN_Buffer(&dataASN[2], NonceObjId, sizeof(NonceObjId));
        SetASN_Buffer(&dataASN[4], req->nonce, req->nonceSz);
        /* Calculate size of nonce extension. */
        ret = SizeASN_Items(ocspNonceExtASN, dataASN, ocspNonceExtASN_Length,
                            &sz);
        /* Check buffer big enough for encoding if supplied. */
        if ((ret == 0) && (output != NULL) && (sz > (int)size)) {
            ret = BUFFER_E;
        }
        if ((ret == 0) && (output != NULL)) {
            /* Encode nonce extension. */
            SetASN_Items(ocspNonceExtASN, dataASN, ocspNonceExtASN_Length,
                         output);
        }
        if (ret == 0) {
            /* Return size of encoding. */
            ret = sz;
        }

        FREE_ASNSETDATA(dataASN, req->heap);
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for OCSPRequest.
 * RFC 6960, 4.1.1 - ASN.1 Specification of the OCSP Request
 */
static const ASNItem ocspRequestASN[] = {
            /* OCSPRequest */
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* tbsRequest */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* version not written - v1 */
                    /* requestorName not written */
                    /* requestList */
/*  2 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
                        /* Request */
/*  3 */                { 3, ASN_SEQUENCE, 1, 1, 0 },
                            /* reqCert */
/*  4 */                    { 4, ASN_SEQUENCE, 1, 1, 0 },
                                /* hashAlgorithm */
/*  5 */                        { 5, ASN_SEQUENCE, 1, 1, 0 },
/*  6 */                            { 6, ASN_OBJECT_ID, 0, 0, 0 },
                                /* issuerNameHash */
/*  7 */                        { 5, ASN_OCTET_STRING, 0, 0, 0 },
                                /* issuerKeyHash */
/*  8 */                        { 5, ASN_OCTET_STRING, 0, 0, 0 },
                                /* serialNumber */
/*  9 */                        { 5, ASN_INTEGER, 0, 0, 0 },
                    /* requestExtensions */
/* 10 */            { 2, ASN_CONTEXT_SPECIFIC | 2, 1, 0, 0 },
                /* optionalSignature not written. */
};

/* Number of items in ASN.1 template for OCSPRequest. */
#define ocspRequestASN_Length (sizeof(ocspRequestASN) / sizeof(ASNItem))
#endif

int EncodeOcspRequest(OcspRequest* req, byte* output, word32 size)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    byte seqArray[5][MAX_SEQ_SZ];
    /* The ASN.1 of the OCSP Request is an onion of sequences */
    byte algoArray[MAX_ALGO_SZ];
    byte issuerArray[MAX_ENCODED_DIG_SZ];
    byte issuerKeyArray[MAX_ENCODED_DIG_SZ];
    byte snArray[MAX_SN_SZ];
    byte extArray[MAX_OCSP_EXT_SZ];
    word32 seqSz[5], algoSz, issuerSz, issuerKeySz, extSz, totalSz;
    int i, snSz;

    WOLFSSL_ENTER("EncodeOcspRequest");

#ifdef NO_SHA
    algoSz = SetAlgoID(SHA256h, algoArray, oidHashType, 0);
#else
    algoSz = SetAlgoID(SHAh, algoArray, oidHashType, 0);
#endif

    issuerSz    = SetDigest(req->issuerHash,    KEYID_SIZE,    issuerArray);
    issuerKeySz = SetDigest(req->issuerKeyHash, KEYID_SIZE,    issuerKeyArray);
    snSz        = SetSerialNumber(req->serial,  req->serialSz, snArray,
                                                          MAX_SN_SZ, MAX_SN_SZ);
    extSz       = 0;

    if (snSz < 0)
        return snSz;

    if (req->nonceSz) {
        /* TLS Extensions use this function too - put extensions after
         * ASN.1: Context Specific [2].
         */
        extSz = EncodeOcspRequestExtensions(req, extArray + 2,
                                            OCSP_NONCE_EXT_SZ);
        extSz += SetExplicit(2, extSz, extArray);
    }

    totalSz = algoSz + issuerSz + issuerKeySz + snSz;
    for (i = 4; i >= 0; i--) {
        seqSz[i] = SetSequence(totalSz, seqArray[i]);
        totalSz += seqSz[i];
        if (i == 2) totalSz += extSz;
    }

    if (output == NULL)
        return totalSz;
    if (totalSz > size)
        return BUFFER_E;

    totalSz = 0;
    for (i = 0; i < 5; i++) {
        XMEMCPY(output + totalSz, seqArray[i], seqSz[i]);
        totalSz += seqSz[i];
    }

    XMEMCPY(output + totalSz, algoArray, algoSz);
    totalSz += algoSz;

    XMEMCPY(output + totalSz, issuerArray, issuerSz);
    totalSz += issuerSz;

    XMEMCPY(output + totalSz, issuerKeyArray, issuerKeySz);
    totalSz += issuerKeySz;

    XMEMCPY(output + totalSz, snArray, snSz);
    totalSz += snSz;

    if (extSz != 0) {
        XMEMCPY(output + totalSz, extArray, extSz);
        totalSz += extSz;
    }

    return totalSz;
#else
    DECL_ASNSETDATA(dataASN, ocspRequestASN_Length);
    word32 extSz = 0;
    int sz;
    int ret = 0;

    WOLFSSL_ENTER("EncodeOcspRequest");

    CALLOC_ASNSETDATA(dataASN, ocspRequestASN_Length, ret, req->heap);

    if (ret == 0) {
        /* Set OID of hash algorithm use on issuer and key. */
    #ifdef NO_SHA
        SetASN_OID(&dataASN[6], SHA256h, oidHashType);
    #else
        SetASN_OID(&dataASN[6], SHAh, oidHashType);
    #endif
        /* Set issuer, issuer key hash and serial number of certificate being
         * checked. */
        SetASN_Buffer(&dataASN[7], req->issuerHash, KEYID_SIZE);
        SetASN_Buffer(&dataASN[8], req->issuerKeyHash, KEYID_SIZE);
        SetASN_Buffer(&dataASN[9], req->serial, req->serialSz);
        /* Only extension to write is nonce - check if one to encode. */
        if (req->nonceSz) {
            /* Get size of extensions and leave space for them in encoding. */
            ret = extSz = EncodeOcspRequestExtensions(req, NULL, 0);
            SetASN_Buffer(&dataASN[10], NULL, extSz);
            if (ret > 0) {
                ret = 0;
            }
        }
        else {
            /* Don't write out extensions. */
            dataASN[10].noOut = 1;
        }
    }
    if (ret == 0) {
        /* Calculate size of encoding. */
        ret = SizeASN_Items(ocspRequestASN, dataASN, ocspRequestASN_Length,
                &sz);
    }
    /* Check buffer big enough for encoding if supplied. */
    if ((ret == 0) && (output != NULL) && (sz > (int)size)) {
        ret = BUFFER_E;
    }
    if ((ret == 0) && (output != NULL)) {
        /* Encode OCSPRequest. */
        SetASN_Items(ocspRequestASN, dataASN, ocspRequestASN_Length, output);
        if (req->nonceSz) {
            /* Encode extensions into space provided. */
            ret = EncodeOcspRequestExtensions(req,
                (byte*)dataASN[10].data.buffer.data, extSz);
            if (ret > 0) {
                ret = 0;
            }
        }
    }

    if (ret == 0) {
        /* Return size of encoding. */
        ret = sz;
    }

    FREE_ASNSETDATA(dataASN, req->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


int InitOcspRequest(OcspRequest* req, DecodedCert* cert, byte useNonce,
                                                                     void* heap)
{
    int ret;

    WOLFSSL_ENTER("InitOcspRequest");

    if (req == NULL)
        return BAD_FUNC_ARG;

    ForceZero(req, sizeof(OcspRequest));
    req->heap = heap;

    if (cert) {
        XMEMCPY(req->issuerHash,    cert->issuerHash,    KEYID_SIZE);
        XMEMCPY(req->issuerKeyHash, cert->issuerKeyHash, KEYID_SIZE);

        req->serial = (byte*)XMALLOC(cert->serialSz, req->heap,
                                                     DYNAMIC_TYPE_OCSP_REQUEST);
        if (req->serial == NULL)
            return MEMORY_E;

        XMEMCPY(req->serial, cert->serial, cert->serialSz);
        req->serialSz = cert->serialSz;

        if (cert->extAuthInfoSz != 0 && cert->extAuthInfo != NULL) {
            req->url = (byte*)XMALLOC(cert->extAuthInfoSz + 1, req->heap,
                                                     DYNAMIC_TYPE_OCSP_REQUEST);
            if (req->url == NULL) {
                XFREE(req->serial, req->heap, DYNAMIC_TYPE_OCSP);
                return MEMORY_E;
            }

            XMEMCPY(req->url, cert->extAuthInfo, cert->extAuthInfoSz);
            req->urlSz = cert->extAuthInfoSz;
            req->url[req->urlSz] = 0;
        }
    }

    if (useNonce) {
        WC_RNG rng;

    #ifndef HAVE_FIPS
        ret = wc_InitRng_ex(&rng, req->heap, INVALID_DEVID);
    #else
        ret = wc_InitRng(&rng);
    #endif
        if (ret != 0) {
            WOLFSSL_MSG("\tCannot initialize RNG. Skipping the OSCP Nonce.");
        } else {
            if (wc_RNG_GenerateBlock(&rng, req->nonce, MAX_OCSP_NONCE_SZ) != 0)
                WOLFSSL_MSG("\tCannot run RNG. Skipping the OSCP Nonce.");
            else
                req->nonceSz = MAX_OCSP_NONCE_SZ;

            wc_FreeRng(&rng);
        }
    }

    return 0;
}

void FreeOcspRequest(OcspRequest* req)
{
    WOLFSSL_ENTER("FreeOcspRequest");

    if (req) {
        if (req->serial)
            XFREE(req->serial, req->heap, DYNAMIC_TYPE_OCSP_REQUEST);
        req->serial = NULL;

#ifdef OPENSSL_EXTRA
        if (req->serialInt) {
            if (req->serialInt->isDynamic) {
                XFREE(req->serialInt->data, NULL, DYNAMIC_TYPE_OPENSSL);
            }
            XFREE(req->serialInt, NULL, DYNAMIC_TYPE_OPENSSL);
        }
        req->serialInt = NULL;
#endif

        if (req->url)
            XFREE(req->url, req->heap, DYNAMIC_TYPE_OCSP_REQUEST);
        req->url = NULL;
    }
}


int CompareOcspReqResp(OcspRequest* req, OcspResponse* resp)
{
    int cmp = -1; /* default as not matching, cmp gets set on each check */
    OcspEntry *single, *next, *prev = NULL, *top;

    WOLFSSL_ENTER("CompareOcspReqResp");

    if (req == NULL) {
        WOLFSSL_MSG("\tReq missing");
        return -1;
    }
    if (resp == NULL || resp->single == NULL) {
        WOLFSSL_MSG("\tResp missing");
        return 1;
    }

    /* Nonces are not critical. The responder may not necessarily add
     * the nonce to the response. */
    if (req->nonceSz && resp->nonce != NULL
#ifndef WOLFSSL_FORCE_OCSP_NONCE_CHECK
            && resp->nonceSz != 0
#endif
    ) {
        cmp = req->nonceSz - resp->nonceSz;
        if (cmp != 0) {
            WOLFSSL_MSG("\tnonceSz mismatch");
            return cmp;
        }

        cmp = XMEMCMP(req->nonce, resp->nonce, req->nonceSz);
        if (cmp != 0) {
            WOLFSSL_MSG("\tnonce mismatch");
            return cmp;
        }
    }

    /* match based on found status and return */
    for (single = resp->single; single; single = next) {
        cmp = req->serialSz - single->status->serialSz;
        if (cmp == 0) {
            cmp = XMEMCMP(req->serial, single->status->serial, req->serialSz)
               || XMEMCMP(req->issuerHash, single->issuerHash, OCSP_DIGEST_SIZE)
               || XMEMCMP(req->issuerKeyHash, single->issuerKeyHash, OCSP_DIGEST_SIZE);
            if (cmp == 0) {
                /* match found */
                if (resp->single != single && prev) {
                    /* move to top of list */
                    top = resp->single;
                    resp->single = single;
                    prev->next = single->next;
                    single->next = top;
                }
                break;
            }
        }
        next = single->next;
        prev = single;
    }

    if (cmp != 0) {
        WOLFSSL_MSG("\trequest and response mismatch");
        return cmp;
    }

    return 0;
}

#endif /* HAVE_OCSP */


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for certificate name hash. */
static const ASNItem nameHashASN[] = {
/*  0 */    { 0, ASN_OBJECT_ID, 0, 0, 1 },
/*  1 */    { 0, ASN_SEQUENCE, 1, 0, 0 },
};

/* Number of items in ASN.1 template for certificate name hash. */
#define nameHashASN_Length (sizeof(nameHashASN) / sizeof(ASNItem))
#endif /* WOLFSSL_ASN_TEMPLATE */

/* store WC_SHA hash of NAME */
int GetNameHash(const byte* source, word32* idx, byte* hash, int maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    length;  /* length of all distinguished names */
    int    ret;
    word32 dummy;
    byte   tag;

    WOLFSSL_ENTER("GetNameHash");

    dummy = *idx;
    if (GetASNTag(source, &dummy, &tag, maxIdx) == 0 && tag == ASN_OBJECT_ID) {
        WOLFSSL_MSG("Trying optional prefix...");

        if (GetLength(source, idx, &length, maxIdx) < 0)
            return ASN_PARSE_E;

        *idx += length;
        WOLFSSL_MSG("Got optional prefix");
    }

    /* For OCSP, RFC2560 section 4.1.1 states the issuer hash should be
     * calculated over the entire DER encoding of the Name field, including
     * the tag and length. */
    dummy = *idx;
    if (GetSequence(source, idx, &length, maxIdx) < 0)
        return ASN_PARSE_E;

    ret = CalcHashId(source + dummy, length + *idx - dummy, hash);

    *idx += length;

    return ret;
#else
    ASNGetData dataASN[nameHashASN_Length];
    int ret;

    XMEMSET(dataASN, 0, sizeof(dataASN));
    /* Ignore the OID even when present. */
    GetASN_OID(&dataASN[0], oidIgnoreType);
    /* Decode certificate name. */
    ret = GetASN_Items(nameHashASN, dataASN, nameHashASN_Length, 0, source, idx,
           maxIdx);
    if (ret == 0) {
        /* For OCSP, RFC2560 section 4.1.1 states the issuer hash should be
         * calculated over the entire DER encoding of the Name field, including
         * the tag and length. */
        /* Calculate hash of complete name including SEQUENCE. */
        ret = CalcHashId(GetASNItem_Addr(dataASN[1], source),
                         GetASNItem_Length(dataASN[1], source), hash);
    }

    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}


#ifdef HAVE_CRL

/* initialize decoded CRL */
void InitDecodedCRL(DecodedCRL* dcrl, void* heap)
{
    WOLFSSL_MSG("InitDecodedCRL");

    XMEMSET(dcrl, 0, sizeof(DecodedCRL));
    dcrl->heap = heap;
#ifdef WOLFSSL_HEAP_TEST
    dcrl->heap = (void*)WOLFSSL_HEAP_TEST;
#endif
}


/* free decoded CRL resources */
void FreeDecodedCRL(DecodedCRL* dcrl)
{
    RevokedCert* tmp = dcrl->certs;

    WOLFSSL_MSG("FreeDecodedCRL");

    while(tmp) {
        RevokedCert* next = tmp->next;
        XFREE(tmp, dcrl->heap, DYNAMIC_TYPE_REVOKED);
        tmp = next;
    }
}


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for revoked certificates.
 * X.509: RFC 5280, 5.1 - CRL Fields
 */
static const ASNItem revokedASN[] = {
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* userCertificate    CertificateSerialNumber */
/*  1 */        { 1, ASN_INTEGER, 0, 0, 0 },
                /* revocationDate     Time */
/*  2 */        { 1, ASN_UTC_TIME, 0, 0, 2 },
/*  3 */        { 1, ASN_GENERALIZED_TIME, 0, 0, 2 },
                /* crlEntryExensions  Extensions */
/*  4 */        { 1, ASN_SEQUENCE, 1, 0, 1 },
};

/* Number of items in ASN.1 template for revoked certificates. */
#define revokedASN_Length (sizeof(revokedASN) / sizeof(ASNItem))
#endif

/* Get Revoked Cert list, 0 on success */
static int GetRevoked(const byte* buff, word32* idx, DecodedCRL* dcrl,
                      int maxIdx)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int    ret, len;
    word32 end;
    byte   b;
    RevokedCert* rc;

    WOLFSSL_ENTER("GetRevoked");

    if (GetSequence(buff, idx, &len, maxIdx) < 0)
        return ASN_PARSE_E;

    end = *idx + len;

    rc = (RevokedCert*)XMALLOC(sizeof(RevokedCert), dcrl->heap,
                                                          DYNAMIC_TYPE_REVOKED);
    if (rc == NULL) {
        WOLFSSL_MSG("Alloc Revoked Cert failed");
        return MEMORY_E;
    }

    if (GetSerialNumber(buff, idx, rc->serialNumber, &rc->serialSz,
                                                                maxIdx) < 0) {
        XFREE(rc, dcrl->heap, DYNAMIC_TYPE_REVOKED);
        return ASN_PARSE_E;
    }

    /* add to list */
    rc->next = dcrl->certs;
    dcrl->certs = rc;
    dcrl->totalCerts++;

    /* get date */
    ret = GetDateInfo(buff, idx, NULL, &b, NULL, maxIdx);
    if (ret < 0) {
        WOLFSSL_MSG("Expecting Date");
        return ret;
    }

    /* skip extensions */
    *idx = end;

    return 0;
#else
    DECL_ASNGETDATA(dataASN, revokedASN_Length);
    int ret = 0;
    word32 serialSz = EXTERNAL_SERIAL_SIZE;
    RevokedCert* rc;

    /* Allocate a new revoked certificate object. */
    rc = (RevokedCert*)XMALLOC(sizeof(RevokedCert), dcrl->heap,
            DYNAMIC_TYPE_CRL);
    if (rc == NULL) {
        ret = MEMORY_E;
    }

    CALLOC_ASNGETDATA(dataASN, revokedASN_Length, ret, dcrl->heap);

    if (ret == 0) {
        /* Set buffer to place serial number into. */
        GetASN_Buffer(&dataASN[1], rc->serialNumber, &serialSz);
        /* Decode the Revoked */
        ret = GetASN_Items(revokedASN, dataASN, revokedASN_Length, 1, buff, idx,
                maxIdx);
    }
    if (ret == 0) {
        /* Store size of serial number. */
        rc->serialSz = serialSz;
        /* TODO: use revocation date */
        /* TODO: use extensions, only v2 */
        /* Add revoked certificate to chain. */
        rc->next = dcrl->certs;
        dcrl->certs = rc;
        dcrl->totalCerts++;
    }

    FREE_ASNGETDATA(dataASN, dcrl->heap);
    if ((ret != 0) && (rc != NULL)) {
        XFREE(rc, dcrl->heap, DYNAMIC_TYPE_CRL);
    }
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* Parse the revoked certificates of a CRL.
 *
 * @param [in] dcrl    Decoded CRL object.
 * @param [in] buff    Buffer holding CRL.
 * @param [in] idx     Index into buffer of revoked certificates.
 * @param [in] maxIdx  Maximum index of revoked cartificates data.
 * @return  0 on success.
 * @return  ASN_PARSE_E on failure.
 */
static int ParseCRL_RevokedCerts(DecodedCRL* dcrl, const byte* buff, word32 idx,
    word32 maxIdx)
{
    int ret = 0;

    /* Parse each revoked cerificate. */
    while ((ret == 0) && (idx < maxIdx)) {
        /* Parse a revoked certificate. */
        if (GetRevoked(buff, &idx, dcrl, maxIdx) < 0) {
            ret = ASN_PARSE_E;
        }
    }

    return ret;
}
#endif /* WOLFSSL_ASN_TEMPLATE */

#ifndef WOLFSSL_ASN_TEMPLATE
/* Get CRL Signature, 0 on success */
static int GetCRL_Signature(const byte* source, word32* idx, DecodedCRL* dcrl,
                            int maxIdx)
{
    int    length;
    int    ret;

    WOLFSSL_ENTER("GetCRL_Signature");

    ret = CheckBitString(source, idx, &length, maxIdx, 1, NULL);
    if (ret != 0)
        return ret;
    dcrl->sigLength = length;

    dcrl->signature = (byte*)&source[*idx];
    *idx += dcrl->sigLength;

    return 0;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */

int VerifyCRL_Signature(SignatureCtx* sigCtx, const byte* toBeSigned,
                        word32 tbsSz, const byte* signature, word32 sigSz,
                        word32 signatureOID, Signer *ca, void* heap)
{
    /* try to confirm/verify signature */
#ifndef IGNORE_KEY_EXTENSIONS
    if ((ca->keyUsage & KEYUSE_CRL_SIGN) == 0) {
        WOLFSSL_MSG("CA cannot sign CRLs");
        return ASN_CRL_NO_SIGNER_E;
    }
#endif /* IGNORE_KEY_EXTENSIONS */

    InitSignatureCtx(sigCtx, heap, INVALID_DEVID);
    if (ConfirmSignature(sigCtx, toBeSigned, tbsSz, ca->publicKey,
                         ca->pubKeySize, ca->keyOID, signature, sigSz,
                         signatureOID, NULL) != 0) {
        WOLFSSL_MSG("CRL Confirm signature failed");
        return ASN_CRL_CONFIRM_E;
    }

    return 0;
}

#ifdef WOLFSSL_ASN_TEMPLATE
/* Find the signer for the CRL and verify the signature.
 *
 * @param [in] dcrl  Decoded CRL object.
 * @param [in] buff  Buffer holding CRL.
 * @param [in] cm    Certificate manager object.
 * @return  0 on success.
 * @return  ASN_CRL_NO_SIGNER_E when no signer found.
 * @return  ASN_CRL_CONFIRM_E when signature did not verify.
 */
static int PaseCRL_CheckSignature(DecodedCRL* dcrl, const byte* buff, void* cm)
{
    int ret = 0;
    Signer* ca = NULL;
    SignatureCtx sigCtx;

    /* OpenSSL doesn't add skid by default for CRLs cause firefox chokes.
     * If experiencing issues uncomment NO_SKID define in CRL section of
     * wolfssl/wolfcrypt/settings.h */
#ifndef NO_SKID
    if (dcrl->extAuthKeyIdSet) {
        /* more unique than issuerHash */
        ca = GetCA(cm, dcrl->extAuthKeyId);
    }
    /* Check issuerHash matched CA's subjectNameHash. */
    if ((ca != NULL) && (XMEMCMP(dcrl->issuerHash, ca->subjectNameHash,
                                 KEYID_SIZE) != 0)) {
        ca = NULL;
    }
    if (ca == NULL) {
        ca = GetCAByName(cm, dcrl->issuerHash); /* last resort */
        /* If AKID is available then this CA doesn't have the public
         * key required */
        if (ca && dcrl->extAuthKeyIdSet) {
            WOLFSSL_MSG("CA SKID doesn't match AKID");
            ca = NULL;
        }
    }
#else
    ca = GetCA(cm, dcrl->issuerHash);
#endif /* !NO_SKID */
    WOLFSSL_MSG("About to verify CRL signature");

    if (ca == NULL) {
        WOLFSSL_MSG("Did NOT find CRL issuer CA");
        ret = ASN_CRL_NO_SIGNER_E;
    }

    if (ret == 0) {
        WOLFSSL_MSG("Found CRL issuer CA");
        /* Verify CRL signature with CA. */
        ret = VerifyCRL_Signature(&sigCtx, buff + dcrl->certBegin,
           dcrl->sigIndex - dcrl->certBegin, dcrl->signature, dcrl->sigLength,
           dcrl->signatureOID, ca, dcrl->heap);
    }

    return ret;
}
#endif

#ifndef WOLFSSL_ASN_TEMPLATE
static int ParseCRL_CertList(DecodedCRL* dcrl, const byte* buf,
        word32* inOutIdx, int sz)
{
    word32 oid, dateIdx, idx, checkIdx;
    int version;
#ifdef WOLFSSL_NO_CRL_NEXT_DATE
    int doNextDate = 1;
#endif
    byte tag;

    if (dcrl == NULL || inOutIdx == NULL || buf == NULL) {
        return BAD_FUNC_ARG;
    }

    /* may have version */
    idx = *inOutIdx;

    checkIdx = idx;
    if (GetASNTag(buf, &checkIdx, &tag, sz) == 0 && tag == ASN_INTEGER) {
        if (GetMyVersion(buf, &idx, &version, sz) < 0)
            return ASN_PARSE_E;
    }

    if (GetAlgoId(buf, &idx, &oid, oidIgnoreType, sz) < 0)
        return ASN_PARSE_E;

    if (GetNameHash(buf, &idx, dcrl->issuerHash, sz) < 0)
        return ASN_PARSE_E;

    if (GetBasicDate(buf, &idx, dcrl->lastDate, &dcrl->lastDateFormat, sz) < 0)
        return ASN_PARSE_E;

    dateIdx = idx;

    if (GetBasicDate(buf, &idx, dcrl->nextDate, &dcrl->nextDateFormat, sz) < 0)
    {
#ifndef WOLFSSL_NO_CRL_NEXT_DATE
        (void)dateIdx;
        return ASN_PARSE_E;
#else
        dcrl->nextDateFormat = ASN_OTHER_TYPE;  /* skip flag */
        doNextDate = 0;
        idx = dateIdx;
#endif
    }

#ifdef WOLFSSL_NO_CRL_NEXT_DATE
    if (doNextDate)
#endif
    {
#ifndef NO_ASN_TIME
        if (!XVALIDATE_DATE(dcrl->nextDate, dcrl->nextDateFormat, AFTER)) {
            WOLFSSL_MSG("CRL after date is no longer valid");
            return CRL_CERT_DATE_ERR;
        }
#endif
    }

    checkIdx = idx;
    if (idx != dcrl->sigIndex &&
           GetASNTag(buf, &checkIdx, &tag, sz) == 0 && tag != CRL_EXTENSIONS) {

        int len;

        if (GetSequence(buf, &idx, &len, sz) < 0)
            return ASN_PARSE_E;
        len += idx;

        while (idx < (word32)len) {
            if (GetRevoked(buf, &idx, dcrl, len) < 0)
                return ASN_PARSE_E;
        }
    }

    *inOutIdx = idx;

    return 0;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */


#ifndef NO_SKID
static int ParseCRL_AuthKeyIdExt(const byte* input, int sz, DecodedCRL* dcrl)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    word32 idx = 0;
    int length = 0, ret = 0;
    byte tag;

    WOLFSSL_ENTER("ParseCRL_AuthKeyIdExt");

    if (GetSequence(input, &idx, &length, sz) < 0) {
        WOLFSSL_MSG("\tfail: should be a SEQUENCE\n");
        return ASN_PARSE_E;
    }

    if (GetASNTag(input, &idx, &tag, sz) < 0) {
        return ASN_PARSE_E;
    }

    if (tag != (ASN_CONTEXT_SPECIFIC | 0)) {
        WOLFSSL_MSG("\tinfo: OPTIONAL item 0, not available\n");
        return 0;
    }

    if (GetLength(input, &idx, &length, sz) <= 0) {
        WOLFSSL_MSG("\tfail: extension data length");
        return ASN_PARSE_E;
    }

    dcrl->extAuthKeyIdSet = 1;
    /* Get the hash or hash of the hash if wrong size. */
    ret = GetHashId(input + idx, length, dcrl->extAuthKeyId);

    return ret;
#else
    DECL_ASNGETDATA(dataASN, authKeyIdASN_Length);
    int ret = 0;
    word32 idx = 0;

    WOLFSSL_ENTER("ParseCRL_AuthKeyIdExt");

    CALLOC_ASNGETDATA(dataASN, authKeyIdASN_Length, ret, dcrl->heap);

    if (ret == 0) {
        /* Parse an authority key identifier. */
        ret = GetASN_Items(authKeyIdASN, dataASN, authKeyIdASN_Length, 1, input,
                           &idx, sz);
    }
    if (ret == 0) {
        /* Key id is optional. */
        if (dataASN[1].data.ref.data == NULL) {
            WOLFSSL_MSG("\tinfo: OPTIONAL item 0, not available");
        }
        else {
            /* Get the hash or hash of the hash if wrong size. */
            ret = GetHashId(dataASN[1].data.ref.data,
                dataASN[1].data.ref.length, dcrl->extAuthKeyId);
        }
    }

    FREE_ASNGETDATA(dataASN, dcrl->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}
#endif


#ifndef WOLFSSL_ASN_TEMPLATE
static int ParseCRL_Extensions(DecodedCRL* dcrl, const byte* buf,
        word32* inOutIdx, word32 sz)
{
    int length;
    word32 idx;
    word32 ext_bound; /* boundary index for the sequence of extensions */
    word32 oid;
    byte tag;

    WOLFSSL_ENTER("ParseCRL_Extensions");
    (void)dcrl;

    if (inOutIdx == NULL)
        return BAD_FUNC_ARG;

    idx = *inOutIdx;

    /* CRL Extensions are optional */
    if ((idx + 1) > sz)
        return 0;

    /* CRL Extensions are optional */
    if (GetASNTag(buf, &idx, &tag, sz) < 0)
        return 0;

    /* CRL Extensions are optional */
    if (tag != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
        return 0;

    if (GetLength(buf, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    if (GetSequence(buf, &idx, &length, sz) < 0)
        return ASN_PARSE_E;

    ext_bound = idx + length;

    while (idx < (word32)ext_bound) {
        word32 localIdx;
        int ret;

        if (GetSequence(buf, &idx, &length, sz) < 0) {
            WOLFSSL_MSG("\tfail: should be a SEQUENCE");
            return ASN_PARSE_E;
        }

        oid = 0;
        if (GetObjectId(buf, &idx, &oid, oidCrlExtType, sz) < 0) {
            WOLFSSL_MSG("\tfail: OBJECT ID");
            return ASN_PARSE_E;
        }

        /* check for critical flag */
        if ((idx + 1) > (word32)sz) {
            WOLFSSL_MSG("\tfail: malformed buffer");
            return BUFFER_E;
        }

        localIdx = idx;
        if (GetASNTag(buf, &localIdx, &tag, sz) == 0 && tag == ASN_BOOLEAN) {
            WOLFSSL_MSG("\tfound optional critical flag, moving past");
            ret = GetBoolean(buf, &idx, sz);
            if (ret < 0)
                return ret;
        }

        ret = GetOctetString(buf, &idx, &length, sz);
        if (ret < 0)
            return ret;

        if (oid == AUTH_KEY_OID) {
        #ifndef NO_SKID
            ret = ParseCRL_AuthKeyIdExt(buf + idx, length, dcrl);
            if (ret < 0) {
                WOLFSSL_MSG("\tcouldn't parse AuthKeyId extension");
                return ret;
            }
        #endif
        }

        idx += length;
    }

    *inOutIdx = idx;

    return 0;
}
#else
/* Parse the extensions of a CRL.
 *
 * @param [in] dcrl    Decoded CRL object.
 * @param [in] buff    Buffer holding CRL.
 * @param [in] idx     Index into buffer of extensions.
 * @param [in] maxIdx  Maximum index of extension data.
 * @return  0 on success.
 * @return  ASN_PARSE_E on failure.
 */
static int ParseCRL_Extensions(DecodedCRL* dcrl, const byte* buf, word32 idx,
        word32 maxIdx)
{
    DECL_ASNGETDATA(dataASN, certExtASN_Length);
    int ret = 0;

    ALLOC_ASNGETDATA(dataASN, certExtASN_Length, ret, dcrl->heap);

    while ((ret == 0) && (idx < maxIdx)) {
        byte critical = 0;

        /* Clear dynamic data. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * certExtASN_Length);
        /* Ensure OID is an extention type. */
        GetASN_OID(&dataASN[1], oidCertExtType);
        /* Set criticality variable. */
        dataASN[2].data.u8 = &critical;
        /* Parse extension wrapper. */
        ret = GetASN_Items(certExtASN, dataASN, certExtASN_Length, 0, buf, &idx,
                maxIdx);
        if (ret == 0) {
            /* OID in extension. */
            word32 oid = dataASN[1].data.oid.sum;
            /* Length of extension data. */
            int length = dataASN[3].length;

            if (oid == AUTH_KEY_OID) {
            #ifndef NO_SKID
                /* Parse Authority Key Id extesion.
                 * idx is at start of OCTET_STRING data. */
                ret = ParseCRL_AuthKeyIdExt(buf + idx, length, dcrl);
                if (ret != 0) {
                    WOLFSSL_MSG("\tcouldn't parse AuthKeyId extension");
                }
            #endif
            }
            /* TODO: Parse CRL Number extension */
            /* TODO: check criticality */
            /* Move index on to next extension. */
            idx += length;
        }
    }

    if (ret < 0) {
        ret = ASN_PARSE_E;
    }
    return ret;
}
#endif /* !WOLFSSL_ASN_TEMPLATE */


#ifdef WOLFSSL_ASN_TEMPLATE
/* ASN.1 template for a CRL- CertificateList.
 * X.509: RFC 5280, 5.1 - CRL Fields
 */
static const ASNItem crlASN[] = {
            /* CertificateList */
/*  0 */    { 0, ASN_SEQUENCE, 1, 1, 0 },
                /* tbsCertList */
/*  1 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
                    /* version     Version OPTIONAL if present must be v2 */
/*  2 */            { 2, ASN_INTEGER, 0, 0, 1 },
                    /* signature */
/*  3 */            { 2, ASN_SEQUENCE, 1, 1, 0 },
/*  4 */                { 3, ASN_OBJECT_ID, 0, 0, 0 },
/*  5 */                { 3, ASN_TAG_NULL, 0, 0, 1 },
                    /* issuer */
/*  6 */            { 2, ASN_SEQUENCE, 1, 0, 0 },
                    /* thisUpdate */
/*  7 */            { 2, ASN_UTC_TIME, 0, 0, 2 },
/*  8 */            { 2, ASN_GENERALIZED_TIME, 0, 0, 2 },
                    /* nextUpdate */
/*  9 */            { 2, ASN_UTC_TIME, 0, 0, 3 },
/* 10 */            { 2, ASN_GENERALIZED_TIME, 0, 0, 3 },
                    /* revokedCertificates */
/* 11 */            { 2, ASN_SEQUENCE, 1, 0, 1 },
                    /* crlExtensions */
/* 12 */            { 2, ASN_CONTEXT_SPECIFIC | 0, 1, 1, 1 },
/* 13 */                { 3, ASN_SEQUENCE, 1, 0, 0 },
                /* signatureAlgorithm */
/* 14 */        { 1, ASN_SEQUENCE, 1, 1, 0 },
/* 15 */            { 2, ASN_OBJECT_ID, 0, 0, 0 },
/* 16 */            { 2, ASN_TAG_NULL, 0, 0, 1 },
                /* signatureValue */
/* 17 */        { 1, ASN_BIT_STRING, 0, 0, 0 },
};

/* Number of items in ASN.1 template for a CRL- CertificateList. */
#define crlASN_Length (sizeof(crlASN) / sizeof(ASNItem))
#endif

/* parse crl buffer into decoded state, 0 on success */
int ParseCRL(DecodedCRL* dcrl, const byte* buff, word32 sz, void* cm)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    Signer*      ca = NULL;
    SignatureCtx sigCtx;
    int          ret = 0;
    int          len;
    word32       idx = 0;

    WOLFSSL_MSG("ParseCRL");

    /* raw crl hash */
    /* hash here if needed for optimized comparisons
     * wc_Sha sha;
     * wc_InitSha(&sha);
     * wc_ShaUpdate(&sha, buff, sz);
     * wc_ShaFinal(&sha, dcrl->crlHash); */

    if (GetSequence(buff, &idx, &len, sz) < 0)
        return ASN_PARSE_E;

    dcrl->certBegin = idx;
    /* Normalize sz for the length inside the outer sequence. */
    sz = len + idx;

    if (GetSequence(buff, &idx, &len, sz) < 0)
        return ASN_PARSE_E;
    dcrl->sigIndex = len + idx;

    if (ParseCRL_CertList(dcrl, buff, &idx, dcrl->sigIndex) < 0)
        return ASN_PARSE_E;

    if (ParseCRL_Extensions(dcrl, buff, &idx, dcrl->sigIndex) < 0)
        return ASN_PARSE_E;

    idx = dcrl->sigIndex;

    if (GetAlgoId(buff, &idx, &dcrl->signatureOID, oidSigType, sz) < 0)
        return ASN_PARSE_E;

    if (GetCRL_Signature(buff, &idx, dcrl, sz) < 0)
        return ASN_PARSE_E;

    /* openssl doesn't add skid by default for CRLs cause firefox chokes
       if experiencing issues uncomment NO_SKID define in CRL section of
       wolfssl/wolfcrypt/settings.h */
#ifndef NO_SKID
    if (dcrl->extAuthKeyIdSet) {
        ca = GetCA(cm, dcrl->extAuthKeyId); /* more unique than issuerHash */
    }
    if (ca != NULL && XMEMCMP(dcrl->issuerHash, ca->subjectNameHash,
                KEYID_SIZE) != 0) {
        ca = NULL;
    }
    if (ca == NULL) {
        ca = GetCAByName(cm, dcrl->issuerHash); /* last resort */
        /* If AKID is available then this CA doesn't have the public
         * key required */
        if (ca && dcrl->extAuthKeyIdSet) {
            WOLFSSL_MSG("CA SKID doesn't match AKID");
            ca = NULL;
        }
    }
#else
    ca = GetCA(cm, dcrl->issuerHash);
#endif /* !NO_SKID */
    WOLFSSL_MSG("About to verify CRL signature");

    if (ca == NULL) {
        WOLFSSL_MSG("Did NOT find CRL issuer CA");
        ret = ASN_CRL_NO_SIGNER_E;
        goto end;
    }

    WOLFSSL_MSG("Found CRL issuer CA");
    ret = VerifyCRL_Signature(&sigCtx, buff + dcrl->certBegin,
           dcrl->sigIndex - dcrl->certBegin, dcrl->signature, dcrl->sigLength,
           dcrl->signatureOID, ca, dcrl->heap);

end:
    return ret;
#else
    DECL_ASNGETDATA(dataASN, crlASN_Length);
    int ret = 0;
    /* Default version - v1 = 0 */
    byte version = 0;
    word32 idx = 0;
    /* Size of buffer for date. */
    word32 lastDateSz = MAX_DATE_SIZE;
    word32 nextDateSz = MAX_DATE_SIZE;

    WOLFSSL_MSG("ParseCRL");

    CALLOC_ASNGETDATA(dataASN, crlASN_Length, ret, dcrl->heap);

    if (ret == 0) {
        /* Set variable to store version. */
        GetASN_Int8Bit(&dataASN[2], &version);
        /* Set expecting signature OID. */
        GetASN_OID(&dataASN[4], oidSigType);
        /* Set buffer to put last and next date into. */
        GetASN_Buffer(&dataASN[7], dcrl->lastDate, &lastDateSz);
        GetASN_Buffer(&dataASN[8], dcrl->lastDate, &lastDateSz);
        GetASN_Buffer(&dataASN[9], dcrl->nextDate, &nextDateSz);
        GetASN_Buffer(&dataASN[10], dcrl->nextDate, &nextDateSz);
        /* Set expecting signature OID. */
        GetASN_OID(&dataASN[14], oidSigType);
        /* Decode the CRL. */
        ret = GetASN_Items(crlASN, dataASN, crlASN_Length, 1, buff, &idx, sz);
    }
    /* Version must be v2 = 1 if present. */
    if ((ret == 0) && (dataASN[2].tag != 0) && (version != 1)) {
        ret = ASN_PARSE_E;
    }
    /* Check minimum size of last date. */
    if ((ret == 0) && (lastDateSz < MIN_DATE_SIZE)) {
        ret = ASN_PARSE_E;
    }
    /* Check minimum size of next date. */
    if ((ret == 0) && (nextDateSz < MIN_DATE_SIZE)) {
        ret = ASN_PARSE_E;
    }
    /* 'signatureAlgorithm' OID must be the same as 'signature' OID. */
    if ((ret == 0) && (dataASN[15].data.oid.sum != dataASN[4].data.oid.sum)) {
        ret = ASN_PARSE_E;
    }
    if (ret == 0) {
        /* Store offset of to be signed part. */
        dcrl->certBegin = dataASN[1].offset;
        /* Store index of signature. */
        dcrl->sigIndex = dataASN[14].offset;
        /* Store address and length of signature data. */
        GetASN_GetRef(&dataASN[17], &dcrl->signature, &dcrl->sigLength);
        /* Get the signature OID. */
        dcrl->signatureOID = dataASN[15].data.oid.sum;
        /* Get the format/tag of the last and next date. */
        dcrl->lastDateFormat = (dataASN[7].tag != 0) ? dataASN[7].tag
                                                     : dataASN[8].tag;
        dcrl->nextDateFormat = (dataASN[9].tag != 0) ? dataASN[9].tag
                                                     : dataASN[10].tag;
    #ifndef NO_ASN_TIME
        if (dcrl->nextDateFormat != 0) {
            /* Next date was set, so validate it. */
            if (!XVALIDATE_DATE(dcrl->nextDate, dcrl->nextDateFormat, AFTER)) {
                WOLFSSL_MSG("CRL after date is no longer valid");
                ret = CRL_CERT_DATE_ERR;
            }
        }
    }
    if (ret == 0) {
    #endif
        /* Calculate the Hash id from the issuer name. */
        ret = CalcHashId(GetASNItem_Addr(dataASN[6], buff),
                         GetASNItem_Length(dataASN[6], buff), dcrl->issuerHash);
        if (ret < 0) {
            ret = ASN_PARSE_E;
        }
    }
    if ((ret == 0) && (dataASN[11].tag != 0)) {
        /* Parse revoked cerificates - starting after SEQUENCE OF. */
        ret = ParseCRL_RevokedCerts(dcrl, buff,
            GetASNItem_DataIdx(dataASN[11], buff),
            GetASNItem_EndIdx(dataASN[11], buff));
    }
    if (ret == 0) {
        /* Parse the extensions - starting after SEQUENCE OF. */
        ret = ParseCRL_Extensions(dcrl, buff,
            GetASNItem_DataIdx(dataASN[13], buff),
            GetASNItem_EndIdx(dataASN[13], buff));
    }
    if (ret == 0) {
        /* Find signer and verify signature. */
        ret = PaseCRL_CheckSignature(dcrl, buff, cm);
    }

    FREE_ASNGETDATA(dataASN, dcrl->heap);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#endif /* HAVE_CRL */



#ifdef WOLFSSL_CERT_PIV

#ifdef WOLFSSL_ASN_TEMPLATE
/* Template for PIV. */
static const ASNItem pivASN[] = {
/*  0 */    { 0, ASN_PIV_CERT, 0, 0, 0 },
/*  1 */    { 0, ASN_PIV_NONCE, 0, 0, 1 },
/*  2 */    { 0, ASN_PIV_SIGNED_NONCE, 0, 0, 1 },
};

#define pivASN_Length (sizeof(pivASN) / sizeof(ASNItem))

static const ASNItem pivCertASN[] = {
                /* 0x53 = 0x40 | 0x13 */
/*  0 */        { 1, ASN_APPLICATION | 0x13, 0, 1, 0 },
                     /* 0x70 = 0x40 | 0x10 + 0x20 (CONSTRUCTED) */
/*  1 */             { 2, ASN_APPLICATION | 0x10, 1, 0, 0 },
                     /* 0x71 = 0x40 | 0x11 + 0x20 (CONSTRUCTED) */
/*  2 */             { 2, ASN_APPLICATION | 0x11, 1, 0, 1 },
                     /* 0xFE = 0xC0 | 0x1E + 0x20 (CONSTRUCTED) */
/*  3 */             { 2, ASN_PRIVATE | 0x1e, 1, 0, 1 },
};

#define pivCertASN_Length (sizeof(pivCertASN) / sizeof(ASNItem))
#endif

int wc_ParseCertPIV(wc_CertPIV* piv, const byte* buf, word32 totalSz)
{
#ifndef WOLFSSL_ASN_TEMPLATE
    int length = 0;
    word32 idx = 0;

    WOLFSSL_ENTER("wc_ParseCertPIV");

    if (piv == NULL || buf == NULL || totalSz == 0)
        return BAD_FUNC_ARG;

    XMEMSET(piv, 0, sizeof(wc_CertPIV));

    /* Detect Identiv PIV (with 0x0A, 0x0B and 0x0C sections) */
    /* Certificate (0A 82 05FA) */
    if (GetASNHeader(buf, ASN_PIV_CERT, &idx, &length, totalSz) >= 0) {
        /* Identiv Type PIV card */
        piv->isIdentiv = 1;

        piv->cert =   &buf[idx];
        piv->certSz = length;
        idx += length;

        /* Nonce (0B 14) */
        if (GetASNHeader(buf, ASN_PIV_NONCE, &idx, &length, totalSz) >= 0) {
            piv->nonce =   &buf[idx];
            piv->nonceSz = length;
            idx += length;
        }

        /* Signed Nonce (0C 82 0100) */
        if (GetASNHeader(buf, ASN_PIV_SIGNED_NONCE, &idx, &length, totalSz) >= 0) {
            piv->signedNonce =   &buf[idx];
            piv->signedNonceSz = length;
        }

        idx = 0;
        buf = piv->cert;
        totalSz = piv->certSz;
    }

    /* Certificate Buffer Total Size (53 82 05F6) */
    if (GetASNHeader(buf, ASN_APPLICATION | ASN_PRINTABLE_STRING, &idx,
                                                   &length, totalSz) < 0) {
        return ASN_PARSE_E;
    }
    /* PIV Certificate (70 82 05ED) */
    if (GetASNHeader(buf, ASN_PIV_TAG_CERT, &idx, &length,
                                                         totalSz) < 0) {
        return ASN_PARSE_E;
    }

    /* Capture certificate buffer pointer and length */
    piv->cert =   &buf[idx];
    piv->certSz = length;
    idx += length;

    /* PIV Certificate Info (71 01 00) */
    if (GetASNHeader(buf, ASN_PIV_TAG_CERT_INFO, &idx, &length,
                                                        totalSz) >= 0) {
        if (length >= 1) {
            piv->compression = (buf[idx] & ASN_PIV_CERT_INFO_COMPRESSED);
            piv->isX509 =      ((buf[idx] & ASN_PIV_CERT_INFO_ISX509) != 0);
        }
        idx += length;
    }

    /* PIV Error Detection (FE 00) */
    if (GetASNHeader(buf, ASN_PIV_TAG_ERR_DET, &idx, &length,
                                                        totalSz) >= 0) {
        piv->certErrDet =   &buf[idx];
        piv->certErrDetSz = length;
        idx += length;
    }

    return 0;
#else
    /* pivCertASN_Length is longer than pivASN_Length */
    DECL_ASNGETDATA(dataASN, pivCertASN_Length);
    int ret = 0;
    word32 idx;
    byte info;

    WOLFSSL_ENTER("wc_ParseCertPIV");

    ALLOC_ASNGETDATA(dataASN, pivCertASN_Length, ret, NULL);

    if (ret == 0) {
        /* Clear dynamic data. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * pivASN_Length);
        /* Start parsing from start of buffer. */
        idx = 0;
        /* Parse Identiv wrapper. */
        ret = GetASN_Items(pivASN, dataASN, pivASN_Length, 1, buf, &idx,
                totalSz);
        if (ret == 0) {
            /* Identiv wrapper found. */
            piv->isIdentiv = 1;
            /* Get nonce reference. */
            if (dataASN[1].tag != 0) {
                GetASN_GetConstRef(&dataASN[1], &piv->nonce, &piv->nonceSz);
            }
            /* Get signedNonce reference. */
            if (dataASN[2].tag != 0) {
                GetASN_GetConstRef(&dataASN[2], &piv->signedNonce,
                    &piv->signedNonceSz);
            }
            /* Get the certificate data for parsing. */
            GetASN_GetConstRef(&dataASN[0], &buf, &totalSz);
        }
        ret = 0;
    }
    if (ret == 0) {
        /* Clear dynamic data and set variable to put cert info into. */
        XMEMSET(dataASN, 0, sizeof(*dataASN) * pivCertASN_Length);
        GetASN_Int8Bit(&dataASN[2], &info);
        /* Start parsing from start of buffer. */
        idx = 0;
        /* Parse PIV cetificate data. */
        ret = GetASN_Items(pivCertASN, dataASN, pivCertASN_Length, 1, buf, &idx,
                totalSz);
        if (ret == 0) {
            /* Get X.509 certificate reference. */
            GetASN_GetConstRef(&dataASN[1], &piv->cert, &piv->certSz);
            /* Set the certificate info if available. */
            if (dataASN[2].tag != 0) {
                /* Bits 1 and 2 are compression. */
                piv->compression = info & ASN_PIV_CERT_INFO_COMPRESSED;
                /* Bits 3 is X509 flag. */
                piv->isX509 = ((info & ASN_PIV_CERT_INFO_ISX509) != 0);
            }
            /* Get X.509 certificate error detecton reference. */
            GetASN_GetConstRef(&dataASN[3], &piv->certErrDet,
                     &piv->certErrDetSz);
        }
        ret = 0;
    }

    FREE_ASNGETDATA(dataASN, NULL);
    return ret;
#endif /* WOLFSSL_ASN_TEMPLATE */
}

#endif /* WOLFSSL_CERT_PIV */



#ifdef HAVE_SMIME

/*****************************************************************************
* wc_MIME_parse_headers - Reads the char array in and parses out MIME headers
* and parameters into headers.  Will continue until in has no more content.
*
* RETURNS:
* returns zero on success, non-zero on error.
*/
int wc_MIME_parse_headers(char* in, int inLen, MimeHdr** headers)
{
    MimeHdr* nextHdr = NULL;
    MimeHdr* curHdr = NULL;
    MimeParam* nextParam = NULL;
    size_t start = 0;
    size_t end = 0;
    char* nameAttr = NULL;
    char* bodyVal = NULL;
    MimeTypes mimeType = MIME_HDR;
    MimeStatus mimeStatus = MIME_NAMEATTR;
    int ret = -1;
    size_t pos = 0;
    size_t lineLen = 0;
    char* curLine = NULL;
    char* ptr = NULL;

    if (in == NULL || inLen <= 0 || in[inLen] != '\0' || headers == NULL) {
        ret = BAD_FUNC_ARG;
        goto error;
    }
    nextHdr = (MimeHdr*)XMALLOC(sizeof(MimeHdr), NULL, DYNAMIC_TYPE_PKCS7);
    nextParam = (MimeParam*)XMALLOC(sizeof(MimeParam), NULL,
                                    DYNAMIC_TYPE_PKCS7);
    if (nextHdr == NULL || nextParam == NULL) {
        ret = MEMORY_E;
        goto error;
    }
    XMEMSET(nextHdr, 0, (word32)sizeof(MimeHdr));
    XMEMSET(nextParam, 0, (word32)sizeof(MimeParam));

    curLine = XSTRTOK(in, "\r\n", &ptr);
    if (curLine == NULL) {
        ret = ASN_PARSE_E;
        goto error;
    }

    while (curLine != NULL) {
        /* Leftover from previous line, add params to previous header. */
        if (curLine[0] == ' ' && curHdr) {
            mimeType = MIME_PARAM;
        }
        else {
            mimeType = MIME_HDR;
        }
        start = 0;
        lineLen = XSTRLEN(curLine);
        if (lineLen == 0) {
            ret = BAD_FUNC_ARG;
            goto error;
        }

        for (pos = 0; pos < lineLen; pos++) {
            char cur = curLine[pos];

            if (mimeStatus == MIME_NAMEATTR && ((cur == ':' &&
                mimeType == MIME_HDR) || (cur == '=' &&
                mimeType == MIME_PARAM)) && pos >= 1) {
                mimeStatus = MIME_BODYVAL;
                end = pos-1;
                if (nameAttr != NULL)
                    XFREE(nameAttr, NULL, DYNAMIC_TYPE_PKCS7);
                ret = wc_MIME_header_strip(curLine, &nameAttr, start, end);
                if (ret) {
                    goto error;
                }
                start = pos+1;
            }
            else if (mimeStatus == MIME_BODYVAL && cur == ';' && pos >= 1) {
                end = pos-1;
                if (bodyVal != NULL)
                    XFREE(bodyVal, NULL, DYNAMIC_TYPE_PKCS7);
                ret = wc_MIME_header_strip(curLine, &bodyVal, start, end);
                if (ret) {
                    goto error;
                }
                if (mimeType == MIME_HDR) {
                    nextHdr->name = nameAttr;
                    nameAttr = NULL;
                    nextHdr->body = bodyVal;
                    bodyVal = NULL;
                    nextHdr->next = curHdr;
                    curHdr = nextHdr;
                    nextHdr = (MimeHdr*)XMALLOC(sizeof(MimeHdr), NULL,
                                                DYNAMIC_TYPE_PKCS7);
                    if (nextHdr == NULL) {
                        ret = MEMORY_E;
                        goto error;
                    }
                    XMEMSET(nextHdr, 0, (word32)sizeof(MimeHdr));
                }
                else {
                    nextParam->attribute = nameAttr;
                    nameAttr = NULL;
                    nextParam->value = bodyVal;
                    bodyVal = NULL;
                    nextParam->next = curHdr->params;
                    curHdr->params = nextParam;
                    nextParam = (MimeParam*)XMALLOC(sizeof(MimeParam), NULL,
                                                    DYNAMIC_TYPE_PKCS7);
                    if (nextParam == NULL) {
                        ret = MEMORY_E;
                        goto error;
                    }
                    XMEMSET(nextParam, 0, (word32)sizeof(MimeParam));
                }
                mimeType = MIME_PARAM;
                mimeStatus = MIME_NAMEATTR;
                start = pos+1;
            }
        }

        end = lineLen-1;
        /* Omit newline characters. */
        while ((curLine[end] == '\r' || curLine[end] == '\n') && end > 0) {
            end--;
        }
        if (end >= start && mimeStatus == MIME_BODYVAL) {
            ret = wc_MIME_header_strip(curLine, &bodyVal, start, end);
            if (ret) {
                goto error;
            }
            if (mimeType == MIME_HDR) {
                nextHdr->name = nameAttr;
                nameAttr = NULL;
                nextHdr->body = bodyVal;
                bodyVal = NULL;
                nextHdr->next = curHdr;
                curHdr = nextHdr;
                nextHdr = (MimeHdr*)XMALLOC(sizeof(MimeHdr), NULL,
                                            DYNAMIC_TYPE_PKCS7);
                if (nextHdr == NULL) {
                    ret = MEMORY_E;
                    goto error;
                }
                XMEMSET(nextHdr, 0, (word32)sizeof(MimeHdr));
            } else {
                nextParam->attribute = nameAttr;
                nameAttr = NULL;
                nextParam->value = bodyVal;
                bodyVal = NULL;
                nextParam->next = curHdr->params;
                curHdr->params = nextParam;
                nextParam = (MimeParam*)XMALLOC(sizeof(MimeParam), NULL,
                                                DYNAMIC_TYPE_PKCS7);
                if (nextParam == NULL) {
                    ret = MEMORY_E;
                    goto error;
                }
                XMEMSET(nextParam, 0, (word32)sizeof(MimeParam));
            }
        }

        curLine = XSTRTOK(NULL, "\r\n", &ptr);
        mimeStatus = MIME_NAMEATTR;
    }

    *headers = curHdr;
    ret = 0; /* success if at this point */

error:
    if (ret != 0)
        wc_MIME_free_hdrs(curHdr);
    wc_MIME_free_hdrs(nextHdr);
    if (nameAttr != NULL)
        XFREE(nameAttr, NULL, DYNAMIC_TYPE_PKCS7);
    if (bodyVal != NULL)
        XFREE(bodyVal, NULL, DYNAMIC_TYPE_PKCS7);
    XFREE(nextParam, NULL, DYNAMIC_TYPE_PKCS7);

    return ret;
}

/*****************************************************************************
* wc_MIME_header_strip - Reads the string in from indices start to end, strips
* out disallowed/separator characters and places the rest into *out.
*
* RETURNS:
* returns zero on success, non-zero on error.
*/
int wc_MIME_header_strip(char* in, char** out, size_t start, size_t end)
{
    size_t inPos = start;
    size_t outPos = 0;
    size_t inLen = 0;

    if (end < start || in == NULL || out == NULL) {
        return BAD_FUNC_ARG;
    }

    inLen = XSTRLEN(in);
    if (start > inLen || end > inLen) {
        return BAD_FUNC_ARG;
    }

    *out = (char*)XMALLOC(((end-start)+2)*sizeof(char), NULL,
                          DYNAMIC_TYPE_PKCS7);
    if (*out == NULL) {
        return MEMORY_E;
    }

    while (inPos <= end) {
        if (in[inPos] >= MIME_HEADER_ASCII_MIN && in[inPos] <=
            MIME_HEADER_ASCII_MAX && in[inPos] != ';' && in[inPos] != '\"') {
            (*out)[outPos] = in[inPos];
            outPos++;
        }
        inPos++;
    }
    (*out)[outPos] = '\0';

    return 0;
}

/*****************************************************************************
* wc_MIME_find_header_name - Searches through all given headers until a header with
* a name matching the provided name is found.
*
* RETURNS:
* returns a pointer to the found header, if no match was found, returns NULL.
*/
MimeHdr* wc_MIME_find_header_name(const char* name, MimeHdr* header)
{
    size_t len = XSTRLEN(name);

    while (header) {
        if (!XSTRNCMP(name, header->name, len)) {
            return header;
        }
        header = header->next;
    }

    return header;
}

/*****************************************************************************
* wc_MIME_find_param_attr - Searches through all parameters until a parameter
* with a attribute matching the provided attribute is found.
*
* RETURNS:
* returns a pointer to the found parameter, if no match was found,
* returns NULL.
*/
MimeParam* wc_MIME_find_param_attr(const char* attribute,
                                    MimeParam* param)
{
    size_t len = XSTRLEN(attribute);

    while (param) {
        if (!XSTRNCMP(attribute, param->attribute, len)) {
            return param;
        }
        param = param->next;
    }

    return param;
}

/*****************************************************************************
* wc_MIME_canonicalize - Canonicalize a line by converting all line endings
* to CRLF.
*
* RETURNS:
* returns a pointer to a canonicalized line on success, NULL on error.
*/
char* wc_MIME_canonicalize(const char* line)
{
    size_t end = 0;
    char* canonLine = NULL;

    if (line == NULL || XSTRLEN(line) == 0) {
        return NULL;
    }

    end = XSTRLEN(line);
    while (end >= 1 && ((line[end-1] == '\r') || (line[end-1] == '\n'))) {
        end--;
    }

    /* Need 2 chars for \r\n and 1 for EOL */
    canonLine = (char*)XMALLOC((end+3)*sizeof(char), NULL, DYNAMIC_TYPE_PKCS7);
    if (canonLine == NULL) {
        return NULL;
    }

    XSTRNCPY(canonLine, line, end);
    canonLine[end] = '\r';
    canonLine[end+1] = '\n';
    canonLine[end+2] = '\0';

    return canonLine;
}

/*****************************************************************************
* wc_MIME_free_hdrs - Frees all MIME headers, parameters and strings starting from
* the provided header pointer.
*
* RETURNS:
* returns zero on success, non-zero on error.
*/
int wc_MIME_free_hdrs(MimeHdr* head)
{
    MimeHdr* curHdr = NULL;
    MimeParam* curParam = NULL;

    while (head) {
        while (head->params) {
            curParam = head->params;
            head->params = head->params->next;
            XFREE(curParam->attribute, NULL, DYNAMIC_TYPE_PKCS7);
            XFREE(curParam->value, NULL, DYNAMIC_TYPE_PKCS7);
            XFREE(curParam, NULL, DYNAMIC_TYPE_PKCS7);
        }
        curHdr = head;
        head = head->next;
        XFREE(curHdr->name, NULL, DYNAMIC_TYPE_PKCS7);
        XFREE(curHdr->body, NULL, DYNAMIC_TYPE_PKCS7);
        XFREE(curHdr, NULL, DYNAMIC_TYPE_PKCS7);
    }

    return 0;
}

#endif /* HAVE_SMIME */


#undef ERROR_OUT

#endif /* !NO_ASN */

#ifdef WOLFSSL_SEP


#endif /* WOLFSSL_SEP */

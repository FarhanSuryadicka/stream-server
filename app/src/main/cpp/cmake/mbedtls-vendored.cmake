# Vendored Mbed TLS 3.6.2 — built by listing sources, not via its own build.
#
# Same approach the project already takes for libjpeg-turbo: Mbed TLS's own
# CMakeLists pulls in programs/, tests/, framework/ and the optional Everest and
# p256-m accelerators, none of which are wanted here and several of which do not
# cross-compile cleanly for the NDK. Listing library/*.c keeps the vendored tree
# untouched upstream source while the build stays under this project's control.
#
# Split into the three canonical libraries (crypto / x509 / tls) because
# libdatachannel and libSRTP link them by those names.

# Both builds share this file: the Android app links it from app/src/main/cpp,
# and the PC receiver links the same vendored tree from experiment/receiver so
# the two sides cannot end up on different crypto versions. The caller may set
# UCV_CPP_DIR; it defaults to the including directory for the Android build.
if(NOT DEFINED UCV_CPP_DIR)
    set(UCV_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()
set(UCV_MBEDTLS_DIR ${UCV_CPP_DIR}/mbedtls)
set(UCV_MBEDTLS_INCLUDE_DIR ${UCV_MBEDTLS_DIR}/include CACHE INTERNAL "")

file(GLOB UCV_MBEDCRYPTO_SOURCES
    ${UCV_MBEDTLS_DIR}/library/aes.c
    ${UCV_MBEDTLS_DIR}/library/aesce.c
    ${UCV_MBEDTLS_DIR}/library/aesni.c
    ${UCV_MBEDTLS_DIR}/library/aria.c
    ${UCV_MBEDTLS_DIR}/library/asn1parse.c
    ${UCV_MBEDTLS_DIR}/library/asn1write.c
    ${UCV_MBEDTLS_DIR}/library/base64.c
    ${UCV_MBEDTLS_DIR}/library/bignum.c
    ${UCV_MBEDTLS_DIR}/library/bignum_core.c
    ${UCV_MBEDTLS_DIR}/library/bignum_mod.c
    ${UCV_MBEDTLS_DIR}/library/bignum_mod_raw.c
    ${UCV_MBEDTLS_DIR}/library/block_cipher.c
    ${UCV_MBEDTLS_DIR}/library/camellia.c
    ${UCV_MBEDTLS_DIR}/library/ccm.c
    ${UCV_MBEDTLS_DIR}/library/chacha20.c
    ${UCV_MBEDTLS_DIR}/library/chachapoly.c
    ${UCV_MBEDTLS_DIR}/library/cipher.c
    ${UCV_MBEDTLS_DIR}/library/cipher_wrap.c
    ${UCV_MBEDTLS_DIR}/library/cmac.c
    ${UCV_MBEDTLS_DIR}/library/constant_time.c
    ${UCV_MBEDTLS_DIR}/library/ctr_drbg.c
    ${UCV_MBEDTLS_DIR}/library/des.c
    ${UCV_MBEDTLS_DIR}/library/dhm.c
    ${UCV_MBEDTLS_DIR}/library/ecdh.c
    ${UCV_MBEDTLS_DIR}/library/ecdsa.c
    ${UCV_MBEDTLS_DIR}/library/ecjpake.c
    ${UCV_MBEDTLS_DIR}/library/ecp.c
    ${UCV_MBEDTLS_DIR}/library/ecp_curves.c
    ${UCV_MBEDTLS_DIR}/library/ecp_curves_new.c
    ${UCV_MBEDTLS_DIR}/library/entropy.c
    ${UCV_MBEDTLS_DIR}/library/entropy_poll.c
    ${UCV_MBEDTLS_DIR}/library/error.c
    ${UCV_MBEDTLS_DIR}/library/gcm.c
    ${UCV_MBEDTLS_DIR}/library/hkdf.c
    ${UCV_MBEDTLS_DIR}/library/hmac_drbg.c
    ${UCV_MBEDTLS_DIR}/library/lmots.c
    ${UCV_MBEDTLS_DIR}/library/lms.c
    ${UCV_MBEDTLS_DIR}/library/md.c
    ${UCV_MBEDTLS_DIR}/library/md5.c
    ${UCV_MBEDTLS_DIR}/library/memory_buffer_alloc.c
    ${UCV_MBEDTLS_DIR}/library/nist_kw.c
    ${UCV_MBEDTLS_DIR}/library/oid.c
    ${UCV_MBEDTLS_DIR}/library/padlock.c
    ${UCV_MBEDTLS_DIR}/library/pem.c
    ${UCV_MBEDTLS_DIR}/library/pk.c
    ${UCV_MBEDTLS_DIR}/library/pk_ecc.c
    ${UCV_MBEDTLS_DIR}/library/pk_wrap.c
    ${UCV_MBEDTLS_DIR}/library/pkcs12.c
    ${UCV_MBEDTLS_DIR}/library/pkcs5.c
    ${UCV_MBEDTLS_DIR}/library/pkparse.c
    ${UCV_MBEDTLS_DIR}/library/pkwrite.c
    ${UCV_MBEDTLS_DIR}/library/platform.c
    ${UCV_MBEDTLS_DIR}/library/platform_util.c
    ${UCV_MBEDTLS_DIR}/library/poly1305.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_aead.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_cipher.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_client.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_driver_wrappers_no_static.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_ecp.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_ffdh.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_hash.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_mac.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_pake.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_rsa.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_se.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_slot_management.c
    ${UCV_MBEDTLS_DIR}/library/psa_crypto_storage.c
    ${UCV_MBEDTLS_DIR}/library/psa_its_file.c
    ${UCV_MBEDTLS_DIR}/library/psa_util.c
    ${UCV_MBEDTLS_DIR}/library/ripemd160.c
    ${UCV_MBEDTLS_DIR}/library/rsa.c
    ${UCV_MBEDTLS_DIR}/library/rsa_alt_helpers.c
    ${UCV_MBEDTLS_DIR}/library/sha1.c
    ${UCV_MBEDTLS_DIR}/library/sha256.c
    ${UCV_MBEDTLS_DIR}/library/sha3.c
    ${UCV_MBEDTLS_DIR}/library/sha512.c
    ${UCV_MBEDTLS_DIR}/library/threading.c
    ${UCV_MBEDTLS_DIR}/library/timing.c
    ${UCV_MBEDTLS_DIR}/library/version.c
    ${UCV_MBEDTLS_DIR}/library/version_features.c
)

set(UCV_MBEDX509_SOURCES
    ${UCV_MBEDTLS_DIR}/library/x509.c
    ${UCV_MBEDTLS_DIR}/library/x509_create.c
    ${UCV_MBEDTLS_DIR}/library/x509_crl.c
    ${UCV_MBEDTLS_DIR}/library/x509_crt.c
    ${UCV_MBEDTLS_DIR}/library/x509_csr.c
    ${UCV_MBEDTLS_DIR}/library/x509write.c
    ${UCV_MBEDTLS_DIR}/library/x509write_crt.c
    ${UCV_MBEDTLS_DIR}/library/x509write_csr.c
    ${UCV_MBEDTLS_DIR}/library/pkcs7.c
)

set(UCV_MBEDTLS_SOURCES
    ${UCV_MBEDTLS_DIR}/library/debug.c
    ${UCV_MBEDTLS_DIR}/library/mps_reader.c
    ${UCV_MBEDTLS_DIR}/library/mps_trace.c
    ${UCV_MBEDTLS_DIR}/library/net_sockets.c
    ${UCV_MBEDTLS_DIR}/library/ssl_cache.c
    ${UCV_MBEDTLS_DIR}/library/ssl_ciphersuites.c
    ${UCV_MBEDTLS_DIR}/library/ssl_client.c
    ${UCV_MBEDTLS_DIR}/library/ssl_cookie.c
    ${UCV_MBEDTLS_DIR}/library/ssl_debug_helpers_generated.c
    ${UCV_MBEDTLS_DIR}/library/ssl_msg.c
    ${UCV_MBEDTLS_DIR}/library/ssl_ticket.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls12_client.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls12_server.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls13_client.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls13_generic.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls13_keys.c
    ${UCV_MBEDTLS_DIR}/library/ssl_tls13_server.c
)

add_library(mbedcrypto STATIC ${UCV_MBEDCRYPTO_SOURCES})
add_library(mbedx509  STATIC ${UCV_MBEDX509_SOURCES})
add_library(mbedtls   STATIC ${UCV_MBEDTLS_SOURCES})

foreach(_t mbedcrypto mbedx509 mbedtls)
    # BUILD_INTERFACE, not a bare path: these targets land in the export sets
    # above, and CMake rejects exporting a source-directory include path that has
    # no install-tree counterpart. Nothing is ever installed here, so scoping the
    # path to the build is both correct and sufficient.
    target_include_directories(${_t}
        PUBLIC $<BUILD_INTERFACE:${UCV_MBEDTLS_DIR}/include>)
    # DTLS-SRTP is the whole point here: WebRTC mandates it, and libdatachannel
    # asks Mbed TLS for the SRTP keying material through this option. It is NOT
    # in the default config, so leaving it off fails at link time with a missing
    # mbedtls_ssl_conf_dtls_srtp_protection_profiles.
    target_compile_definitions(${_t} PUBLIC MBEDTLS_SSL_DTLS_SRTP)
    # Upstream default config warns unless one of these is picked; the NDK has a
    # working /dev/urandom, so the platform entropy source is fine.
    target_compile_options(${_t} PRIVATE -Wno-unused-function)
endforeach()

target_link_libraries(mbedx509 PUBLIC mbedcrypto)
target_link_libraries(mbedtls  PUBLIC mbedx509)

# The names libdatachannel and libSRTP link against.
add_library(MbedTLS::mbedcrypto ALIAS mbedcrypto)
add_library(MbedTLS::mbedx509   ALIAS mbedx509)
add_library(MbedTLS::mbedtls    ALIAS mbedtls)

# libdatachannel links the umbrella target; it must pull in all three.
add_library(ucv_mbedtls_all INTERFACE)
target_link_libraries(ucv_mbedtls_all INTERFACE mbedtls mbedx509 mbedcrypto)
add_library(MbedTLS::MbedTLS ALIAS ucv_mbedtls_all)

# libdatachannel and libSRTP both declare install(EXPORT ...) rules, and CMake
# refuses to export a target that links something outside the export set — even
# though nothing here is ever installed (everything is statically linked into
# libuvcserver.so). Registering these targets in the same export sets satisfies
# that check. Must happen BEFORE add_subdirectory, because install(EXPORT ...)
# is evaluated inside those projects.
install(TARGETS mbedtls mbedx509 mbedcrypto ucv_mbedtls_all
        EXPORT LibDataChannelTargets)
install(TARGETS mbedtls mbedx509 mbedcrypto
        EXPORT libSRTPTargets)

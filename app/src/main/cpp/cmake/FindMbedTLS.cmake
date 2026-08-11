# Satisfies find_package(MbedTLS) for the vendored, in-tree Mbed TLS.
#
# libdatachannel and libSRTP both call find_package(MbedTLS REQUIRED). There is
# no system Mbed TLS on an Android NDK cross-build, so this module simply hands
# back the static targets built by ../mbedtls-vendored.cmake. Keeping it as a
# find-module (rather than patching those projects) means the vendored trees stay
# byte-identical to upstream and can be re-synced without replaying edits.
if(TARGET mbedcrypto)
  set(MbedTLS_FOUND TRUE)
  set(MBEDTLS_FOUND TRUE)
  set(MBEDTLS_LIBRARIES     mbedtls mbedx509 mbedcrypto)
  set(MBEDTLS_INCLUDE_DIRS  "${UCV_MBEDTLS_INCLUDE_DIR}")
  set(MBEDTLS_CRYPTO_LIBRARY mbedcrypto)
  set(MBEDTLS_X509_LIBRARY   mbedx509)
  set(MBEDTLS_TLS_LIBRARY    mbedtls)
  set(MBEDTLS_VERSION        "3.6.2")
else()
  set(MbedTLS_FOUND FALSE)
  set(MBEDTLS_FOUND FALSE)
  if(MbedTLS_FIND_REQUIRED)
    message(FATAL_ERROR "vendored Mbed TLS targets are not defined yet; "
                        "include mbedtls-vendored.cmake before add_subdirectory")
  endif()
endif()

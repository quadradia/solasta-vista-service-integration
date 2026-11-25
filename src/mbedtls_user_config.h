/*
 * mbedTLS build options for the Vista sync service (Windows XP SP3, Pentium II).
 * Applied on top of the stock mbedtls_config.h via MBEDTLS_USER_CONFIG_FILE.
 */
#ifndef VISTA_SYNC_MBEDTLS_USER_CONFIG_H
#define VISTA_SYNC_MBEDTLS_USER_CONFIG_H

/* TLS 1.2 only. TLS 1.3 is not needed and adds code and PSA key-exchange paths. */
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_EARLY_DATA

/* The stock Windows entropy source calls BCryptGenRandom (bcrypt.dll, Vista+).
 * Replace it with mbedtls_hardware_poll(), implemented in src/http.cpp with
 * CryptGenRandom (advapi32.dll, available on XP). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* We do our own Winsock I/O; net_sockets.c is not needed. */
#undef MBEDTLS_NET_C

/* VIA PadLock is irrelevant on the target CPU. AES-NI stays: it is only used
 * after a CPUID check, which a Pentium II answers with "no". */
#undef MBEDTLS_PADLOCK_C

/* Nothing on the client needs file-system access from mbedTLS itself. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

#endif

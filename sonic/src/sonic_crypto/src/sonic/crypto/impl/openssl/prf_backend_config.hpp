#pragma once

#if !defined(SONIC_OPENSSL_USE_LOWLEVEL)
#define SONIC_OPENSSL_USE_LOWLEVEL 1
#endif

#if defined(OPENSSL_NO_DEPRECATED_3_0)
#undef SONIC_OPENSSL_USE_LOWLEVEL
#define SONIC_OPENSSL_USE_LOWLEVEL 0
#endif

#if SONIC_OPENSSL_USE_LOWLEVEL
#if !(defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#undef SONIC_OPENSSL_USE_LOWLEVEL
#define SONIC_OPENSSL_USE_LOWLEVEL 0
#endif
#endif

#if SONIC_OPENSSL_USE_LOWLEVEL
#if defined(__AES__)
#define SONIC_OPENSSL_HAS_AESNI 1
#else
#define SONIC_OPENSSL_HAS_AESNI 0
#endif
#endif

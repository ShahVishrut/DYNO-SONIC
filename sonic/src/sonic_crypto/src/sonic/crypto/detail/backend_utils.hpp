#pragma once

#if defined(SONIC_CRYPTO_BACKEND_BEARSSL)
#include "sonic/crypto/impl/bearssl/detail/backend.hpp"
#include "sonic/crypto/impl/bearssl/detail/utils.hpp"
#include "sonic/crypto/impl/bearssl/prf_backend.hpp"
#else
#include "sonic/crypto/impl/openssl/detail/utils.hpp"
#include "sonic/crypto/impl/openssl/prf_backend_selector.hpp"
#endif

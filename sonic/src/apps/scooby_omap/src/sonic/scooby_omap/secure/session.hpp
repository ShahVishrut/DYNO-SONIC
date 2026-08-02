#pragma once

#include "sonic/sgxbridge/secure/crypto_traits.hpp"
#include "sonic/sgxbridge/secure/message_builder.hpp"
#include "sonic/sgxbridge/secure/session.hpp"

namespace sn::scooby::omap {

#if defined(SCOOBY_SECURE_COMM)
using secure_traits = sn::sgxbridge::secure::aes_gcm_traits;
inline constexpr bool kSecureCommEnabled = true;
#else
using secure_traits = sn::sgxbridge::secure::null_traits;
inline constexpr bool kSecureCommEnabled = false;
#endif

using secure_session_t = sn::sgxbridge::secure::session<secure_traits>;
using secure_builder_t = sn::sgxbridge::secure::message_builder<secure_traits>;

}

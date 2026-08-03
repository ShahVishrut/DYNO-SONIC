#pragma once

#include "backend_cpp.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#include "backend_x64.hpp"
#endif

namespace sn {
namespace obliv {
namespace detail {

#ifdef SN_OBLIV_BACKEND_OVERRIDE
#error "Do not use SN_OBLIV_BACKEND_OVERRIDE."
using default_backend = SN_OBLIV_BACKEND_OVERRIDE;
#else
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
// Force backend_cpp to avoid inline assembly syntax mismatches (AT&T vs Intel)
// across different translation units compiled with different -masm flags.
using default_backend = backends::backend_cpp;
#else
using default_backend = backends::backend_cpp;
#endif
#endif

}
}
}

include_guard(GLOBAL)

function(_oe_set_global_property name value)
  set_property(GLOBAL PROPERTY "${name}" "${value}")
endfunction()

function(_oe_get_global_property out_var name)
  get_property(_oe_tmp GLOBAL PROPERTY "${name}")
  set("${out_var}" "${_oe_tmp}" PARENT_SCOPE)
endfunction()

function(oe_init_toolchain)
  get_property(_oe_initialized GLOBAL PROPERTY OE_TOOLCHAIN_INITIALIZED)
  if(_oe_initialized)
    return()
  endif()

  find_program(OE_EDGER8R oeedger8r REQUIRED)
  find_package(PkgConfig REQUIRED)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_oe_compiler_tag "clang")
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
        OUTPUT_VARIABLE _oe_resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_oe_resource_dir)
      set(_oe_resource_include "${_oe_resource_dir}/include")
    endif()
    if(_oe_resource_dir)
      message(STATUS "OpenEnclave clang resource dir: ${_oe_resource_dir}")
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_oe_compiler_tag "gcc")
  else()
    message(FATAL_ERROR "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}' for OpenEnclave targets")
  endif()

  set(OE_HOST_PKG "oehost-${_oe_compiler_tag}")
  set(OE_HOSTXX_PKG "oehost-${_oe_compiler_tag}++")
  set(OE_ENCLAVE_PKG "oeenclave-${_oe_compiler_tag}")
  set(OE_ENCLAVEXX_PKG "oeenclave-${_oe_compiler_tag}++")

  pkg_check_modules(OEHOST REQUIRED ${OE_HOST_PKG})
  pkg_check_modules(OEHOSTXX REQUIRED ${OE_HOSTXX_PKG})
  pkg_check_modules(OEENCLAVE REQUIRED ${OE_ENCLAVE_PKG})
  pkg_check_modules(OEENCLAVEXX REQUIRED ${OE_ENCLAVEXX_PKG})

  list(GET OEENCLAVE_INCLUDE_DIRS 0 _oe_enclave_primary_include)
  if(NOT _oe_enclave_primary_include)
    message(FATAL_ERROR "Unable to determine OpenEnclave include directory from pkg-config")
  endif()

  set(_oe_mitigation_flags
      "-mllvm"
      "-x86-speculative-load-hardening"
      "-fstack-protector-strong"
      "-fno-omit-frame-pointer"
  )

  foreach(_flag IN LISTS _oe_mitigation_flags)
    foreach(_list_var
            OEENCLAVE_CFLAGS OEENCLAVE_CFLAGS_OTHER
            OEENCLAVEXX_CFLAGS OEENCLAVEXX_CFLAGS_OTHER
            OEHOST_CFLAGS OEHOST_CFLAGS_OTHER
            OEHOSTXX_CFLAGS OEHOSTXX_CFLAGS_OTHER)
      if(DEFINED ${_list_var})
        list(REMOVE_ITEM ${_list_var} "${_flag}")
      endif()
    endforeach()
  endforeach()

  set(_oe_edl_search_paths ${OEENCLAVE_INCLUDE_DIRS})
  list(APPEND _oe_edl_search_paths "${_oe_enclave_primary_include}/openenclave/edl/sgx")

  set(_oe_edger8r_args)
  foreach(_path IN LISTS _oe_edl_search_paths)
    list(APPEND _oe_edger8r_args --search-path "${_path}")
  endforeach()
  list(APPEND _oe_edger8r_args -DOE_SGX)

  set(OE_CRYPTO_LIB "openssl_3")
  if(DEFINED ENV{OE_CRYPTO_LIB})
    set(OE_CRYPTO_LIB "$ENV{OE_CRYPTO_LIB}")
  endif()

  pkg_get_variable(OE_CRYPTO_LIBS_RAW ${OE_ENCLAVEXX_PKG} ${OE_CRYPTO_LIB}libs)
  if(NOT OE_CRYPTO_LIBS_RAW)
    message(FATAL_ERROR "Unable to resolve OpenEnclave crypto library list for '${OE_CRYPTO_LIB}'")
  endif()
  separate_arguments(OE_CRYPTO_LIBS_RAW)

  find_program(OE_OPENSSL_EXECUTABLE openssl REQUIRED)
  find_program(OE_SIGN_TOOL oesign REQUIRED)

  _oe_set_global_property(OE_TOOLCHAIN_INITIALIZED TRUE)
  _oe_set_global_property(OE_EDGER8R "${OE_EDGER8R}")
  _oe_set_global_property(OE_EDGER8R_COMMON_ARGS "${_oe_edger8r_args}")
  _oe_set_global_property(OE_EDGER8R_SEARCH_PATHS "${_oe_edl_search_paths}")
  _oe_set_global_property(OE_ENCLAVE_PRIMARY_INCLUDE "${_oe_enclave_primary_include}")
  if(DEFINED _oe_resource_include)
    _oe_set_global_property(OE_CLANG_RESOURCE_INCLUDE "${_oe_resource_include}")
  endif()

  _oe_set_global_property(OE_HOST_CFLAGS "${OEHOST_CFLAGS}")
  _oe_set_global_property(OE_HOST_CFLAGS_OTHER "${OEHOST_CFLAGS_OTHER}")
  _oe_set_global_property(OE_HOSTXX_CFLAGS "${OEHOSTXX_CFLAGS}")
  _oe_set_global_property(OE_HOSTXX_CFLAGS_OTHER "${OEHOSTXX_CFLAGS_OTHER}")
  _oe_set_global_property(OE_HOST_INCLUDE_DIRS "${OEHOST_INCLUDE_DIRS}")
  _oe_set_global_property(OE_HOSTXX_INCLUDE_DIRS "${OEHOSTXX_INCLUDE_DIRS}")
  _oe_set_global_property(OE_HOST_LIBRARIES "${OEHOST_LIBRARIES}")
  _oe_set_global_property(OE_HOSTXX_LIBRARIES "${OEHOSTXX_LIBRARIES}")
  _oe_set_global_property(OE_HOST_LDFLAGS "${OEHOST_LDFLAGS}")
  _oe_set_global_property(OE_HOST_LDFLAGS_OTHER "${OEHOST_LDFLAGS_OTHER}")

  _oe_set_global_property(OE_ENCLAVE_CFLAGS "${OEENCLAVE_CFLAGS}")
  _oe_set_global_property(OE_ENCLAVE_CFLAGS_OTHER "${OEENCLAVE_CFLAGS_OTHER}")
  _oe_set_global_property(OE_ENCLAVEXX_CFLAGS "${OEENCLAVEXX_CFLAGS}")
  _oe_set_global_property(OE_ENCLAVEXX_CFLAGS_OTHER "${OEENCLAVEXX_CFLAGS_OTHER}")
  _oe_set_global_property(OE_ENCLAVE_INCLUDE_DIRS "${OEENCLAVE_INCLUDE_DIRS}")
  _oe_set_global_property(OE_ENCLAVEXX_INCLUDE_DIRS "${OEENCLAVEXX_INCLUDE_DIRS}")
  _oe_set_global_property(OE_ENCLAVE_LIBRARIES "${OEENCLAVE_LIBRARIES}")
  _oe_set_global_property(OE_ENCLAVEXX_LIBRARIES "${OEENCLAVEXX_LIBRARIES}")
  _oe_set_global_property(OE_ENCLAVE_LDFLAGS "${OEENCLAVE_LDFLAGS}")
  _oe_set_global_property(OE_ENCLAVE_LDFLAGS_OTHER "${OEENCLAVE_LDFLAGS_OTHER}")
  _oe_set_global_property(OE_CRYPTO_LIBS "${OE_CRYPTO_LIBS_RAW}")

  _oe_set_global_property(OE_OPENSSL_EXECUTABLE "${OE_OPENSSL_EXECUTABLE}")
  _oe_set_global_property(OE_SIGN_TOOL "${OE_SIGN_TOOL}")
endfunction()

function(oe_add_edl_stubs out_trusted_sources out_untrusted_sources edl output_dir)
  cmake_parse_arguments(
      OE_STUB
      ""
      "TRUSTED_HEADERS_VAR;UNTRUSTED_HEADERS_VAR;ARGS_HEADER_VAR"
      "SEARCH_PATHS"
      ${ARGN}
  )
  if(NOT edl)
    message(FATAL_ERROR "oe_add_edl_stubs requires an EDL file")
  endif()
  if(NOT output_dir)
    message(FATAL_ERROR "oe_add_edl_stubs requires an output directory")
  endif()

  oe_init_toolchain()

  _oe_get_global_property(_oe_edger8r OE_EDGER8R)
  _oe_get_global_property(_oe_common_args OE_EDGER8R_COMMON_ARGS)

  if(NOT _oe_edger8r)
    message(FATAL_ERROR "oe_init_toolchain must be called before oe_add_edl_stubs")
  endif()

  get_filename_component(_oe_prefix "${edl}" NAME_WE)
  set(_trusted_c "${output_dir}/${_oe_prefix}_t.c")
  set(_trusted_h "${output_dir}/${_oe_prefix}_t.h")
  set(_trusted_args "${output_dir}/${_oe_prefix}_args.h")
  set(_untrusted_c "${output_dir}/${_oe_prefix}_u.c")
  set(_untrusted_h "${output_dir}/${_oe_prefix}_u.h")

  set(_edger8r_args "${_oe_common_args}")
  foreach(_path IN LISTS OE_STUB_SEARCH_PATHS)
    list(APPEND _edger8r_args --search-path "${_path}")
  endforeach()

  add_custom_command(
      OUTPUT "${_untrusted_c}" "${_untrusted_h}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
      COMMAND ${_oe_edger8r} "${edl}"
          --untrusted
          ${_edger8r_args}
          --untrusted-dir "${output_dir}"
      DEPENDS "${edl}"
      COMMENT "Generating OpenEnclave untrusted stubs for ${edl}"
      VERBATIM
  )

  add_custom_command(
      OUTPUT "${_trusted_c}" "${_trusted_h}" "${_trusted_args}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
      COMMAND ${_oe_edger8r} "${edl}"
          --trusted
          ${_edger8r_args}
          --trusted-dir "${output_dir}"
      DEPENDS "${edl}"
      COMMENT "Generating OpenEnclave trusted stubs for ${edl}"
      VERBATIM
  )

  set("${out_trusted_sources}" "${_trusted_c}" PARENT_SCOPE)
  set("${out_untrusted_sources}" "${_untrusted_c}" PARENT_SCOPE)
  if(OE_STUB_TRUSTED_HEADERS_VAR)
    set("${OE_STUB_TRUSTED_HEADERS_VAR}" "${_trusted_h}" PARENT_SCOPE)
  endif()
  if(OE_STUB_UNTRUSTED_HEADERS_VAR)
    set("${OE_STUB_UNTRUSTED_HEADERS_VAR}" "${_untrusted_h}" PARENT_SCOPE)
  endif()
  if(OE_STUB_ARGS_HEADER_VAR)
    set("${OE_STUB_ARGS_HEADER_VAR}" "${_trusted_args}" PARENT_SCOPE)
  endif()
endfunction()

function(oe_configure_host_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "oe_configure_host_target expected an existing target (${target})")
  endif()

  cmake_parse_arguments(
      OE_HOST
      ""
      ""
      "INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS;COMPILE_OPTIONS_C;COMPILE_OPTIONS_CXX;LINK_OPTIONS;LINK_LIBRARIES"
      ${ARGN}
  )

  oe_init_toolchain()

  _oe_get_global_property(_oe_host_includes OE_HOST_INCLUDE_DIRS)
  _oe_get_global_property(_oe_hostxx_includes OE_HOSTXX_INCLUDE_DIRS)
  _oe_get_global_property(_oe_host_cflags OE_HOST_CFLAGS)
  _oe_get_global_property(_oe_host_cflags_other OE_HOST_CFLAGS_OTHER)
  _oe_get_global_property(_oe_hostxx_cflags OE_HOSTXX_CFLAGS)
  _oe_get_global_property(_oe_hostxx_cflags_other OE_HOSTXX_CFLAGS_OTHER)
  _oe_get_global_property(_oe_host_libs OE_HOST_LIBRARIES)
  _oe_get_global_property(_oe_hostxx_libs OE_HOSTXX_LIBRARIES)
  _oe_get_global_property(_oe_host_ldflags OE_HOST_LDFLAGS)
  _oe_get_global_property(_oe_host_ldflags_other OE_HOST_LDFLAGS_OTHER)
  _oe_get_global_property(_oe_resource_include OE_CLANG_RESOURCE_INCLUDE)

  set(_includes ${OE_HOST_INCLUDE_DIRECTORIES} ${_oe_host_includes} ${_oe_hostxx_includes})
  list(REMOVE_DUPLICATES _includes)
  if(_includes)
    target_include_directories("${target}" PRIVATE ${_includes})
  endif()

  if(OE_HOST_COMPILE_DEFINITIONS)
    target_compile_definitions("${target}" PRIVATE ${OE_HOST_COMPILE_DEFINITIONS})
  endif()

  if(OE_HOST_COMPILE_OPTIONS)
    target_compile_options("${target}" PRIVATE ${OE_HOST_COMPILE_OPTIONS})
  endif()

  if(OE_HOST_COMPILE_OPTIONS_C)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:C>:${OE_HOST_COMPILE_OPTIONS_C}>)
  endif()

  if(OE_HOST_COMPILE_OPTIONS_CXX)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OE_HOST_COMPILE_OPTIONS_CXX}>)
  endif()

  if(_oe_host_cflags OR _oe_host_cflags_other)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:C>:${_oe_host_cflags};${_oe_host_cflags_other}>)
  endif()

  if(_oe_hostxx_cflags OR _oe_hostxx_cflags_other)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${_oe_hostxx_cflags};${_oe_hostxx_cflags_other}>)
  endif()

  set(_link_libs ${_oe_host_libs} ${_oe_hostxx_libs} ${OE_HOST_LINK_LIBRARIES})
  if(_link_libs)
    target_link_libraries("${target}" PRIVATE ${_link_libs})
  endif()

  set(_link_opts ${_oe_host_ldflags} ${_oe_host_ldflags_other} ${OE_HOST_LINK_OPTIONS})
  if(_link_opts)
    target_link_options("${target}" PRIVATE ${_link_opts})
  endif()

  if(_oe_resource_include)
    target_include_directories("${target}" SYSTEM PRIVATE "${_oe_resource_include}")
    target_compile_options("${target}" PRIVATE -isystem "${_oe_resource_include}")
  endif()
endfunction()

function(oe_configure_enclave_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "oe_configure_enclave_target expected an existing target (${target})")
  endif()

  cmake_parse_arguments(
      OE_ENC
      ""
      ""
      "INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS;COMPILE_OPTIONS_C;COMPILE_OPTIONS_CXX;LINK_OPTIONS;LINK_LIBRARIES"
      ${ARGN}
  )

  oe_init_toolchain()

  _oe_get_global_property(_oe_enclave_includes OE_ENCLAVE_INCLUDE_DIRS)
  _oe_get_global_property(_oe_enclavexx_includes OE_ENCLAVEXX_INCLUDE_DIRS)
  _oe_get_global_property(_oe_enclave_cflags OE_ENCLAVE_CFLAGS)
  _oe_get_global_property(_oe_enclave_cflags_other OE_ENCLAVE_CFLAGS_OTHER)
  _oe_get_global_property(_oe_enclavexx_cflags OE_ENCLAVEXX_CFLAGS)
  _oe_get_global_property(_oe_enclavexx_cflags_other OE_ENCLAVEXX_CFLAGS_OTHER)
  _oe_get_global_property(_oe_enclave_libs OE_ENCLAVE_LIBRARIES)
  _oe_get_global_property(_oe_enclavexx_libs OE_ENCLAVEXX_LIBRARIES)
  _oe_get_global_property(_oe_enclave_ldflags OE_ENCLAVE_LDFLAGS)
  _oe_get_global_property(_oe_enclave_ldflags_other OE_ENCLAVE_LDFLAGS_OTHER)
  _oe_get_global_property(_oe_crypto_libs OE_CRYPTO_LIBS)
  _oe_get_global_property(_oe_resource_include OE_CLANG_RESOURCE_INCLUDE)

  set(_includes ${OE_ENC_INCLUDE_DIRECTORIES} ${_oe_enclave_includes} ${_oe_enclavexx_includes})
  list(REMOVE_DUPLICATES _includes)
  if(_includes)
    target_include_directories("${target}" PRIVATE ${_includes})
  endif()

  if(OE_ENC_COMPILE_DEFINITIONS)
    target_compile_definitions("${target}" PRIVATE ${OE_ENC_COMPILE_DEFINITIONS})
  endif()

  if(OE_ENC_COMPILE_OPTIONS)
    target_compile_options("${target}" PRIVATE ${OE_ENC_COMPILE_OPTIONS})
  endif()

  if(OE_ENC_COMPILE_OPTIONS_C)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:C>:${OE_ENC_COMPILE_OPTIONS_C}>)
  endif()

  if(OE_ENC_COMPILE_OPTIONS_CXX)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OE_ENC_COMPILE_OPTIONS_CXX}>)
  endif()

  if(_oe_enclave_cflags OR _oe_enclave_cflags_other)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:C>:${_oe_enclave_cflags};${_oe_enclave_cflags_other}>)
  endif()

  if(_oe_enclavexx_cflags OR _oe_enclavexx_cflags_other)
    target_compile_options("${target}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${_oe_enclavexx_cflags};${_oe_enclavexx_cflags_other}>)
  endif()

  set(_link_libs ${_oe_enclave_libs} ${_oe_enclavexx_libs} ${_oe_crypto_libs} ${OE_ENC_LINK_LIBRARIES})
  if(_link_libs)
    target_link_libraries("${target}" PRIVATE ${_link_libs})
  endif()

  set(_link_opts ${_oe_enclave_ldflags} ${_oe_enclave_ldflags_other} ${OE_ENC_LINK_OPTIONS})
  if(_link_opts)
    target_link_options("${target}" PRIVATE ${_link_opts})
  endif()

  if(_oe_resource_include)
    target_include_directories("${target}" SYSTEM PRIVATE "${_oe_resource_include}")
    target_compile_options("${target}" PRIVATE -isystem "${_oe_resource_include}")
  endif()
endfunction()

function(oe_add_enclave_signing out_signed out_private out_public)
  cmake_parse_arguments(
      OE_SIGN
      ""
      "UNSIGNED_TARGET;CONFIG;OUTPUT_PATH;SIGNING_DIR"
      ""
      ${ARGN}
  )

  if(NOT OE_SIGN_UNSIGNED_TARGET)
    message(FATAL_ERROR "oe_add_enclave_signing requires UNSIGNED_TARGET")
  endif()
  if(NOT OE_SIGN_CONFIG)
    message(FATAL_ERROR "oe_add_enclave_signing requires CONFIG")
  endif()
  if(NOT OE_SIGN_OUTPUT_PATH)
    message(FATAL_ERROR "oe_add_enclave_signing requires OUTPUT_PATH")
  endif()

  if(NOT EXISTS "${OE_SIGN_CONFIG}")
    message(FATAL_ERROR "Enclave signing config '${OE_SIGN_CONFIG}' missing")
  endif()

  oe_init_toolchain()

  _oe_get_global_property(_oe_openssl OE_OPENSSL_EXECUTABLE)
  _oe_get_global_property(_oe_sign_tool OE_SIGN_TOOL)

  if(NOT OE_SIGN_SIGNING_DIR)
    set(OE_SIGN_SIGNING_DIR "${CMAKE_CURRENT_BINARY_DIR}/signing")
  endif()

  file(MAKE_DIRECTORY "${OE_SIGN_SIGNING_DIR}")

  set(_private_key "${OE_SIGN_SIGNING_DIR}/private.pem")
  set(_public_key "${OE_SIGN_SIGNING_DIR}/public.pem")
  set(_signed_image "${OE_SIGN_OUTPUT_PATH}")

  add_custom_command(
      OUTPUT "${_private_key}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${OE_SIGN_SIGNING_DIR}"
      COMMAND ${_oe_openssl} genrsa -out "${_private_key}" -3 3072
      COMMENT "Generating OpenEnclave signing key (private.pem)"
      VERBATIM
  )

  add_custom_command(
      OUTPUT "${_public_key}"
      COMMAND ${_oe_openssl} rsa -pubout -in "${_private_key}" -out "${_public_key}"
      DEPENDS "${_private_key}"
      COMMENT "Deriving OpenEnclave signing key (public.pem)"
      VERBATIM
  )

  add_custom_command(
      OUTPUT "${_signed_image}"
      COMMAND ${_oe_sign_tool} sign
          -e $<TARGET_FILE:${OE_SIGN_UNSIGNED_TARGET}>
          -c "${OE_SIGN_CONFIG}"
          -k "${_private_key}"
          -o "${_signed_image}"
      DEPENDS ${OE_SIGN_UNSIGNED_TARGET} "${OE_SIGN_CONFIG}" "${_private_key}"
      COMMENT "Signing OpenEnclave image (${_signed_image})"
      VERBATIM
  )

  set("${out_private}" "${_private_key}" PARENT_SCOPE)
  set("${out_public}" "${_public_key}" PARENT_SCOPE)
  set("${out_signed}" "${_signed_image}" PARENT_SCOPE)
endfunction()

# libssh2 as an ESP-IDF component, with a crypto backend written against the
# PSA API that ESP-IDF's Mbed TLS 4 exposes (see port/psa_backend.c).
#
# The upstream CMake build is not reused: it probes the host for headers and
# functions, which is meaningless when cross compiling. The source list below is
# src/Makefile.inc, and the probe results live in port/libssh2_config.h.
#
# The sources are compiled from a copy in the build directory rather than in
# place. libssh2 picks its crypto backend in src/crypto.h with a quoted
# #include, which the compiler always resolves next to crypto.h itself; copying
# the tree is what lets a backend that lives outside it be selected without
# editing the submodule.

set(libssh2_dir "${CMAKE_CURRENT_LIST_DIR}/libssh2")
set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/libssh2-src")

if(NOT EXISTS "${libssh2_dir}/src/session.c")
    message(FATAL_ERROR "The libssh2 submodule is empty. Run 'make prepare' (or 'git submodule update --init --recursive').")
endif()

set(libssh2_names
    agent.c
    bcrypt_pbkdf.c
    channel.c
    chacha.c
    cipher-chachapoly.c
    comp.c
    crypt.c
    global.c
    hostkey.c
    keepalive.c
    kex.c
    knownhost.c
    mac.c
    misc.c
    packet.c
    pem.c
    poly1305.c
    publickey.c
    scp.c
    session.c
    sftp.c
    transport.c
    userauth.c
    userauth_kbd_packet.c
    version.c
)

# Every .c file is copied, because a few of them are #included by others
# (blowfish.c by bcrypt_pbkdf.c, agent_win.c by agent.c) rather than compiled
# on their own. Only the names listed above are handed to the compiler.
file(GLOB libssh2_all_sources "${libssh2_dir}/src/*.c")
foreach(source ${libssh2_all_sources})
    get_filename_component(source_name "${source}" NAME)
    configure_file("${source}" "${gen_dir}/${source_name}" COPYONLY)
endforeach()

set(libssh2_srcs "")
foreach(name ${libssh2_names})
    list(APPEND libssh2_srcs "${gen_dir}/${name}")
endforeach()

# Private headers travel with the sources; crypto.h is the one exception.
file(GLOB libssh2_headers "${libssh2_dir}/src/*.h")
foreach(header ${libssh2_headers})
    get_filename_component(header_name "${header}" NAME)
    if(NOT header_name STREQUAL "crypto.h")
        configure_file("${header}" "${gen_dir}/${header_name}" COPYONLY)
    endif()
endforeach()

# Teach crypto.h about the PSA backend by adding one more branch to the
# selection it already has for the backends upstream ships.
file(READ "${libssh2_dir}/src/crypto.h" crypto_header)
string(REPLACE
    "#elif defined(LIBSSH2_WINCNG)"
    "#elif defined(LIBSSH2_PSA)\n#include \"psa_backend.h\"\n#elif defined(LIBSSH2_WINCNG)"
    crypto_header "${crypto_header}")
file(GENERATE OUTPUT "${gen_dir}/crypto.h" CONTENT "${crypto_header}")

configure_file("${CMAKE_CURRENT_LIST_DIR}/port/psa_backend.h" "${gen_dir}/psa_backend.h" COPYONLY)

idf_component_register(
    SRCS
        ${libssh2_srcs}
        "port/psa_backend.c"
        "port/tweetnacl.c"
        "port/badge_ed25519.c"
    INCLUDE_DIRS
        "${libssh2_dir}/include"
        "include"
    PRIV_INCLUDE_DIRS
        "port"
        "${gen_dir}"
    REQUIRES mbedtls
    PRIV_REQUIRES lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
    LIBSSH2_PSA
)

# Upstream is warning clean under its own flags, not under the extra warnings
# IDF turns on. None of these are defects.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-char-subscripts
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-format-truncation
    -Wno-missing-field-initializers
    -Wno-unterminated-string-initialization
)

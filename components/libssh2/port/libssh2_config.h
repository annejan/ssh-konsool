/* SPDX-License-Identifier: BSD-3-Clause
 *
 * libssh2 build configuration for ESP-IDF (newlib + lwIP).
 *
 * Hand written instead of generated: the CMake probes in the libssh2 tree run
 * against the host, and cross compiling them for the ESP32-P4 buys nothing when
 * the answers are all known up front.
 */

#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

/* Headers newlib and lwIP provide */
#define HAVE_UNISTD_H
#define HAVE_INTTYPES_H
#define HAVE_SYS_SELECT_H
#define HAVE_SYS_UIO_H
#define HAVE_SYS_SOCKET_H
#define HAVE_SYS_IOCTL_H
#define HAVE_SYS_TIME_H
#define HAVE_ARPA_INET_H
#define HAVE_NETINET_IN_H

/* Functions */
#define HAVE_GETTIMEOFDAY
#define HAVE_STRTOLL
#define HAVE_SNPRINTF
#define HAVE_SELECT

/* lwIP has no poll.h, so libssh2 uses select() for its blocking helpers. */

/* Non-blocking sockets: lwIP honours O_NONBLOCK through fcntl() */
#define HAVE_O_NONBLOCK

#endif /* LIBSSH2_CONFIG_H */

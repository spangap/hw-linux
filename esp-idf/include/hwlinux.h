/**
 * hwlinux.h — the station's environment when the station is a Linux process.
 *
 * One process is one station. What a chip would have had burned into it or
 * wired to it comes from the environment instead, read once at the first
 * call and stable for the life of the process:
 *
 *   SPANGAP_NODE_ID     a small integer, the station's identity. It is the
 *                       last byte of the MAC the platform reads, so every
 *                       station on one host is a distinct device.
 *   SPANGAP_NODE_DIR    the station's directory. It is created, `state/` is
 *                       created under it, `fixed` is linked into it, and the
 *                       process chdir()s there — which is what makes the
 *                       platform's relative filesystem roots resolve.
 *   SPANGAP_BIND_ADDR   the loopback address this station's listeners bind,
 *                       127.0.0.1<id> by default, so every station keeps the
 *                       canonical port numbers.
 *   SPANGAP_ETHER       host:port of the virtual ether.
 *   SPANGAP_FIXED_DIR   the build's merged read-only data tree, linked in as
 *                       ./fixed.
 */
#pragma once

#include "service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The station's identity. 1 when unset. */
int hwLinuxNodeId(void);

/** The loopback address every listener binds, in dotted-quad form. */
const char* hwLinuxBindAddr(void);

/** `host:port` of the virtual ether. Empty when no ether was named. */
const char* hwLinuxEtherAddr(void);

#ifdef __cplusplus
}

/**
 * Board bring-up. onStart() runs before spangapInit(): it makes the station's
 * directory, puts `state/` and the `fixed` link in it, and chdir()s there, so
 * the roots the platform opens by relative path land inside this station and
 * nowhere near another's.
 */
class HwLinuxBoard : public Service {
public:
    void onStart() override;
};
#endif

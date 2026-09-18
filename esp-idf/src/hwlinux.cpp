/**
 * hwlinux.cpp — the station's environment and identity. See hwlinux.h.
 *
 * Everything here runs before the platform exists, so it uses write(2) and
 * the C library directly and reports failure to stderr: there is no log task,
 * no storage and no console yet.
 */
#include "hwlinux.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"

namespace {

struct Env {
    int  nodeId = 1;
    char nodeDir[512] = ".";
    char bindAddr[64] = "127.0.0.1";
    char ether[128] = "";
    char fixedDir[512] = "";
};

Env& env()
{
    static Env e;
    static bool loaded = false;
    if (loaded) return e;
    loaded = true;

    const char* id = getenv("SPANGAP_NODE_ID");
    if (id && *id) {
        long v = strtol(id, nullptr, 10);
        if (v > 0 && v < 255) e.nodeId = (int)v;
    }
    const char* dir = getenv("SPANGAP_NODE_DIR");
    if (dir && *dir) snprintf(e.nodeDir, sizeof e.nodeDir, "%s", dir);

    const char* bind = getenv("SPANGAP_BIND_ADDR");
    if (bind && *bind) snprintf(e.bindAddr, sizeof e.bindAddr, "%s", bind);
    else               snprintf(e.bindAddr, sizeof e.bindAddr, "127.0.0.1%d", e.nodeId);

    const char* eth = getenv("SPANGAP_ETHER");
    if (eth && *eth) snprintf(e.ether, sizeof e.ether, "%s", eth);

    const char* fixed = getenv("SPANGAP_FIXED_DIR");
    if (fixed && *fixed) snprintf(e.fixedDir, sizeof e.fixedDir, "%s", fixed);

    return e;
}

void fail(const char* what, const char* path)
{
    fprintf(stderr, "hw-linux: %s %s: %s\n", what, path, strerror(errno));
}

void ensureDir(const char* path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) fail("mkdir", path);
}

}  // namespace

extern "C" int hwLinuxNodeId(void) { return env().nodeId; }
extern "C" const char* hwLinuxBindAddr(void) { return env().bindAddr; }
extern "C" const char* hwLinuxEtherAddr(void) { return env().ether; }

/* The identity the platform reads everywhere it wants a device address: a
 * locally administered unicast MAC whose last byte is the node id. */
extern "C" esp_err_t esp_efuse_mac_get_default(uint8_t* mac)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    mac[0] = 0x02;              /* locally administered, unicast */
    mac[1] = 0x73;              /* 's' */
    mac[2] = 0x69;              /* 'i' */
    mac[3] = 0x6d;              /* 'm' */
    mac[4] = 0x00;
    mac[5] = (uint8_t)env().nodeId;
    return ESP_OK;
}

void HwLinuxBoard::onStart()
{
    Env& e = env();

    ensureDir(e.nodeDir);
    if (chdir(e.nodeDir) != 0) {
        fail("chdir", e.nodeDir);
        return;
    }
    ensureDir("state");

    /* `fixed` is the build's merged read-only tree, reached through a link so
     * every station shares one copy and a rebuild is picked up with no copy
     * step. Replaced rather than kept: the build directory moves. */
    if (e.fixedDir[0]) {
        unlink("fixed");
        if (symlink(e.fixedDir, "fixed") != 0) fail("symlink", e.fixedDir);
    }

    char cwd[512] = {};
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
    fprintf(stderr, "hw-linux: node %d at %s, bind %s, ether %s\n",
            e.nodeId, cwd, e.bindAddr, e.ether[0] ? e.ether : "(none)");
    fflush(stderr);
}

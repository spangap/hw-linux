/**
 * hwlinux.cpp — the station's environment and identity. See hwlinux.h.
 *
 * Everything here runs before the platform exists, so it uses write(2) and
 * the C library directly and reports failure to stderr: there is no log task,
 * no storage and no console yet.
 */
#include "hwlinux.h"

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"

namespace {

/* One formatted line onto a descriptor with write(2). stdio takes a lock, and
 * on this port a signal can switch tasks while a task holds it: the next task
 * to print would wait on it forever. */
void say(int fd, const char* fmt, ...)
{
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    ssize_t w = write(fd, line, (size_t)n);
    (void)w;
}

/* ESP-IDF's log until the platform's own logger takes it over: the same
 * formatting, put on stdout with write(2). */
int logToStdout(const char* fmt, va_list ap)
{
    char line[640];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0) return n;
    int len = n >= (int)sizeof line ? (int)sizeof line - 1 : n;
    ssize_t w = write(STDOUT_FILENO, line, (size_t)len);
    (void)w;
    return n;
}

__attribute__((constructor)) void earlyLog()
{
    esp_log_set_vprintf(logToStdout);
}

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
        if (v > 0 && v < 65536) e.nodeId = (int)v;   /* two MAC bytes' worth */
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

/* SPANGAP_BOARD, read once: one flat JSON object, of which the front end's
 * members are taken. A member that is absent or the wrong type reads as
 * unsaid; a document that does not parse is said once on stderr and read as
 * no board at all. */
struct FrontEnd {
    bool told = false;
    char part[32] = "";
    char txCal[512] = "";
    int  gainDb = 0;
    int  rxGainDb = 0;
    int  maxDbm = 0;
};

FrontEnd& frontEnd()
{
    static FrontEnd f;
    static bool loaded = false;
    if (loaded) return f;
    loaded = true;

    const char* text = getenv("SPANGAP_BOARD");
    if (!text || !*text) return f;
    cJSON* doc = cJSON_Parse(text);
    if (!cJSON_IsObject(doc)) {
        say(STDERR_FILENO, "hw-linux: SPANGAP_BOARD is not a JSON object; no board taken\n");
        cJSON_Delete(doc);
        return f;
    }
    auto str = [&](const char* key, char* out, size_t cap) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(doc, key);
        if (cJSON_IsString(v)) snprintf(out, cap, "%s", v->valuestring);
    };
    auto num = [&](const char* key) -> double {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(doc, key);
        return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    };
    f.told = true;
    str("fem_part", f.part, sizeof f.part);
    str("fem_tx_cal", f.txCal, sizeof f.txCal);
    f.gainDb = (int)num("fem_gain_db");
    f.rxGainDb = (int)num("fem_rx_gain_db");
    f.maxDbm = (int)std::floor(num("max_dbm"));   /* a ceiling: never rounded up */
    cJSON_Delete(doc);
    return f;
}

void fail(const char* what, const char* path)
{
    say(STDERR_FILENO, "hw-linux: %s %s: %s\n", what, path, strerror(errno));
}

void ensureDir(const char* path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) fail("mkdir", path);
}

}  // namespace

/* The station's clock comes up, and the tick with it (src/tick.cpp). */
void hwLinuxTickStart(void);

extern "C" int hwLinuxNodeId(void) { return env().nodeId; }
extern "C" const char* hwLinuxBindAddr(void) { return env().bindAddr; }
extern "C" const char* hwLinuxEtherAddr(void) { return env().ether; }

extern "C" bool hwLinuxFrontEnd(const char** part, const char** txCal,
                                int* gainDb, int* rxGainDb, int* maxDbm)
{
    const FrontEnd& f = frontEnd();
    if (!f.told) return false;
    *part = f.part;
    *txCal = f.txCal;
    *gainDb = f.gainDb;
    *rxGainDb = f.rxGainDb;
    *maxDbm = f.maxDbm;
    return true;
}

/* The identity the platform reads everywhere it wants a device address: a
 * locally administered unicast MAC whose last two bytes are the node id. */
extern "C" esp_err_t esp_efuse_mac_get_default(uint8_t* mac)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    int id = env().nodeId;
    mac[0] = 0x02;              /* locally administered, unicast */
    mac[1] = 0x73;              /* 's' */
    mac[2] = 0x69;              /* 'i' */
    mac[3] = 0x6d;              /* 'm' */
    mac[4] = (uint8_t)(id >> 8);
    mac[5] = (uint8_t)(id & 0xff);
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
    say(STDERR_FILENO, "hw-linux: node %d at %s, bind %s, ether %s\n",
        e.nodeId, cwd, e.bindAddr, e.ether[0] ? e.ether : "(none)");

    hwLinuxTickStart();
}

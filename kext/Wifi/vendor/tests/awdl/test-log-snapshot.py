#!/usr/bin/env python3
"""Exercise production log reads with a busy, wrapped or malformed kernel ring."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
client = (root / "tools/mtkctl.c").read_text()
start = client.index("static int cmd_log(io_connect_t c)")
end = client.index("static void cmd_scan", start)
production = client[start:end]
stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MTK_LOG_CHUNK 3800
#define IOCTL_VERSION 1
#define IOCTL_80211_LOG 1
typedef int io_connect_t;
typedef unsigned kern_return_t;
struct ioctl_log {
    unsigned version;
    uint32_t len;
    uint64_t cursor, head;
    char data[MTK_LOG_CHUNK];
};
enum { EMPTY, BUSY, WRAPPED, STUCK, OVERSIZE, ERROR, MAXIMUM, BAD_CURSOR, BAD_VERSION };
unsigned mode, calls;
uint64_t position;
static kern_return_t ioctl_get(io_connect_t c, unsigned id, void *out, size_t len) {
    (void)c; (void)id; (void)len;
    struct ioctl_log *l = out;
    memset(l, 0, sizeof(*l));
    l->version = IOCTL_VERSION;
    ++calls;
    if (mode == ERROR) return 99;
    if (mode == EMPTY) return 0;
    l->len = MTK_LOG_CHUNK;
    memset(l->data, 'x', l->len);
    if (mode == OVERSIZE) l->len = MTK_LOG_CHUNK + 1;
    if (mode == WRAPPED && calls == 2) position = 20000;
    if (mode == MAXIMUM && calls == 1) position = 1001000;
    if (mode != STUCK || calls == 1) position += MTK_LOG_CHUNK;
    l->cursor = position;
    // Writer continuously adds enough bytes that no read is ever empty.
    l->head = mode == MAXIMUM ? 1262144 + calls * 1000 : 10000 + calls * 1000;
    if (mode == WRAPPED && calls > 1) l->head = position + 10000;
    if (mode == BAD_CURSOR) l->cursor = l->head + 1;
    if (mode == BAD_VERSION) l->version = 99;
    return 0;
}
'''
tests = r'''
static void run(unsigned scenario, int expected, unsigned max_calls, long bytes) {
    mode = scenario; calls = 0; position = 0;
    long before = ftell(stdout);
    assert(cmd_log(0) == expected);
    assert(calls <= max_calls);
    assert(ftell(stdout) - before == bytes);
}
int main(void) {
    assert(freopen("/dev/null", "w", stderr));
    // Regular files have meaningful ftell; stdout is redirected by the harness.
    run(EMPTY, 0, 1, 0);
    run(BUSY, 0, 3, 11000);
    run(WRAPPED, 0, 2, 3800); // Overwritten data cannot be recovered.
    run(STUCK, 1, 2, 3800);
    run(OVERSIZE, 1, 1, 0);
    run(ERROR, 1, 1, 0);
    run(MAXIMUM, 0, 70, 262144);
    run(BAD_CURSOR, 1, 1, 0);
    run(BAD_VERSION, 1, 1, 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mtk-log-snapshot-") as temp:
    source = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    source.write_text(stubs + production + tests)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
    with (Path(temp) / "output").open("wb") as output:
        subprocess.run([str(binary)], stdout=output, check=True, timeout=10)
print("PASS bounded log snapshots: busy/wrapped/full ring, stalled cursor, invalid lengths/version and errors.")

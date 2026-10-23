//
//  Log.h
//  MtkWireless
//
//  BTLog → IOLog plus a ring buffer that the loader publishes as "MtkBtLog"
//  on the USB device (IOLog output from early boot is often lost).
//

#ifndef MtkWirelessBT_Log_h
#define MtkWirelessBT_Log_h

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <stdarg.h>

#define MTKBT_LOG_SIZE 8192

inline char *mtkbt_log_buf() { static char buf[MTKBT_LOG_SIZE]; return buf; }
inline size_t &mtkbt_log_len() { static size_t len; return len; }

inline void mtkbt_log(const char *fmt, ...)
{
    static volatile UInt32 busy;
    char line[256];
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    int n = snprintf(line, sizeof(line), "[%llu.%03llu] ", ns / 1000000000ULL, (ns / 1000000ULL) % 1000);
    if (n < 0 || (size_t)n >= sizeof(line))
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    // Concurrent writers (two controllers starting at once) skip the ring
    // rather than corrupt it; IOLog still has the line.
    if (!OSCompareAndSwap(0, 1, &busy))
        return;
    size_t l = strlen(line);
    char *buf = mtkbt_log_buf();
    size_t &len = mtkbt_log_len();
    if (len + l + 1 > MTKBT_LOG_SIZE) {
        // drop the older half
        size_t keep = len / 2;
        memmove(buf, buf + len - keep, keep);
        len = keep;
    }
    memcpy(buf + len, line, l);
    len += l;
    buf[len] = '\0';
    busy = 0;
}

#define BTLog(fmt, x...)                              \
    do {                                              \
        IOLog("MtkWirelessBT: " fmt, ##x);            \
        mtkbt_log(fmt, ##x);                          \
    } while (0)

#endif /* MtkWirelessBT_Log_h */

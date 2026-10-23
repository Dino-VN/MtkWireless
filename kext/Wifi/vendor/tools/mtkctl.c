// mtkctl: talk to SimpleMtkWlan (or AirportMtkWlan) through its IOUserClient.
// Build: clang -O2 -Wall -I../include/ClientKit -o mtkctl mtkctl.c -framework IOKit -framework CoreFoundation
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CFNumber.h>
#include <mach/mach_error.h>
#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "Common.h"

static const char *state_names[] = { "INIT", "SCAN", "AUTH", "ASSOC", "RUN" };

static io_connect_t open_driver(void)
{
    /* SimpleMtkWlan, or the PCI wrapper of the AirportMtkWlan build. */
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("SimpleMtkWlan"));
    if (!svc)
        svc = IOServiceGetMatchingService(kIOMainPortDefault,
            IOServiceMatching("MtkPCIEDeviceWrapper"));
    if (!svc) {
        fprintf(stderr, "SimpleMtkWlan/AirportMtkWlan service not found (driver not loaded?)\n");
        exit(1);
    }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        exit(1);
    }
    return conn;
}

static kern_return_t ioctl_get(io_connect_t c, uint32_t id, void *out, size_t len)
{
    size_t outlen = len;
    return IOConnectCallStructMethod(c, id, NULL, 0, out, &outlen);
}

static kern_return_t ioctl_set(io_connect_t c, uint32_t id, const void *in, size_t len)
{
    return IOConnectCallStructMethod(c, id | IOCTL_MASK, in, len, NULL, NULL);
}

static void cmd_info(io_connect_t c)
{
    struct ioctl_driver_info info = {0};
    kern_return_t kr = ioctl_get(c, IOCTL_80211_DRIVER_INFO, &info, sizeof(info));
    if (kr) { printf("driver info: error 0x%x\n", kr); return; }
    printf("interface: %s\nfirmware:  %.32s\ndriver:    %.32s\n",
        info.bsd_name, info.fw_version, info.driver_version);
}

static int cmd_state(io_connect_t c)
{
    struct ioctl_state st = {0};
    kern_return_t kr = ioctl_get(c, IOCTL_80211_STATE, &st, sizeof(st));
    if (kr) { printf("state: error 0x%x\n", kr); return -1; }
    printf("state: %s (%d)\n",
        st.state >= 0 && st.state <= 4 ? state_names[st.state] : "?", st.state);
    return st.state;
}

static const char *phy_names[] = { "11a", "11b", "11g", "11n", "11ac", "11ax" };

/* Link details; the SSID is left out so the output can be shared. */
static void cmd_sta(io_connect_t c)
{
    struct ioctl_sta_info st = {0};
    kern_return_t kr = ioctl_get(c, IOCTL_80211_STA_INFO, &st, sizeof(st));
    if (kr) { printf("sta: not associated (0x%x)\n", kr); return; }
    printf("sta: mode=%s channel=%u bw=%u rssi=%d noise=%d mcs=%d rate=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
        (unsigned)st.op_mode < 6 ? phy_names[st.op_mode] : "?", st.channel,
        st.band_width, st.rssi, st.noise, st.cur_mcs, st.rate,
        st.bssid[0], st.bssid[1], st.bssid[2], st.bssid[3], st.bssid[4], st.bssid[5]);
}

static void print_cipher(unsigned int c)
{
    if (c & MTK80211_CIPHER_CCMP) printf("CCMP ");
    if (c & MTK80211_CIPHER_TKIP) printf("TKIP ");
    if (c & (MTK80211_CIPHER_WEP40 | MTK80211_CIPHER_WEP104)) printf("WEP ");
}

static void cmd_results(io_connect_t c)
{
    int n = 0;
    printf("%-32s %-17s %4s %5s  %s\n", "SSID", "BSSID", "CH", "RSSI", "SECURITY");
    for (int i = 0; i < 256; i++) {
        struct ioctl_network_info ni = {0};
        kern_return_t kr = ioctl_get(c, IOCTL_80211_SCAN_RESULT, &ni, sizeof(ni));
        if (kr == kIONoScanResult)
            break;
        if (kr) { printf("scan result: error 0x%x\n", kr); break; }
        char ssid[NWID_LEN + 1] = {0};
        memcpy(ssid, ni.ssid, NWID_LEN);
        printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x %4u %5d  ", ssid[0] ? ssid : "<hidden>",
            ni.bssid[0], ni.bssid[1], ni.bssid[2], ni.bssid[3], ni.bssid[4], ni.bssid[5],
            ni.channel, ni.rssi);
        /* rsn_protos carries net80211 IEEE80211_PROTO_* bits. */
        if (ni.rsn_protos & MTK80211_PROTO_RSN) printf("WPA2 ");
        if (ni.rsn_protos & MTK80211_PROTO_WPA) printf("WPA ");
        if (ni.rsn_akms & MTK80211_AKM_PSK) printf("PSK ");
        if (ni.rsn_akms & MTK80211_AKM_8021X) printf("802.1X ");
        print_cipher(ni.rsn_ciphers);
        printf("\n");
        n++;
    }
    printf("%d network(s)\n", n);
}

static void cmd_debug(io_connect_t c)
{
    struct ioctl_debug d = {0};
    kern_return_t kr = ioctl_get(c, IOCTL_80211_DEBUG, &d, sizeof(d));
    if (kr) { printf("debug: error 0x%x (old driver without IOCTL_80211_DEBUG?)\n", kr); return; }
    printf("ic_state=%u sc_flags=0x%x nodes=%u intr_index=%d (%s)\n", d.ic_state, d.sc_flags,
        d.nodes, d.intr_index, d.intr_is_msi ? "MSI" : "INTx");
    printf("intr=%u mcu_events=%u last_eid=0x%x bad_seq=%u mcu_timeouts=%u\n",
        d.intr_count, d.mcu_events, d.last_mcu_eid, d.mcu_bad_seq, d.mcu_timeouts);
    printf("scan requests=%u done=%u watchdog=%u\n",
        d.scan_requests, d.scan_done_count, d.scan_watchdog_fired);
    printf("rx normal=%u unknown=%u fill_fail=%u\n",
        d.rx_pkt_normal, d.rx_pkt_unknown, d.rx_fill_fail);
    printf("lpctl=0x%08x int_sta=0x%08x int_ena=0x%08x\n", d.lpctl, d.int_sta, d.int_ena);
    printf("rxmcu  cpu=%u dma=%u cons=%u\n", d.rxmcu_cpu, d.rxmcu_dma, d.rxmcu_cons);
    printf("rxfwdl cpu=%u dma=%u cons=%u\n", d.rxfwdl_cpu, d.rxfwdl_dma, d.rxfwdl_cons);
    printf("rxdata cpu=%u dma=%u cons=%u\n", d.rxdata_cpu, d.rxdata_dma, d.rxdata_cons);
    printf("txmcu  cpu=%u dma=%u cons=%u\n", d.txmcu_cpu, d.txmcu_dma, d.txmcu_cons);
    printf("txdata cpu=%u dma=%u cons=%u\n", d.txdata_cpu, d.txdata_dma, d.txdata_cons);
    printf("port_valid=%u node_flags=0x%x wcid=%u rsn_supp_state=%u\n",
        d.port_valid, d.node_flags, d.wcid, d.rsn_supp_state);
    printf("rx delivered=%u normal_mcu=%u event=%u notify=%u if_ierr=%u if_oerr=%u\n",
        d.rx_delivered, d.rx_normal_mcu, d.rx_event, d.rx_notify, d.if_ierrors, d.if_oerrors);
    printf("aspm lnkctl 0x%x -> 0x%x  l1ss_ctl1 0x%x -> 0x%x  parent_l1ss 0x%x  (bit31 = captured)\n",
        d.aspm_lnkctl_before, d.aspm_lnkctl_after, d.aspm_l1ss_before, d.aspm_l1ss_after,
        d.aspm_parent_l1ss_before);
    printf("glo_cfg=0x%08x pci_cmd_status=0x%08x\n", d.glo_cfg, d.pci_status);
    printf("txdata@dma desc buf0=0x%08x ctrl=0x%08x buf1=0x%08x info=0x%08x\n",
        d.txdata_desc[0], d.txdata_desc[1], d.txdata_desc[2], d.txdata_desc[3]);
    printf("txdata@dma txp buf0=0x%08x len=0x%08x buf1=0x%08x token=%u\n",
        d.txdata_txp[0], d.txdata_txp[1], d.txdata_txp[2], d.txdata_txp[3]);
    printf("dummy_cr=0x%08x mcu_cmd=0x%08x mcu_cmd_irqs=%u last_mcu_cmd=0x%08x\n",
        d.dummy_cr, d.mcu_cmd, d.mcu_cmd_irqs, d.last_mcu_cmd);
    printf("knobs=0x%x dma_stalls=%u full_resets=%u\n", d.knobs, d.dma_stalls, d.full_resets);
    printf("rx ctl=%u bar=%u dup=%u\n", d.rx_ctl, d.rx_bar, d.rx_dup);
    printf("rx ba agreements=%u below_winstart=%u above_winend=%u slide=%u jump=%u no_buf=%u\n",
        d.ba_agreements, d.ba_below_winstart, d.ba_above_winend, d.ba_window_slide,
        d.ba_window_jump, d.ba_no_buf);
    printf("rx ba gap_timeouts=%u frames_lost=%u gap_timeout=%u ms\n",
        d.ba_gap_timeout, d.ba_frame_lost, d.ba_gap_timeout_ms);
    printf("rx batches=%u\n", d.rx_batches);
    printf("tx stalls=%u nobufs=%u ring_full=%u txwi_inuse=%u peak=%u oactive=%u\n",
        d.tx_stalls, d.tx_nobufs, d.tx_ring_full, d.txwi_inuse, d.txwi_peak, d.tx_oactive);
}

static void cmd_debug_set(io_connect_t c, uint32_t op, uint32_t val)
{
    struct ioctl_debug_set req = { .version = IOCTL_VERSION, .op = op, .val = val };
    kern_return_t kr = ioctl_set(c, IOCTL_80211_DEBUG, &req, sizeof(req));
    printf("debug op %u val 0x%x: %s (0x%x)\n", op, val, kr ? "error" : "ok", kr);
}

static int cmd_awdl_create(io_connect_t c)
{
    // v1/v2 can enter native startup without registering a logical link.
    // Refuse those builds before sending the external method, even when
    // this new executable is used from an old diagnostic directory.
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("AirportMtkWlan"));
    CFTypeRef version = svc ? IORegistryEntryCreateCFProperty(svc,
        CFSTR("MTKAWDLDiagnosticVersion"), kCFAllocatorDefault, 0) : NULL;
    int64_t diagnostic_version = 0;
    if (version && CFGetTypeID(version) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)version, kCFNumberSInt64Type, &diagnostic_version);
    if (version) CFRelease(version);
    if (svc) IOObjectRelease(svc);
    if (diagnostic_version < 3) {
        fprintf(stderr, "AWDL creation refused: load diagnostic v3 or newer first.\n");
        return 1;
    }
    struct ioctl_debug_set req = {
        .version = IOCTL_VERSION, .op = IOCTL_DEBUG_OP_AWDL_CREATE, .val = 0
    };
    kern_return_t kr = ioctl_set(c, IOCTL_80211_DEBUG, &req, sizeof(req));
    if (kr) {
        fprintf(stderr, "AWDL creation failed: 0x%x (%s)\n", kr, mach_error_string(kr));
        if (kr == kIOReturnUnsupported)
            fprintf(stderr, "Native Skywalk AWDL data path is unavailable; creation was blocked before native startup.\n");
        fprintf(stderr, "Check MTKAWDLCreateStage/Status on AirportMtkWlan and mtkctl log.\n");
        return 1;
    }
    printf("Native AWDL interface registered. Verify awdl0 with ifconfig; AirDrop still requires native sync.\n");
    return 0;
}

static int cmd_log(io_connect_t c)
{
    // Capture the head from the first read. Following a moving head until
    // the ring is empty can run forever while the kernel keeps logging.
    struct ioctl_log l = {0};
    uint64_t snapshot_head = 0, previous_cursor = 0;
    const unsigned max_reads = (256 * 1024) / MTK_LOG_CHUNK + 2;
    for (unsigned read = 0; read < max_reads; ++read) {
        kern_return_t kr = ioctl_get(c, IOCTL_80211_LOG, &l, sizeof(l));
        if (kr) { fprintf(stderr, "log: error 0x%x\n", kr); return 1; }
        if (l.version != IOCTL_VERSION || l.len > sizeof(l.data) ||
            l.cursor > l.head || l.cursor < l.len ||
            (read && l.cursor < previous_cursor) ||
            (l.len && read && l.cursor == previous_cursor)) {
            fprintf(stderr, "log: invalid or non-advancing response\n");
            return 1;
        }
        if (!read) snapshot_head = l.head;
        uint64_t chunk_start = l.cursor - l.len;
        size_t count = chunk_start < snapshot_head ?
            (size_t)(snapshot_head - chunk_start) : 0;
        if (count > l.len) count = l.len;
        if (count && fwrite(l.data, 1, count, stdout) != count) return 1;
        previous_cursor = l.cursor;
        if (!l.len || l.cursor >= snapshot_head) {
            fprintf(stderr, "[log head=%llu]\n", (unsigned long long)snapshot_head);
            return 0;
        }
    }
    fprintf(stderr, "log: snapshot read limit reached (head=%llu)\n",
        (unsigned long long)snapshot_head);
    return 1;
}

static void cmd_scan(io_connect_t c, int wait_s)
{
    struct ioctl_scan sc = { .version = IOCTL_VERSION };
    kern_return_t kr = ioctl_set(c, IOCTL_80211_SCAN, &sc, sizeof(sc));
    printf("scan request: %s (0x%x)\n", kr ? "error" : "ok", kr);
    for (int i = 0; i < wait_s; i++) {
        sleep(1);
        cmd_state(c);
    }
    cmd_results(c);
}

static void cmd_join(io_connect_t c, const char *ssid, const char *pass)
{
    struct ioctl_join j = { .version = IOCTL_VERSION };
    j.nwid.version = IOCTL_VERSION;
    j.nwid.len = (unsigned int)strnlen(ssid, NWID_LEN);
    memcpy(j.nwid.nwid, ssid, j.nwid.len);
    j.wpa_key.version = IOCTL_VERSION;
    if (pass) {
        j.wpa_key.len = (unsigned int)strnlen(pass, WPA_KEY_LEN - 1);
        memcpy(j.wpa_key.key, pass, j.wpa_key.len);
    }
    kern_return_t kr = ioctl_set(c, IOCTL_80211_JOIN, &j, sizeof(j));
    printf("join request: %s (0x%x)\n", kr ? "error" : "ok", kr);
    memset(&j, 0, sizeof(j));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: mtkctl info|state|sta|debug|log|awdl-create|knob <hex>|gap <ms>|dwell <ms|auto>|reset|dump|scan [seconds]|results|join <ssid> [password]\n");
        return 2;
    }
    io_connect_t c = open_driver();
    int result = 0;
    if (!strcmp(argv[1], "info")) { cmd_info(c); cmd_state(c); }
    else if (!strcmp(argv[1], "state")) cmd_state(c);
    else if (!strcmp(argv[1], "sta")) cmd_sta(c);
    else if (!strcmp(argv[1], "scan")) cmd_scan(c, argc > 2 ? atoi(argv[2]) : 8);
    else if (!strcmp(argv[1], "results")) cmd_results(c);
    else if (!strcmp(argv[1], "debug")) cmd_debug(c);
    else if (!strcmp(argv[1], "log")) result = cmd_log(c);
    else if (!strcmp(argv[1], "awdl-create")) result = cmd_awdl_create(c);
    else if (!strcmp(argv[1], "knob") && argc >= 3)
        cmd_debug_set(c, IOCTL_DEBUG_OP_KNOBS, (uint32_t)strtoul(argv[2], NULL, 0));
    else if (!strcmp(argv[1], "reset")) cmd_debug_set(c, IOCTL_DEBUG_OP_RESET, 0);
    else if (!strcmp(argv[1], "dump")) cmd_debug_set(c, IOCTL_DEBUG_OP_DUMP, 0);
    else if (!strcmp(argv[1], "gap") && argc >= 3)
        cmd_debug_set(c, IOCTL_DEBUG_OP_BA_GAP, (uint32_t)strtoul(argv[2], NULL, 0));
    else if (!strcmp(argv[1], "dwell") && argc >= 3)
        cmd_debug_set(c, IOCTL_DEBUG_OP_BGSCAN_DWELL,
            !strcmp(argv[2], "auto") ? 0xffffffffU : (uint32_t)strtoul(argv[2], NULL, 0));
    else if (!strcmp(argv[1], "join") && argc >= 3) cmd_join(c, argv[2], argc > 3 ? argv[3] : NULL);
    else { fprintf(stderr, "unknown command\n"); return 2; }
    IOServiceClose(c);
    return result;
}

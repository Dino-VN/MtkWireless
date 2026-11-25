#!/usr/bin/env python3
"""ASan/UBSan tests of production Wi-Fi RX stall, SER, and deaf-RX recovery.

Extracts mwx_intr, mwx_request_fullreset, mwx_dma_rx_pending, mwx_dma_timeo,
mwx_scan_reset_rx_deaf, and mwx_scan_check_rx_deaf verbatim from MtkMwx.cpp
and exercises them against a host register/ring model.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HAL = ROOT / 'kext/Wifi/vendor/SimpleMtkWlan/hal_mwx/MtkMwx.cpp'
REG = HAL.with_name('if_mwxreg.h')


def function(path, signature):
    text = path.read_text()
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def definitions(names):
    result = []
    for path in (REG, HAL):
        lines = iter(path.read_text().splitlines())
        for line in lines:
            match = re.match(r'\s*#define\s+(\w+)', line)
            block = line
            while block.endswith('\\'):
                block += '\n' + next(lines)
            if match and match[1] in names:
                result.append(block)
    return '\n'.join(result)


hal_text = HAL.read_text()
enum_start = hal_text.index('enum {\n\tMWX_FULLRESET_NONE')
enum_end = hal_text.index('};', enum_start) + 2
fullreset_enum = hal_text[enum_start:enum_end]

defs = definitions({
    'MWX_KNOB_AUTORESET',
    'MT_MCU_CMD', 'MT_MCU_CMD_WAKE_RX_PCIE', 'MT_MCU_CMD_STOP_DMA_FW_RELOAD',
    'MT_MCU_CMD_STOP_DMA', 'MT_MCU_CMD_RESET_DONE', 'MT_MCU_CMD_RECOVERY_DONE',
    'MT_MCU_CMD_NORMAL_STATE', 'MT_MCU_CMD_ERROR_MASK',
    'MT_WFDMA0_GLO_CFG', 'MT_WFDMA0_GLO_CFG_TX_DMA_EN',
    'MT_WFDMA0_GLO_CFG_RX_DMA_EN',
    'MT_WFDMA0_HOST_INT_STA', 'MT_WFDMA0_HOST_INT_ENA',
    'MT_DMA_DMA_IDX',
    'MT_INT_RX_DONE_DATA', 'MT_INT_RX_DONE_WM', 'MT_INT_RX_DONE_WM2',
    'MT_INT_TX_DONE_ALL', 'MT_INT_MCU_CMD',
    'HOST_RX_DONE_INT_ENA0', 'HOST_RX_DONE_INT_ENA2', 'HOST_RX_DONE_INT_ENA4',
    'MT_INT_TX_DONE_MCU_WM', 'MT_INT_TX_DONE_FWDL', 'MT_INT_TX0_TO_TX14',
    'HOST_TX_DONE_INT_ENA16', 'HOST_TX_DONE_INT_ENA17', 'MCU2HOST_SW_INT_ENA',
})

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define IFF_RUNNING 0x40
#define MWX_DEV_LOG(...) ((void)0)
#define DEVNAME(sc) "mwx0"
''' + defs + '\n' + fullreset_enum + r'''

struct ifnet { int if_flags = IFF_RUNNING; };
struct ieee80211com { ifnet ic_if; };
struct task { int queued = 0; };
struct taskq {};
struct mwx_desc {};
struct mwx_queue_data {};
struct mwx_queue {
    uint32_t mq_regbase = 0;
    uint32_t mq_count = 256;
    uint32_t mq_cons = 0;
    uint32_t mq_prod = 0;
    mwx_desc dummy_desc{};
    mwx_queue_data dummy_data{};
    mwx_desc *mq_desc = &dummy_desc;
    mwx_queue_data *mq_data = &dummy_data;
};
struct mwx_softc {
    ieee80211com sc_ic;
    uint32_t sc_intr_mask = MT_INT_RX_DONE_DATA | MT_INT_RX_DONE_WM |
                            MT_INT_RX_DONE_WM2 | MT_INT_TX_DONE_ALL |
                            MT_INT_MCU_CMD;
    uint32_t sc_knobs = MWX_KNOB_AUTORESET;
    uint32_t sc_intr_count = 0;
    uint32_t sc_dma_stalls = 0;
    uint32_t sc_full_resets = 0;
    uint32_t sc_fullreset_last_reason = 0;
    uint64_t sc_fullreset_last_at_us = 0;
    uint32_t sc_ser_events = 0;
    uint32_t sc_rx_irq_rescues = 0;
    uint32_t sc_rx_deaf_resets = 0;
    uint32_t sc_rx_deaf_scans = 0;
    uint32_t sc_rx_deaf_channels = 0;
    uint32_t sc_rx_deaf_mdrdy = 0;
    uint32_t sc_rx_deaf_baseline = 0;
    uint32_t sc_scan_start_rx = 0;
    uint32_t sc_mcu_cmd_irqs = 0;
    uint32_t sc_last_mcu_cmd = 0;
    int sc_dma_stall_logged = 0;
    int8_t sc_resetting = 0;
    bool sc_detaching = false;
    volatile int sc_sys_suspended = 0;
    uint32_t sc_rx_pkt_normal = 0;
    uint32_t sc_rx_pkt_normal_mcu = 0;
    int sc_dma_to = 0;
    taskq queue;
    taskq *sc_nswq = &queue;
    task sc_fullreset_task;
    mwx_queue sc_txq, sc_txmcuq, sc_rxq, sc_rxmcuq, sc_rxfwdlq;
};

static uint64_t now_us = 100000000ULL;
static uint64_t mtk_join_now() { return now_us; }
static std::map<uint32_t, uint32_t> regs;
static uint32_t rx_done_calls = 0, rx_fill_calls = 0, tx_done_calls = 0, dump_calls = 0;
static std::vector<mwx_queue *> rx_done_qs;

static uint32_t mwx_read(mwx_softc *, uint32_t reg) {
    auto it = regs.find(reg);
    return it == regs.end() ? 0 : it->second;
}
static void mwx_write(mwx_softc *, uint32_t reg, uint32_t val) {
    if (reg == MT_WFDMA0_HOST_INT_STA || reg == MT_MCU_CMD) {
        regs[reg] &= ~val;
    } else {
        regs[reg] = val;
    }
}
static bool mtk_wlan_dma_hw_index(uint32_t hw, uint32_t count, uint32_t *out) {
    if (hw == UINT32_MAX || count == 0 || hw >= count) return false;
    *out = hw;
    return true;
}
static void mwx_dump_dma_state(mwx_softc *, const char *) { ++dump_calls; }
static void mwx_dma_rx_done(mwx_softc *, mwx_queue *q) {
    ++rx_done_calls;
    rx_done_qs.push_back(q);
    q->mq_cons = mwx_read(nullptr, q->mq_regbase + MT_DMA_DMA_IDX);
}
static int mwx_queue_fill(mwx_softc *, mwx_queue *) { ++rx_fill_calls; return 0; }
static void mwx_dma_tx_done(mwx_softc *) { ++tx_done_calls; }
static void task_add(taskq *, task *t) { ++t->queued; }
static int timeout_add_sec(int *t, int sec) { *t = sec; return 0; }

static bool mwx_request_fullreset(struct mwx_softc *, const char *, uint32_t);
static void mwx_scan_reset_rx_deaf(struct mwx_softc *);
static bool mwx_scan_check_rx_deaf(struct mwx_softc *, uint32_t, uint32_t, uint32_t, uint32_t);
''' + '\n'.join(function(HAL, sig) for sig in (
    'int\nmwx_intr(',
    'static bool\nmwx_request_fullreset(',
    'static bool\nmwx_dma_rx_pending(',
    'void\nmwx_dma_timeo(',
    'static void\nmwx_scan_reset_rx_deaf(',
    'static bool\nmwx_scan_check_rx_deaf(',
)) + r'''

static void init_queues(mwx_softc &sc) {
    sc.sc_rxq.mq_regbase = 0xd4520;
    sc.sc_rxmcuq.mq_regbase = 0xd4540;
    sc.sc_rxfwdlq.mq_regbase = 0xd4500;
    sc.sc_txq.mq_regbase = 0xd4300;
    sc.sc_txmcuq.mq_regbase = 0xd4410;
}

static void test_deaf_rx_scans() {
    mwx_softc sc;
    init_queues(sc);

    // 1. Healthy single-channel empty scan (Yuki post-reboot scan #7: ch=1, mdrdy=11)
    sc.sc_rx_pkt_normal = 153401;
    sc.sc_scan_start_rx = 153401;
    assert(!mwx_scan_check_rx_deaf(&sc, 1, 0, 0, 11));
    assert(sc.sc_rx_deaf_scans == 1 && sc.sc_fullreset_task.queued == 0);
    // Next scan sees beacons -> resets streak immediately
    assert(!mwx_scan_check_rx_deaf(&sc, 14, 22, 22, 88));
    assert(sc.sc_rx_deaf_scans == 0 && sc.sc_rx_deaf_channels == 0);

    // 2. Healthy boot scan where beacons arrive via normal_mcu (rx_norm stays 0)
    sc.sc_rx_pkt_normal = 0;
    sc.sc_rx_pkt_normal_mcu = 0;
    sc.sc_scan_start_rx = 0;
    sc.sc_rx_pkt_normal_mcu = 12;
    assert(!mwx_scan_check_rx_deaf(&sc, 41, 0, 0, 800));
    assert(sc.sc_rx_deaf_scans == 0);

    // 3. Faraday cage / no Wi-Fi on air (mdrdy == 0) never triggers reset
    sc.sc_scan_start_rx = sc.sc_rx_pkt_normal + sc.sc_rx_pkt_normal_mcu;
    for (int i = 0; i < 10; ++i)
        assert(!mwx_scan_check_rx_deaf(&sc, 41, 0, 0, 0));
    assert(sc.sc_fullreset_task.queued == 0);

    // 4. Yuki Nov 24 1.1.2 replay: 41 ch, beacon=0, probe=0, mdrdy=1100, rx frozen
    sc.sc_rx_pkt_normal = 82667;
    sc.sc_rx_pkt_normal_mcu = 0;
    sc.sc_scan_start_rx = 82667;
    assert(!mwx_scan_check_rx_deaf(&sc, 41, 0, 0, 1100));
    assert(sc.sc_rx_deaf_scans == 1 && sc.sc_fullreset_task.queued == 0);
    assert(mwx_scan_check_rx_deaf(&sc, 41, 0, 0, 1200));
    assert(sc.sc_fullreset_task.queued == 1);
    assert(sc.sc_resetting == 1);
    assert(sc.sc_rx_deaf_resets == 1);
    assert(sc.sc_fullreset_last_reason == MWX_FULLRESET_DEAF_RX);

    // 5. Cooldown prevents back-to-back resets within 30 s
    sc.sc_resetting = 0;
    now_us += 10000000ULL;
    assert(!mwx_scan_check_rx_deaf(&sc, 41, 0, 0, 1000));
    assert(!mwx_scan_check_rx_deaf(&sc, 41, 0, 0, 1000));
    assert(sc.sc_fullreset_task.queued == 1);

    // 6. Yuki Nov 25 1.1.3 broken_backhaul replay (15 ch + 2 ch) after cooldown
    now_us += 25000000ULL; // total 35 s elapsed
    assert(!mwx_scan_check_rx_deaf(&sc, 15, 0, 0, 300));
    assert(mwx_scan_check_rx_deaf(&sc, 2, 0, 0, 40));
    assert(sc.sc_fullreset_task.queued == 2);
    assert(sc.sc_rx_deaf_resets == 2);

    // 7. Chunked AWDL dead scans (4 ch x 4 chunks = 16 ch)
    sc.sc_resetting = 0;
    now_us += 35000000ULL;
    assert(!mwx_scan_check_rx_deaf(&sc, 4, 0, 0, 20));
    assert(!mwx_scan_check_rx_deaf(&sc, 4, 0, 0, 20));
    assert(!mwx_scan_check_rx_deaf(&sc, 4, 0, 0, 20));
    assert(mwx_scan_check_rx_deaf(&sc, 4, 0, 0, 20));
    assert(sc.sc_fullreset_task.queued == 3);

    // 8. Host RX arriving between two deaf scans resets streak
    sc.sc_resetting = 0;
    now_us += 35000000ULL;
    sc.sc_scan_start_rx = sc.sc_rx_pkt_normal + sc.sc_rx_pkt_normal_mcu;
    assert(!mwx_scan_check_rx_deaf(&sc, 15, 0, 0, 300));
    assert(sc.sc_rx_deaf_scans == 1);
    sc.sc_rx_pkt_normal++; // frame arrived between scans
    sc.sc_scan_start_rx = sc.sc_rx_pkt_normal + sc.sc_rx_pkt_normal_mcu;
    assert(!mwx_scan_check_rx_deaf(&sc, 15, 0, 0, 300));
    assert(sc.sc_rx_deaf_scans == 1 && sc.sc_fullreset_task.queued == 3);

    // 9. MWX_KNOB_AUTORESET disabled suppresses automatic reset
    sc.sc_knobs &= ~MWX_KNOB_AUTORESET;
    assert(!mwx_scan_check_rx_deaf(&sc, 15, 0, 0, 300));
    assert(sc.sc_fullreset_task.queued == 3);
}

static void test_dma_timeo_and_ser() {
    mwx_softc sc;
    init_queues(sc);
    regs.clear();
    rx_done_calls = rx_fill_calls = tx_done_calls = dump_calls = 0;
    rx_done_qs.clear();

    // 1. Healthy DMA with missed RX data IRQ -> rescued by mwx_dma_timeo
    regs[MT_WFDMA0_GLO_CFG] = MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN;
    regs[sc.sc_rxq.mq_regbase + MT_DMA_DMA_IDX] = 7;
    sc.sc_txq.mq_prod = 2; // pending TX
    mwx_dma_timeo(&sc);
    assert(sc.sc_rx_irq_rescues == 1);
    assert(rx_done_calls == 1 && rx_done_qs[0] == &sc.sc_rxq);
    assert(sc.sc_rxq.mq_cons == 7);
    assert(rx_fill_calls == 2); // rxmcuq + rxfwdlq
    assert(tx_done_calls == 1);
    assert(sc.sc_dma_to == 2);
    assert(sc.sc_fullreset_task.queued == 0);

    // 2. All-ones GLO_CFG (bus unreadable): does not crash or touch rings
    regs[MT_WFDMA0_GLO_CFG] = UINT32_MAX;
    uint32_t prev_fill = rx_fill_calls;
    mwx_dma_timeo(&sc);
    assert(sc.sc_dma_stalls == 1 && rx_fill_calls == prev_fill);
    assert(sc.sc_fullreset_task.queued == 0 && sc.sc_dma_to == 2);

    // 3. RX_DMA_EN cleared -> triggers full reset
    sc.sc_dma_stall_logged = 0;
    regs[MT_WFDMA0_GLO_CFG] = MT_WFDMA0_GLO_CFG_TX_DMA_EN; // RX_DMA_EN = 0
    now_us += 40000000ULL;
    sc.sc_dma_to = 0;
    mwx_dma_timeo(&sc);
    assert(sc.sc_dma_stalls == 2);
    assert(sc.sc_fullreset_task.queued == 1);
    assert(sc.sc_fullreset_last_reason == MWX_FULLRESET_DMA_DISABLED);
    assert(sc.sc_dma_to == 0); // not re-armed once reset queued

    // 4. Polled SER in mwx_dma_timeo (MT_MCU_CMD_STOP_DMA)
    sc.sc_resetting = 0;
    now_us += 40000000ULL;
    regs[MT_WFDMA0_GLO_CFG] = MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN;
    regs[MT_MCU_CMD] = MT_MCU_CMD_STOP_DMA;
    mwx_dma_timeo(&sc);
    assert(sc.sc_ser_events == 1);
    assert(sc.sc_fullreset_task.queued == 2);
    assert(sc.sc_fullreset_last_reason == MWX_FULLRESET_SER);
    assert(regs[MT_MCU_CMD] == 0);

    // 5. IRQ-driven SER in mwx_intr: STOP_DMA_FW_RELOAD triggers full reset,
    //    while WAKE_RX_PCIE and NORMAL_STATE do not.
    sc.sc_resetting = 0;
    now_us += 40000000ULL;
    regs[MT_WFDMA0_HOST_INT_STA] = MT_INT_MCU_CMD;
    regs[MT_MCU_CMD] = MT_MCU_CMD_WAKE_RX_PCIE | MT_MCU_CMD_NORMAL_STATE;
    mwx_intr(&sc);
    assert(sc.sc_ser_events == 1 && sc.sc_fullreset_task.queued == 2);

    regs[MT_WFDMA0_HOST_INT_STA] = MT_INT_MCU_CMD;
    regs[MT_MCU_CMD] = MT_MCU_CMD_STOP_DMA_FW_RELOAD;
    mwx_intr(&sc);
    assert(sc.sc_ser_events == 2 && sc.sc_fullreset_task.queued == 3);
    assert(sc.sc_fullreset_last_reason == MWX_FULLRESET_SER);
}

int main() {
    test_deaf_rx_scans();
    test_dma_timeo_and_ser();
    std::puts("PASS: Wi-Fi deaf-RX detection, SER reset, WFDMA RX/TX watchdog, missed-IRQ rescue, cooldown and knob guard");
}
'''

with tempfile.TemporaryDirectory(prefix='mtk-wifi-rx-recovery-') as directory:
    cpp = Path(directory) / 'recovery.cpp'
    binary = cpp.with_suffix('')
    cpp.write_text(code)
    subprocess.run(['/usr/bin/clang++', '-std=c++17', '-g', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-Wall', '-Wno-unused-function', str(cpp), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)

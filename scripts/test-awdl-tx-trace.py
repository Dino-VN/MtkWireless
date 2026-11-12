#!/usr/bin/env python3
"""Offline ASan/UBSan extraction tests for bounded HAL ordinary-unicast TX traces.

The descriptor builders, token/DMA/TX-free paths, AWDL TX/peer/quiesce functions,
trace publisher and raw-TXS AWDL arm are extracted verbatim from current source.
Only OS objects, mbuf/DMA transport, MCU replies, time and register IO are host
boundaries. Uses a fresh, offline, Wi-Fi-only Rust FFI, never a retained archive.
The real Rust TXS/TX-free parsers and existing rate logic remain in the loop.
Rust itself is not sanitizer-instrumented; no firmware, RF or kernel execution.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "kext/Wifi/vendor"
HAL = VENDOR / "SimpleMtkWlan/hal_mwx/MtkMwx.cpp"
HEADER = HAL.with_suffix(".hpp")
REG = HAL.with_name("if_mwxreg.h")
IEEE = VENDOR / "mtk80211/openbsd/net80211/ieee80211.h"
INPUT = IEEE.with_name("ieee80211_input.c")
NODE = IEEE.with_name("ieee80211_node.h")
TRACE = VENDOR / "include/AWDL/AwdlTxTrace.hpp"
TEMPLATE = ROOT / "tests/awdl-tx-trace.cpp"
COMPANION = ROOT / "scripts/test-awdl-native-lifecycle.py"
spec = importlib.util.spec_from_file_location("awdl_trace_build_support", COMPANION)
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class Extractor:
    def __init__(self):
        self.sources = {}
        self.inventory = []

    def text(self, path):
        if path not in self.sources:
            self.sources[path] = path.read_text()
        return self.sources[path]

    def retain(self, path, name, text):
        self.inventory.append({"path": str(path), "name": name,
                               "sha256": build.digest(text.encode()), "bytes": len(text.encode())})
        return text

    def function(self, path, signature):
        source = self.text(path)
        at = source.index(signature)
        return self.retain(path, signature, source[at:source.index("\n}", at) + 3])

    def structure(self, path, name):
        source = self.text(path)
        at = source.index("struct " + name + " {")
        end = source.index("\n}", at)
        return self.retain(path, name, source[at:source.index(";", end) + 1])

    def block(self, path, name, start, end):
        source = self.text(path)
        at = source.index(start)
        return self.retain(path, name, source[at:source.index(end, at)])

    def definitions(self, path, prefixes):
        lines = self.text(path).splitlines()
        out, i = [], 0
        while i < len(lines):
            text = lines[i]
            match = re.match(r"\s*#define\s+(\w+)", text)
            while text.endswith("\\") and i + 1 < len(lines):
                i += 1
                text += "\n" + lines[i]
            if match and match[1].startswith(prefixes):
                out.append(text)
            i += 1
        return self.retain(path, "macros " + repr(prefixes), "\n".join(out))


def generate(ex):
    macros = ex.definitions(REG, ("MT_TX", "MT_LMAC", "MT_HDR_", "MWX_MAX_", "MWX_WTBL_",
        "MT_PACKET_", "MT_HW_TXP_", "MT_MSDU_", "MCU_UNI_CMD_STA_REC_UPDATE", "STA_REC_",
        "WTBL_", "STA_TYPE_", "NETWORK_", "CONN_STATE_", "EXTRA_INFO_", "MT_DMA_", "MWX_TXWI_MAX"))
    macros += "\n" + ex.definitions(HAL, ("MWX_KNOB_",))
    macros += "\n" + ex.definitions(IEEE, ("IEEE80211_FC", "IEEE80211_QOS_", "IEEE80211_ADDR_LEN",
        "IEEE80211_IS_MULTICAST", "IEEE80211_AID", "IEEE80211_ACTION_"))
    macros += "\n" + ex.definitions(NODE, ("IEEE80211_NODE_QOS",))
    wire = "\n".join(ex.structure(REG, name) for name in ("mwx_desc", "mt76_connac_txp_ptr",
        "mt76_txwi", "sta_req_hdr", "sta_rec_basic", "sta_rec_wtbl", "wtbl_generic", "wtbl_rx", "wtbl_hdr_trans", "wtbl_ht"))
    frames = "\n".join(ex.structure(IEEE, name) for name in ("ieee80211_frame", "ieee80211_qosframe",
        "ieee80211_qosframe_addr4", "ieee80211_frame_min"))
    helpers = "\n".join(ex.function(IEEE, "static __inline " + typ + "\n" + name + "(")
        for typ, name in [("int", "ieee80211_has_seq"), ("int", "ieee80211_has_addr4"),
                         ("int", "ieee80211_has_qos"), ("int", "ieee80211_has_htc"),
                         ("u_int16_t", "ieee80211_get_qos")])
    helpers += "\n" + ex.function(INPUT, "u_int\nieee80211_get_hdrlen(")
    source = ex.text(HEADER)
    at = source.index("    struct AwdlTxResult {")
    result = ex.retain(HEADER, "AwdlTxResult", source[at:source.index("    };", at) + 6])
    reg = ex.text(REG)
    at = reg.index("MT_PHY_TYPE_CCK")
    at = reg.rfind("enum", 0, at)
    phy = ex.retain(REG, "PHY modes", reg[at:reg.index("\n};", at) + 3])
    functions = "\n".join(ex.function(HAL, signature) for signature in [
        "static int mwx_awdl_rate_start(", "static int mwx_awdl_fixed_rate(", "static void mwx_awdl_reset_rate(",
        "static uint16_t mwx_awdl_fixed_rate_value(", "uint32_t\nmt7921_mac_tx_rate_val(",
        "void\nmt7921_mac_write_txwi_80211(", "static inline uint8_t\nmt7921_lmac_mapping(",
        "void\nmt7921_mac_write_txwi(", "struct mwx_txwi *\nmwx_txwi_get(", "void\nmwx_txwi_put(",
        "int\nmwx_txwi_enqueue(", "static inline int\nmwx_dma_free_slots(", "int\nmwx_tx_full(",
        "static int\nmwx_dma_txwi_enqueue_locked(", "int\nmwx_dma_txwi_enqueue(",
        "static int\nmwx_dma_tx_cleanup_locked(", "void\nmwx_dma_tx_cleanup(", "static int\nmwx_eapol_keyinfo(",
        "int\nmwx_tx(", "void\nmwx_mac_tx_free("])
    tail = "\n".join(ex.function(HAL, signature) for signature in [
        "void\nmt7921_mcu_tx_done_event(", "bool MtkMwx::awdlCanTransmit(", "IOReturn MtkMwx::awdlTryTransmit(",
        "void *\nmwx_append_len(", "void *\nmwx_append_tlv(", "struct mbuf *\nmwx_fill_sta_req_hdr(",
        "static int mwx_awdl_remove_station(", "static bool mwx_awdl_txba_enabled(", "IOReturn MtkMwx::awdlPeer(",
        "static void mwx_awdl_quiesce(mwx_softc *sc,", "void\nmt7921_mcu_add_basic_tlv(",
        "int\nmt7921_mcu_wtbl_generic_tlv("])
    parts = {
        "@MACROS@": macros, "@WIRE@": wire, "@FRAMES@": frames, "@FRAME_HELPERS@": helpers,
        "@QUEUE_ENUM@": ex.block(REG, "TX queues", "enum mt76_txq_id {", "\n};") + "\n};",
        "@PHY_ENUM@": phy, "@RESULT_TYPE@": result,
        "@NODES@": ex.structure(HAL, "mwx_node") + "\n" + ex.structure(HAL, "mwx_vif"),
        "@NATIVE@": ex.block(HAL, "native UMI containment", "// Native UMI containment,", "\nenum mwx_hw_type"),
        "@TX_STRUCTS@": "\n".join(ex.structure(HAL, n) for n in ("mwx_txwi", "mwx_txwi_desc", "mwx_queue_data", "mwx_queue")),
        "@TRACE_HELPERS@": ex.block(HAL, "HAL trace hooks and publisher", "// MtkWireless: all TX diagnostic state", "// Only one operation may own"),
        "@OPERATIONS@": ex.block(HAL, "AWDL gate operations", "class MwxAwdlCall {", "\nstatic bool mwx_awdl_current(")
            + ex.function(HAL, "static bool mwx_awdl_current(mwx_softc *sc,"),
        "@RATES@": ex.block(HAL, "hardware rate table", "const struct mwx_rate {", "\n};") + "\n};",
        "@FUNCTIONS@": functions,
        "@TXS_ARM@": ex.block(HAL, "actual AWDL TXS arm", "\tconst uint8_t *raw=reinterpret_cast<const uint8_t *>(txs_data);", "\tstruct ieee80211com *ic ="),
        "@TAIL_FUNCTIONS@": tail,
    }
    program = TEMPLATE.read_text()
    for marker, text in parts.items():
        assert program.count(marker) == 1, marker
        program = program.replace(marker, text)
    assert not re.search(r"@[A-Z_]+@", program)
    return program


def source_contract(ex):
    submit = ex.function(HAL, "static int\nmwx_dma_txwi_enqueue_locked(")
    assert submit.index("return EBUSY") < submit.index("mwx_awdl_tx_trace_submit(sc,mt)") < submit.index("mwx_write(")
    publish = ex.function(HAL, "void MtkMwx::awdlPublishDiagnostics()")
    assert "mwx_awdl_tx_trace_publish(com,mwxController());" in publish
    assert "sc_awdl_enabled" not in publish[:publish.index("mwx_awdl_tx_trace_publish")]
    # Watchdog -> frontend cache -> HAL publisher remains reachable with sync off.
    controller = VENDOR / "AirportMtkWlan/AirportMtkWlan.cpp"
    frontend = VENDOR / "AirportMtkWlan/AirportMtkAwdlInterface.cpp"
    assert "publishCacheDiagnostics()" in ex.text(controller)
    cache = ex.function(frontend, "void AirportMtkAwdlInterface::publishCacheDiagnostics()")
    assert "if(hal) hal->awdlPublishDiagnostics();" in cache
    return ["timestamp follows last admission failure and precedes doorbell",
            "HAL publish hook has no enabled guard", "watchdog cache publisher calls HAL after sync off"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnostics", type=Path, help="new empty directory below build/diagnostics")
    args = parser.parse_args()
    parent = ROOT / "build/diagnostics"
    parent.mkdir(parents=True, exist_ok=True)
    if args.diagnostics:
        out = args.diagnostics.resolve()
        if not out.is_relative_to(parent.resolve()):
            parser.error("diagnostics must be below build/diagnostics")
        out.mkdir(parents=True, exist_ok=False)
    else:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out = Path(tempfile.mkdtemp(prefix="awdl-tx-trace-" + stamp + "-", dir=parent))
    print("HAL TX trace evidence: " + str(out), flush=True)
    run = build.CommandLog(out, build.host_environment())
    result = {"started_at": build.utc(), "diagnostics": str(out), "result": "FAIL"}
    ex, provenance, archive = Extractor(), None, None
    inputs = [HAL, HEADER, REG, IEEE, INPUT, NODE, TRACE, TEMPLATE, COMPANION, Path(__file__).resolve(),
              ROOT / "kext/Wifi/mtk_wlan.h", VENDOR / "include/AWDL/AwdlProtocol.hpp"]
    before = {str(p): build.digest(p.read_bytes()) for p in inputs}
    (out / "source-before.json").write_text(json.dumps(before, indent=2) + "\n")
    try:
        program = generate(ex)
        (out / "hal-tx-trace.cpp").write_text(program)
        (out / "extraction.json").write_text(json.dumps(ex.inventory, indent=2) + "\n")
        result["source_contract"] = source_contract(ex)
        provenance = build.build_fresh_archive(out, run)
        (out / "archive-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        archive = Path(provenance["archive"])
        run.checked("rust-parser-tests", [provenance["cargo"], "test", "--offline", "--locked", "-p", "mtk-wlan",
                    "--target-dir", str(out / "rust-host"), "connac2::event::tests"], 300, cwd=ROOT / "rust")
        print(run.output("rust-parser-tests"), end="", flush=True)
        flags = ["/usr/bin/clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-sign-compare",
                 "-Wno-misleading-indentation", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-pthread", "-I" + str(VENDOR / "include"),
                 "-I" + str(ROOT / "kext/Wifi"), "-I" + str(ROOT / "kext/include")]
        run.checked("compile", flags + [str(out / "hal-tx-trace.cpp"), str(archive), "-o", str(out / "hal-tx-trace")], 180)
        run.checked("run", [str(out / "hal-tx-trace")], 180)
        print(run.output("run"), end="", flush=True)
        result["result"] = "PASS"
    except Exception as error:
        result["error"] = repr(error)
        print(repr(error), flush=True)
    finally:
        after = {str(p): build.digest(p.read_bytes()) for p in inputs}
        changes = {p: {"before": before[p], "after": after[p]} for p in before if before[p] != after[p]}
        result.update(ended_at=build.utc(), source_changes=changes,
                      archive_unchanged=bool(archive and archive.is_file() and build.digest(archive.read_bytes()) == provenance["sha256"]),
                      limitations=["C++ ASan/UBSan only; real Rust archive is not sanitizer-instrumented",
                                   "IOKit, MCU, DMA hardware and timing are explicit host boundaries; no RF/kernel test",
                                   "Reset quiesce/put are executed; firmware reset/re-enable is a modeled boundary",
                                   "TX-free duplicates after normal completed-token reuse have no generation on the wire",
                                   "No descriptors, RA/ACK policy, native UMI, or DMA ownership is repaired by this trace"])
        if changes or not result["archive_unchanged"]:
            result["result"] = "FAIL"
        (out / "source-after.json").write_text(json.dumps(after, indent=2) + "\n")
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("HAL TX trace " + result["result"] + "; evidence: " + str(out), flush=True)
    return int(result["result"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())

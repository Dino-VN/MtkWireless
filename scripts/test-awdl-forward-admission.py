#!/usr/bin/env python3
"""Offline, production-extracted selective AWDL admission regression.

The real tick/service/native notification and lifecycle bodies run against fresh
Wi-Fi-only Rust FFI and layout checks. Only OS/IOKit/HAL boundaries are modeled.
The baseline uses the real amtk_awdl_forward=0 escape hatch. A second, copied
Rust workspace removes legacy aging ONLY as a retention-only negative control.
Neither copy changes production sources. All comparisons use identical durations.
Unchanged native-window assertion companions run with default/off boot-argument
boundaries; a copied-header no-settling control must reproduce the original
slow-first/fast-later assertion failure. No upstream assertion is weakened.
C++ uses ASan/UBSan; Rust is a normal host build. No firmware, native runtime,
AP ACK, RF, transfer progress, or throughput/root-cause claim is made.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('awdl_lifecycle', ROOT / 'scripts/test-awdl-native-lifecycle.py')
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)


def one(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new, 1)


def model_boundaries(text):
    # Edits below are to explicit host OS/HAL stubs, never production functions.
    text = one(text, ' void setTimeoutUS(unsigned n) { pending=true; us=n; }',
               ' void setTimeoutUS(unsigned n) { pending=true; us=n; ++calls; }')
    text = one(text, ' bool awdlIsEnabled() { return on; }',
               ' bool awdlIsEnabled() { return on && storage.sc_awdl_enabled && !storage.sc_resetting && !storage.sc_detaching && !storage.sc_awdl_stopping; }')
    text = one(text, ' bool awdlGrantSnapshot(mtkawdl::Reservation &out) { out=grant; return on; }',
               ' bool awdlGrantSnapshot(mtkawdl::Reservation &out) { if(!awdlIsEnabled())return false; out=grant; return true; }')
    text = one(text, ' bool on=false,thirdChannel=false,homeChannel8=false,allow11=false,dense=false;', r'''
 bool on=false,thirdChannel=false,homeChannel8=false,allow11=false,dense=false;
 bool modelPS=false,modelScan=false,modelJoin=false,modelOffchan=true;
 uint64_t modelPMAt=0,modelPMRefused=0;
 std::set<unsigned> modelLegal{1,6,9,11,36,44,149};
 std::function<void()> modelBeforeAbort,modelAfterAbort,modelAfterSleep,modelReserveEntry;
 std::function<void(unsigned)> modelTx;
''')
    text = one(text, ' bool awdlChannelAllowed(unsigned c) { return fault.social&&(c==6||c==44||(thirdChannel&&c==149)||(homeChannel8&&c==8)||(allow11&&c==11)); }',
               ' bool awdlChannelAllowed(unsigned c) { return fault.social&&modelLegal.count(c); }')
    text = one(text, ' uint8_t awdlOnlyChannel() { return infraChannel; }',
               ' uint8_t awdlOnlyChannel() { return modelOffchan ? 0 : infraChannel; }')
    text = one(text, ' bool awdlChannelReachable(unsigned c) { return awdlChannelAllowed(c)&&(!infraChannel||infraChannel==c); }',
               ' bool awdlChannelReachable(unsigned c) { return awdlChannelAllowed(c)&&(!awdlOnlyChannel()||awdlOnlyChannel()==c); }')
    text = one(text, ' void awdlInfraSleep(unsigned) { ++sleeps; }', r'''
 void awdlInfraSleep(unsigned c) {
  ++sleeps;
  if(!modelPS && infraChannel && c!=infraChannel && !modelJoin && on) {
   // Immediate successful Null submission, NOT an AP ACK.
   modelPS=true; modelPMAt=clockUS.load();
  }
  auto hook=modelAfterSleep; if(hook)hook();
 }''')
    text = one(text, ' void awdlInfraWakeIfIdle() { ++wakes; }', r'''
 void awdlInfraWakeIfIdle() {
  ++wakes; if(!modelPS)return;
  if(!infraChannel){modelPS=false;return;}
  const auto now=clockUS.load();
  bool away=now-modelPMAt<10000 || (grant.channel!=infraChannel &&
    ((grant.pending&&now<grant.deadline)||(grant.granted&&now<grant.deadline+1000)));
  if(!on || modelJoin)away=false;
  if(!away)modelPS=false;
 }''')
    text = one(text, ' bool awdlCanTransmit(unsigned c) { return grant.canTransmit(c,clockUS.load()); }', r'''
 bool awdlCanTransmit(unsigned c) { return on && storage.sc_awdl_enabled &&
   !storage.sc_resetting && !storage.sc_detaching && !storage.sc_awdl_stopping &&
   grant.canTransmit(c,clockUS.load()); }''')
    text = one(text, ' void awdlAbort() { gatedBoundary(); ++aborts; grant.cancel(); }', r'''
 void awdlAbort() { gatedBoundary(); if(modelBeforeAbort)modelBeforeAbort();
  ++aborts; grant.cancel(); auto hook=modelAfterAbort; if(hook)hook(); }''')
    text = one(text, '  if(timing)*timing={}; gatedBoundary(); ++reserves; requested=c; duration=ms;', r'''
  if(timing)*timing={}; gatedBoundary(); ++reserves; requested=c; duration=ms;
  if(modelReserveEntry)modelReserveEntry();
  if(!on||!storage.sc_awdl_enabled||storage.sc_resetting||storage.sc_detaching||storage.sc_awdl_stopping||modelJoin||modelScan) return kIOReturnNotReady;
  if(!awdlChannelReachable(c))return kIOReturnBadArgument;
  if(infraChannel && c!=infraChannel) {
   if(!modelPS)awdlInfraSleep(c);
   if(!modelPS||clockUS.load()-modelPMAt<2000){++modelPMRefused;return kIOReturnNotReady;}
   duration=std::min(duration,300u);
  }
  // Actual reserve cancels the old reservation before an acquisition.
  if(modelBeforeAbort)modelBeforeAbort();
  grant.cancel();
''')
    text = one(text, ' IOReturn awdlTransmit(mbuf_t m,unsigned) { gatedBoundary(); ++transmits; mbuf_freem(m); return 0; }', r'''
 IOReturn awdlTransmit(mbuf_t m,unsigned c) { gatedBoundary();
  assert(awdlCanTransmit(c)); if(modelTx)modelTx(c); ++transmits; mbuf_freem(m); return 0; }''')
    text = one(text, '  onStart=onEnd=nullptr; radio.onPeer=nullptr; radio.onReserve=nullptr; radio.onReserveTiming=nullptr; radio.wait=nullptr; radio.onEnable=nullptr;',
               '  onStart=onEnd=nullptr; radio.onPeer=nullptr; radio.onReserve=nullptr; radio.onReserveTiming=nullptr; radio.wait=nullptr; radio.onEnable=nullptr; radio.modelBeforeAbort=radio.modelAfterAbort=radio.modelAfterSleep=radio.modelReserveEntry=nullptr; radio.modelTx=nullptr;')
    return text


def retained_control(out, run, fresh):
    workspace = out / 'retention-only-workspace'
    workspace.mkdir()
    for name in L.rust_input_hashes():
        dst = workspace / Path(name).relative_to('rust')
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, dst)
    path = workspace / 'mtk-wlan/src/awdl/timeline.rs'
    original = path.read_text()
    modified = one(original, '                    && self.observations.wrapping_sub(e.observed) < 8\n', '')
    modified = one(modified, '        if !retain && observations.wrapping_sub(entry.observed) >= 8 {',
                   '        if false && !retain && observations.wrapping_sub(entry.observed) >= 8 {')
    path.write_text(modified)
    (out / 'retention-only-change.json').write_text(json.dumps({
        'path': str(path), 'original_sha256': L.digest(original.encode()),
        'negative_control_sha256': L.digest(modified.encode()),
        'change': 'Disable only legacy other-channel aging in copied lead/observe; never enabled in production.'}, indent=2)+'\n')
    result = run.checked('cargo-retention-only', [fresh['cargo'], 'build', '--offline', '--locked',
        '-p', 'mtk-ffi', '--no-default-features', '--features', 'wifi', '--target-dir', str(out / 'rust-retention-only'),
        '--message-format=json-render-diagnostics'], 300, cwd=workspace)
    rows = [json.loads(line) for line in result.splitlines() if line.startswith('{')]
    artifacts = {r['target']['name']: r for r in rows if r.get('reason') == 'compiler-artifact'}
    assert 'mtk_bt' not in artifacts and not artifacts['mtk_ffi']['fresh']
    return next(Path(p) for p in artifacts['mtk_ffi']['filenames'] if p.endswith('.a'))


def native_windows_companions(ex, out, run, fresh, flags):
    # Original assertion literals remain byte-for-byte unchanged. Use their
    # existing OS/HAL boundary fixture, not the latency/PM simulation above.
    program = one(L.original_program(ex, 'native-windows'), '#include "extracted.cpp"',
                  '#include "extracted-production.cpp"')
    for mode in ('default', 'off'):
        driver = program
        if mode == 'off':
            driver = one(driver, 'resetFaults();gate.mutex.lock();',
                         'resetFaults();bootArguments["amtk_awdl_forward"]=0;gate.mutex.lock();')
        name = 'native-windows-' + mode
        (out / (name + '.cpp')).write_text(driver)
        run.checked('compile-' + name, flags + [str(out / (name + '.cpp')), fresh['archive'], '-o', str(out / name)])
        print(run.checked('run-' + name, [str(out / name)], 120), flush=True)
    # Prove the unchanged sparse, slow-first/fast-later assertion catches the
    # regression. Disable ONLY the new grace in a copied production helper.
    # The default and boot-argument-off runs above still use the actual header.
    header = L.VENDOR / 'include/AWDL/AwdlForwardAdmission.hpp'
    copied = out / 'no-settling-include/AWDL/AwdlForwardAdmission.hpp'
    copied.parent.mkdir(parents=True)
    original = header.read_text()
    condition = 'if(eligible[i] && legacy && now-last<8*cycle && timing.recoveryGrace(p.channel,*legacy))'
    modified = one(original, condition, condition.replace('if(', 'if(false && ', 1))
    copied.write_text(modified)
    (out / 'no-settling-change.json').write_text(json.dumps({
        'source': str(header), 'copy': str(copied), 'original_sha256': L.digest(original.encode()),
        'negative_control_sha256': L.digest(modified.encode()),
        'change': 'Disable only selective settling grace in copied header; original assertions unchanged.'}, indent=2)+'\n')
    name = 'native-windows-no-settling'
    copied_flags = flags[:1] + ['-I'+str(copied.parent.parent)] + flags[1:]
    run.checked('compile-' + name, copied_flags + [str(out / 'native-windows-default.cpp'), fresh['archive'], '-o', str(out / name)])
    status = run('run-' + name, [str(out / name)], 120)
    log = (out / ('run-' + name + '.log')).read_text()
    assertion = 'Assertion failed: (requests149>=8&&starts149>=8&&pairedStarts-pairedEnds<=1)'
    assert status != 0 and assertion in log and 'runtime error:' not in log, 'Settling negative control failed for an unexpected reason'
    print('PASS unchanged native-window assertions in default/off modes; no-settling control hits the original cold-outlier assertion', flush=True)
    return {'default': 'PASS', 'off': 'PASS', 'no_settling': 'EXPECTED original cold-outlier assertion failure'}


def source_hashes():
    sources = L.protected_hashes()
    for path in [Path(__file__).resolve(), ROOT / 'tests/awdl-radio/forward.cpp']:
        sources[str(path.relative_to(ROOT))] = L.digest(path.read_bytes())
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='development-only reuse; default ALWAYS builds fresh offline FFI')
    args = parser.parse_args()
    out = Path(tempfile.mkdtemp(prefix='awdl-forward-', dir=ROOT / 'build/diagnostics'))
    print('Evidence: '+str(out), flush=True)
    before = source_hashes()
    (out / 'source-before.json').write_text(json.dumps(before, indent=2)+'\n')
    run = L.CommandLog(out, L.host_environment())
    if args.archive:
        fresh = {'archive': str(args.archive.resolve()), 'mode': 'development-reuse'}
    else:
        fresh = L.build_fresh_archive(out, run)
    (out / 'archive-provenance.json').write_text(json.dumps(fresh, indent=2)+'\n')
    ex = L.Extractor()
    extraction = L.generate(ex)
    (out / 'extracted-production.cpp').write_text(extraction)
    (out / 'extracted.cpp').write_text(model_boundaries(extraction))
    source = ROOT / 'tests/awdl-radio/forward.cpp'
    shutil.copyfile(source, out / 'forward.cpp')
    flags = ['/usr/bin/clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-unused-function', '-fno-omit-frame-pointer',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
        '-I'+str(L.VENDOR / 'include'), '-I'+str(out)]
    run.checked('compile', flags+[str(out / 'forward.cpp'), fresh['archive'], '-o', str(out / 'forward')])
    output = run.checked('run', [str(out / 'forward')], 120)
    print(output, flush=True)
    results = {'production': 'PASS', 'archive': fresh,
        'scope': 'Actual extracted scheduler; host OS/HAL latency/PM boundaries, C++ ASan/UBSan, normal Rust host build. No RF, AP ACK, firmware behavior or throughput claim.',
        'issued_counter': 'ForwardIssued counts calls into awdlReserve, not firmware submission; actual issue is independently recorded by the ROC trace.',
        'rows': [dict(json.loads(s[7:]), control='production') for s in output.splitlines() if s.startswith('RESULT ')]}
    results['native_windows'] = native_windows_companions(ex, out, run, fresh, flags)
    (out / 'extraction.json').write_text(json.dumps(ex.inventory, indent=2)+'\n')
    if not args.archive:
        retained = retained_control(out, run, fresh)
        run.checked('compile-retention-only', flags+['-DRETENTION_ONLY=1', str(out / 'forward.cpp'), str(retained), '-o', str(out / 'retention-only')])
        output = run.checked('run-retention-only', [str(out / 'retention-only')], 120)
        print(output, flush=True)
        results['retention_only'] = 'EXPECTED ZERO channel-6 coverage; retained outlier also receives no resampling'
        results['rows'] += [dict(json.loads(s[7:]), control='retention-only') for s in output.splitlines() if s.startswith('RESULT ')]
        print(run.checked('rust-awdl-tests', [fresh['cargo'], 'test', '--offline', '--locked', '-p', 'mtk-wlan',
            '--target-dir', str(out / 'rust-host'), 'awdl::', '--', '--nocapture'], 300, cwd=ROOT / 'rust'))
        print(run.checked('rust-forward-ffi-tests', [fresh['cargo'], 'test', '--offline', '--locked', '-p', 'mtk-ffi',
            '--no-default-features', '--features', 'wifi', '--target-dir', str(out / 'rust-host'),
            'awdl::tests::forward_', '--', '--nocapture'], 300, cwd=ROOT / 'rust'))
    after = source_hashes()
    stability = {'stable': before == after, 'changed': sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))}
    (out / 'source-stability.json').write_text(json.dumps(stability, indent=2)+'\n')
    assert stability['stable'], 'Production/test inputs changed during testing; rerun: '+repr(stability['changed'])
    (out / 'results.json').write_text(json.dumps(results, indent=2)+'\n')
    print('PASS selective admission regression; evidence '+str(out), flush=True)


if __name__ == '__main__':
    main()

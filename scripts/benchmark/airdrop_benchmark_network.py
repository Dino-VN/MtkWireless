"""Manage benchmark conditions using macOS networking and restore the initial state."""
import json
import re
import shlex
import subprocess
import time

from airdrop_benchmark_packets import write_json

SUDO = '/usr/bin/sudo'
IFCONFIG = '/sbin/ifconfig'
WIFI_BANDS = {'wifi-2.4': 2.4, 'wifi-5': 5, 'wifi-6': 6}
ACTIONS = {'wifi-scan': '--wifi-scan', 'wifi-disconnect': '--wifi-disassociate',
           'wifi-radio-off': '--wifi-off'}
SCENARIOS = ('wifi', 'direct', *ACTIONS, *WIFI_BANDS)
DEFAULT_SCENARIOS = ('wifi', 'wifi-scan', 'wifi-disconnect', 'direct')


def run_helper(binary, command):
    return subprocess.run([str(binary), command], check=True, text=True,
                          capture_output=True, timeout=8).stdout


def network_state(binary):
    return json.loads(run_helper(binary, '--network-state'))


def same_ap(initial, current):
    if not current.get('wifi_associated'):
        return False
    if not initial.get('ssid') or not current.get('ssid'):
        return True
    return initial['ssid'] == current['ssid']


def wait_for_state(binary, predicate, seconds=12):
    deadline = time.monotonic() + seconds
    while True:
        state = network_state(binary)
        if predicate(state) or time.monotonic() >= deadline:
            return state
        time.sleep(.5)


def public_state(state):
    return {key: value for key, value in state.items() if key != 'ssid'}


def matches_scenario(state, scenario):
    if not state.get('wifi_powered') or state.get('ethernet_up') != []:
        return False
    if scenario == 'direct':
        return state.get('wifi_associated') is False
    if not state.get('wifi_associated'):
        return False
    return scenario not in WIFI_BANDS or state.get('band_ghz') == WIFI_BANDS[scenario]


def action_state_matches(state, scenario):
    if scenario == 'wifi-disconnect':
        return matches_scenario(state, 'direct')
    if scenario == 'wifi-radio-off':
        return state.get('wifi_powered') is False and state.get('ethernet_up') == []
    return matches_scenario(state, scenario)


class NetworkSession:
    def __init__(self, binary, initial, output):
        self.binary = binary
        self.initial = initial
        self.output = output
        self.ethernet_changed = []
        self.wifi_may_have_changed = False
        self.recovery = output / 'network-recovery.txt'
        interfaces = initial.get('ethernet_up')
        if (not initial.get('wifi_interface') or not isinstance(interfaces, list) or
                any(not re.fullmatch(r'en[0-9]+', name) for name in interfaces)):
            raise RuntimeError('macOS could not identify the Wi-Fi and Ethernet interfaces')

    def prepare(self):
        subprocess.run([SUDO, '-v'], check=True)
        commands = [shlex.join([SUDO, IFCONFIG, name, 'up'])
                    for name in self.initial['ethernet_up']]
        self.recovery.write_text(
            'Run only if the benchmark was interrupted before restoring your network.\n'
            + '\n'.join(commands)
            + '\nUse the macOS Wi-Fi menu to restore the previous Wi-Fi connection or power state.\n',
            encoding='utf-8')
        for name in self.initial['ethernet_up']:
            self.ethernet_changed.append(name)
            subprocess.run([SUDO, '-n', IFCONFIG, name, 'down'], check=True, timeout=8)
        now = wait_for_state(self.binary, lambda state: state['ethernet_up'] == [], seconds=3)
        if now['ethernet_up'] != []:
            raise RuntimeError(f'Ethernet is still up: {now["ethernet_up"]}')
        if not now.get('wifi_powered'):
            self.wifi_may_have_changed = True
            run_helper(self.binary, '--wifi-on')
            now = wait_for_state(self.binary, lambda state: state.get('wifi_powered'))
            if not now.get('wifi_powered'):
                raise RuntimeError('macOS did not turn Wi-Fi on')

    def prepare_scenario(self, scenario):
        state = network_state(self.binary)
        if state['ethernet_up'] != []:
            raise RuntimeError(f'Ethernet came back up: {state["ethernet_up"]}')
        if not state.get('wifi_powered'):
            self.wifi_may_have_changed = True
            run_helper(self.binary, '--wifi-on')
            state = wait_for_state(self.binary, lambda sample: sample.get('wifi_powered'))
        if matches_scenario(state, scenario):
            return state
        self.wifi_may_have_changed = True
        if scenario == 'direct':
            print('Leaving the Wi-Fi access point. The Wi-Fi radio stays on for AirDrop.', flush=True)
            run_helper(self.binary, '--wifi-disassociate')
        else:
            band = f' on {WIFI_BANDS[scenario]} GHz' if scenario in WIFI_BANDS else ''
            answer = input(f'Connect to a Wi-Fi network{band} in the macOS Wi-Fi menu, '
                           'then press Enter. Type skip to skip this condition. ').strip().lower()
            if answer == 'skip':
                return None
        state = wait_for_state(self.binary, lambda sample: matches_scenario(sample, scenario), seconds=8)
        if not matches_scenario(state, scenario):
            observed = 'associated' if state.get('wifi_associated') else 'not associated'
            band = state.get('band_ghz')
            print(f'Skipping {scenario}. Current state: {observed}, band {band} GHz.', flush=True)
            return None
        return state

    def restore(self):
        errors = []
        wifi_verification = 'unchanged'
        if self.ethernet_changed or self.wifi_may_have_changed:
            print('Restoring the original network state…', flush=True)
        for name in self.ethernet_changed:
            try:
                result = subprocess.run([SUDO, '-n', IFCONFIG, name, 'up'], timeout=8)
                if result.returncode:
                    print(f'Authorize bringing {name} back up.', flush=True)
                    subprocess.run([SUDO, IFCONFIG, name, 'up'], check=True)
            except (KeyboardInterrupt, OSError, subprocess.SubprocessError):
                errors.append(f'Could not bring {name} back up.')
        if self.wifi_may_have_changed:
            try:
                wifi_verification = self.restore_wifi()
            except (KeyboardInterrupt, OSError, ValueError, RuntimeError,
                    subprocess.SubprocessError) as error:
                wifi_verification = 'failed'
                errors.append(str(error) or 'Wi-Fi restoration interrupted.')
        try:
            current = network_state(self.binary)
            up = current.get('ethernet_up') or []
            for name in self.ethernet_changed:
                if name not in up:
                    message = f'Could not bring {name} back up.'
                    if message not in errors:
                        errors.append(message)
        except (OSError, ValueError, subprocess.SubprocessError):
            errors.append('Could not verify Ethernet restoration.')
        if errors:
            for message in errors:
                print(message, flush=True)
            print(f'Recovery steps: {self.recovery}', flush=True)
        elif self.recovery.exists():
            self.recovery.unlink()
        if wifi_verification == 'association_only':
            print('Wi-Fi association restored. macOS did not expose the network name for verification.', flush=True)
        write_json(self.output / 'network-restoration.json',
                   {'restored': not errors, 'errors': errors,
                    'wifi_verification': wifi_verification,
                    'initial_state': public_state(self.initial)})
        return not errors

    def restore_wifi(self):
        original = self.initial
        now = network_state(self.binary)
        if not original['wifi_powered']:
            if now.get('wifi_powered'):
                run_helper(self.binary, '--wifi-off')
            if wait_for_state(self.binary, lambda state: not state.get('wifi_powered')
                              ).get('wifi_powered'):
                raise RuntimeError('Wi-Fi radio is still on.')
            return 'radio_off'
        if not now.get('wifi_powered'):
            run_helper(self.binary, '--wifi-on')
            now = wait_for_state(self.binary, lambda state: state.get('wifi_powered'))
        if not original['wifi_associated']:
            if now.get('wifi_associated'):
                run_helper(self.binary, '--wifi-disassociate')
            now = wait_for_state(self.binary, lambda state: not state.get('wifi_associated'))
            if now.get('wifi_associated') or not now.get('wifi_powered'):
                raise RuntimeError('Restore the original Wi-Fi state from the macOS Wi-Fi menu.')
            return 'radio_on_disconnected'
        if not same_ap(original, now):
            # Ask macOS to use its saved networks. Keep all preferred-network options intact.
            run_helper(self.binary, '--wifi-off')
            time.sleep(1)
            run_helper(self.binary, '--wifi-on')
            now = wait_for_state(self.binary, lambda state: same_ap(original, state), seconds=20)
        if not same_ap(original, now):
            raise RuntimeError('Rejoin the original Wi-Fi network from the macOS Wi-Fi menu.')
        return 'original_ssid' if original.get('ssid') and now.get('ssid') else 'association_only'

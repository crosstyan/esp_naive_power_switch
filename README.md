# ESP32 UDP Power Switch

ESP-IDF firmware and a Python client for controlling two active-low target lines from an ESP32-S3 control board:

- K6 `RESETn`: ESP32 `GPIO18`
- K5 `SARADC0_BOOT`: ESP32 `GPIO19`
- RGB status LED: ESP32-S3 `GPIO48`

The default hardware model is two external logic-level N-MOSFETs wired as low-side switches in parallel with the target buttons. `GPIO18` drives the K6 `RESETn` MOSFET gate, and `GPIO19` drives the K5 `SARADC0_BOOT` MOSFET gate.

In the default MOSFET configuration, commanded target-line assertion drives the ESP32 GPIO high, turning the MOSFET on and pulling the target line low. Commanded release drives the GPIO low, turning the MOSFET off and leaving the target pull-up/button circuit in control.

Full wiring assumptions are documented in [docs/hardware_wiring.md](docs/hardware_wiring.md).

## Build And Flash

```sh
cd /Users/crosstyan/External/Code/power_switch
source ~/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.wchusbserial58CF0262251 flash monitor
```

The firmware joins Wi-Fi SSID `WH-iot` with password `wh213215` and logs the destination in the serial monitor:

```text
UDP server ready at 192.168.x.y:44444
```

Use that IP with the Python client.

The GPIO48 RGB LED shows basic status:

- amber: joining or reconnecting Wi-Fi
- green: UDP server ready
- blue flash: packet received
- red: RESETn pulse active
- purple: Maskrom entry sequence active

## Python Client

```sh
python3 tools/power_switch_client.py <esp32-ip> status
python3 tools/power_switch_client.py <esp32-ip> assert boot
python3 tools/power_switch_client.py <esp32-ip> release boot
python3 tools/power_switch_client.py <esp32-ip> pulse reset --duration-ms 300
python3 tools/power_switch_client.py <esp32-ip> reset
python3 tools/power_switch_client.py <esp32-ip> reset --pulse-ms 500
python3 tools/power_switch_client.py <esp32-ip> maskrom
python3 tools/power_switch_client.py <esp32-ip> maskrom --reset-ms 300 --boot-hold-ms 1500
python3 tools/power_switch_client.py <esp32-ip> hold-boot
python3 tools/power_switch_client.py <esp32-ip> hold-reset
python3 tools/power_switch_client.py <esp32-ip> release-all
```

Programmatic use:

```python
from tools.power_switch_client import PowerSwitchClient
from tools.power_switch_client import LINE_BOOT, LINE_RESET

client = PowerSwitchClient("192.168.1.123")
client.set_lines(LINE_BOOT, LINE_BOOT)  # hold K5 low
client.pulse_lines(LINE_RESET, 300)     # pulse K6 low
client.set_lines(LINE_BOOT, 0)          # release K5

client.reset()
client.enter_maskrom()
client.hold_boot()
client.release_all()
```

To hold a line low indefinitely, use one of the hold commands and later release it:

```sh
# Hold K5 SARADC0_BOOT low.
python3 tools/power_switch_client.py <esp32-ip> assert boot

# Optional manual Maskrom sequence: keep K5 low, pulse K6, then release K5.
python3 tools/power_switch_client.py <esp32-ip> pulse reset --duration-ms 300
python3 tools/power_switch_client.py <esp32-ip> release boot

# Hold K6 RESETn low, keeping the target in reset.
python3 tools/power_switch_client.py <esp32-ip> assert reset
python3 tools/power_switch_client.py <esp32-ip> release reset
```

## UDP Protocol

Full binary protocol reference: [docs/binary_protocol.md](docs/binary_protocol.md).
Hardware polarity reference: [docs/hardware_wiring.md](docs/hardware_wiring.md).

Requests use network byte order:

```text
!4sHBBHH
magic      4 bytes  "PSW2"
sequence   uint16
command    uint8   1=status, 2=set_lines, 3=pulse_lines
flags      uint8   must be 0
arg0       uint16  command-specific argument
arg1       uint16  command-specific argument
```

Line bits:

```text
bit0 RESETn
bit1 SARADC0_BOOT
```

Commands:

```text
status:
  arg0 ignored
  arg1 ignored

set_lines:
  arg0 mask of lines to update
  arg1 asserted line bits after update

pulse_lines:
  arg0 mask of lines to pulse
  arg1 pulse duration in ms, clamped to 50..10000
```

Acknowledgements:

```text
!4sHBBH
magic      4 bytes  "PSA1"
sequence   uint16   copied from request
status     uint8    0=ok, 1=bad_magic, 2=bad_length, 3=bad_command,
                   4=internal_error, 5=bad_flags, 6=bad_mask
state_bits uint8    bit0=RESETn asserted, bit1=SARADC0_BOOT asserted
detail     uint16   command/status-specific detail
```

Timing arguments are clamped by firmware to `50..10000 ms`. The device protocol only exposes line primitives. The Python client implements `reset()` and `enter_maskrom()` as convenience wrappers around `set_lines()` and `pulse_lines()`.

There is no authentication or encryption in this v1 protocol; keep the ESP32 on a trusted IoT network.

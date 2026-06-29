# Hardware Wiring Assumptions

This project controls two active-low target button lines by making the ESP32-S3 act like a remote button press.

## Pin Mapping

| ESP32-S3 pin | Target function | Button | Default role |
| --- | --- | --- | --- |
| `GPIO18` | `RESETn` | K6 | Reset MOSFET gate drive |
| `GPIO19` | `SARADC0_BOOT` | K5 | Boot/Maskrom MOSFET gate drive |
| `GPIO48` | WS2812 RGB LED data | n/a | Local status LED |

The firmware assumes `GPIO18` and `GPIO19` are free output-capable pins on the chosen ESP32-S3 board. If your board routes one of these pins to native USB, JTAG, another peripheral, or board-specific hardware, change `CONFIG_POWER_SWITCH_RESET_GPIO` or `CONFIG_POWER_SWITCH_BOOT_GPIO` before flashing.

## Default MOSFET Wiring

The checked-in defaults assume one external logic-level N-MOSFET per target button line. Each MOSFET is used as a low-side switch, electrically equivalent to pressing the corresponding button.

For each line:

| MOSFET node | Connection |
| --- | --- |
| Gate | ESP32-S3 control GPIO, optionally through a small series resistor such as `100..1000 ohm` |
| Gate pulldown | Recommended `~100k` from gate to ground so the MOSFET stays off while the ESP32 boots or resets |
| Source | Shared ground |
| Drain | Target key node that the physical button shorts to ground |

Use a shared ground between the ESP32 board and the target board.

For the schematic you showed, prefer connecting the MOSFET drain to the button-side key node, on the same node that K5/K6 shorts to ground. That preserves the target board's existing `100R` series resistor between the SoC signal and the button node.

The ESP32 GPIO must drive only the MOSFET gate. It must not drive the target `RESETn` or `SARADC0_BOOT` signal high.

## Default Firmware Polarity

The default config is for MOSFET gate drive:

```text
CONFIG_POWER_SWITCH_CONTROL_GPIO_OPEN_DRAIN=n
CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL=1
```

That means:

| Protocol state | ESP32 GPIO | MOSFET | Target line |
| --- | --- | --- | --- |
| released | low | off | released by target pull-up/circuit |
| asserted | high | on | pulled low, like a button press |

So these commands have this hardware effect:

| Command | ESP32 output | Target effect |
| --- | --- | --- |
| `assert reset` | `GPIO18` high | K6 `RESETn` held low |
| `release reset` | `GPIO18` low | K6 `RESETn` released |
| `assert boot` | `GPIO19` high | K5 `SARADC0_BOOT` held low |
| `release boot` | `GPIO19` low | K5 `SARADC0_BOOT` released |

## Direct Wiring Fallback

If you remove the MOSFETs and connect the ESP32 GPIOs directly to the target button nodes, use open-drain active-low output instead:

```text
CONFIG_POWER_SWITCH_CONTROL_GPIO_OPEN_DRAIN=y
CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL=0
```

Only direct-wire this if the target button node is safe for the ESP32 GPIO:

- shared ground
- target pull-up voltage is no higher than `3.3 V`
- no contention with another push-pull driver
- target line current is within the ESP32 GPIO sink rating

If any of those are uncertain, keep the MOSFET or use an opto/level-shifting interface.

## Reset And Maskrom Behavior

K6 `RESETn` is the reset line. Holding it asserted keeps the target in reset. Pulsing it low performs a normal reset.

K5 `SARADC0_BOOT` is the boot strap line. Holding it low by itself does not reset the target. To enter Maskrom, hold K5 low, pulse K6 `RESETn` low, keep K5 low through boot strap sampling, then release K5.

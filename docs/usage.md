# Usage examples

Feature-by-feature cookbook for devices flashed with this firmware. All examples assume the device is already adopted into Home Assistant via the ESPHome integration (see [`../README.md#setup`](../README.md#setup) if you're not there yet). Replace `<device_name>` with your device's ESPHome name (the `name:` under `esphome:` in your device YAML).

## Buzzer / RTTTL melodies

Devices with a buzzer (e.g. Ultima) support RTTTL melody playback. You can play melodies from Home Assistant.

**Play a custom RTTTL melody:**
```yaml
service: esphome.<device_name>_rtttl_input_set
data:
  value: "mario:d=4,o=5,b=100:16e6,16e6,32p,8e6,16c6,8e6,8g6,8p,8g"
```

**Play a preset melody:**
```yaml
service: esphome.<device_name>_rtttl_preset_set
data:
  option: "Doorbell"
```

Available presets: `Doorbell`, `Notification`, `Alert`, `Success`, `Error`, `Mario`, `Zelda`, `Pacman`, `Star Wars`, `Nokia`

**RTTTL format:**
```
name:d=duration,o=octave,b=bpm:notes
```

**Resources for RTTTL melodies:**
- [PICAXE RTTTL Collection](https://picaxe.com/rtttl-ringtones-for-tune-command/)
- [Online RTTTL Player/Editor](https://adamonsoon.github.io/rtttl-play/)

---

## WS2812 LED effects

Devices with WS2812 LEDs (e.g. Ultima) support various light effects controllable from Home Assistant.

**RMT symbol buffer (advanced):**

The WS2812 driver uses the ESP32-S3 RMT peripheral. The TX symbol pool is shared with the IR transmitter (192 symbols total across 4 channels). The default allocation is 96 symbols for WS2812 and 96 for IR TX. If you are not using IR and want to allocate more symbols to WS2812, override the substitution in your device YAML:

```yaml
substitutions:
  ws2812_rmt_symbols: "192"   # increase only if IR TX is disabled
  ir_tx_rmt_symbols: "0"      # set to 0 if not used
```

**Turn on with effect:**
```yaml
service: light.turn_on
target:
  entity_id: light.<device_name>_ws2812
data:
  effect: "Rainbow"
```

**Available effects:**

| Category | Effects |
|----------|---------|
| Common | Rainbow, Color Wipe, Scan, Twinkle, Random Twinkle, Fireworks, Flicker, Pulse, Strobe |
| Lambda | Fire, Confetti, Candy Cane, Meteor, Running Lights, Breathing RGB, Color Chase, Sparkle, Christmas |

**Quick presets via dropdown:**
```yaml
service: esphome.<device_name>_ws2812_preset_set
data:
  option: "Rainbow"
```

Available presets:

| Category | Presets |
|----------|---------|
| Solid Colors | White, Warm White, Red, Green, Blue, Purple, Cyan, Orange |
| Moods | Night Light, Cozy |
| Effects | Rainbow, Fire, Twinkle, Confetti, Party, Christmas |
| Alerts | Alert |

**Short notification blinks (status indicators):**
```yaml
service: esphome.<device_name>_ws2812_notify_set
data:
  option: "OK"
```

| Notification | Color | Pattern |
|--------------|-------|---------|
| OK | Green | Double blink |
| Warning | Orange | Triple blink |
| Error | Red | Rapid 5x blink |
| Info | Blue | Single long blink |
| Busy | Yellow | Fade out |
| Ready | Cyan | Pulse up then off |
| Attention | Magenta | Double flash |
| Boot | White | Sweep fade |

---

## IR remote (transmit)

Devices with an IR transmitter can send IR codes to control TVs, ACs, etc.

**Send a raw IR code:**
```yaml
service: esphome.<device_name>_ir_send
data:
  code: "0x20DF10EF"  # Example: LG TV Power
```

Check [`../libraries/ir/codes/`](../libraries/ir/codes/) for available IR code packs. The Ultima device YAML has commented-out `!include` lines for each pack — uncomment the one(s) you want and re-flash.

**RMT symbol buffer (advanced):**

The IR transmitter and WS2812 share the ESP32-S3 RMT TX symbol pool (192 symbols total). Defaults are 96 each. Override in your device YAML if needed:

```yaml
substitutions:
  ir_tx_rmt_symbols: "128"    # increase if WS2812 is not used
  ir_rx_rmt_symbols: "96"     # RX pool is independent (192 symbols total)
  ws2812_rmt_symbols: "64"    # reduce if giving more to IR TX
```

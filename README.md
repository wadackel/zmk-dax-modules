# zmk-dax-modules

ZMK modules shared by the dax-series keyboards. Each feature is off unless its
Kconfig option is set, so a keyboard takes only what it needs.

Built against the ZMK main line (Zephyr 4.1), `cormoran/zmk`
`main+custom-studio-protocol`.

| Feature | Kconfig | Needs | What it does |
|---|---|---|---|
| OS layer | `CONFIG_ZMK_OS_LAYER`, `CONFIG_ZMK_OS_LAYER_PROFILE_<n>=<layer>` | BLE | Keeps a layer active, locked, while a given BLE profile is selected. A non-locking `&to` does not drop it. |
| Trackball settings | `CONFIG_ZMK_TRACKBALL_SETTINGS` | `zmk-feature-custom-settings`, `zmk-driver-paw3222`, a `trackball` devicetree label with `res-cpi` | Registers the sensor's CPI as a runtime setting (`dax_trackball` / `cpi`) and applies it at boot and on change. |

## Tests

Logic that does not need Zephyr is kept apart from the ZMK glue and tested on the host:

```sh
tests/gesture_core/run.sh
```

## Use

```yaml
# config/west.yml
- name: zmk-dax-modules
  remote: wadackel
  revision: <commit SHA>
```

Settings registered here are edited with
[daxial](https://github.com/wadackel/daxial) through the custom-settings
Studio RPC.

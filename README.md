# zmk-dax-modules

ZMK modules shared by the dax-series keyboards. Each feature is off unless its
Kconfig option is set, so a keyboard takes only what it needs.

Built against the ZMK main line (Zephyr 4.1), `cormoran/zmk`
`main+custom-studio-protocol`.

| Feature | Kconfig | Needs | What it does |
|---|---|---|---|
| OS layer | `CONFIG_ZMK_OS_LAYER`, `CONFIG_ZMK_OS_LAYER_PROFILE_<n>=<layer>` | BLE | Keeps a layer active, locked, while a given BLE profile is selected. A non-locking `&to` does not drop it. |
| Gestures | `CONFIG_ZMK_DAX_GESTURE` (on when the devicetree has a `dax,input-processor-gesture` node), `CONFIG_ZMK_DAX_GESTURE_MAX_GESTURES` | `CONFIG_ZMK_POINTING` | Stroke gestures on a pointing device. Gesture sets are chosen by the active layer; see `dts/bindings/input_processors/dax,input-processor-gesture.yaml`. |
| Runtime gestures | `CONFIG_ZMK_DAX_GESTURE_RUNTIME` (on by default with the two below) | `zmk-feature-custom-settings`, `CONFIG_ZMK_BEHAVIOR_LOCAL_IDS` | Serves the gesture tuning, profiles and gestures from custom settings under the `dax_gesture` subsystem, with the devicetree as defaults. Key layout is documented at the top of `src/gesture/gesture_settings.c`. |
| Gesture events | `CONFIG_ZMK_DAX_GESTURE_RPC` (on by default with Studio RPC) | `cormoran/zmk` custom Studio RPC | Reports recognized strokes and fired gestures to a client that asks, and captures a drawn pattern for it. Wire format is at the top of `src/gesture/gesture_rpc.c`. |
| Trackball settings | `CONFIG_ZMK_TRACKBALL_SETTINGS` | `zmk-feature-custom-settings`, `zmk-driver-paw3222`, a `trackball` devicetree label with `res-cpi` | Registers the sensor's CPI as a runtime setting (`dax_trackball` / `cpi`) and applies it at boot and on change. |
| Hold macro | `CONFIG_ZMK_DAX_HOLD_MACRO` (on when the devicetree has a `dax,behavior-hold-macro` node and runtime macros are on) | `zmk-feature-runtime-macro` | `&dax_hold_macro <press slot> <release slot>` plays one runtime macro on press and another on release, so a macro can hold a modifier or a layer while the key is down. Include `<behaviors/hold_macro.dtsi>`. A play the runtime macro player refuses as busy is retried, in order. |
| Combos | `CONFIG_ZMK_DAX_COMBO`, `CONFIG_ZMK_DAX_COMBO_STUDIO_RPC` (on by default with Studio), `CONFIG_ZMK_DAX_COMBO_MAX_COMBOS` | `zmk-feature-custom-settings`, ZMK built with its `src/combo.c` (it is, on a central) | Combos edited at runtime under the `dax_combo` subsystem, with a `dax,runtime-combo-defaults` node as defaults. They are matched by a copy of ZMK's `src/combo.c` (`src/combo/combo_engine.c`), placed in the event chain where ZMK's own combos sit, so a combo pressed while a hold-tap is undecided behaves as a `zmk,combos` combo does. The storage layout and the RPC are those of `cormoran/zmk-feature-runtime-combo`, with a one-key combo allowed. When updating ZMK, diff `combo_engine.c` against the new `src/combo.c`; the build stops if `src/combo.c` is renamed. |
| Default macros | `CONFIG_ZMK_DAX_MACRO_DEFAULTS` (on by default with runtime macros) | `zmk-feature-runtime-macro` | Makes a discard put a devicetree macro that was never saved back to its devicetree body. Without it the macro has no record to go back to and keeps the edit. |

## Tests

Logic that does not need Zephyr is kept apart from the ZMK glue and tested on the host:

```sh
tests/gesture_core/run.sh
```

The directories under `tests/gesture/`, `tests/hold-macro/`, `tests/macro/`, `tests/combo/` and
`tests/combo-zmk/` are ZMK `native_sim` tests: a keymap, mocked key and pointer input, and the HID reports expected from them. They run from
a west workspace that has this module checked out, the same way ZMK's own tests do (`west build -b
native_sim//zmk_test_mock` with `-DZMK_CONFIG=<test dir>`). `zmk-config-dax6` has a runner for them
(`tests/run.sh`).

`tests/combo-zmk/` is ZMK's own `app/tests/combo/` with the compatible changed to
`dax,runtime-combo-defaults` and the snapshots kept, so it fails if the combo engine stops matching
ZMK. The only other change is in `layer-filter-0`, `layer-filter-1` and `overlapping-combos-0`,
whose one-hour timeout becomes 65535ms, the most the storage holds.

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

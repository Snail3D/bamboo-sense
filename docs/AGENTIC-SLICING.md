# Agentic Slicing: headless CAD-to-part pipeline (no GUI, no human)

How an agent turns "print me a thing" into gcode on a Bambu P2S — every step
CLI-driven, verified live 2026-09-07/08. Companion to CRACK.md (which gets the
gcode ONTO the printer). This doc is the half that comes before: model → plate.

## The pipeline at a glance

```
find/generate ──► validate ──► plate ──► slice ──► md5 ──► serve ──► sign ──► print
   (3 modes)       (STL ok?)   (arrange)  (Orca)   (whole   (http)   (dongle)  (monitor)
                                                    3mf)
```

Three sourcing modes, all headless:
1. **FOUND** — GitHub repo search for STLs (`gh search repos "flexi rex"`) →
   raw.githubusercontent.com download. Watch for LFS files and 404s (`grep -c
   "facet normal"` = 0 on a binary STL is fine; 14-byte files are 404s).
2. **PARAMETRIC** — OpenSCAD `.scad` files render with `/opt/homebrew/bin/openscad
   -o out.stl in.scad`. Customizer variables can be overridden with `-D
   ring_count=8`. This is the killer mode for "make it 20% bigger" requests.
3. **GENERATED** — programmatic generators. `tusosos/articulated-toys` is an
   agent skill with presets (dragon/snake/griffin/centipede...) producing
   print-in-place articulated STLs with tunable clearance/segments/wings:
   `python3 gen.py --preset dragon -o dragon.stl`.

## Slicing with OrcaSlicer CLI (BambuStudio CLI works the same, when it works)

```sh
pkill -f OrcaSlicer   # MUST be dead first — single-instance swallows CLI args
/Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer \
  --arrange 1 \
  --load-settings "process.json;machine.json" \
  --load-filaments "filament.json" \
  --slice 0 --debug 3 \
  --export-3mf out.gcode.3mf model1.stl model2.stl model3.stl ...
```

- **Multiple input STLs land on ONE plate** (verified: 5 fidgets, 168 layers,
  3h22m estimate). Add `--arrange 1` to lay them out automatically.
- **Never use `--orient 1` for print-in-place parts** — articulated joints must
  keep their designed orientation. Arrange ≠ orient: arrange only places.
- `--debug 3-4` shows per-plate progress and `update_slice_result_valid_state`
  lines; the LAST plate logged is the real completion signal.
- **Exit codes lie through pipes**: `cmd | tail` returns tail's status. Always
  `echo "EXIT: $?"` on the SAME line or check for the output 3mf + plate_1.gcode.

### The P2S segfault (bambulab/BambuStudio#11893)
BambuStudio AND OrcaSlicer CLI segfault (SIGSEGV, exit 139, right after
`update_slice_result_valid_state`) slicing P2S profiles out of the box. Root
cause: machine preset carries a dual-variant string for a single-extruder
machine — `extruder_variant_list: ["Direct Drive Standard,Direct Drive High
Flow"]` — and a variant SET larger than 1 trips
`DynamicPrintConfig::support_different_extruders()` → multi-extruder path →
crash. Fix: collapse to one variant in the machine profile.

### Building the profile JSONs (when /tmp/bambu-api is wiped)
Profiles inherit from app presets — resolve the chain flat, keep `from:
"system"` (NOT "user", or the printer-compat check fails), and keep
`compatible_printers` matching. Working set (2026-09, OrcaSlicer 2.x presets at
`/Applications/OrcaSlicer.app/Contents/Resources/ profiles/BBL/`):
- machine: `Bambu Lab P2S 0.4 nozzle.json` → flatten + collapse
  `extruder_variant_list`/`printer_extruder_variant` to `["Direct Drive Standard"]`
  + add `nozzle_volume_type: "Standard"`
- process: `0.16mm Standard @Bambu Lab P2S.json` → flatten
- filament: `Bambu PLA Basic @Bambu Lab P2S.json` → flatten
Resolution recipe: start at the preset, follow every `inherits` field up,
deep-merge child-over-parent, drop `inherits` at the end. (Script it in python;
~50 lines.) For color/param variants, patch `filament_colour` / settings per
slice run — each run is independent, so per-model overrides are trivial.

### Slicer output → print command
- `md5` for `project_file` = md5 of the **whole output .gcode.3mf** (NOT the
  `Metadata/plate_1.gcode.md5` inside).
- `param` stays `"Metadata/plate_1.gcode"` regardless of filename.
- Check `; total layer number:` and `; model printing time:` from the gcode
  header (unzip -p) BEFORE firing — sanity-check the estimate.

## The complete agentic loop (all verified)

```sh
# 1. source (any mode above) → /tmp/bswork/*.stl
# 2. slice, multi-model plating
OrcaSlicer --arrange 1 --load-settings ... --slice 0 --export-3mf pack.gcode.3mf a.stl b.stl
# 3. fire — tools/print_file.sh does serve+md5+sign+monitor in one shot
bash tools/print_file.sh pack.gcode.3mf 1   # tray index = AMS slot
# 4. watch: curl http://<dongle>/printer → gcode_state/mc_percent/layer_num
# 5. recover: signed clean_print_error / stop (see CRACK.md §6)
```

Human hands touch nothing. The Mac (or Pi) is the slicer brain; the dongle is
the printer-side actuator. Next step for full autonomy: dongle hosts the 3mf
itself (microSD) so no computer needs to stay awake during the print.

## Gotchas that cost us hours
- OrcaSlicer GUI running = CLI args swallowed silently. Kill it first.
- `from: "user"` in flattened profiles → "process not compatible with printer"
  (exit 239-ish), a different failure than the segfault.
- `nozzle_volume_type` missing → harmless warning, but add it anyway.
- Flexi-style models scale up ⇒ perimeters must scale too (README math), else
  joints snap.
- Generator STLs sometimes print their own advice (`Layer height: 0.2mm...`)
  into stderr — don't confuse it with slicing output.

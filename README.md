# ElectricEquipment:ITE:LiquidCooled

A new EnergyPlus object for simulating **direct-to-chip liquid-cooled IT equipment** in data centers. Heat is removed via a facility water system (FWS) plant loop through an external CDU (Coolant Distribution Unit) heat exchanger. A small configurable fraction of heat leaks to the zone air.

Built against the [NatLabRockies EnergyPlus](https://github.com/NatLabRockies/EnergyPlus) fork at tag `v26.1.0`.

---

## What it models

```
  IT chips
     │  CPU heat
     ▼
  Cold plates  ←─ rack coolant loop (pump) ─→  CDU heat exchanger
                                                      │
                                              Facility Water System
                                              (chilled water PlantLoop)
                                                      │
                                              to chiller / district cooling
     │
     └─ air leakage fraction → zone air (handled by ZoneHVAC)
```

**Physics model:**
- CPU power: biquadratic curve f(cpu_load_fraction, T_rack_supply)
- CDU: fixed approach temperature — T_rack_sup = T_fws_sup + ΔT_approach (matches CDU spec sheets)
- Chip junction temperature: T_junc = T_rack_supply + (Q_total / N_units) × R_jc
- Energy balance: UPS_input = Q_liquid + Q_zone (air leakage + UPS losses)

---

## Installation

### What's in this directory

```
├── README.md                     ← this file
├── WINDOWS_BUILD_GUIDE.md        ← instructions for installing in Windows
├── case_study/                   ← Tampa case-study IDFs (air, chilled water, warm water)
├── scripts/compute_metrics.py    ← annual PUE / WUE / EUE from eplusout.sql
├── weather/                      ← Tampa TMY3 weather file
├── binary_files/                 ← drop-in replacement for Mac ARM users
│   ├── energyplus                   compiled executable (Mac ARM64)
│   ├── libenergyplusapi.26.1.0.dylib  shared library containing simulation code
│   ├── Energy+.idd                  input data dictionary
│   └── Energy+.schema.epJSON        JSON schema
└── source_files/                 ← for developers who want to modify the C++
    ├── idd/Energy+.idd.in
    └── src/EnergyPlus/
        ├── ITE_LiquidCooled.hh        the new object
        ├── ITE_LiquidCooled.cc
        ├── InternalHeatGains.cc
        ├── CMakeLists.txt
        ├── DataLoopNode.hh
        ├── Plant/PlantManager.cc
        ├── Plant/Enums.hh
        ├── Plant/DataPlant.hh
        ├── Data/EnergyPlusData.hh
        ├── Data/EnergyPlusData.cc
        └── Data/CommonIncludes.hh
```

### Which path is right for you?

| Your situation | What to do |
|---|---|
| **Mac, just want to run simulations** | Use `binary_files/` — fully self-contained, no building needed |
| **Mac, want to modify the C++ code** | Clone NatLabRockies/EnergyPlus at v26.1.0, drop in `source_files/`, rebuild |
| **Windows or Linux** | Clone NatLabRockies/EnergyPlus at v26.1.0, drop in `source_files/`, rebuild (the Mac binary won't run) |

### Mac: installing the binary (no coding required)

**First, remove the macOS quarantine flag** (required for all downloaded binaries):

```bash
xattr -d com.apple.quarantine binary_files/energyplus
xattr -d com.apple.quarantine binary_files/libenergyplusapi.26.1.0.dylib
```

Make sure EnergyPlus 26.1 is already installed, then replace four files:

```bash
# Back up originals first
cp /Applications/EnergyPlus-26-1-0/energyplus /Applications/EnergyPlus-26-1-0/energyplus.bak
cp /Applications/EnergyPlus-26-1-0/Energy+.idd /Applications/EnergyPlus-26-1-0/Energy+.idd.bak
cp /Applications/EnergyPlus-26-1-0/libenergyplusapi.26.1.0.dylib /Applications/EnergyPlus-26-1-0/libenergyplusapi.26.1.0.dylib.bak
cp /Applications/EnergyPlus-26-1-0/Energy+.schema.epJSON /Applications/EnergyPlus-26-1-0/Energy+.schema.epJSON.bak

# Copy in the new files (from binary_files/ in this zip)
cp binary_files/energyplus /Applications/EnergyPlus-26-1-0/energyplus
cp binary_files/Energy+.idd /Applications/EnergyPlus-26-1-0/Energy+.idd
cp binary_files/Energy+.schema.epJSON /Applications/EnergyPlus-26-1-0/Energy+.schema.epJSON
cp binary_files/libenergyplusapi.26.1.0.dylib /Applications/EnergyPlus-26-1-0/libenergyplusapi.26.1.0.dylib
```

Verify it worked:
```bash
energyplus --version
# should print: EnergyPlus, Version 26.1.0-6f2e40d102
```

### Building from source (Mac, Windows, or Linux)

Needed if you want to modify the C++ — including on Mac, since the prebuilt
`binary_files/` are only as current as the last rebuild.

Clone the NatLabRockies fork at `v26.1.0`:

```bash
git clone https://github.com/NatLabRockies/EnergyPlus.git EnergyPlus-src
cd EnergyPlus-src
git checkout v26.1.0

# Copy in the changed files from source_files/ (mirror the directory structure):
#   idd/Energy+.idd.in
#   src/EnergyPlus/ITE_LiquidCooled.hh
#   src/EnergyPlus/ITE_LiquidCooled.cc
#   src/EnergyPlus/InternalHeatGains.cc
#   src/EnergyPlus/CMakeLists.txt
#   src/EnergyPlus/DataLoopNode.hh
#   src/EnergyPlus/Plant/PlantManager.cc
#   src/EnergyPlus/Plant/Enums.hh
#   src/EnergyPlus/Plant/DataPlant.hh
#   src/EnergyPlus/Data/EnergyPlusData.hh
#   src/EnergyPlus/Data/EnergyPlusData.cc
#   src/EnergyPlus/Data/CommonIncludes.hh

# Configure once (creates build/), then build:
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FORTRAN=OFF -DBUILD_TESTING=OFF
cmake --build build --target energyplus -j 8
```

A first build from a clean clone takes **30–60 minutes**. Re-running the last
command after editing only `ITE_LiquidCooled.cc/.hh` takes **~2 minutes**,
since only the changed files recompile.

The build products land in `build/Products/`. To refresh `binary_files/`:

```bash
cp build/Products/energyplus-26.1.0        binary_files/energyplus
cp build/Products/libenergyplusapi.26.1.0.dylib  binary_files/
cp build/Products/Energy+.idd build/Products/Energy+.schema.epJSON binary_files/

# REQUIRED: the staged executable carries an absolute rpath pointing into your
# build tree, so it aborts on any other Mac with
#   dyld: Library not loaded: @rpath/libenergyplusapi.26.1.0.dylib
# Rewrite it to the executable's own folder and re-sign (Apple Silicon kills an
# edited Mach-O that is not re-signed):
OLD_RPATH=$(otool -l binary_files/energyplus | awk '/LC_RPATH/{getline; getline; print $2; exit}')
install_name_tool -rpath "$OLD_RPATH" @executable_path binary_files/energyplus
codesign --force --sign - binary_files/energyplus
```

Verify the rpath is portable before shipping — it must print `@executable_path`
and no absolute path:

```bash
otool -l binary_files/energyplus | grep -A2 LC_RPATH
```

Confirm the build actually picked up your changes before shipping it — e.g. for
the UPS meters:

```bash
strings -a binary_files/libenergyplusapi.26.1.0.dylib | grep "ITE Liquid Cooled UPS"
```

---

## Case study: Tampa, FL (annual, TMY3)

Three configurations of the same 557 m² data hall (2,143 kW design IT load, 71 racks, UPS efficiency 0.94, lighting 6.89 W/m²):

| IDF | Cooling | Heat rejection |
|---|---|---|
| `case_study/air_cooled.idf` | Air-cooled baseline (`ElectricEquipment:ITE:AirCooled`) | Water-cooled chiller + cooling tower |
| `case_study/chilled_water.idf` | Direct-to-chip, 12 °C facility water | Water-cooled chiller (COP 5.5) + cooling tower |
| `case_study/warm_water.idf` | Direct-to-chip, 45 °C facility water | Dry cooler; air-cooled room chiller for the CRAH |

In both liquid-cooled cases 20 % of the chip heat leaks to the room air and is removed by a CRAH.

```bash
energyplus -w weather/USA_FL_Tampa.Intl.AP.722110_TMY3.epw -d output/warm_water case_study/warm_water.idf
python scripts/compute_metrics.py output/warm_water/eplusout.sql
```

Each case should finish with `0 Severe Errors`. Warnings are expected — 28, 11 and 80
for air-cooled, chilled-water and warm-water respectively. The metrics script then
prints the values in the table below.

| | Air-cooled | Chilled water | Warm water |
|---|---|---|---|
| Facility electricity (MWh/yr) | 23,633 | 15,339 | 14,668 |
| IT electricity (MWh/yr) | 16,508 | 11,753 | 11,753 |
| Annual PUE | 1.43 | 1.31 | 1.25 |
| Peak hourly PUE | 1.68 | 1.50 | 1.48 |
| WUE (L/kWh) | 3.45 | 2.46 | 0 |
| EUE | 0.50 | 0.77 | 0.80 |
| EUE, 100 % liquid-heat reuse | — | 1.38 | 1.44 |

`compute_metrics.py` uses E_IT = ITE-CPU + ITE-Fans (IT load at the UPS output), PUE = site electricity / E_IT, WUE = site water / E_IT, and EUE = ITE-CPU / site electricity. Peak PUE needs the hourly meters already requested in the case-study IDFs. EUE with reuse additionally needs:

```idf
Output:Variable, *, ITE Liquid Cooled Heat Extraction Energy, Hourly;
```

---

## Adding the object to your own IDF

### Minimum required fields

```idf
ElectricEquipment:ITE:LiquidCooled,
  My Datacenter ITE,       !- Name
  My Space,                !- Zone or Space Name (Zone or Space object)
  Watts/Area,              !- Design Power Input Calculation Method (Watts/Unit or Watts/Area)
  ,                        !- Watts per Unit {W}         (if Watts/Unit)
  250,                     !- Number of Units            (number of racks)
  5000,                    !- Watts per Floor Area {W/m2} (if Watts/Area)
  Always On Discrete,      !- Design Power Input Schedule Name
  CPU Load Schedule,       !- CPU Loading Schedule Name (0-1 fraction)
  CPU Power Curve,         !- CPU Power Curve Name (biquadratic: f(load, T_rack_sup))
  18.0,                    !- FWS Design Supply Temperature {C}
  0.120,                   !- FWS Design Volumetric Flow Rate {m3/s}
  ITE FWS Inlet Node,      !- FWS Coolant Inlet Node Name
  ITE FWS Outlet Node,     !- FWS Coolant Outlet Node Name
  0.060,                   !- Rack Coolant Design Volumetric Flow Rate {m3/s}
  20.0,                    !- Rack Coolant Design Supply Temperature {C}
  2.0,                     !- CDU Approach Temperature {deltaC}  (T_rack_sup − T_fws_sup)
  0.001,                   !- Thermal Resistance Junction-to-Coolant {K/W} (per rack)
  85.0,                    !- Maximum Junction Temperature {C}
  0.05,                    !- Air Side Heat Leakage Fraction
  0.95,                    !- UPS Efficiency
  1.0,                     !- Fraction of UPS Losses to Zone
  ITE-CPU,                 !- CPU End-Use Subcategory
  ITE-UPS;                 !- UPS End-Use Subcategory
```

### Wiring into the plant loop

The object must appear on the **demand side** of a PlantLoop:

```idf
Branch,
  CW ITE Branch,
  ,
  ElectricEquipment:ITE:LiquidCooled, My Datacenter ITE,
  ITE FWS Inlet Node, ITE FWS Outlet Node;
```

That branch goes in the demand-side BranchList. The supply side can be a chiller, DistrictCooling, or any other plant equipment. See `case_study/chilled_water.idf` for a complete wired-up example.

---

## Sizing guidance

| Parameter | How to size |
|---|---|
| FWS flow rate | Q_design / (cp × rho × ΔT) — e.g. 10°C rise → ~24 L/s per MW |
| CDU approach temperature | Read from the CDU spec sheet — typically 1–5°C; 2°C is a good starting point |
| R_jc per rack | ΔT_target / (Q_total / N_racks) — e.g. 10°C / 10 kW = 0.001 K/W |
| Rack flow | Typically 40–60% of FWS flow for a two-loop architecture |
| Air leakage fraction | 0.02–0.10 for well-sealed racks with a rear-door HX; up to ~0.20 for moderate containment without one (both case studies use 0.20) |

---

## Output variables

All variables are available at any reporting frequency (TimeStep, Hourly, Daily, etc.):

```idf
Output:Variable, *, ITE Liquid Cooled CPU Electricity Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled CPU Electricity Energy, Hourly;
Output:Variable, *, ITE Liquid Cooled UPS Electricity Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled UPS Electricity Energy, Hourly;
Output:Variable, *, ITE Liquid Cooled Heat Extraction Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Heat Extraction Energy, Hourly;
Output:Variable, *, ITE Liquid Cooled Zone Sensible Heat Gain Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Zone Sensible Heat Gain Energy, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Supply Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Return Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Mass Flow Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Volumetric Flow Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Rack Coolant Supply Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Rack Coolant Return Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled CDU Log Mean Temperature Difference, Hourly;
Output:Variable, *, ITE Liquid Cooled CDU Approach Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Peak Chip Junction Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Time Above Maximum Junction Temperature, Hourly;
```

---

## Development Credits

The liquid cooling module was developed and validated by Dipashreya Sur, Yun-Dam Ko, and Cade Napier. The authors used Claude Sonnet 4.6 to streamline code development. 


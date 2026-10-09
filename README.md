# ElectricEquipment:ITE:LiquidCooled

A new EnergyPlus object for simulating **direct-to-chip liquid-cooled IT equipment** in data centers. Heat is removed via a facility water system (FWS) plant loop through an external CDU (Coolant Distribution Unit) heat exchanger. A small configurable fraction of heat leaks to the zone air.

Developed for the NatLabRockies EnergyPlus v26.1 fork.

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
        ├── ITE_LiquidCooled.hh
        ├── ITE_LiquidCooled.cc
        ├── InternalHeatGains.cc
        ├── CMakeLists.txt
        └── Plant/PlantManager.cc
```

### Which path is right for you?

| Your situation | What to do |
|---|---|
| **Mac, just want to run simulations** | Use `binary_files/` — fully self-contained, no building needed |
| **Mac, want to modify the C++ code** | Clone the NREL/EnergyPlus repo at v26.1.0, drop in `source_files/`, rebuild |
| **Windows or Linux** | Clone the NREL/EnergyPlus repo at v26.1.0, drop in `source_files/`, rebuild (the Mac binary won't run) |

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

# Copy in the new files (from binary_files/ in this zip)
cp binary_files/energyplus /Applications/EnergyPlus-26-1-0/energyplus
cp binary_files/Energy+.idd /Applications/EnergyPlus-26-1-0/Energy+.idd
cp binary_files/Energy+.schema.epJSON /Applications/EnergyPlus-26-1-0/Energy+.schema.epJSON
cp binary_files/libenergyplusapi.26.1.0.dylib /Applications/EnergyPlus-26-1-0/libenergyplusapi.26.1.0.dylib
```

Verify it worked:
```bash
energyplus --version
# should print: EnergyPlus, Version 26.1.0
```

### Windows / Linux: building from source

Clone the NatLabRockies EnergyPlus v26.1 fork, then:

```bash
cd EnergyPlus-src

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

cd build
cmake --build . --target energyplus
```

Build time is ~2 minutes (only the changed files recompile).

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

Expected result: `EnergyPlus Completed Successfully — 0 Severe Errors`

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
| Air leakage fraction | 0.02–0.10 depending on rack sealing quality |

---

## Output variables

All variables are available at any reporting frequency (TimeStep, Hourly, Daily, etc.):

```idf
Output:Variable, *, ITE Liquid Cooled CPU Electricity Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled UPS Electricity Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Heat Extraction Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Zone Sensible Heat Gain Rate, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Supply Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Return Temperature, Hourly;
Output:Variable, *, ITE Liquid Cooled Facility Water Mass Flow Rate, Hourly;
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


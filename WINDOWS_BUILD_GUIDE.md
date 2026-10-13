# Windows Build Guide — ElectricEquipment:ITE:LiquidCooled

*(Updated with all fixes discovered during first Windows build)*

This guide walks a Windows user through building the liquid-cooling patch from source and installing it into EnergyPlus 26.1. You only need to do this once.

**Estimated time:** 45–60 minutes (most of it is downloading Visual Studio and compiling)

---

## Step 1 — Install the three free tools

You need a C++ compiler, a build tool, and Git. Install them in this order.

### 1a. Visual Studio Community (the C++ compiler)

1. Go to [https://visualstudio.microsoft.com/vs/community/](https://visualstudio.microsoft.com/vs/community/) and click **Download**  
2. Run the installer  
3. When the installer asks what to install, check **"Desktop development with C++"** — this is the only workload you need  
4. Click **Install** and wait (this is the big download, \~5 GB)

Install whatever the current version of Visual Studio Community is. The build system will detect it automatically.

### 1b. CMake

1. Go to [https://cmake.org/download/](https://cmake.org/download/)  
2. Under "Latest Release", download the **Windows x64 Installer** (the `.msi` file)  
3. Run it — on the "Install Options" screen, select **"Add CMake to the system PATH for all users"**

### 1c. Git

1. Go to [https://git-scm.com/download/win](https://git-scm.com/download/win) and download the installer  
2. Run it with all default options

After installing all three, **restart your computer** before continuing. This ensures Visual Studio registers itself correctly so CMake can find it.

---

## Step 1d — Disable Smart App Control (personal computers only)

Windows Smart App Control blocks unsigned executables, including ones you compile yourself. You must turn it off before building.

1. Open **Windows Security** from the Start menu  
2. Click **App & browser control**  
3. Click **Smart App Control settings**  
4. Set it to **Off**

**Note:** Once disabled, Smart App Control cannot be re-enabled without resetting the PC. On managed work/university computers, contact IT instead of doing this yourself.

---

## Step 2 — Get the EnergyPlus source code

Open **Command Prompt** (search "cmd" in the Start menu) and run:

```
git clone https://github.com/NatLabRockies/EnergyPlus.git
cd EnergyPlus
git checkout v26.1.0
```

**Important:** The tag is `v26.1.0`, with the trailing `.0`. The NatLabRockies fork and NREL/EnergyPlus both carry this tag at the same commit (`6f2e40d`), so either clone gives identical source.

This downloads the source into a folder called `EnergyPlus` wherever you ran the command. It will take a few minutes.

---

## Step 3 — Copy in the liquid-cooling files

From the `source_files/` folder in this handoff, copy each file into the matching location inside the `EnergyPlus` folder you just cloned:

| File in `source_files/` | Copy to (inside the `EnergyPlus` clone) |
| :---- | :---- |
| `src/EnergyPlus/ITE_LiquidCooled.hh` | `src/EnergyPlus/ITE_LiquidCooled.hh` |
| `src/EnergyPlus/ITE_LiquidCooled.cc` | `src/EnergyPlus/ITE_LiquidCooled.cc` |
| `src/EnergyPlus/InternalHeatGains.cc` | `src/EnergyPlus/InternalHeatGains.cc` |
| `src/EnergyPlus/CMakeLists.txt` | `src/EnergyPlus/CMakeLists.txt` |
| `src/EnergyPlus/Plant/PlantManager.cc` | `src/EnergyPlus/Plant/PlantManager.cc` |
| `idd/Energy+.idd.in` | `idd/Energy+.idd.in` |

Click **Yes** when Windows asks to replace existing files.

---

## Step 3b — Apply Windows compatibility patches

The handoff was developed on Mac. Six additional source files need small patches for Windows/MSVC compatibility. A Python script handles all of them automatically.

Save the script below as `patch_for_windows.py` inside your `EnergyPlus` folder, then run it:

```
python patch_for_windows.py
```

**Script contents — save as `EnergyPlus\patch_for_windows.py`:**

```py
import re, os

base = os.path.dirname(os.path.abspath(__file__))
src  = os.path.join(base, 'src', 'EnergyPlus')

def patch(path, old, new, label):
    with open(path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    if old in content:
        content = content.replace(old, new)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'  PATCHED: {label}')
    else:
        print(f'  SKIP (already applied or not found): {label}')

# ── 1. EnergyPlusData.hh — forward declaration ─────────────────────────────
hh = os.path.join(src, 'Data', 'EnergyPlusData.hh')
patch(hh,
    'namespace AirflowNetwork {\n    struct Solver;\n} // namespace AirflowNetwork',
    'namespace AirflowNetwork {\n    struct Solver;\n} // namespace AirflowNetwork\n'
    'namespace ITELiquidCooled {\n    struct ITELiquidCooledGlobals;\n} // namespace ITELiquidCooled',
    'EnergyPlusData.hh — forward declaration')

patch(hh,
    '    std::unique_ptr<InternalHeatGainsData> dataInternalHeatGains;',
    '    std::unique_ptr<InternalHeatGainsData> dataInternalHeatGains;\n'
    '    std::unique_ptr<ITELiquidCooled::ITELiquidCooledGlobals> dataITELiquidCooled;',
    'EnergyPlusData.hh — data field')

# ── 2. EnergyPlusData.cc — include + constructor ────────────────────────────
cc_data = os.path.join(src, 'Data', 'EnergyPlusData.cc')
with open(cc_data, 'r', errors='ignore') as f:
    lines = f.readlines()

if 'ITE_LiquidCooled.hh' not in ''.join(lines):
    insert_idx = 0
    for i, line in enumerate(lines):
        if '#include <EnergyPlus/' in line:
            insert_idx = i + 1
    lines.insert(insert_idx, '#include <EnergyPlus/ITE_LiquidCooled.hh>\n')
    with open(cc_data, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print('  PATCHED: EnergyPlusData.cc — include')
else:
    print('  SKIP (already applied): EnergyPlusData.cc — include')

patch(cc_data,
    'dataInternalHeatGains = std::make_unique<InternalHeatGainsData>();',
    'dataInternalHeatGains = std::make_unique<InternalHeatGainsData>();\n'
    '    dataITELiquidCooled = std::make_unique<ITELiquidCooled::ITELiquidCooledGlobals>();',
    'EnergyPlusData.cc — constructor')

# ── 3. Plant/Enums.hh — PlantEquipmentType enum ─────────────────────────────
enums = os.path.join(src, 'Plant', 'Enums.hh')
patch(enums,
    '    PurchSteam,\n    Num',
    '    PurchSteam,\n    ITELiquidCooled,\n    Num',
    'Enums.hh — PlantEquipmentType')

# ── 4. DataLoopNode.hh — ConnectionObjectType enum ──────────────────────────
dln = os.path.join(src, 'DataLoopNode.hh')
patch(dln,
    '        ElectricEquipmentITEAirCooled,',
    '        ElectricEquipmentITEAirCooled,\n        ElectricEquipmentITELiquidCooled,',
    'DataLoopNode.hh — ConnectionObjectType')

# ── 5. Plant/DataPlant.hh — name arrays and ValidLoopEquipTypes ─────────────
dp = os.path.join(src, 'Plant', 'DataPlant.hh')
patch(dp,
    '        "DistrictHeating:Steam"};',
    '        "DistrictHeating:Steam",\n        "ElectricEquipment:ITE:LiquidCooled"};',
    'DataPlant.hh — PlantEquipTypeNames')
patch(dp,
    '        "DISTRICTHEATING:STEAM"};',
    '        "DISTRICTHEATING:STEAM",\n        "ELECTRICEQUIPMENT:ITE:LIQUIDCOOLED"};',
    'DataPlant.hh — PlantEquipTypeNamesUC')
patch(dp,
    '        LoopType::Both   //        "DistrictHeating:Steam"\n    };',
    '        LoopType::Both,  //        "DistrictHeating:Steam"\n'
    '        LoopType::Plant  //        "ElectricEquipment:ITE:LiquidCooled"\n    };',
    'DataPlant.hh — ValidLoopEquipTypes')

# ── 6. CMakeLists.txt — wrap GCC-only flag for MSVC ─────────────────────────
cmake = os.path.join(src, 'CMakeLists.txt')
patch(cmake,
    'target_compile_options(energyplus PRIVATE -Wno-deprecated-declarations)',
    'if(NOT MSVC)\n  target_compile_options(energyplus PRIVATE -Wno-deprecated-declarations)\nendif()',
    'CMakeLists.txt — MSVC flag guard')

# ── 7. ITE_LiquidCooled.cc — resolve format() ambiguity with MSVC ───────────
ite_cc = os.path.join(src, 'ITE_LiquidCooled.cc')
with open(ite_cc, 'r', encoding='utf-8') as f:
    content = f.read()
fixed = re.sub(r'(?<![:\w])format\(', 'EnergyPlus::format(', content)
with open(ite_cc, 'w', encoding='utf-8') as f:
    f.write(fixed)
print('  PATCHED: ITE_LiquidCooled.cc — format() ambiguity')

print('\nAll patches applied. Proceed to Step 4.')
```

---

## Step 4 — Build

In Command Prompt, navigate into the `EnergyPlus` folder and run:

```
mkdir build
cd build
cmake .. -A x64
cmake --build . --target energyplus --config Release
```

CMake will automatically detect your installed version of Visual Studio. You do not need to specify `-G "Visual Studio 17 2022"` — omitting the `-G` flag lets CMake pick the right generator for whatever version you have installed.

The compile step takes 10–20 minutes on first run. When it finishes you should see output ending in `Build succeeded.`

---

## Step 5 — Find the output files

After the build, the files you need are in two locations:

| File | Location inside the build folder |
| :---- | :---- |
| `energyplus.exe` | `build\Products\Release\energyplus.exe` |
| `energyplusapi.dll` | `build\Products\Release\energyplusapi.dll` |
| `Energy+.idd` | `build\Products\Energy+.idd` |
| `Energy+.schema.epJSON` | `build\Products\Energy+.schema.epJSON` |

**Note:** The IDD and schema are in `build\Products\`, not `build\Products\Release\`. The `energyplusapi.dll` is the main simulation library and must be copied alongside `energyplus.exe`.

---

## Step 6 — Install into EnergyPlus 26.1

EnergyPlus on Windows is typically installed at `C:\EnergyPlusV26-1-0\`.

1. **Back up the originals first:**

```
copy "C:\EnergyPlusV26-1-0\energyplus.exe" "C:\EnergyPlusV26-1-0\energyplus.exe.bak"
copy "C:\EnergyPlusV26-1-0\Energy+.idd"    "C:\EnergyPlusV26-1-0\Energy+.idd.bak"
```

2. **Copy in the new files** (run from inside the `EnergyPlus` folder):

```
copy "build\Products\Release\energyplus.exe"        "C:\EnergyPlusV26-1-0\energyplus.exe"
copy "build\Products\Release\energyplusapi.dll"     "C:\EnergyPlusV26-1-0\energyplusapi.dll"
copy "build\Products\Energy+.idd"                   "C:\EnergyPlusV26-1-0\Energy+.idd"
copy "build\Products\Energy+.schema.epJSON"         "C:\EnergyPlusV26-1-0\Energy+.schema.epJSON"
```

3. **Verify it worked:**

```
"C:\EnergyPlusV26-1-0\energyplus.exe" --version
```

Should print: `EnergyPlus, Version 26.1.0`

---

## Step 7 — Run the example simulation

```
"C:\EnergyPlusV26-1-0\energyplus.exe" ^
  -w "C:\EnergyPlusV26-1-0\WeatherData\USA_FL_Tampa.Intl.AP.722110_TMY3.epw" ^
  -d output ^
  case_study\chilled_water.idf
```

Expected result: `EnergyPlus Completed Successfully — 0 Severe Errors`

---

## Troubleshooting

**`cmake` is not recognized** Close and reopen Command Prompt after installing CMake. If it still fails, search "Edit the system environment variables" in the Start menu → Environment Variables → check that CMake's `bin` folder is in `Path`.

**`git` is not recognized** Same fix — close and reopen Command Prompt after installing Git.

**CMake can't find the compiler** Make sure you restarted your computer after installing Visual Studio, and that "Desktop development with C++" was selected in the installer.

**`generate_embeddable_epJSON_schema.exe` blocked by Device Guard / Smart App Control** See Step 1d. Turn off Smart App Control in Windows Security → App & browser control → Smart App Control settings.

**`ElectricEquipment:ITE:LiquidCooled is not a valid Object Type`** The `energyplusapi.dll` was not copied. Make sure you copy both `energyplus.exe` and `energyplusapi.dll` in Step 6\.

**Build fails with a red error message** Make sure you ran the patch script in Step 3b before building. A missing patch is the most common cause of compile errors.

**Wrong EnergyPlus install path** If EnergyPlus is not at `C:\EnergyPlusV26-1-0\`, search your C: drive for `energyplus.exe` to find it, then substitute that path in the commands above.  

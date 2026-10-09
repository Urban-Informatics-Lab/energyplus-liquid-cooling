// EnergyPlus, Copyright (c) 1996-present, The Board of Trustees of the University of Illinois,
// The Regents of the University of California, through Lawrence Berkeley National Laboratory
// (subject to receipt of any required approvals from the U.S. Dept. of Energy), Oak Ridge
// National Laboratory, managed by UT-Battelle, Alliance for Energy Innovation, LLC, and other
// contributors. All rights reserved.
//
// NOTICE: This Software was developed under funding from the U.S. Department of Energy and the
// U.S. Government consequently retains certain rights. As such, the U.S. Government has been
// granted for itself and others acting on its behalf a paid-up, nonexclusive, irrevocable,
// worldwide license in the Software to reproduce, distribute copies to the public, prepare
// derivative works, and perform publicly and display publicly, and to permit others to do so.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.

#ifndef EnergyPlus_ITE_LiquidCooled_hh_INCLUDED
#define EnergyPlus_ITE_LiquidCooled_hh_INCLUDED

// ============================================================================================
//  ITE_LiquidCooled.hh
//
//  New EnergyPlus object:  ElectricEquipment:ITE:LiquidCooled
//
//  Models direct-to-chip (cold-plate) liquid cooling for IT equipment in data centers.
//  Implements the two-loop architecture from ASHRAE TC 9.9:
//
//    ┌──────────────────────────────────────────────────────────────────────┐
//    │  Datacom Equipment Center                                            │
//    │                                                                      │
//    │   Server cold plates  ──►  Rack CDU manifold                        │
//    │     (secondary loop)              │                                  │
//    │                                   ▼                                  │
//    │                        External CDU heat exchanger                   │
//    │                        T_rack_sup = T_fws_sup + ΔT_approach         │
//    │                                   │                                  │
//    └───────────────────────────────────┼──────────────────────────────────┘
//                                        ▼
//                        Facility Water System (FWS)  ←── EnergyPlus PlantLoop
//                                        │
//                                   Chiller + CWS
//
//  Physics per timestep:
//
//  (1) IT power (with bivariate curve f(cpu_load, T_rack_supply)):
//        W_cpu = W_design × f_oper × Curve(f_load, T_rack_sup)
//
//  (2) Heat partition:
//        Q_liq = W_cpu × (1 − air_leakage_frac)   [W → External CDU / FWS loop]
//        Q_air = W_cpu × air_leakage_frac          [W → zone convection]
//
//  (3) Facility water side  (Q = ρ·V̇·cₚ·ΔT,  equivalently Q = ṁ·cₚ·ΔT):
//        ṁ_fws      = ρ_fws(T_fws_sup) × V̇_fws
//        T_fws_ret  = T_fws_sup + Q_liq / (ṁ_fws × cₚ_fws)
//
//  (4) External CDU heat exchanger — fixed approach temperature:
//        T_rack_sup = T_fws_sup + ΔT_approach
//        T_rack_ret = T_rack_sup + Q_liq / C_rack
//        [If ΔT_approach = 0: ideal HX, T_rack_sup = T_fws_sup]
//
//  (5) LMTD (counterflow, for output):
//        ΔT₁       = T_rack_ret − T_fws_ret   (hot-in vs cold-out)
//        ΔT₂       = T_rack_sup − T_fws_sup   (hot-out vs cold-in)
//        ΔT_lm     = (ΔT₁ − ΔT₂) / ln(ΔT₁/ΔT₂)
//
//  (6) Peak chip junction temperature:
//        T_junc = T_rack_sup + Q_liq × R_junction_to_coolant
//
//  Integration points in the EnergyPlus 26.1 source tree:
//    src/EnergyPlus/CMakeLists.txt          — add ITE_LiquidCooled.cc / .hh
//    src/EnergyPlus/EnergyPlusData.hh       — add dataITELiquidCooled field
//    src/EnergyPlus/InternalHeatGains.cc    — call GetITELiquidCooledInput() and
//                                             CalcZoneITELiquidCooledComponents()
//    src/EnergyPlus/PlantLoopEquipment.cc   — register PlantEquipmentType::ITELiquidCooled
//    src/EnergyPlus/DataPlant.hh            — add ITELiquidCooled to enum
//    Energy+.idd                            — paste ITE_LiquidCooled.idd snippet
// ============================================================================================

#include <string>
#include <vector>

#include <EnergyPlus/Data/BaseData.hh>
#include <EnergyPlus/EPVector.hh>
#include <EnergyPlus/EnergyPlus.hh>
#include <EnergyPlus/Plant/DataPlant.hh>
#include <EnergyPlus/Plant/PlantLocation.hh>
#include <EnergyPlus/ScheduleManager.hh>

namespace EnergyPlus {

struct EnergyPlusData; // forward declaration

namespace ITELiquidCooled {

    static constexpr std::string_view cCurrentModuleObject{"ElectricEquipment:ITE:LiquidCooled"};

    enum class DesignPowerMethod
    {
        Invalid    = -1,
        WattsPerUnit = 1, // W_design = Watts_per_unit × NumUnits
        WattsPerArea  // W_design = Watts_per_area × FloorArea
    };

    // -----------------------------------------------------------------------
    //  ITELiquidCooledEquipment
    //
    //  One instance per IDF object.  Inherits PlantComponent so the plant
    //  loop manager calls simulate() on the demand side of the FWS loop.
    // -----------------------------------------------------------------------
    struct ITELiquidCooledEquipment : public PlantComponent
    {
        // --- Identity ----------------------------------------------------------
        std::string Name;
        std::string ZoneName;
        int ZoneNum    = 0;
        int SpaceNum   = 0;
        Real64 FloorAreaM2 = 0.0;

        // --- IT equipment design -----------------------------------------------
        DesignPowerMethod powerMethod   = DesignPowerMethod::WattsPerUnit;
        Real64 DesignPowerPerUnit       = 0.0;  // [W/unit]
        int    NumUnits                 = 1;
        Real64 DesignPowerPerArea       = 0.0;  // [W/m²]
        Real64 DesignTotalPower         = 0.0;  // [W] computed at input time

        // --- Facility Water System (FWS) loop — EnergyPlus PlantLoop side ------
        // Q = ρ·V̇_fws·cₚ·ΔT  =  ṁ_fws·cₚ·(T_fws_ret − T_fws_sup)
        Real64 FWSDesignVolFlowRate     = 0.0;  // [m³/s] V̇_fws
        Real64 FWSDesignMassFlowRate    = 0.0;  // [kg/s] set during sizing
        Real64 FWSDesignSupplyTemp      = 18.0; // [°C]   chilled water supply at design

        std::string CoolantInletNodeName;   // FWS inlet to CDU
        std::string CoolantOutletNodeName;  // FWS outlet from CDU
        int CoolantInletNodeNum             = 0;
        int CoolantOutletNodeNum            = 0;
        PlantLocation plantLoc{};

        // --- Rack secondary loop (server cold-plate side) ----------------------
        // Q = ρ·V̇_rack·cₚ·ΔT  =  ṁ_rack·cₚ·(T_rack_ret − T_rack_sup)
        Real64 RackDesignVolFlowRate    = 0.0;  // [m³/s] V̇_rack
        Real64 RackDesignMassFlowRate   = 0.0;  // [kg/s] ṁ_rack = ρ·V̇_rack
        Real64 RackDesignSupplyTemp     = 20.0; // [°C]   T_rack_sup at design (ASHRAE W4)

        // --- External CDU heat exchanger — approach temperature -----------------
        // T_rack_sup = T_fws_sup + CDUApproachTemp. Zero means ideal (no approach).
        Real64 CDUApproachTemp          = 2.0;  // [deltaC]  T_rack_sup − T_fws_sup

        // --- Thermal / loss model ----------------------------------------------
        Real64 AirLeakageFraction             = 0.05; // fraction of W_cpu to zone air
        Real64 ThermalResistJunctionToCoolant = 0.04; // [K/W]  R_jc worst chip
        Real64 MaxJunctionTemp                = 85.0; // [°C]   T_j limit
        Real64 UPSEfficiency                  = 0.95;
        Real64 FractionUPSLossesToZone        = 1.0;

        // --- Schedule indices --------------------------------------------------
        Sched::Schedule *OperSched    = nullptr; // operating multiplier
        Sched::Schedule *CPULoadSched = nullptr; // CPU load fraction

        // --- Curve indices -----------------------------------------------------
        // Bivariate f(cpu_load_fraction, T_rack_supply_C) → IT power fraction.
        // Must equal 1.0 at design point (1.0, RackDesignSupplyTemp).
        int CPUPowerFuncLoadTempCurveNum = 0;

        // --- One-time / per-environment flags ----------------------------------
        bool MyOneTimeFlag   = true;
        bool MyEnvrnFlag     = true;
        bool MySizeFlag      = true;
        bool MyPlantScanFlag = true;

        // -----------------------------------------------------------------------
        //  Current-timestep simulation state
        // -----------------------------------------------------------------------

        // IT load
        Real64 CPULoadFraction     = 1.0;  // from schedule
        Real64 CPUPower            = 0.0;  // [W]
        Real64 UPSLoss             = 0.0;  // [W]
        Real64 UPSPower            = 0.0;  // [W] UPS loss electricity
        Real64 TotalITPower        = 0.0;  // [W] = CPUPower / UPSEfficiency

        // Heat split
        Real64 LiquidHeatExtracted = 0.0;  // [W] Q_liq → plant loop
        Real64 ZoneConvGainRate    = 0.0;  // [W] Q_air → zone convection

        // FWS loop (EnergyPlus PlantLoop, primary circuit)
        // Q = ρ·V̇_fws·cₚ·(T_fws_ret − T_fws_sup)
        Real64 FWSSupplyTemp       = 18.0; // [°C] from plant inlet node
        Real64 FWSReturnTemp       = 0.0;  // [°C] computed, set on outlet node
        Real64 FWSMassFlowRate     = 0.0;  // [kg/s] ṁ_fws actual
        Real64 FWSVolFlowRate      = 0.0;  // [m³/s] V̇_fws = ṁ_fws/ρ (reported)
        Real64 FWSCoolantCp        = 4182.0; // [J/kg·K] cₚ_fws (from FluidProperties)
        Real64 FWSCoolantDensity   = 998.2;  // [kg/m³] ρ_fws (from FluidProperties)

        // Rack secondary loop (cold-plate side)
        // Q = ρ·V̇_rack·cₚ·(T_rack_ret − T_rack_sup)
        Real64 RackSupplyTemp      = 20.0; // [°C] T_rack_sup → cold plates
        Real64 RackReturnTemp      = 0.0;  // [°C] T_rack_ret ← cold plates
        Real64 RackMassFlowRate    = 0.0;  // [kg/s] ṁ_rack (fixed at design)

        // CDU heat exchanger outputs
        Real64 CDU_LMTD            = 0.0;  // [K]   log-mean temperature difference
        Real64 CDU_ApproachTemp    = 0.0;  // [K]   T_rack_sup − T_fws_sup (reported)

        // Chip thermal model
        Real64 JunctionTemp             = 0.0;  // [°C] T_junc = T_rack_sup + Q·R_jc
        Real64 TimeAboveMaxJunctionTemp = 0.0;  // [hr]

        // --- Energy accumulators -----------------------------------------------
        Real64 CPUEnergy                 = 0.0; // [J]
        Real64 UPSEnergy                 = 0.0; // [J]
        Real64 LiquidHeatExtractedEnergy = 0.0; // [J]
        Real64 ZoneConvGainEnergy        = 0.0; // [J]

        // --- End-use subcategory labels ----------------------------------------
        std::string CPUEndUseSubcategory = "ITE-CPU";
        std::string UPSEndUseSubcategory = "ITE-UPS";

        // -----------------------------------------------------------------------
        //  PlantComponent virtual interface
        // -----------------------------------------------------------------------
        void simulate(EnergyPlusData &state,
                      const PlantLocation &calledFromLocation,
                      bool FirstHVACIteration,
                      Real64 &CurLoad,
                      bool RunFlag) override;

        void getDesignCapacities([[maybe_unused]] EnergyPlusData &state,
                                 const PlantLocation &calledFromLocation,
                                 Real64 &MaxLoad,
                                 Real64 &MinLoad,
                                 Real64 &OptLoad) override;

        void onInitLoopEquip(EnergyPlusData &state,
                             const PlantLocation &calledFromLocation) override;

        void oneTimeInit([[maybe_unused]] EnergyPlusData &state) override {}

        static ITELiquidCooledEquipment *factory(EnergyPlusData &state,
                                                  const std::string &objectName);

    private:
        void init(EnergyPlusData &state);
        void calc(EnergyPlusData &state);
        void reportVars(EnergyPlusData &state);
        void size(EnergyPlusData &state);

        Real64 calcLMTD(Real64 dT1, Real64 dT2) const;
    };

    // -----------------------------------------------------------------------
    //  Module-level global state  (stored as dataITELiquidCooled in EnergyPlusData)
    // -----------------------------------------------------------------------
    struct ITELiquidCooledGlobals : BaseGlobalStruct
    {
        bool getInputFlag = true;
        EPVector<ITELiquidCooledEquipment> equipment;
        // Per-zone air-side convective gain accumulator [W], indexed 0-based by ZoneNum-1.
        // Zeroed and refilled each zone-timestep by CalcZoneITELiquidCooledComponents().
        std::vector<Real64> zoneConvGainW;

        void init_constant_state([[maybe_unused]] EnergyPlusData &state) override {}
        void init_state([[maybe_unused]] EnergyPlusData &state) override {}
        void clear_state() override
        {
            getInputFlag = true;
            equipment.clear();
            zoneConvGainW.clear();
        }
    };

    // -----------------------------------------------------------------------
    //  Free functions called from EnergyPlus integration points
    // -----------------------------------------------------------------------
    void GetITELiquidCooledInput(EnergyPlusData &state);
    void CalcZoneITELiquidCooledComponents(EnergyPlusData &state);
    void SimITELiquidCooled(EnergyPlusData &state,
                             const std::string &compName,
                             bool FirstHVACIteration,
                             Real64 &CurLoad,
                             bool RunFlag,
                             int &CompIndex,
                             const PlantLocation &calledFromLocation);

} // namespace ITELiquidCooled

// -----------------------------------------------------------------------
//  EnergyPlusData additions (add to EnergyPlusData.hh and .cc):
//
//  EnergyPlusData.hh:
//    #include <EnergyPlus/ITE_LiquidCooled.hh>
//    std::unique_ptr<ITELiquidCooled::ITELiquidCooledGlobals> dataITELiquidCooled;
//
//  EnergyPlusData.cc constructor:
//    dataITELiquidCooled = std::make_unique<ITELiquidCooled::ITELiquidCooledGlobals>();
// -----------------------------------------------------------------------

} // namespace EnergyPlus

#endif // EnergyPlus_ITE_LiquidCooled_hh_INCLUDED

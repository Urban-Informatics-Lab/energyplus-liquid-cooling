// EnergyPlus, Copyright (c) 1996-present, The Board of Trustees of the University of Illinois,
// The Regents of the University of California, through Lawrence Berkeley National Laboratory
// (subject to receipt of any required approvals from the U.S. Dept. of Energy), Oak Ridge
// National Laboratory, managed by UT-Battelle, Alliance for Energy Innovation, LLC, and other
// contributors. All rights reserved.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.

// ============================================================================================
//  ITE_LiquidCooled.cc
//
//  Implementation of ElectricEquipment:ITE:LiquidCooled for EnergyPlus 26.1.
//
//  Key physics implemented here:
//
//    ── Facility Water System (FWS) side — EnergyPlus PlantLoop ──────────────
//    Q_liq  = ρ_fws · V̇_fws · cₚ_fws · (T_fws_ret − T_fws_sup)
//           = ṁ_fws · cₚ_fws · ΔT_fws
//
//    ── External CDU heat exchanger — fixed approach temperature ─────────────
//    T_rack_sup = T_fws_sup + ΔT_approach
//
//    ── LMTD output (counterflow, for reporting) ──────────────────────────────
//    ΔT₁       = T_rack_ret − T_fws_ret
//    ΔT₂       = T_rack_sup − T_fws_sup
//    ΔT_lm     = (ΔT₁ − ΔT₂) / ln(ΔT₁/ΔT₂)
//
//    ── Rack secondary loop ───────────────────────────────────────────────────
//    Q_liq  = ρ_rack · V̇_rack · cₚ_rack · (T_rack_ret − T_rack_sup)
//    T_rack_ret = T_rack_sup + Q_liq / (ṁ_rack · cₚ_rack)
//
//    ── Chip junction temperature ─────────────────────────────────────────────
//    T_junc = T_rack_sup + Q_liq · R_junction_to_coolant
// ============================================================================================

#include <EnergyPlus/ITE_LiquidCooled.hh>

#include <cmath>

#include <EnergyPlus/BranchNodeConnections.hh>
#include <EnergyPlus/CurveManager.hh>
#include <EnergyPlus/Data/EnergyPlusData.hh>
#include <EnergyPlus/DataEnvironment.hh>
#include <EnergyPlus/DataGlobalConstants.hh>
#include <EnergyPlus/DataGlobals.hh>
#include <EnergyPlus/DataHeatBalance.hh>
#include <EnergyPlus/DataHVACGlobals.hh>
#include <EnergyPlus/DataIPShortCuts.hh>
#include <EnergyPlus/DataLoopNode.hh>
#include <EnergyPlus/Plant/DataPlant.hh>
#include <EnergyPlus/FluidProperties.hh>
#include <EnergyPlus/General.hh>
#include <EnergyPlus/HeatBalanceInternalHeatGains.hh>
#include <EnergyPlus/InputProcessing/InputProcessor.hh>
#include <EnergyPlus/NodeInputManager.hh>
#include <EnergyPlus/OutputProcessor.hh>
#include <EnergyPlus/Plant/PlantLocation.hh>
#include <EnergyPlus/PlantUtilities.hh>
#include <EnergyPlus/ScheduleManager.hh>
#include <EnergyPlus/UtilityRoutines.hh>

namespace EnergyPlus::ITELiquidCooled {

// ============================================================================
//  GetITELiquidCooledInput
//  Reads all ElectricEquipment:ITE:LiquidCooled objects from the IDF once.
//  Called from InternalHeatGains::GetInternalHeatGainsInput().
// ============================================================================
void GetITELiquidCooledInput(EnergyPlusData &state)
{
    if (!state.dataITELiquidCooled->getInputFlag) return;
    state.dataITELiquidCooled->getInputFlag = false;

    static constexpr std::string_view routineName{"GetITELiquidCooledInput"};

    auto &ip = *state.dataInputProcessing->inputProcessor;
    int numObjects = ip.getNumObjectsFound(state, cCurrentModuleObject);
    if (numObjects == 0) {
        // Size zone gain accumulator even when there are no objects
        state.dataITELiquidCooled->zoneConvGainW.assign(
            state.dataGlobal->NumOfZones, 0.0);
        return;
    }

    auto &equipment = state.dataITELiquidCooled->equipment;
    equipment.resize(numObjects);

    auto &cAlphaFields   = state.dataIPShortCut->cAlphaFieldNames;
    auto &cNumericFields = state.dataIPShortCut->cNumericFieldNames;
    auto &cAlphaArgs     = state.dataIPShortCut->cAlphaArgs;
    auto &rNumericArgs   = state.dataIPShortCut->rNumericArgs;
    auto &lAlphaBlanks   = state.dataIPShortCut->lAlphaFieldBlanks;
    auto &lNumericBlanks = state.dataIPShortCut->lNumericFieldBlanks;

    bool errorsFound = false;

    for (int idx = 0; idx < numObjects; ++idx) {
        int numAlphas = 0, numNumbers = 0, ioStatus = 0;
        ip.getObjectItem(state, cCurrentModuleObject, idx + 1,
                         cAlphaArgs, numAlphas,
                         rNumericArgs, numNumbers, ioStatus,
                         lNumericBlanks, lAlphaBlanks,
                         cAlphaFields, cNumericFields);

        auto &eq = equipment[idx];

        // ── A1: Name ──────────────────────────────────────────────────────────
        eq.Name = cAlphaArgs(1);
        if (Util::FindItemInList(eq.Name, equipment,
                                 &ITELiquidCooledEquipment::Name, idx) > 0) {
            ShowSevereError(state, format("{}: duplicate name \"{}\"", routineName, eq.Name));
            errorsFound = true;
        }

        // ── A2: Zone or Space Name ────────────────────────────────────────────
        // Accept either a Zone name or a Space name (identical to how other IHG
        // objects work: try Zone first, then Space, derive zone from space).
        eq.ZoneName = cAlphaArgs(2);
        eq.ZoneNum  = Util::FindItemInList(
            eq.ZoneName, state.dataHeatBal->Zone);
        if (eq.ZoneNum == 0) {
            // Try as a Space name
            eq.SpaceNum = Util::FindItemInList(
                eq.ZoneName, state.dataHeatBal->space);
            if (eq.SpaceNum == 0) {
                ShowSevereError(state, format("{}: Zone or Space \"{}\" not found for object \"{}\"",
                                              routineName, eq.ZoneName, eq.Name));
                errorsFound = true;
            } else {
                eq.ZoneNum     = state.dataHeatBal->space(eq.SpaceNum).zoneNum;
                eq.FloorAreaM2 = state.dataHeatBal->space(eq.SpaceNum).FloorArea;
            }
        } else {
            eq.SpaceNum    = state.dataHeatBal->Zone(eq.ZoneNum).spaceIndexes[0];
            eq.FloorAreaM2 = state.dataHeatBal->Zone(eq.ZoneNum).FloorArea;
        }

        // ── A3: Design Power Calculation Method ───────────────────────────────
        // Alpha args from getObjectItem come back uppercased for \type choice fields.
        const std::string &method = cAlphaArgs(3);
        if (Util::SameString(method, "Watts/Unit")) {
            eq.powerMethod = DesignPowerMethod::WattsPerUnit;
        } else if (Util::SameString(method, "Watts/Area")) {
            eq.powerMethod = DesignPowerMethod::WattsPerArea;
        } else {
            ShowSevereError(state, format("{}: unknown Design Power Method \"{}\" for \"{}\" (expected Watts/Unit or Watts/Area)",
                                          routineName, method, eq.Name));
            errorsFound = true;
        }

        // ── N1: Watts per Unit  N2: Number of Units  N3: Watts per Floor Area ─
        eq.DesignPowerPerUnit = rNumericArgs(1);
        eq.NumUnits           = static_cast<int>(rNumericArgs(2));
        eq.DesignPowerPerArea = rNumericArgs(3);

        eq.DesignTotalPower = (eq.powerMethod == DesignPowerMethod::WattsPerUnit)
            ? eq.DesignPowerPerUnit * eq.NumUnits
            : eq.DesignPowerPerArea * eq.FloorAreaM2;

        if (eq.DesignTotalPower <= 0.0) {
            ShowWarningError(state,
                format("{}: Design Total Power <= 0 for \"{}\"; check inputs.", routineName, eq.Name));
        }

        // ── A4: Design Power Input Schedule ──────────────────────────────────
        if (!lAlphaBlanks(4)) {
            eq.OperSched = Sched::GetSchedule(state, cAlphaArgs(4));
            if (eq.OperSched == nullptr) {
                ShowSevereError(state, format("{}: schedule \"{}\" not found for \"{}\"",
                                              routineName, cAlphaArgs(4), eq.Name));
                errorsFound = true;
            }
        }

        // ── A5: CPU Loading Schedule ──────────────────────────────────────────
        if (!lAlphaBlanks(5)) {
            eq.CPULoadSched = Sched::GetSchedule(state, cAlphaArgs(5));
            if (eq.CPULoadSched == nullptr) {
                ShowSevereError(state, format("{}: CPU loading schedule \"{}\" not found for \"{}\"",
                                              routineName, cAlphaArgs(5), eq.Name));
                errorsFound = true;
            }
        }

        // ── A6: CPU Power Curve f(cpu_load, T_rack_supply) ───────────────────
        if (!lAlphaBlanks(6)) {
            eq.CPUPowerFuncLoadTempCurveNum =
                Curve::GetCurveIndex(state, cAlphaArgs(6));
            if (eq.CPUPowerFuncLoadTempCurveNum == 0) {
                ShowSevereError(state, format("{}: curve \"{}\" not found for \"{}\"",
                                              routineName, cAlphaArgs(6), eq.Name));
                errorsFound = true;
            }
        }

        // ── N4: FWS Design Supply Temperature [°C] ────────────────────────────
        if (!lNumericBlanks(4)) eq.FWSDesignSupplyTemp = rNumericArgs(4);

        // ── N5: FWS Design Volumetric Flow Rate [m³/s]  (V̇_fws) ─────────────
        eq.FWSDesignVolFlowRate = rNumericArgs(5);
        if (eq.FWSDesignVolFlowRate <= 0.0) {
            ShowSevereError(state,
                format("{}: FWS Volumetric Flow Rate must be > 0 for \"{}\"", routineName, eq.Name));
            errorsFound = true;
        }

        // ── A7: FWS Coolant Inlet Node Name ───────────────────────────────────
        eq.CoolantInletNodeName = cAlphaArgs(7);
        eq.CoolantInletNodeNum  = Node::GetOnlySingleNode(
            state, eq.CoolantInletNodeName, errorsFound,
            Node::ConnectionObjectType::ElectricEquipmentITELiquidCooled,
            eq.Name,
            Node::FluidType::Water,
            Node::ConnectionType::Inlet,
            Node::CompFluidStream::Primary,
            Node::ObjectIsNotParent);

        // ── A8: FWS Coolant Outlet Node Name ──────────────────────────────────
        eq.CoolantOutletNodeName = cAlphaArgs(8);
        eq.CoolantOutletNodeNum  = Node::GetOnlySingleNode(
            state, eq.CoolantOutletNodeName, errorsFound,
            Node::ConnectionObjectType::ElectricEquipmentITELiquidCooled,
            eq.Name,
            Node::FluidType::Water,
            Node::ConnectionType::Outlet,
            Node::CompFluidStream::Primary,
            Node::ObjectIsNotParent);

        Node::TestCompSet(
            state, std::string(cCurrentModuleObject), eq.Name,
            eq.CoolantInletNodeName, eq.CoolantOutletNodeName,
            "Facility Water Nodes");

        // ── N6: Rack Secondary Loop Volumetric Flow Rate [m³/s] (V̇_rack) ─────
        eq.RackDesignVolFlowRate = rNumericArgs(6);
        if (eq.RackDesignVolFlowRate <= 0.0) {
            ShowSevereError(state,
                format("{}: Rack Coolant Volumetric Flow Rate must be > 0 for \"{}\"",
                        routineName, eq.Name));
            errorsFound = true;
        }

        // ── N7: Rack Design Supply Temperature [°C]  (T_rack_sup at design) ───
        if (!lNumericBlanks(7)) eq.RackDesignSupplyTemp = rNumericArgs(7);

        // ── N8: CDU Approach Temperature [deltaC] — T_rack_sup − T_fws_sup ──────
        if (!lNumericBlanks(8)) eq.CDUApproachTemp = rNumericArgs(8);

        // ── N9: Thermal Resistance Junction-to-Coolant [K/W] ─────────────────
        if (!lNumericBlanks(9)) eq.ThermalResistJunctionToCoolant = rNumericArgs(9);

        // ── N10: Maximum Junction Temperature [°C] ────────────────────────────
        if (!lNumericBlanks(10)) eq.MaxJunctionTemp = rNumericArgs(10);

        // ── N11: Air Side Heat Leakage Fraction ───────────────────────────────
        if (!lNumericBlanks(11)) {
            eq.AirLeakageFraction = rNumericArgs(11);
            if (eq.AirLeakageFraction < 0.0 || eq.AirLeakageFraction > 1.0) {
                ShowSevereError(state,
                    format("{}: Air Leakage Fraction must be 0-1 for \"{}\"", routineName, eq.Name));
                errorsFound = true;
            }
        }

        // ── N12: UPS Efficiency ───────────────────────────────────────────────
        if (!lNumericBlanks(12)) {
            eq.UPSEfficiency = rNumericArgs(12);
            if (eq.UPSEfficiency <= 0.0 || eq.UPSEfficiency > 1.0) {
                ShowSevereError(state,
                    format("{}: UPS Efficiency must be (0,1] for \"{}\"", routineName, eq.Name));
                errorsFound = true;
            }
        }

        // ── N13: Fraction of UPS Losses to Zone ──────────────────────────────
        if (!lNumericBlanks(13)) eq.FractionUPSLossesToZone = rNumericArgs(13);

        // ── A9: CPU End-Use Subcategory  ──────────────────────────────────────
        if (!lAlphaBlanks(9)) eq.CPUEndUseSubcategory = cAlphaArgs(9);

        // ── A10: UPS End-Use Subcategory ──────────────────────────────────────
        if (!lAlphaBlanks(10)) eq.UPSEndUseSubcategory = cAlphaArgs(10);

        // ── Register output variables ─────────────────────────────────────────
        SetupOutputVariable(state,
            "ITE Liquid Cooled CPU Electricity Rate",
            Constant::Units::W, eq.CPUPower,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled CPU Electricity Energy",
            Constant::Units::J, eq.CPUEnergy,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Sum,
            eq.Name,
            Constant::eResource::Electricity,
            OutputProcessor::Group::Building,
            OutputProcessor::EndUseCat::InteriorEquipment,
            eq.CPUEndUseSubcategory, eq.ZoneName);

        SetupOutputVariable(state,
            "ITE Liquid Cooled UPS Electricity Rate",
            Constant::Units::W, eq.UPSPower,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled UPS Electricity Energy",
            Constant::Units::J, eq.UPSEnergy,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Sum,
            eq.Name,
            Constant::eResource::Electricity,
            OutputProcessor::Group::Building,
            OutputProcessor::EndUseCat::InteriorEquipment,
            eq.UPSEndUseSubcategory, eq.ZoneName);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Heat Extraction Rate",
            Constant::Units::W, eq.LiquidHeatExtracted,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Heat Extraction Energy",
            Constant::Units::J, eq.LiquidHeatExtractedEnergy,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Sum,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Zone Sensible Heat Gain Rate",
            Constant::Units::W, eq.ZoneConvGainRate,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Zone Sensible Heat Gain Energy",
            Constant::Units::J, eq.ZoneConvGainEnergy,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Sum,
            eq.Name);

        // FWS loop outputs (primary, EnergyPlus PlantLoop side)
        SetupOutputVariable(state,
            "ITE Liquid Cooled Facility Water Supply Temperature",
            Constant::Units::C, eq.FWSSupplyTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Facility Water Return Temperature",
            Constant::Units::C, eq.FWSReturnTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Facility Water Mass Flow Rate",
            Constant::Units::kg_s, eq.FWSMassFlowRate,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Facility Water Volumetric Flow Rate",
            Constant::Units::m3_s, eq.FWSVolFlowRate,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        // Rack secondary loop outputs
        SetupOutputVariable(state,
            "ITE Liquid Cooled Rack Coolant Supply Temperature",
            Constant::Units::C, eq.RackSupplyTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Rack Coolant Return Temperature",
            Constant::Units::C, eq.RackReturnTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        // CDU heat exchanger outputs
        SetupOutputVariable(state,
            "ITE Liquid Cooled CDU Log Mean Temperature Difference",
            Constant::Units::deltaC, eq.CDU_LMTD,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled CDU Approach Temperature",
            Constant::Units::deltaC, eq.CDU_ApproachTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        // Chip junction temperature outputs
        SetupOutputVariable(state,
            "ITE Liquid Cooled Peak Chip Junction Temperature",
            Constant::Units::C, eq.JunctionTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Average,
            eq.Name);

        SetupOutputVariable(state,
            "ITE Liquid Cooled Time Above Maximum Junction Temperature",
            Constant::Units::hr, eq.TimeAboveMaxJunctionTemp,
            OutputProcessor::TimeStepType::Zone, OutputProcessor::StoreType::Sum,
            eq.Name);

        // Register convective zone gain so the heat-balance solver picks it up automatically
        SetupSpaceInternalGain(state,
            eq.SpaceNum, 1.0, eq.Name,
            DataHeatBalance::IntGainType::ElectricEquipmentITEAirCooled,
            &eq.ZoneConvGainRate);
    }

    // Size per-zone gain accumulator
    state.dataITELiquidCooled->zoneConvGainW.assign(state.dataGlobal->NumOfZones, 0.0);

    if (errorsFound) {
        ShowFatalError(state, format("{}: Aborting due to input errors.", routineName));
    }
}

// ============================================================================
//  PlantComponent interface
// ============================================================================

ITELiquidCooledEquipment *ITELiquidCooledEquipment::factory(EnergyPlusData &state,
                                                              const std::string &objectName)
{
    if (state.dataITELiquidCooled->getInputFlag) GetITELiquidCooledInput(state);
    for (auto &eq : state.dataITELiquidCooled->equipment) {
        if (Util::SameString(eq.Name, objectName)) return &eq;
    }
    ShowFatalError(state,
        format("ITELiquidCooledEquipment::factory: object \"{}\" not found.", objectName));
    return nullptr;
}

void ITELiquidCooledEquipment::onInitLoopEquip(EnergyPlusData &state,
                                                const PlantLocation &)
{
    init(state);
    size(state);
}

void ITELiquidCooledEquipment::getDesignCapacities([[maybe_unused]] EnergyPlusData &state,
                                                    const PlantLocation &,
                                                    Real64 &MaxLoad,
                                                    Real64 &MinLoad,
                                                    Real64 &OptLoad)
{
    // The FWS loop must handle Q_liq = IT_power × (1 − air_leakage)
    MaxLoad = DesignTotalPower * (1.0 - AirLeakageFraction);
    MinLoad = 0.0;
    OptLoad = MaxLoad;
}

void ITELiquidCooledEquipment::simulate(EnergyPlusData &state,
                                         const PlantLocation &,
                                         bool /*FirstHVACIteration*/,
                                         Real64 & /*CurLoad*/,
                                         bool /*RunFlag*/)
{
    init(state);
    calc(state);
    reportVars(state);
}

// ============================================================================
//  init — plant scan, begin-environment reset, per-timestep node read
// ============================================================================
void ITELiquidCooledEquipment::init(EnergyPlusData &state)
{
    static constexpr std::string_view routineName{"ITELiquidCooledEquipment::init"};

    // One-time: locate this component on the plant loop
    if (MyPlantScanFlag) {
        bool errFlag = false;
        PlantUtilities::ScanPlantLoopsForObject(
            state, Name,
            DataPlant::PlantEquipmentType::ITELiquidCooled,
            plantLoc, errFlag,
            /*LowLimitTemp=*/0.0, /*HighLimitTemp=*/100.0,
            /*TempDesCondIn=*/Node::SensedNodeFlagValue,
            CoolantInletNodeNum);
        if (errFlag) {
            ShowFatalError(state,
                format("{}: plant scan failed for \"{}\".", routineName, Name));
        }
        MyPlantScanFlag = false;
    }

    // One-time: initialise node max-flow
    if (MyOneTimeFlag && state.dataGlobal->BeginSimFlag) {
        PlantUtilities::InitComponentNodes(
            state, 0.0, FWSDesignMassFlowRate,
            CoolantInletNodeNum, CoolantOutletNodeNum);
        MyOneTimeFlag = false;
    }

    // Begin-environment: reset transient state
    if (MyEnvrnFlag && state.dataGlobal->BeginEnvrnFlag) {
        PlantUtilities::InitComponentNodes(
            state, 0.0, FWSDesignMassFlowRate,
            CoolantInletNodeNum, CoolantOutletNodeNum);
        FWSSupplyTemp  = FWSDesignSupplyTemp;
        FWSReturnTemp  = FWSDesignSupplyTemp;
        FWSMassFlowRate = 0.0;
        RackSupplyTemp  = RackDesignSupplyTemp;
        RackReturnTemp  = RackDesignSupplyTemp;
        TimeAboveMaxJunctionTemp = 0.0;
        MyEnvrnFlag = false;
    }
    if (!state.dataGlobal->BeginEnvrnFlag) MyEnvrnFlag = true;

    // Each timestep: read actual FWS supply temperature from plant node
    Real64 inletT = state.dataLoopNodes->Node(CoolantInletNodeNum).Temp;
    if (inletT > 0.0 && inletT < 100.0) FWSSupplyTemp = inletT;
}

// ============================================================================
//  size — compute design mass flow rates from volumetric flow + fluid density
// ============================================================================
void ITELiquidCooledEquipment::size(EnergyPlusData &state)
{
    if (!MySizeFlag) return;

    static constexpr std::string_view routineName{"ITELiquidCooledEquipment::size"};

    auto *glycol = plantLoc.loop->glycol;

    // ── FWS side (primary, EnergyPlus PlantLoop)
    // ṁ_fws = ρ_fws(T_fws_sup) × V̇_fws
    FWSCoolantDensity  = glycol->getDensity(state, FWSDesignSupplyTemp, routineName);
    FWSDesignMassFlowRate = FWSDesignVolFlowRate * FWSCoolantDensity;

    PlantUtilities::RegisterPlantCompDesignFlow(
        state, CoolantInletNodeNum, FWSDesignVolFlowRate);

    // ── Rack secondary side (assumed same fluid for simplicity)
    // ṁ_rack = ρ_rack(T_rack_sup) × V̇_rack
    Real64 rackDensity = glycol->getDensity(state, RackDesignSupplyTemp, routineName);
    RackDesignMassFlowRate = RackDesignVolFlowRate * rackDensity;

    MySizeFlag = false;
}

// ============================================================================
//  calc — core physics each timestep
// ============================================================================
void ITELiquidCooledEquipment::calc(EnergyPlusData &state)
{
    static constexpr std::string_view routineName{"ITELiquidCooledEquipment::calc"};

    auto *glycol = plantLoc.loop->glycol;

    // ── Step 1: IT load fraction ─────────────────────────────────────────────
    Real64 operFrac = (OperSched != nullptr)
        ? max(0.0, min(1.0, OperSched->getCurrentVal()))
        : 1.0;
    CPULoadFraction = (CPULoadSched != nullptr)
        ? max(0.0, min(1.0, CPULoadSched->getCurrentVal()))
        : 1.0;

    // ── Step 2: IT CPU power — curve f(cpu_load, T_rack_supply) ─────────────
    // The bivariate curve accounts for how power varies with both loading and
    // cold-plate supply temperature (warmer supply → slightly higher CPU power).
    Real64 curveFactor = (CPUPowerFuncLoadTempCurveNum > 0)
        ? max(0.0, Curve::CurveValue(
              state, CPUPowerFuncLoadTempCurveNum, CPULoadFraction, RackSupplyTemp))
        : CPULoadFraction; // linear default

    CPUPower = DesignTotalPower * operFrac * curveFactor;

    // ── Step 3: UPS losses ───────────────────────────────────────────────────
    Real64 upsInput = (UPSEfficiency > 0.0) ? (CPUPower / UPSEfficiency) : CPUPower;
    UPSPower    = upsInput - CPUPower;
    UPSLoss     = UPSPower * FractionUPSLossesToZone;
    TotalITPower = upsInput;

    // ── Step 4: Heat partition ────────────────────────────────────────────────
    LiquidHeatExtracted = CPUPower * (1.0 - AirLeakageFraction);
    ZoneConvGainRate    = CPUPower * AirLeakageFraction + UPSLoss;

    // ── Step 5: FWS (facility water) side  ───────────────────────────────────
    //   Q = ṁ_fws · cₚ_fws · (T_fws_ret − T_fws_sup)
    //     = ρ_fws · V̇_fws · cₚ_fws · ΔT_fws
    FWSCoolantCp      = glycol->getSpecificHeat(state, FWSSupplyTemp, routineName);
    FWSCoolantDensity = glycol->getDensity(state, FWSSupplyTemp, routineName);

    // Request design flow; PlantUtilities clamps to available loop flow
    Real64 reqFlow = FWSDesignMassFlowRate;
    PlantUtilities::SetComponentFlowRate(
        state, reqFlow, CoolantInletNodeNum, CoolantOutletNodeNum, plantLoc);
    FWSMassFlowRate = reqFlow;
    FWSVolFlowRate  = (FWSCoolantDensity > 0.0) ? FWSMassFlowRate / FWSCoolantDensity : 0.0;

    if (FWSMassFlowRate > HVAC::SmallMassFlow) {
        // T_fws_ret = T_fws_sup + Q / (ṁ_fws · cₚ_fws)
        FWSReturnTemp = FWSSupplyTemp +
                        LiquidHeatExtracted / (FWSMassFlowRate * FWSCoolantCp);
    } else {
        // No FWS flow: liquid cannot be extracted this step; all goes to zone
        FWSReturnTemp   = FWSSupplyTemp;
        ZoneConvGainRate += LiquidHeatExtracted;
        LiquidHeatExtracted = 0.0;
    }

    // ── Step 6: Rack secondary loop  ─────────────────────────────────────────
    //   Q = ṁ_rack · cₚ_rack · (T_rack_ret − T_rack_sup)
    //     = ρ_rack · V̇_rack · cₚ_rack · ΔT_rack
    Real64 rackCp      = glycol->getSpecificHeat(state, RackDesignSupplyTemp, routineName);
    Real64 rackDensity = glycol->getDensity(state, RackDesignSupplyTemp, routineName);
    RackMassFlowRate = RackDesignVolFlowRate * rackDensity; // fixed at design value

    Real64 C_rack = RackMassFlowRate * rackCp; // [W/K]
    Real64 C_fws  = FWSMassFlowRate  * FWSCoolantCp;

    // ── Step 7: External CDU heat exchanger — fixed approach temperature ──────
    //   T_rack_sup = T_fws_sup + ΔT_approach  (CDUApproachTemp from input)
    RackSupplyTemp = FWSSupplyTemp + CDUApproachTemp;

    // T_rack_ret = T_rack_sup + Q/(ṁ_rack·cₚ_rack)
    RackReturnTemp = (C_rack > 0.0)
        ? RackSupplyTemp + LiquidHeatExtracted / C_rack
        : RackSupplyTemp;

    // ── Step 8: LMTD (counterflow) — for output and design verification ──────
    //   ΔT₁ = T_rack_ret − T_fws_ret   (hot-inlet vs cold-outlet)
    //   ΔT₂ = T_rack_sup − T_fws_sup   (hot-outlet vs cold-inlet)
    CDU_ApproachTemp = RackSupplyTemp - FWSSupplyTemp;
    CDU_LMTD = calcLMTD(RackReturnTemp - FWSReturnTemp,
                         RackSupplyTemp - FWSSupplyTemp);

    // ── Step 9: Chip junction temperature ────────────────────────────────────
    //   T_junc = T_rack_sup + Q_per_unit · R_jc
    //   R_jc (K/W) is a per-unit resistance; dividing total Q by NumUnits gives
    //   the heat load for the single hottest parallel cooling path.
    //   For Watts/Area objects, set NumUnits to the number of racks represented.
    Real64 q_per_unit = LiquidHeatExtracted / static_cast<Real64>(std::max(NumUnits, 1));
    JunctionTemp = RackSupplyTemp + q_per_unit * ThermalResistJunctionToCoolant;

    if (JunctionTemp > MaxJunctionTemp) {
        TimeAboveMaxJunctionTemp += state.dataGlobal->TimeStepZone; // hours
    }

    // ── Step 10: Set FWS outlet node conditions for plant loop solver ─────────
    state.dataLoopNodes->Node(CoolantOutletNodeNum).Temp         = FWSReturnTemp;
    state.dataLoopNodes->Node(CoolantOutletNodeNum).MassFlowRate = FWSMassFlowRate;
}

// ============================================================================
//  reportVars — accumulate energy (W → J)
// ============================================================================
void ITELiquidCooledEquipment::reportVars(EnergyPlusData &state)
{
    const Real64 dt = state.dataGlobal->TimeStepZoneSec; // [s]
    CPUEnergy                 = CPUPower            * dt;
    UPSEnergy                 = UPSPower            * dt;
    LiquidHeatExtractedEnergy = LiquidHeatExtracted * dt;
    ZoneConvGainEnergy        = ZoneConvGainRate    * dt;
}

// ============================================================================
//  CDU heat exchanger helpers
// ============================================================================

// Log-mean temperature difference.
// ΔT_lm = (ΔT₁ − ΔT₂) / ln(ΔT₁/ΔT₂)
// Falls back to the arithmetic mean when ΔT₁ ≈ ΔT₂ (balanced flow limit).
Real64 ITELiquidCooledEquipment::calcLMTD(Real64 dT1, Real64 dT2) const
{
    // Both differences must be positive for a physically valid counterflow HX
    if (dT1 <= 0.0 || dT2 <= 0.0) return 0.0;
    if (std::abs(dT1 - dT2) < 1.0e-6 * max(dT1, dT2)) {
        return 0.5 * (dT1 + dT2); // arithmetic mean when dT1 ~= dT2
    }
    return (dT1 - dT2) / std::log(dT1 / dT2);
}

// ============================================================================
//  CalcZoneITELiquidCooledComponents
//  Accumulates air-side convective gains per zone.
//  Called from InternalHeatGains::CalcZoneITEq() each zone timestep.
// ============================================================================
void CalcZoneITELiquidCooledComponents(EnergyPlusData &state)
{
    auto &zoneGain = state.dataITELiquidCooled->zoneConvGainW;
    std::fill(zoneGain.begin(), zoneGain.end(), 0.0);

    for (auto &eq : state.dataITELiquidCooled->equipment) {
        if (eq.ZoneNum > 0 && eq.ZoneNum <= static_cast<int>(zoneGain.size())) {
            zoneGain[eq.ZoneNum - 1] += eq.ZoneConvGainRate; // 0-based index
        }
    }
}

// ============================================================================
//  SimITELiquidCooled
//  Entry point for PlantLoopEquipment demand-side dispatch.
// ============================================================================
void SimITELiquidCooled(EnergyPlusData &state,
                         const std::string &compName,
                         bool FirstHVACIteration,
                         Real64 &CurLoad,
                         bool RunFlag,
                         int &CompIndex,
                         const PlantLocation &calledFromLocation)
{
    if (state.dataITELiquidCooled->getInputFlag) GetITELiquidCooledInput(state);

    if (CompIndex == 0) {
        int found = Util::FindItemInList(
            compName, state.dataITELiquidCooled->equipment,
            &ITELiquidCooledEquipment::Name);
        if (found == 0) {
            ShowFatalError(state,
                format("SimITELiquidCooled: component \"{}\" not found.", compName));
        }
        CompIndex = found; // 1-based
    }

    state.dataITELiquidCooled->equipment[CompIndex - 1]
        .simulate(state, calledFromLocation, FirstHVACIteration, CurLoad, RunFlag);
}

} // namespace EnergyPlus::ITELiquidCooled

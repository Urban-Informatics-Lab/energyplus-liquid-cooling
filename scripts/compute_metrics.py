"""Annual PUE / WUE / EUE from EnergyPlus SQL output.

    python compute_metrics.py run1/eplusout.sql [run2/eplusout.sql ...]

Annual values come from the AnnualBuildingUtilityPerformanceSummary end-use
tables, which EnergyPlus always writes (Output:SQLite must be enabled).
Peak hourly PUE is reported only if hourly Electricity:Facility and
ITE-CPU / ITE-Fans meters were requested.

  E_IT       = ITE-CPU + ITE-Fans subcategories (IT load at UPS output)
  E_facility = total site electricity (includes UPS losses, lighting, cooling)
  PUE        = E_facility / E_IT
  WUE        = total site water [L] / E_IT [kWh]
  EUE        = E_compute / E_facility,  E_compute = ITE-CPU
  EUE_reuse  = (E_compute + Q_liq) / E_facility, if the variable
               "ITE Liquid Cooled Heat Extraction Energy" was reported
"""

import argparse
import sqlite3

GJ_TO_KWH = 1e9 / 3.6e6
J_TO_KWH = 1 / 3.6e6
SUMMARY = "AnnualBuildingUtilityPerformanceSummary"


def tab(con, table, row, col):
    r = con.execute(
        "SELECT Value FROM TabularDataWithStrings WHERE ReportName=? AND "
        "TableName=? AND RowName=? AND ColumnName=?",
        (SUMMARY, table, row, col)).fetchone()
    try:
        return float(r[0])
    except (TypeError, ValueError):
        return 0.0


def hourly(con, name):
    rows = con.execute(
        "SELECT rd.Value FROM ReportData rd "
        "JOIN ReportDataDictionary d ON d.ReportDataDictionaryIndex = rd.ReportDataDictionaryIndex "
        "JOIN Time t ON t.TimeIndex = rd.TimeIndex "
        "WHERE d.Name = ? AND d.ReportingFrequency = 'Hourly' "
        "AND (t.WarmupFlag = 0 OR t.WarmupFlag IS NULL) ORDER BY rd.TimeIndex",
        (name,)).fetchall()
    return [r[0] for r in rows]


def variable_sum(con, name):
    r = con.execute(
        "SELECT SUM(rd.Value) FROM ReportData rd "
        "JOIN ReportDataDictionary d ON d.ReportDataDictionaryIndex = rd.ReportDataDictionaryIndex "
        "JOIN Time t ON t.TimeIndex = rd.TimeIndex "
        "WHERE d.Name = ? AND (t.WarmupFlag = 0 OR t.WarmupFlag IS NULL) "
        "GROUP BY d.ReportDataDictionaryIndex", (name,)).fetchone()
    return r[0] if r else None


def metrics(sql):
    con = sqlite3.connect(sql)
    sub = lambda s: tab(con, "End Uses By Subcategory", f"Interior Equipment:{s}", "Electricity")
    cpu, fans, ups = sub("ITE-CPU"), sub("ITE-Fans"), sub("ITE-UPS")
    fac = tab(con, "End Uses", "Total End Uses", "Electricity")
    water_m3 = tab(con, "End Uses", "Total End Uses", "Water")
    it = cpu + fans
    if it <= 0:
        raise SystemExit(f"{sql}: no ITE-CPU / ITE-Fans end use found")

    m = {
        "Facility electricity [MWh]": fac * GJ_TO_KWH / 1e3,
        "IT electricity [MWh]": it * GJ_TO_KWH / 1e3,
        "UPS loss [MWh]": ups * GJ_TO_KWH / 1e3,
        "Site water [m3]": water_m3,
        "PUE (annual)": fac / it,
        "WUE [L/kWh]": water_m3 * 1e3 / (it * GJ_TO_KWH),
        "EUE": cpu / fac,
    }

    q_liq = variable_sum(con, "ITE Liquid Cooled Heat Extraction Energy")
    if q_liq is not None:
        m["EUE (100% reuse)"] = (cpu * GJ_TO_KWH + q_liq * J_TO_KWH) / (fac * GJ_TO_KWH)

    f = hourly(con, "Electricity:Facility")
    c = hourly(con, "ITE-CPU:InteriorEquipment:Electricity")
    if f and c:
        fn = hourly(con, "ITE-Fans:InteriorEquipment:Electricity") or [0.0] * len(c)
        m["PUE (peak hourly)"] = max(fi / (ci + ni) for fi, ci, ni in zip(f, c, fn) if ci + ni > 0)

    if ups == 0:
        print(f"warning: {sql}: no ITE-UPS end use; PUE excludes UPS losses")
    con.close()
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sql", nargs="+", help="EnergyPlus SQLite output file(s)")
    args = ap.parse_args()

    results = {s: metrics(s) for s in args.sql}
    keys = list(dict.fromkeys(k for m in results.values() for k in m))
    w = max(len(k) for k in keys)
    for s, m in results.items():
        print(f"\n{s}")
        for k in keys:
            v = m.get(k)
            fmt = ".4f" if k[:3] in ("PUE", "WUE", "EUE") else ",.1f"
            print(f"  {k:<{w}}  " + ("n/a" if v is None else format(v, fmt)))


if __name__ == "__main__":
    main()

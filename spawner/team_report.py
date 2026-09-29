#!/usr/bin/env python3
"""Summarize team-assembly snapshots from yspawn-teams.csv.

usage: python3 spawner/team_report.py PATH/TO/yspawn-teams.csv
"""
import csv
import sys
from collections import defaultdict


def report(path):
    teams = defaultdict(list)
    with open(path, newline="") as file:
        for row in csv.DictReader(file):
            if row["kind"] == "team":
                created = row.get("created_frame") or int(row["frame"]) - int(row["age"])
                key = (int(row["house"]), row["team_id"], int(created))
                teams[key].append(row)
    for (house, name, created), rows in sorted(teams.items()):
        first, last = rows[0], rows[-1]
        peak = max(rows, key=lambda row: int(row["present"]))
        launched = next((row["frame"] for row in rows if int(row["script_line"]) >= 0), "never observed")
        full = next((row["frame"] for row in rows
                     if int(row["present"]) >= int(row["wanted"])), "never observed")
        print(f"house {house} {name} created {created}: peak {peak['present']}/{peak['wanted']}"
              f" at frame {peak['frame']}; full {full}; script active {launched};"
              f" last seen {last['frame']}")
        print(f"  peak composition: {peak['composition']}")
    print(f"{len(teams)} large team instances observed")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    report(sys.argv[1])

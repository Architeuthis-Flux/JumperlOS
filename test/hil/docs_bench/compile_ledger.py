"""Compile the naive-agent reports (ledger/*.json) and the workflow's recheck verdicts into one markdown ledger."""
import json, os, glob, sys
SP = os.path.dirname(os.path.abspath(__file__))
LEDGER = f"{SP}/ledger"
items = {x["key"]: x for x in json.load(open(f"{SP}/items/items.json"))}
rechecks = {}
if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
    wf = json.load(open(sys.argv[1]))
    res = wf.get("result", wf)
    for rc in res.get("rechecks", []):
        rechecks.setdefault(rc["key"], []).append(rc)
out = ["# Bench verification ledger (5.7.11.0, " + os.popen("date '+%Y-%m-%d %H:%M'").read().strip() + ")\n"]
tot = {"literal": 0, "substitute": 0, "needs_hand": 0, "could_not_observe": 0}
matches = {"yes": 0, "no": 0, "partial": 0, "n/a": 0}
for key in items:
    f = f"{LEDGER}/{key}.json"
    it = items[key]
    if not os.path.exists(f):
        out.append(f"## {key} - {it['title']}\n\n_no report_\n"); continue
    try:
        r = json.load(open(f))
    except Exception as e:
        out.append(f"## {key} - {it['title']}\n\n_unreadable report: {e}_\n"); continue
    cats = {}
    for s in r.get("steps", []):
        cats[s.get("category")] = cats.get(s.get("category"), 0) + 1
        tot[s.get("category", "could_not_observe")] = tot.get(s.get("category", "could_not_observe"), 0) + 1
        matches[s.get("matches", "n/a")] = matches.get(s.get("matches", "n/a"), 0) + 1
    out.append(f"## {key} - {it['title']} ({it['page']})\n")
    out.append(f"steps: {cats}  \nsummary: {r.get('summary','')}\n")
    if r.get("needs_hand"):
        out.append("needs a hand: " + "; ".join(r["needs_hand"]) + "\n")
    for i, d in enumerate(r.get("discrepancies", [])):
        rc = (rechecks.get(key) or [None] * 99)[i] if key in rechecks and i < len(rechecks[key]) else None
        verdict = rc["verdict"]["verdict"] if rc and rc.get("verdict") else "unchecked"
        sug = rc["verdict"]["suggested_wording"] if rc and rc.get("verdict") else ""
        out.append(f"- **[{d['severity']}] {verdict}**: page says: {d['claim']}  \n  observed: {d['observed']}  \n  repro: {d['how_to_reproduce']}" + (f"  \n  suggested: {sug}" if sug else "") + "\n")
    shots = [s for s in r.get("screenshots", []) if s.endswith(".png")]
    if shots:
        out.append("screenshots: " + ", ".join(os.path.basename(s) for s in shots) + "\n")
out.insert(1, f"Totals - steps by category: {tot}; matches: {matches}\n")
open(f"{LEDGER}/LEDGER.md", "w").write("\n".join(out))
print("\n".join(out)[:6000])

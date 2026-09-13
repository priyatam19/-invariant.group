#!/usr/bin/env python3
"""Render an LPTA JSONL trace (see src/LPTAInstrumentation.cpp) into a single
self-contained HTML report: the pipeline's transformation history, grouped by
IR unit, with per-pass change magnitude and analysis-preservation flags.

Usage: lpta_report.py <trace.jsonl> [-o out.html]
"""
import argparse
import json
import sys
from collections import defaultdict, Counter


def load_records(path):
    records = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"warning: {path}:{lineno}: {e}", file=sys.stderr)
    return records


def report_group_key(r):
    """Grouping key for the report's per-unit sections.

    unit_name alone collides for "loop" and "function" kind events on the
    same function: LPTAInstrumentation.cpp's resolveUnit() sets unit_name to
    the enclosing function's bare name for both (see RESEARCH.md), so a Loop
    pass event and a Function pass event on `foo` both report unit_name=foo.
    module/scc/unknown already self-disambiguate (their unit_name is wrapped
    as "<module:...>"/"<scc:...>"), so only loop needs a suffix here.
    """
    name = r.get("unit_name", "?")
    if r.get("unit_kind") == "loop":
        return f"{name} [loop]"
    return name


def build_report(records, source_path):
    by_unit = defaultdict(list)
    for r in records:
        by_unit[report_group_key(r)].append(r)
    for unit in by_unit.values():
        unit.sort(key=lambda r: r.get("seq", 0))

    total = len(records)
    changed = sum(1 for r in records if r.get("ir_changed"))
    candidates = [r for r in records if r.get("incremental_update_candidate")]
    pass_counter = Counter(r.get("pass", "?") for r in records if r.get("ir_changed"))

    summary = {
        "total_events": total,
        "units": len(by_unit),
        "ir_changed_events": changed,
        "incremental_update_candidates": len(candidates),
        "top_changing_passes": pass_counter.most_common(10),
    }

    payload = {
        "source": source_path,
        "summary": summary,
        "units": {name: rows for name, rows in by_unit.items()},
    }
    return payload


PAGE_TEMPLATE = """<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>LPTA Transformation Report</title>
<style>
  :root {
    color-scheme: light dark;
    --bg: #ffffff; --fg: #1a1a1a; --muted: #666; --border: #ddd;
    --row-hover: #f4f6f8; --accent: #b03030; --accent-bg: #fdecec;
    --chip-bg: #eef1f4; --good: #2a7a2a;
  }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#1e1f22; --fg:#e8e8e8; --muted:#9aa0a6; --border:#3a3b3e;
             --row-hover:#2a2b2e; --accent:#ff6b6b; --accent-bg:#3a1f1f;
             --chip-bg:#2c2d30; --good:#6bcf6b; }
  }
  * { box-sizing: border-box; }
  body { background: var(--bg); color: var(--fg); font-family: -apple-system, Segoe UI, Roboto, sans-serif;
         margin: 0; padding: 24px 16px; font-size: 14px; }
  h1 { font-size: 1.3em; margin: 0 0 4px; }
  .subtitle { color: var(--muted); margin-bottom: 20px; font-size: 0.9em; }
  .summary { display: flex; gap: 12px; flex-wrap: wrap; margin-bottom: 20px; }
  .stat { background: var(--chip-bg); border-radius: 8px; padding: 10px 16px; min-width: 120px; }
  .stat .n { font-size: 1.4em; font-weight: 600; }
  .stat .l { color: var(--muted); font-size: 0.8em; }
  input[type=text] { width: 100%; max-width: 420px; padding: 8px 10px; border-radius: 6px;
    border: 1px solid var(--border); background: var(--bg); color: var(--fg); margin-bottom: 16px; }
  details { border: 1px solid var(--border); border-radius: 8px; margin-bottom: 10px; overflow: hidden; }
  summary { cursor: pointer; padding: 10px 14px; font-weight: 600; display: flex;
    align-items: center; gap: 10px; }
  summary .badge { font-weight: 400; font-size: 0.8em; color: var(--muted); }
  summary .cand { color: var(--accent); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 12.5px; }
  th, td { text-align: left; padding: 5px 8px; border-bottom: 1px solid var(--border);
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 260px; }
  th { color: var(--muted); font-weight: 600; position: sticky; top: 0; background: var(--bg); }
  tr:hover td { background: var(--row-hover); }
  tr.candidate td { background: var(--accent-bg); }
  tr.nochange td { color: var(--muted); }
  .yes { color: var(--good); }
  .no { color: var(--accent); }
  code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  .hidden { display: none !important; }
</style>
</head>
<body>
<h1>LPTA Transformation Report</h1>
<div class="subtitle" id="subtitle"></div>
<div class="summary" id="summary"></div>
<input type="text" id="filter" placeholder="Filter by unit or pass name...">
<div id="units"></div>

<script>
const DATA = __DATA_JSON__;

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function renderSummary() {
  document.getElementById('subtitle').textContent = 'Source: ' + DATA.source;
  const s = DATA.summary;
  const stats = [
    [s.total_events, 'pass invocations'],
    [s.units, 'IR units traced'],
    [s.ir_changed_events, 'events that changed IR'],
    [s.incremental_update_candidates, 'incremental-update candidates'],
  ];
  const box = document.getElementById('summary');
  for (const [n, l] of stats) {
    const st = el('div', 'stat');
    st.appendChild(el('div', 'n', String(n)));
    st.appendChild(el('div', 'l', l));
    box.appendChild(st);
  }
}

function fmtBool(v, goodWhenTrue = true) {
  if (v === undefined || v === null) return '';
  const isGood = goodWhenTrue ? v : !v;
  const span = el('span', isGood ? 'yes' : 'no', v ? 'yes' : 'no');
  return span;
}

function renderUnits() {
  const container = document.getElementById('units');
  const names = Object.keys(DATA.units).sort((a, b) => {
    const av = DATA.units[a].some(r => r.incremental_update_candidate) ? 0 : 1;
    const bv = DATA.units[b].some(r => r.incremental_update_candidate) ? 0 : 1;
    if (av !== bv) return av - bv;
    return a.localeCompare(b);
  });
  for (const name of names) {
    const rows = DATA.units[name];
    const changedCount = rows.filter(r => r.ir_changed).length;
    const candCount = rows.filter(r => r.incremental_update_candidate).length;

    const d = document.createElement('details');
    d.dataset.searchKey = (name + ' ' + rows.map(r => r.pass).join(' ')).toLowerCase();
    if (candCount > 0) d.open = false;

    const sum = el('summary');
    sum.appendChild(el('span', null, name));
    sum.appendChild(el('span', 'badge', `${rows.length} passes, ${changedCount} changed`));
    if (candCount > 0) sum.appendChild(el('span', 'cand', `⚠ ${candCount} incremental-update candidate(s)`));
    d.appendChild(sum);

    const table = el('table');
    const thead = el('thead');
    const hr = el('tr');
    ['#', 'Pass', 'Changed', 'BBs', 'Instrs', '+/-lines', 'CFG Δ', 'DT preserved', 'All preserved', 'Flag'].forEach(h => hr.appendChild(el('th', null, h)));
    thead.appendChild(hr);
    table.appendChild(thead);
    const tbody = el('tbody');
    for (const r of rows) {
      const tr = el('tr', r.incremental_update_candidate ? 'candidate' : (r.ir_changed ? '' : 'nochange'));
      tr.appendChild(el('td', null, String(r.seq)));
      tr.appendChild(el('td', null, r.pass));
      const changedTd = el('td'); changedTd.appendChild(fmtBool(!!r.ir_changed, false)); tr.appendChild(changedTd);
      tr.appendChild(el('td', null, r.invalidated ? '(invalidated)' : `${r.before_bb_count ?? '-'} → ${r.after_bb_count ?? '-'}`));
      tr.appendChild(el('td', null, r.invalidated ? '' : `${r.before_instr_count ?? '-'} → ${r.after_instr_count ?? '-'}`));
      tr.appendChild(el('td', null, r.invalidated ? '' : `+${r.lines_added ?? 0}/-${r.lines_removed ?? 0}`));
      const cfgTd = el('td'); if (r.cfg_changed !== undefined) cfgTd.appendChild(fmtBool(!!r.cfg_changed, false)); tr.appendChild(cfgTd);
      const dtTd = el('td'); if (r.dt_preserved !== undefined) dtTd.appendChild(fmtBool(!!r.dt_preserved, true)); tr.appendChild(dtTd);
      const allTd = el('td'); if (r.all_preserved !== undefined) allTd.appendChild(fmtBool(!!r.all_preserved, true)); tr.appendChild(allTd);
      tr.appendChild(el('td', null, r.incremental_update_candidate ? '⚠ candidate' : ''));
      tbody.appendChild(tr);
    }
    table.appendChild(tbody);
    d.appendChild(table);
    container.appendChild(d);
  }
}

document.getElementById('filter').addEventListener('input', (e) => {
  const q = e.target.value.trim().toLowerCase();
  for (const d of document.querySelectorAll('#units > details')) {
    d.classList.toggle('hidden', q.length > 0 && !d.dataset.searchKey.includes(q));
    if (q.length > 0 && !d.classList.contains('hidden')) d.open = true;
  }
});

renderSummary();
renderUnits();
</script>
</body>
</html>
"""


def render_html(payload):
    # payload can contain arbitrary strings from the trace (function/module
    # names, file paths) that end up inside a <script> block. json.dumps
    # alone doesn't know it's embedded in HTML, so a name containing the
    # literal substring "</script>" would close the tag early and inject
    # markup. Escaping '<', '>', '&' as \u-escapes (valid inside a JSON
    # string/JS literal) neutralizes that without touching JSON validity.
    data = json.dumps(payload)
    data = data.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    return PAGE_TEMPLATE.replace("__DATA_JSON__", data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace", help="path to JSONL trace produced by LPTAInstrumentation")
    ap.add_argument("-o", "--output", default="lpta_report.html", help="output HTML path")
    args = ap.parse_args()

    records = load_records(args.trace)
    if not records:
        print(f"no records found in {args.trace}", file=sys.stderr)
        sys.exit(1)

    payload = build_report(records, args.trace)
    with open(args.output, "w") as f:
        f.write(render_html(payload))

    s = payload["summary"]
    print(f"{s['total_events']} events across {s['units']} IR units "
          f"({s['ir_changed_events']} changed IR, "
          f"{s['incremental_update_candidates']} incremental-update candidates)")
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""test262 设备端结果分析器（主机侧）。

读取 t262_gen.py 的 map.tsv 与 t262_run.sh 产生的 results_*.out，输出：
  - results.json ：每条命令的判定（pass/fail/skip/crash + 输出摘要）
  - summary.txt  ：分级记分卡（总通过率、按目录/特性/模式/错误类型归档、失败样例）
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from collections import Counter, defaultdict
from typing import Any, Dict, List, Tuple

TOP_DIRS = ("test/language", "test/built-ins", "test/intl402", "test/staging",
            "test/annexB", "test/harness")


def top_dir(rel: str) -> str:
    for d in TOP_DIRS:
        if rel.startswith(d):
            return d
    return rel.split("/")[0] + "/" + (rel.split("/")[1] if "/" in rel else "")


def error_kind(out: str, rc: int) -> str:
    m = re.search(r"Exception: ([A-Za-z0-9_]*Error)", out)
    if m:
        return m.group(1)
    if "Test262Error" in out:
        return "Test262Error"
    if re.search(r"^Exception: ([\w\d]+):", out):
        return re.search(r"^Exception: ([\w\d]+):", out).group(1)
    # jsc 的退出码 3 = EXIT_EXCEPTION（脚本抛异常），不是信号；真实信号会被 128+n 编码，
    # 故用 rc & 0x7F 判定时必须排除 3。
    sig = rc & 0x7F
    if sig and sig != 3:
        return f"signal {sig} (crash)"
    if rc == 3:
        return "exception (no message)"
    if rc == 1:
        return "cannot run / timeout"
    return "other"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True, help="含 map.tsv / commands.txt 的目录")
    ap.add_argument("--results", required=True, help="results_*.out 所在目录（或单个文件）")
    ap.add_argument("--out", default=None, help="输出目录（默认同 --work）")
    ap.add_argument("--samples", type=int, default=40, help="列出多少个失败样例")
    args = ap.parse_args()

    out_dir = args.out or args.work
    rows: Dict[int, Tuple[str, str, List[str]]] = {}
    with open(os.path.join(args.work, "map.tsv"), "r", encoding="utf-8") as f:
        next(f)
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            idx = int(parts[0])
            feats = parts[3].split(",") if len(parts) > 3 and parts[3] else []
            rows[idx] = (parts[1], parts[2], feats)

    results_files = ([args.results] if os.path.isfile(args.results)
                     else sorted(glob.glob(os.path.join(args.results, "results_*.out"))))
    res: Dict[int, Tuple[int, str]] = {}
    for path in results_files:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                parts = line.split("|", 2)
                if len(parts) < 3 or not parts[0].isdigit():
                    continue
                res[int(parts[0])] = (int(parts[1]), parts[2])

    judged: List[Dict[str, Any]] = []
    for idx, (mode, test, feats) in sorted(rows.items()):
        if idx not in res:
            judged.append({"idx": idx, "mode": mode, "test": test, "features": feats,
                           "status": "not_run", "rc": None, "output": None, "kind": None})
            continue
        rc, text = res[idx]
        passed = (rc == 0 and text.strip() == "")
        sig = rc & 0x7F
        crash = sig != 0 and sig != 3
        judged.append({
            "idx": idx, "mode": mode, "test": test, "features": feats,
            "status": "pass" if passed else "fail",
            "rc": rc, "output": None if passed else text, "crash": crash,
            "kind": None if passed else error_kind(text, rc),
        })

    n_pass = sum(1 for j in judged if j["status"] == "pass")
    n_fail = sum(1 for j in judged if j["status"] == "fail")
    n_notrun = sum(1 for j in judged if j["status"] == "not_run")
    n_skip = 0
    skip_path = os.path.join(args.work, "skipped.tsv")
    if os.path.exists(skip_path):
        with open(skip_path, "r", encoding="utf-8") as f:
            n_skip = sum(1 for line in f if line.strip())
    denom = n_pass + n_fail
    rate = (n_pass / denom * 100) if denom else 0.0

    by_dir: Dict[str, Counter] = defaultdict(Counter)
    by_feat: Counter = Counter()
    by_mode: Dict[str, Counter] = defaultdict(Counter)
    by_kind: Counter = Counter()
    for j in judged:
        d = top_dir(j["test"])
        by_dir[d][j["status"]] += 1
        by_mode[j["mode"]][j["status"]] += 1
        if j["status"] == "fail":
            by_kind[j["kind"] or "unknown"] += 1
            for feat in j["features"]:
                by_feat[feat] += 1

    with open(os.path.join(out_dir, "results.json"), "w", encoding="utf-8") as f:
        json.dump(judged, f, ensure_ascii=False)

    lines: List[str] = []
    lines.append(f"test262 · Android 移植版 · 通过 {n_pass}/{denom} = {rate:.2f}%"
                 f"（失败 {n_fail}，跳过 {n_skip}，未跑 {n_notrun}）")
    lines.append("")
    lines.append("按顶层目录：")
    for d in sorted(by_dir, key=lambda x: -by_dir[x]["fail"]):
        c = by_dir[d]
        tot = c["pass"] + c["fail"]
        r = (c["pass"] / tot * 100) if tot else 0.0
        lines.append(f"  {d:<18} 通过 {c['pass']:>6}/{tot:<6} = {r:6.2f}%   失败 {c['fail']}")
    lines.append("")
    lines.append("按模式：")
    for m in sorted(by_mode):
        c = by_mode[m]
        tot = c["pass"] + c["fail"]
        r = (c["pass"] / tot * 100) if tot else 0.0
        lines.append(f"  {m:<14} 通过 {c['pass']:>6}/{tot:<6} = {r:6.2f}%   失败 {c['fail']}")
    lines.append("")
    lines.append("失败类型 Top：")
    for k, v in by_kind.most_common(12):
        lines.append(f"  {k:<34} {v}")
    lines.append("")
    lines.append("失败最多的特性 Top：")
    for k, v in by_feat.most_common(12):
        lines.append(f"  {k:<34} {v}")
    lines.append("")
    lines.append(f"失败样例（前 {args.samples} 条）：")
    shown = 0
    for j in judged:
        if j["status"] != "fail" or shown >= args.samples:
            continue
        shown += 1
        out = (j["output"] or "")[:120]
        lines.append(f"  [{j['mode']}] {j['test']}\n        rc={j['rc']} {out}")

    text = "\n".join(lines)
    with open(os.path.join(out_dir, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

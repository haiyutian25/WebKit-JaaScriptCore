#!/usr/bin/env python3
"""比较多份 test262 results.json 的失败集合 —— 用于 L2「JIT 配置矩阵语义一致」。

用法: t262_compare.py 基准.json 对比.json [更多.json ...]
输出: 各配置的通过率，以及相对于基准的「新失败 / 新通过」清单与计数。
"""
from __future__ import annotations

import json
import sys
from typing import Any, Dict, List


def load(path: str) -> Dict[str, str]:
    with open(path, "r", encoding="utf-8") as f:
        data: List[Dict[str, Any]] = json.load(f)
    return {f"{r['test']}|{r['mode']}": r["status"] for r in data}


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    base_path, others = sys.argv[1], sys.argv[2:]
    base = load(base_path)
    print(f"基准 {base_path}: {sum(1 for v in base.values() if v == 'pass')} pass / {len(base)} 项")
    for path in others:
        cur = load(path)
        new_fail = sorted(k for k, v in cur.items() if v == "fail" and base.get(k) == "pass")
        new_pass = sorted(k for k, v in cur.items() if v == "pass" and base.get(k) == "fail")
        n_pass = sum(1 for v in cur.values() if v == "pass")
        print(f"\n{path}: {n_pass} pass / {len(cur)} 项")
        print(f"  相对基准新失败 {len(new_fail)}，新通过 {len(new_pass)}")
        for k in new_fail[:20]:
            print(f"    - FAIL {k}")
        for k in new_pass[:20]:
            print(f"    + PASS {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

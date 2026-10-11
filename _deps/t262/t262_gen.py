#!/usr/bin/env python3
"""test262 → Android 设备批量执行：清单生成器（主机侧）。

运行语义与 WebKit 的 Tools/Scripts/test262/runner.py 完全对齐：

    jsc [feature-flags] [--test262-async] [--exception=<type>]
        harness/sta.js harness/assert.js [harness/doneprintHandle.js]
        [includes/<hash>.js]
        <--strict-file=T | --module-file=T | T>

判定：退出码 0 且无输出 = 通过（负面用例由 --exception=<type> 让 jsc 自己判定期望异常）。

产出（均写入 --out 目录）：
  commands.txt   设备端逐条执行的命令行（同一步长切分用行号索引）
  map.tsv        idx → mode / 测试路径 / features（供分析器归档）
  skipped.tsv    idx → 跳过原因
  includes/*.js  去重后的 includes 拼接文件
  gen.json       本次生成的元信息（test262 版本、统计）
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from typing import Any, Dict, List, Optional

import yaml

METADATA_RE = re.compile(r"/\*(---[\r\n]+[\S\s]*)[\r\n]+\s*---\*/", re.MULTILINE)
TEST_FILE_RE = re.compile(r"(?<!_FIXTURE)\.[jJ][sS]$")
DEFAULT_HARNESS = ["sta.js", "assert.js"]
ASYNC_HARNESS = ["doneprintHandle.js"]

# 上游 runner.py 用 webkitpy 的平台信息决定 config.yaml 的 conditions 是否生效。
# 我们跑在 Android 上：build=release，os=Android —— 因此只匹配 os 的跳过条件不生效
# （与 Linux/macOS 上相比跳过更少，即对我们更严格）。
PLATFORM = {"os": "Android", "os_version": "", "build": "release", "sanitizer": ""}


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_metadata(contents: str) -> Dict[str, Any]:
    m = METADATA_RE.search(contents)
    if not m:
        return {}
    try:
        return yaml.safe_load(m.group(1)) or {}
    except yaml.YAMLError:
        return {}


def get_scenarios(flags: List[str]) -> List[str]:
    """镜像上游 _get_scenarios。"""
    if not flags:
        return ["strict mode", "default"]
    if "raw" in flags:
        return ["raw"]
    if "noStrict" in flags:
        return ["default"]
    if "onlyStrict" in flags:
        return ["strict mode"]
    if "module" in flags:
        return ["module"]
    return ["strict mode", "default"]


def condition_matches(cond: Dict[str, Any]) -> bool:
    for key, value in (cond or {}).items():
        if key == "os":
            if PLATFORM["os"].lower() != str(value).lower():
                return False
        elif key == "os_version":
            return False  # Android 侧不比较版本，视为不匹配
        elif key == "build":
            if PLATFORM["build"] != str(value).lower():
                return False
        elif key == "sanitizer":
            if PLATFORM["sanitizer"].lower() != str(value).lower():
                return False
    return True


def should_skip(rel: str, meta: Dict[str, Any], config: Dict[str, Any]) -> Optional[str]:
    skip = (config or {}).get("skip") or {}
    if rel in set(skip.get("files") or []):
        return "file"
    for pattern in skip.get("paths") or []:
        if re.search(pattern, rel):
            return f"path:{pattern}"
    feats = meta.get("features") or []
    for feat in feats:
        if feat in set(skip.get("features") or []):
            return f"feature:{feat}"
    for entry in skip.get("conditions") or []:
        if not condition_matches(entry.get("if") or {}):
            continue
        if rel in set(entry.get("files") or []):
            return "cond:file"
        for pattern in entry.get("paths") or []:
            if re.search(pattern, rel):
                return f"cond:path:{pattern}"
        for feat in feats:
            if feat in set(entry.get("features") or []):
                return f"cond:feature:{feat}"
    return None


def shq(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--test262", required=True, help="test262 检出目录")
    ap.add_argument("--config", required=True, help="上游 JSTests/test262/config.yaml")
    ap.add_argument("--out", required=True, help="输出目录")
    ap.add_argument("--only", default=None, help="只生成匹配该正则（相对 test/ 的路径）的用例")
    ap.add_argument("--limit", type=int, default=0, help="最多生成多少个测试文件（0=全部）")
    ap.add_argument("--sample", type=int, default=0, help="等距抽样：每 N 个测试文件取 1 个（0=不抽样）")
    ap.add_argument("--envprefix", default="", help="附加在 ./jsc 前的环境变量，如 'JSC_useJIT=0'")
    args = ap.parse_args()

    t262 = os.path.abspath(args.test262)
    harness_dir = os.path.join(t262, "harness")
    config = yaml.safe_load(read_text(args.config)) or {}
    flag_map = config.get("flags") or {}

    out = os.path.abspath(args.out)
    inc_dir = os.path.join(out, "includes")
    os.makedirs(inc_dir, exist_ok=True)

    test_root = os.path.join(t262, "test")
    tests: List[str] = []
    for dirpath, dirnames, filenames in os.walk(test_root):
        dirnames.sort()
        for name in sorted(filenames):
            if not TEST_FILE_RE.search(name):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, t262).replace(os.sep, "/")
            if args.only and not re.search(args.only, rel):
                continue
            tests.append(rel)
    if args.sample and args.sample > 1:
        tests = tests[:: args.sample]
    if args.limit:
        tests = tests[: args.limit]

    inc_cache: Dict[str, str] = {}
    commands: List[str] = []
    rows: List[str] = []
    skipped: List[str] = []
    stats = {"files": 0, "commands": 0, "skipped": 0, "features": {}, "modes": {}}

    for rel in tests:
        full = os.path.join(t262, rel)
        contents = read_text(full)
        meta = parse_metadata(contents)
        stats["files"] += 1

        reason = should_skip(rel, meta, config)
        if reason:
            stats["skipped"] += 1
            skipped.append(f"{rel}\t{reason}")
            continue

        features = meta.get("features") or []
        for feat in features:
            stats["features"][feat] = stats["features"].get(feat, 0) + 1

        # 特性 → jsc 开关（上游 config.yaml 的 flags 映射）
        feat_flags = [f"--{flag_map[f]}=1" for f in features if f in flag_map]
        test_flags = meta.get("flags") or []
        if "CanBlockIsFalse" in test_flags:
            feat_flags.append("--can-block-is-false")

        negative = meta.get("negative") or {}
        if negative.get("type"):
            feat_flags.append(f"--exception={negative['type']}")
        if "async" in test_flags:
            feat_flags.append("--test262-async")

        includes = meta.get("includes") or []
        inc_rel = "-"
        if includes:
            chunks = []
            for name in includes:
                path = os.path.join(harness_dir, name)
                if os.path.exists(path):
                    chunks.append(read_text(path))
            if chunks:
                blob = "".join(chunks)
                digest = hashlib.sha1(blob.encode("utf-8")).hexdigest()[:16]
                if digest not in inc_cache:
                    with open(os.path.join(inc_dir, digest + ".js"), "w",
                              encoding="utf-8", newline="\n") as f:
                        f.write(blob)
                    inc_cache[digest] = digest + ".js"
                inc_rel = inc_cache[digest]

        harness_kind = "none" if "raw" in test_flags else "default"
        if harness_kind == "default" and "async" in test_flags:
            harness_kind = "default+async"

        for scenario in get_scenarios(test_flags):
            argv: List[str] = []
            if args.envprefix:
                argv.append(args.envprefix)
            argv.append("./jsc")
            argv.extend(feat_flags)
            if harness_kind == "default":
                argv.extend("harness/" + h for h in DEFAULT_HARNESS)
            elif harness_kind == "default+async":
                argv.extend("harness/" + h for h in DEFAULT_HARNESS + ASYNC_HARNESS)
            if inc_rel != "-":
                argv.append("includes/" + inc_rel)
            if scenario == "module":
                argv.append(f"--module-file={rel}")
            elif scenario == "strict mode":
                argv.append(f"--strict-file={rel}")
            else:
                argv.append(rel)

            line = " ".join(a if a.startswith("-") or "=" in a.split("/")[0] else shq(a)
                            for a in argv)
            commands.append(line)
            idx = len(commands)
            rows.append(
                "\t".join([str(idx), scenario.replace(" ", "_"), rel, ",".join(features)])
            )
            stats["commands"] += 1
            stats["modes"][scenario] = stats["modes"].get(scenario, 0) + 1

    with open(os.path.join(out, "commands.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(commands) + ("\n" if commands else ""))
    with open(os.path.join(out, "map.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("idx\tmode\ttest\tfeatures\n")
        f.write("\n".join(rows) + ("\n" if rows else ""))
    with open(os.path.join(out, "skipped.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(skipped) + ("\n" if skipped else ""))

    try:
        rev = subprocess.run(["git", "-C", t262, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:
        rev = ""
    stats["test262_revision"] = rev
    stats["sample"] = args.sample
    stats["limit"] = args.limit
    stats["only"] = args.only
    stats["envprefix"] = args.envprefix
    stats["unique_includes"] = len(inc_cache)
    with open(os.path.join(out, "gen.json"), "w", encoding="utf-8") as f:
        json.dump(stats, f, indent=2, ensure_ascii=False)

    print(f"测试文件 {stats['files']}，生成执行项 {stats['commands']}，跳过 {stats['skipped']}，"
          f"includes 去重 {len(inc_cache)} 个，test262 版本 {rev[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

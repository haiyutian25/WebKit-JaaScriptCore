Android JSC 移植版完整测试方案
按「四层金字塔」组织：语义正确性 → 压力下的 JIT/GC 正确性 → 性能基准 → 宿主架构对接。你现在手里已经过了最关键的 JIT 曲线这关（A 曲线 32.6× 加速），剩下的是把正确性补严、把性能数据立成基线、把接口锁定住。下面按可直接执行的顺序展开。
分层总览
层	回答的问题	主要工具	目标阈值
L1 语义正确性	引擎算得对不对	test262 + JSTests/stress	test262 通过率 ≥ 参考值 −1%
L2 压力正确性	JIT/GC 在极端配置下会不会算错	JSC Options 组合 + run-jsc-stress-tests	全配置语义一致
L3 性能基准	快不快、稳不稳	JetStream2/SunSpider + 自建热循环 + 内存 profiling	见下分项
L4 宿主对接	能不能承载 TS 插件架构	自建 API 契约测试	全部通过 + 基线锁定
L1 语义正确性
test262：ES 语言规格的完整回归
test262 是 TC39 官方 ECMAScript 一致性套件，5 万+ 独立测试文件覆盖 ECMA-262、ECMA-402（国际化）、ECMA-404（JSON）。这是唯一能系统性回答"你的移植版到底丢了什么特性"的工具。
执行方式：test262 官方支持多种 runner，Node.js 平台用 test262-harness，Python 平台用 test262-harness-py，还有专门的 C++ runner 可以直接对接你的 JSC shell。JSC 生态最直接的方法是 WebKit 源码自带的 runner，但你要在 Android 上跑，得先用 adb 把 jsc 二进制和 ICU 数据推到设备上，然后本地用 test262-harness 以 --host-type=jsc --host-path=<adb 包装脚本> 的方式调用。
# 本地包装脚本 jsc-adb.sh
#!/bin/bash
cat > /tmp/t.js
adb push /tmp/t.js /data/local/tmp/t.js
adb shell "cd /data/local/tmp && ./jsc t.js"
# 实际跑 test262
git clone https://github.com/tc39/test262 --depth 1
cd test262
test262-harness --host-type=jsc --host-path=./jsc-adb.sh \
  --host-args="-s" \
  --threads=8 \
  --reporter=json \
  'test/language/**/*.js' \
  'test/built-ins/**/*.js'
判分标准（对照 2026 年 9 月的参考值）：
引擎	test262 通过率
QuickJS（ES2025 特性集）	近 100%
LibJS（SerenityOS）	97.95%
你的 JSC 移植版	目标 ≥ 97%
关键不是绝对数字，而是分类看失败：
• test/language/（词法、语法、语义）失败 → 移植版基础有问题，严重
• test/built-ins/Intl/ 失败 → 常见，ICU 数据没打进 APK，中等
• test/built-ins/Atomics/、SharedArrayBuffer 失败 → Android 底层原子指令问题，需要查 NDK 配置
• test/annexB/ 失败 → 无所谓，可选规范
JSTests/stress：WebKit 自家的语义回归
WebKit 源码里的 JSTests/stress 是官方日常回归的主要集合，官方原话："对于日常开发，JSTests/stress 和 JSTests/wasm.yaml 集合就足以捕捉绝大多数 bug"。这些测试比 test262 更贴近 JSC 内部实现，能发现 test262 覆盖不到的 JSC 特有语义。
# 在 macOS/Linux 构建机上跑（不用真的在 Android 上，但要用同一个 jsc 源码版本）
Tools/Scripts/run-javascriptcore-tests --root=<your-build-dir>
Tools/Scripts/run-jsc-stress-tests JSTests/stress --filter <your-focus>
关键做法：把这套测试跑在你移植所基于的那个 JSC 版本上，把它作为基线，然后把同样的测试在 Android 上跑一遍，对比失败集——只在 Android 上失败的测试，才是移植引入的问题。
L2 压力正确性
JIT 是概率性 bug 的高发区，单次跑通过不代表正确。官方推荐的做法是在同一份测试上跑多种 JIT 配置，语义必须完全一致。
JIT 分层组合矩阵
从 WebKit 的官方调试文档中提取的分层配置：
配置	打开的层	判定
JSC_useJIT=false	仅 LLInt	基准语义
JSC_useJIT=true JSC_useDFGJIT=false	LLInt + Baseline	语义必须与基准一致
JSC_useJIT=true JSC_useFTLJIT=false	LLInt + Baseline + DFG	语义必须与基准一致
全开	LLInt + Baseline + DFG + FTL	语义必须与基准一致
对每个配置，跑完整 test262（或至少跑一个具有代表性的子集），所有配置的通过/失败集合必须完全相同。任何差异都指向该层的 codegen bug。
WebKit 官方推荐的极端配置
JSTests 官方跑法会自动在以下配置下各跑一遍：
• 关闭并发 JIT（--jit-llint / --dfg-eager 变体）
• 降低 tier-up 阈值（ftl-eager-no-cjit）
• 连续 GC（--gc-at-end / 每次分配后强制 GC）
# 示例：降低阈值让 FTL 立刻介入，再配合 no-cjit
JSC_useConcurrentJIT=false JSC_ thresholdForFTLOptimizeAfterWarmUp=0 ./jsc hot.js
JSC_useConcurrentJIT=false JSC_thresholdForOptimizeAfterWarmUp=0 ./jsc hot.js
这类组合能把 JIT 的竞态、失效缓存等问题在压测下暴露出来。
GC 压力测试
// 循环创建大对象，配合 --gc-at-end 或连续 GC 配置
function churn() {
  let arr = [];
  for (let i = 0; i < 1e6; i++) {
    arr.push({ x: i, s: `str-${i}` });
    if (arr.length > 10000) arr = [];
  }
}
判定：
• 语义一致性：GC 触发前后所有对象引用正确
• 无崩溃：长跑 10 分钟无 segfault
• 无泄漏：RSS 在 GC 后应回落，不能持续单调上升
L3 性能基准
分项基准矩阵
指标	工具	Android 目标值	备注
启动时间	自测（进程到首次执行 JS）	<150ms	直接影响 CLI 体验
热循环稳态	你已跑的 hot(n) 曲线	14-15 ns/iter（A）	已验证 ✓
JetStream2	JSC shell + JetStream2 套件	相对 iOS 参考值	综合性能
SunSpider	经典套件，启动快，适合移动	相对参考值	老但轻量，适合 CI
内存 - 空载 RSS	adb shell dumpsys meminfo	<50MB	引擎初始化后
内存 - 峰值 RSS	大对象 churn 时	建议上限	观察 GC 曲线
GC 停顿 p99	插入时间探针	<10ms	不能卡流式渲染
体积增量	APK 前后对比	<15MB 可接受，>30MB 危险	libjsc + ICU
长跑稳定性	随机插件循环 1 小时	无崩溃、RSS 稳定	LMK 压测
关键基准的实现
JetStream2：Apple 官方 JS 性能套件，从 JSC shell 运行即可。在 Android 上需要先把它打成静态 bundle。可以作为季度级综合体检，不进日常 CI。
SunSpider：比 JetStream2 轻，适合作为每日快速回归。
流式场景专项——这个是针对你 AI CLI 用途的关键，通用的 JS 基准不测这个：
// 模拟流式 token 处理：小字符串拼接 + 正则 + 对象操作
async function streamToken(token) {
  buffer += token;
  if (buffer.length > 100) {
    const parsed = JSON.parse(buffer);
    buffer = "";
    // 模拟插件变换
    return transform(parsed);
  }
}
测量指标：
• 每 token 的 p50/p99 延迟
• 是否出现 GC 停顿打断流式渲染（>16ms 就可能掉帧）
• 长时间运行的吞吐衰减
L4 宿主架构对接（为 Rust Harness + TS 插件铺路）
这一层不是测 JSC 本身，而是测你的 JSC 嵌入接口能不能承载未来的 Harness 角色。之前建议的 PluginEngine trait 在这里就是测试对象。
API 契约测试
每一个从 Rust 暴露给 JSC 的宿主函数都要有测试：
trait PluginEngine {
    fn eval(&mut self, code: &str) -> Result<JsValue>;
    fn register_host_fn(&mut self, name: &str, f: HostFn) -> Result<()>;
    fn call_js_fn(&mut self, name: &str, args: &[JsValue]) -> Result<JsValue>;
    fn drain_microtasks(&mut self) -> Result<()>;
}
测试矩阵：
契约	测试内容	判分
值传递	Rust String ↔ JS string，数字、bool、null、undefined 双向	无精度损失、无类型漂移
对象序列化	Rust struct ↔ JS object，深嵌套	字段无丢失
异常传播	JS 抛异常 → Rust 收到错误；Rust 返回错误 → JS 收到 throw	异常链完整
回调重入	JS 调宿主函数 → 宿主又调 JS	不死锁、不崩溃
HostFn 崩溃隔离	宿主函数 panic	进程不崩，能恢复
内存生命周期	JS 返回的 JsValue 在 GC 后仍有效	无悬垂指针
microtask 与 async 时序
JSC 有 Promise/microtask 队列，但只有显式调用 drainMicrotasks() 才会跑——嵌入环境没有浏览器的事件循环。这一项经常被忽略，但对 TS 插件至关重要（TS 里的 async/await 编译为 Promise）。
// 测试：Promise 链在 drain 前不会执行
let done = false;
Promise.resolve().then(() => { done = true; });
assert(!done);              // 还没 drain，应该 false
engine.drain_microtasks();  // Rust 侧调用
assert(done);               // drain 后应该 true
语义一致性基线（为未来换引擎做准备）
把同一套 API 契约测试跑在你当前 JSC 移植版和潜在备选 QuickJS 上，确保它们对外暴露的语义完全一致。这是未来可替换性的保险。
分层覆盖详表（完整性视角）
把"完整性"具体化，你需要在每层至少有若干测试：
层次	覆盖内容	对应测试
词法/语法	Unicode 转义、模板字符串、正则字面量	test262 test/language/lexical-grammar/
静态语义	严格模式、TDZ、模块解析	test262 test/language/statements/、test/language/module-code/
运行时语义	原型链、this 绑定、new.target、super	test262 test/language/expressions/
内置对象	Array/String/TypedArray/Map/Set/RegExp	test262 test/built-ins/
Intl	ICU 相关：日期、数字、排序	test262 test/intl402/（依赖 ICU 数据打包）
异步	Promise、async/await、generator、microtask 时序	test262 test/built-ins/Promise/ + 自建 microtask 测试
Atomics/SharedArrayBuffer	原子操作、共享内存	test262 test/built-ins/Atomics/（Android NDK 底层支持）
JIT 正确性	所有代码路径下语义一致	L2 的配置矩阵
GC 正确性	弱引用、finalization、循环引用	JSTests stress/ 相关用例 + 自建
Wasm（如启用）	字节码解析、执行	JSTests/wasm/stress
一键回归的工程化
目标：把上述测试做成 ./scripts/regress.sh，一条命令跑完，输出一张分级记分卡。
JSC Android 移植版 · 回归记分卡 (2026-10-11)
├─ L1 语义：test262 通过 97.2% (目标 ≥97%)     ✓ PASS
│  └─ 失败分类：Intl 312 / AnnexB 410 / 其他 57
├─ L2 JIT 一致性：4 配置 × 抽样 1000 用例       ✓ PASS
├─ L3 性能：
│  ├─ 启动 128ms  (目标 <150ms)               ✓ PASS
│  ├─ 热循环 A 稳态 14.9ns                     ✓ PASS
│  ├─ 空载 RSS 43MB (目标 <50MB)              ✓ PASS
│  ├─ GC 停顿 p99 6ms  (目标 <10ms)           ✓ PASS
│  └─ APK 体积 +12MB (目标 <15MB)             ✓ PASS
└─ L4 宿主契约：17/18 通过                     ✗ FAIL (回调重入死锁)
CI 触发策略：
• 每次改 JSC 构建配置或升级 WebKit 上游：跑全部
• 每日夜间：跑 L1 + L3 核心指标
• 每周：跑 JetStream2 + L2 完整矩阵
• 每次 release 前：跑全部 + 真机长跑 1 小时
参考基线的建立
对照锚点：
WebKit 官方 Results Database —— 有各平台 JSC 的历史数据可查。
建议建一张基线表，把每次回归的数字都存进去。没有基线数据的"快"和"稳"都是主观感受，不能用于工程决策。
最后的关键建议
================================================================================
执行结果（2026-10-11 实测 · Redmi 6 Pro / Android 9 / API 28）
================================================================================
一键记分卡：_deps/regress.ps1（读四层产物，输出 _deps/scorecard.txt）
各层脚本：L1/L2 → _deps/t262/{t262_gen.py, t262_run.sh, run_test262.ps1, l2_matrix.ps1, t262_analyze.py, t262_compare.py}
          L3   → _deps/l3/{run_l3.ps1, l3_probe.js, l3_rss.sh, l3_*.js}
          L4   → _deps/l4/{run_l4.ps1} + 移植工程 Java 源里的 L4ContractTest/L4Probe

JSC Android 移植版 · 回归记分卡（实测）
├─ L1 语义：test262 通过 99.74%（101,804 / 102,065，跳过 486）      ✓ PASS (目标 ≥97%)
│  └─ 失败 261：intl402 110 / language 57 / staging 49 / built-ins 44 / annexB 1
│     （错误类型：Test262Error 211、SyntaxError 27、RangeError 12、SIGSEGV 2、超时 4 ≈ 0.006%）
│  └─ 规模与时耗：53,616 文件 → 102,065 执行项；设备端 6 分片本机并行，3.2 小时（≈8–9 条/秒）
├─ L2 JIT 一致性：3 档抽样（各 5,110 条）× 对照 L1 全量基线          ✓ PASS（新失败 0 / 新通过 0）
│  └─ JSC_useJIT=0 ｜ useJIT=1,DFG=0 ｜ useJIT=1,DFG=1,FTL=0 —— 失败集与 FTL 全开版**逐条相同**
├─ L3 性能与体积：
│  ├─ 启动 487.5 ms（含 fork/exec + 加载 66 MB .so + ICU 初始化）   ✗ 未达 <150ms（口径见下）
│  ├─ 热循环 A 稳态 14.9 ns/iter（32.6× vs 解释器）                  ✓ PASS
│  ├─ 空载 RSS 30.1 MB (目标 <50MB)                                 ✓ PASS
│  ├─ GC 停顿：强制 full GC p50 22.1 / p99 27.1 ms                  ✗ 口径不同（见下）
│  │   └─ 流式路径实测 p99 20 µs / max 1.82 ms ⇒ 无 >16ms 掉帧停顿   ✓ 实质满足
│  ├─ 流式 token：平均 4 µs/token，20k token 共 72 ms，无吞吐衰减    ✓ PASS
│  └─ APK 76.45 MB（lib 65.8 + dex 8.1 + libc++ 1.2）               口径：自带引擎宿主（见下）
│      └─ 优化点：.data 29.4 MB ≈ ICU 数据；.text 27.6 MB
└─ L4 宿主契约：21/21 + L4Probe A–H 全 OK                          ✓ PASS（且**修掉一个真实崩溃缺陷**）
   └─ 回归：JSCJniTest 99/99、Stage5Verification 6/6（修复版 .so）

### 对上文三处假设的更正（实测所得）
1. **microtask 不需要显式 drain** —— 方案 §L4 说"只有显式调用 drainMicrotasks() 才会跑"，本版 JSC **不是**：`runtime/JSLock.cpp:301-315` 的 `JSLock::willReleaseLock()` 会 `vm.drainMicrotasks()`，即**每个 API 调用返回前微任务已排空**。实测 `Promise.resolve().then(...)` / `await` 在 `evaluateScript` 返回时已生效 ⇒ 宿主侧**不需要** `drain_microtasks()`（写成契约说明即可，别照搬浏览器事件循环）。
2. **L1 的执行方式必须换** —— 上文给的 `test262-harness --host-path=./jsc-adb.sh`（每条用例一次 adb 往返）在 10 万条的规模下不可行（每条约 0.6–1 秒 → 数十小时）。实测改用**设备端本机分片**（把命令清单一次性推到设备、`t262_run.sh` 6 分片本地循环），吞吐提升到 **8–9 条/秒**、全量 3.2 小时。语义仍与上游 runner 对齐（strict/default/module/raw、`--exception`、`--test262-async`、`harness/sta.js+assert.js`、includes 去重、config.yaml 跳过规则）。
3. **GC 停顿的判据要分清"谁触发的停顿"** —— 强制 full GC（`jsc` 的 `gc()` = GC+sweep）实测 22 ms，**不代表**流式会被打断；对"流式渲染"真正相关的是自然停顿，实测 **p99 20 µs / max 1.82 ms**，无一帧级停顿。

### 本轮额外收获（L4 抓到的真实缺陷）
传 Java `Map`/`List` 进 JS 后再返回对象 → **进程 SIGABRT**（`JNI DETECTED ERROR: mid == null`）。根因在桥接层的类缓存：接口（`java/util/List|Map`）与实现类（`ArrayList|HashMap`）**共用同一个"首次查询即定值"的槽位** → 之后 `GetMethodID(HashMap 的 "<init>")` 落到接口上解析为 null → `NewObject(…, nullptr)` 被 CheckJNI 判死。已拆槽 + 加空值防御，修复后 L4/JSCJniTest/Stage5 全绿。**教训**：宿主契约类测试（尤其"传容器进去、再取对象回来"）应当进入回归集——既有的 99 条用例恰好都只返回基本类型，漏掉了这条路径。

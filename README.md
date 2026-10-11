# Android JSC 移植（WebKit JavaScriptCore → Android，含 JNI 桥接）

把上游 WebKit 的 **JavaScriptCore** 以 `PORT=JSCOnly` 移植到 Android（arm64-v8a），并新增一层
**JNI 桥接 API**（Java ↔ JavaScript），使 Android 应用可以直接内嵌 JSC 并享有：

- 值双向转换（String / Number / Boolean / null / undefined / Array / Map / 嵌套对象）
- **对象同一性**：同一 Java 对象在 JS 侧始终映射到同一个包装对象；Java 容器（`List`→数组、`Map`→对象）
- **声明式导出**：`@JSExport` / `@JSExportAs` / `@JSProperty`（实例与静态成员、构造器原型链）
- **弱值**：`JSCWeakValue` / `JSCJniWeakValue`（GC 通知 + 确定性回收检测）
- **对象图保活**：`addManagedReference(owner, owned)`（对齐 ObjC 的 external object graph，含分代补扫）
- 内存压力链路：`notifyMemoryPressure` 与系统 `onTrimMemory` 对接
- 并发：多 VM / 多 Context / 共享 Context + GC 交错

**上游基线**：`git@github.com:WebKit/WebKit.git` @ `f3ef8627`（本次移植基于该修订的稀疏检出）

## 一、仓库内容

| 目录 / 文件 | 内容 |
|---|---|
| `port/port-changes.patch` | **移植的全部改动**（对上游 45 个文件的 diff；`git apply` 即可复现） |
| `port/files/` | 新增源码的完整副本：`Source/JavaScriptCore/API/jni/**`（C++ 桥 + Java API + 测试类）、`Source/JavaScriptCore/JNI.cmake` |
| `MyApplication/` | Android 宿主工程（Kotlin / `minSdk 28` / arm64-v8a）：把引擎打进 APK 并在 App 内跑集成回归（直接引用 `port/` 的 Java 源，单一事实来源） |
| `_deps/` | **工具链脚本**（不含依赖与运行产物）：环境与依赖获取、JNI Java 打包、静态 testapi 构建、四层测试链、一键记分卡 |
| `ANDROID-JSC-PORT-PLAN.md` | **移植方案与实施记录**（每个缺陷的根因、修复与验证，含行号引用） |
| `Android JSC 移植版完整测试方案.md` | 四层测试方案 + 实测记分卡 + 对原方案三处假设的更正 |

> **本仓库不包含**（保留在本地工作区，未推送）：
> - 上游 WebKit 源码（约 4 GB）——由「复现步骤」的稀疏检出获取；
> - `_deps/` 下的**第三方依赖与运行产物**：ICU（`icu-*`）、Ruby（`ruby*`）、test262 检出、`jni-java/` 打包产物、`t262/work`、`l3/out`、`l4/out` 等；
> - `docs/` —— 实测证据文件（记分卡、L1 摘要、L3 报告、L4 汇总）：结论已写入本 README 与两份设计文档。

### 工具链明细（`_deps/`，已入库）

| 层 | 脚本 | 作用 |
|---|---|---|
| 环境 | `env.ps1` / `fetch_tools.ps1` | PATH（perl/ruby/cmake/ninja）与 `JSC_ANDROID_*`；依赖获取（ICU / Ruby 等） |
| 构建 | `configure_testapi.ps1` / `build_testapi.ps1` | 上游原教旨路径的静态 JSC + `testapi` 配置与后台构建（含 PATH 修正） |
| 打包 | `build_jni_java.ps1` | 移植工程 Java 源 → `javac` → `jar` → `d8`（产出含 `classes.dex` 的 jar，供 `app_process` 与 APK 使用） |
| L1/L2 | `t262/t262_gen.py` | 生成 test262 执行清单：语义对齐上游 `Tools/Scripts/test262/runner.py`（strict/default/module/raw、`--exception`、`--test262-async`、`harness/sta.js+assert.js`、includes 去重、config.yaml 跳过规则） |
| L1/L2 | `t262/{t262_run.sh,t262_launch.sh,t262_progress.sh}` | 设备端 N 分片本机并行执行（含残留 worker 自清理） |
| L1/L2 | `t262/{run_test262.ps1,l2_matrix.ps1}` | 编排（生成→打包→推送→运行→轮询→拉回→记分）与 JIT 三档矩阵 |
| L1/L2 | `t262/{t262_analyze.py,t262_compare.py}` | 通过率与失败归档（目录/特性/模式/错误类型）、多配置失败集比对 |
| L3 | `l3/run_l3.ps1` + `l3_probe.js` + `l3_rss.sh` + `l3_{startup,baseline}.sh` + `l3_{idle,peak,gc}.js` | 启动基准、RSS 主机侧采样、强制 GC 停顿、流式 token 延迟/衰减、体积构成 |
| L4 | `l4/run_l4.ps1` | 宿主契约套件（`L4ContractTest`/`L4Probe`，源码在 `port/`）+ 既有回归（`JSCJniTest`/`Stage5Verification`） |
| 基准 | `jit_curve.js` / `stage5/bench.js` | JIT 分层性能曲线与热循环基准 |
| 汇总 | `regress.ps1` | 一键四层记分卡 → `scorecard.txt` |

## 二、实测结果（Redmi 6 Pro / Android 9 / API 28）

```
├─ L1 语义：test262 通过 99.74%（101,804 / 102,065，跳过 486）      ✓ 判据 ≥97%
│  └─ 失败 261：intl402 110 / language 57 / staging 49 / built-ins 44 / annexB 1
│     规模：53,616 文件 → 102,065 执行项，设备端 6 分片本机并行，3.2 小时（≈8–9 条/秒）
├─ L2 JIT 一致性：3 档抽样（各 5,110 条）对照 L1 全量基线            ✓ 新失败 0 / 新通过 0
│  └─ useJIT=0 ｜ useJIT=1,DFG=0 ｜ useJIT=1,DFG=1,FTL=0 —— 失败集与 FTL 全开版逐条相同
├─ L3 性能与体积：
│  ├─ 启动 487.5 ms（含 fork/exec + 加载 66 MB .so + ICU 初始化；APK 场景只付一次）
│  ├─ 热循环 A 稳态 14.9 ns/iter（32.6× vs 解释器）                  ✓
│  ├─ 空载 RSS 30.1 MB（<50 ✓）／峰值 102.7 MB／GC 后回落 37.0 MB    ✓ 无泄漏趋势
│  ├─ 流式 20k token：平均 4 µs/token，p99 20 µs，max 1.82 ms，无衰减 ✓ 无 >16ms 掉帧停顿
│  └─ APK 76.45 MB（lib 65.8 + dex 8.1 + libc++ 1.2）；`.data` 29.4 MB ≈ ICU 数据（最大优化点）
└─ L4 宿主契约：21/21 + 最小复现用例 A–H 全 OK                      ✓
   └─ 回归：JSCJniTest 99/99、Stage5Verification 6/6；**并借此修掉一个真实崩溃缺陷**
```

### 本轮修掉的代表性缺陷（详见 `ANDROID-JSC-PORT-PLAN.md`）

| 缺陷 | 现象 | 根因 / 修复 |
|---|---|---|
| **Map/List 类缓存槽冲突** | 传 Java `Map`/`List` 进 JS 后返回对象 → 进程 SIGABRT（`JNI DETECTED ERROR: mid == null`） | `cachedClass()` 是"首次查询即定值"槽位，而接口（`java/util/List|Map`）与实现类（`ArrayList|HashMap`）**共用同一槽** → `GetMethodID(HashMap."<init>")` 落到接口上为 null → `NewObject(…, nullptr)` 触发 CheckJNI abort。已拆槽 + 构造路径空值防御 |
| 跨线程实参串扰 | 3 线程共享 Context 时只有 1 个能取到实参 | `evaluateScript(code, args…)` 是「注入全局 `__jscArgs` → 求值 → 清理」多步序列，各步各自取锁 → 交错覆盖。改为全程持 apiLock |
| 类注册表长期持有的 JS 对象未保活 | debug 包（CheckJNI）在 `close()` 处 SIGABRT：`use of invalid jobject` | 对 `prototype` / `constructor` / `factory` / 方法函数补 `JSValueProtect`，收尾成对 `JSValueUnprotect` |
| wrapper 永久泄漏 | 弱值/包装永不回收 | 对齐 ObjC：`isReachableFromOpaqueRoots` + `scanExternalObjectGraph`，且 **GC 必须走 Full（`collectSync`）**，因为 opaque root 只在 Full GC 清空 |

## 三、复现步骤

### 1) 取上游 WebKit 并应用移植改动

```bash
git clone --filter=blob:none --sparse git@github.com:WebKit/WebKit.git WebKit-JavaScriptCore
cd WebKit-JavaScriptCore
git sparse-checkout set \
  Source/JavaScriptCore Source/WTF Source/bmalloc Source/cmake \
  CMakeLists.txt Source/CMakeLists.txt .gitattributes \
  Source/ThirdParty/unifdef Source/ThirdParty/gtest Source/ThirdParty/gmock \
  Tools/Scripts
git checkout f3ef8627                     # 本移植的上游基线
git apply ../port/port-changes.patch      # 应用移植改动（45 个文件）
```

### 2) 依赖与工具链

```powershell
. _deps\env.ps1        # PATH（perl / ruby / cmake / ninja）+ JSC_ANDROID_* 变量
```

| 依赖 | 说明 |
|---|---|
| Android NDK | r28c（`ANDROID_ABI=arm64-v8a`、`ANDROID_PLATFORM=android-28`、`ANDROID_STL=c++_shared`） |
| ICU 76.1 | arm64 静态库（`libicuuc.a` / `libicui18n.a` / `libicudata.a` + 头文件），交叉编译工程见 `_deps/icu-build`（方案 §2.2） |
| Perl / Ruby | WebKit 生成器脚本以**裸命令名**调用，必须在 PATH 中（本机 perl 取 Git for Windows 自带） |
| CMake / Ninja | 可取自 VS Build Tools |

### 3) 构建引擎

```powershell
# 完整参数（含 JIT/FastMalloc/ICU/JNI 开关）见 _deps\configure_testapi.ps1 与方案 §8.1
cmake -S WebKit-JavaScriptCore -B build-android -G Ninja <…>
ninja -C build-android JavaScriptCore

# ⚠️ 必须手动 strip：直接 ninja 只链接不 strip（507 MB → 65.8 MB）
llvm-strip --strip-unneeded build-android\lib\libJavaScriptCore.so
```

### 4) JNI Java 侧打包（javac → d8）

```powershell
_deps\build_jni_java.ps1      # javac → jar → d8，产出 _deps\jni-java\jsc-jni-test.jar（含 classes.dex）
```

### 5) 部署到设备

`libJavaScriptCore.so` + **`libc++_shared.so`**（NDK `sysroot/usr/lib/aarch64-linux-android/`）+ jar 三者同目录；
Android 10+ 必须**先加载 `libc++_shared`**（linker namespace 不再从主库目录解析依赖，否则进程静默被杀）。

```bash
adb push build-android/lib/libJavaScriptCore.so  /data/local/tmp/
adb push <ndk>/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so /data/local/tmp/
adb push _deps/jni-java/jsc-jni-test.jar /data/local/tmp/
adb shell 'cd /data/local/tmp && CLASSPATH=/data/local/tmp/jsc-jni-test.jar \
  app_process -Djava.library.path=/data/local/tmp /system/bin org.webkit.jni.JSCJniTest'
```

### 6) 四层回归（一条命令，本地工具链）

```powershell
_deps\regress.ps1          # → _deps\scorecard.txt（L1 test262 / L2 JIT 矩阵 / L3 性能 / L4 宿主契约）
```

- L1/L2：`_deps\t262\run_test262.ps1`（全量或抽样）、`_deps\t262\l2_matrix.ps1`（三档配置）
- L3：`_deps\l3\run_l3.ps1`
- L4：`_deps\l4\run_l4.ps1`（契约套件 + 既有回归）

> 上述脚本均在 `_deps/`（已入库）；其**运行产物**（`t262/work`、`l3/out`、`l4/out`）与实测证据（`docs/`）
> 不入库。test262 套件本身也不入库：`git clone --depth 1 https://github.com/tc39/test262 _deps/test262`，
> 并准备上游的 `JSTests/test262/config.yaml` 作为跳过规则来源。

## 四、已知限制与注意事项

- **不扫描 JVM 堆**：跨 GC 保活必须显式 `addManagedReference`（ObjC 的 external object graph 语义已等价实现，但入口是显式 API）。
- **启动成本**：冷启动约 490 ms（低端机 + 66 MB 单库 + ICU 初始化）；APK 宿主只付一次。若需更低：裁剪 ICU 数据（`.data` 29.4 MB）、`-Os`、去掉 `.eh_frame`。
- **体积**：APK 76 MB（引擎 65.8 MB + dex 8.1 MB + libc++ 1.2 MB）。
- **Intl402 的 110 条失败**：集中在 Temporal / Intl.Locale / canonical-tz，属 ICU 数据与版本面，需与同版本上游对照才能定性。
- **上游对 Android 默认关闭 `ENABLE_API_TESTS`**（`Source/cmake/OptionsJSCOnly.cmake`）：本移植改为"未显式指定时仍为 OFF"，从而支持在 Android 上按上游原样跑 `testapi` 基线（静态 JSC）。
- **远程真机 + FRP 隧道环境**：单次 `adb push` 传输超过约 120 秒（低速时约 64 MB）可能**假成功**（打印成功但设备上无文件）→ 压缩后再推 / 切片推送，并**必须** `md5` 对账。

## 五、许可

- `port/` 的移植改动遵循上游 WebKit 的许可（BSD-2-Clause、LGPL-2.1 等，见上游 `Source/JavaScriptCore/LICENSE` 等文件）。
- 本仓库**不含**上游 WebKit 源码，仅含移植补丁、新增源码、工具脚本与文档。

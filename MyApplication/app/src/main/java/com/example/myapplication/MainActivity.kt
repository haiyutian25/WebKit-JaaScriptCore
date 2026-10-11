package com.example.myapplication

import android.app.Activity
import android.content.ComponentCallbacks2
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.webkit.jni.JSCContext
import org.webkit.jni.JSCJniTest
import org.webkit.jni.L4ContractTest
import org.webkit.jni.JSCVirtualMachine
import java.io.File
import java.io.FileOutputStream
import java.io.PrintStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * APK 集成宿主（方案 §7.4）。
 *
 * 与开发期 `app_process` 形态的关键差异，正是本宿主存在的意义：
 *   1. `.so` 由 `jniLibs/arm64-v8a/` 经 **linker namespace** 解析（无需 `-Djava.library.path`）；
 *   2. 进程是**真实 App 沙箱**（SELinux 域、ART 以 App 配置启动）；
 *   3. `onTrimMemory` / `onLowMemory` 这类**系统回调**只在 App 形态下才有。
 *
 * ## 为什么自带文件日志
 *
 * 部分机型（实测 **Redmi K30 Pro / MIUI / API 31**）的 `adb shell` **读不到 logcat**
 * （`logcat -d` 恒为空、`/dev/socket/logdw` Permission denied），而原生崩溃又没有 Java 异常可捕。
 * 因此本宿主把每一行**同时**写入 `filesDir/verify.log`，可用
 * `adb shell run-as com.example.myapplication cat files/verify.log` 取出
 * （debug 包 debuggable，`run-as` 可用）。崩溃前的最后一行即定位依据。
 *
 * 手动触发内存压力：`adb shell am send-trim-memory com.example.myapplication 15`
 */
class MainActivity : Activity() {

    private var virtualMachine: JSCVirtualMachine? = null
    private var jscContext: JSCContext? = null
    private lateinit var output: TextView
    private var trimMemoryCalls = 0
    private var regressionStarted = false

    /** 运行模式：`full`（默认，跑 JSC 与桥接回归）或 `nojsc`（纯 Java 对照探针）。 */
    private val mode: String get() = intent?.getStringExtra("mode") ?: "full"

    /** 文件日志（每行 flush）—— 崩溃时不丢尾部内容。 */
    private var fileOut: FileOutputStream? = null
    private lateinit var logFile: File

    /** 界面日志行数上限 —— 回归打印数百行，避免 TextView 无限增长。 */
    private val uiLines = ArrayDeque<String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 日志位置：优先**共享**的 `Android/media/<pkg>/`（在 API 30+ 上仍可被 `adb shell` 读到，
        // 因此 release（非 debuggable，`run-as` 不可用）也能取日志）；失败则退回内部 `files/`。
        logFile = run {
            val shared = getExternalMediaDirs()?.firstOrNull()
            val dir = if (shared != null && (shared.isDirectory || shared.mkdirs())) shared else filesDir
            File(dir, VERIFY_LOG)
        }
        try {
            fileOut = FileOutputStream(logFile, true)
        } catch (t: Throwable) {
            Log.w(TAG, "无法打开文件日志: $t")
        }

        // 未捕获的 Java 异常也落到文件里（原生崩溃无法捕获，但能留下"走到哪一步"）
        val previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            emit("UNCAUGHT on ${thread.name}: ${error.javaClass.name}: ${error.message}")
            error.stackTrace.take(20).forEach { emit("    at $it") }
            previousHandler?.uncaughtException(thread, error)
        }

        output = TextView(this).apply {
            textSize = 12f
            setPadding(32, 32, 32, 32)
        }

        val jsButton = Button(this).apply {
            text = "JS 能力自检"
            setOnClickListener { onBackground { jsSelfCheck() } }
        }
        val regressButton = Button(this).apply {
            text = "跑桥接回归（90 用例）"
            setOnClickListener { onBackground { bridgeRegression(reRun = true) } }
        }
        val gcButton = Button(this).apply {
            text = "GC + 观测计数"
            setOnClickListener {
                jscContext?.garbageCollect()
                logLine("garbageCollect() 完成 / ${ownershipText()}")
            }
        }
        val statusButton = Button(this).apply {
            text = "显示内存压力状态"
            setOnClickListener { logLine(statusText()) }
        }

        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(jsButton)
            addView(regressButton)
            addView(gcButton)
            addView(statusButton)
            addView(ScrollView(this@MainActivity).apply { addView(output) })
        }
        setContentView(column)

        logLine("===== 新一次运行 " + SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date()) + " =====")
        logLine("设备：${Build.MODEL} / Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}) / " +
            "${Build.SUPPORTED_ABIS.firstOrNull()}")
        logLine("日志文件：$logFile")
        logLine("PHASE: 线程检查 —— 当前线程 = ${Thread.currentThread().name}")

        // 自动跑：便于 adb 侧无需点按即可取到结论
        //   mode=full（默认）：JSC 初始化 + JS 自检 + 桥接回归
        //   mode=nojsc        ：纯 Java 探针（不加载 JSC）—— 用于区分"JSC 相关"与"宿主/ART 相关"
        when (mode) {
            "nojsc" -> onBackground { pureJavaProbe() }
            else -> onBackground {
                if (initializeJsc()) {
                    jsSelfCheck()
                    bridgeRegression(reRun = false)
                    l4Contract()
                }
                logLine(statusText())
            }
        }
    }

    // ---------------------------------------------------------------- JSC 初始化

    /** @return 是否可用于后续验证 */
    private fun initializeJsc(): Boolean {
        return try {
            logLine("PHASE: init 开始（线程 ${Thread.currentThread().name}）")
            val machine = JSCVirtualMachine()
            virtualMachine = machine
            jscContext = JSCContext(machine)
            logLine("✅ JSC 初始化成功（libJavaScriptCore.so 由 jniLibs 经 linker namespace 加载）")
            logLine("evaluateScript(\"1 + 2\") = ${jscContext?.evaluateScript("1 + 2")}")
            logLine("PHASE: init 完成")
            true
        } catch (t: Throwable) {
            logLine("❌ JSC 初始化失败：$t")
            false
        }
    }

    // ---------------------------------------------------------------- 纯 Java 探针

    /**
     * 不加载 JSC 的对照探针：制造与桥接回归同类的负载（文件写入 + 逼近堆上限的压力回收）。
     *
     * 用途：若本探针也会崩溃，则崩溃与 JSC 无关（属宿主/ART，例如 debug 包默认开启的 CheckJNI
     * 抓到的 libopenjdk 缺陷）；若本探针稳定而回归崩，则问题在 JSC 相关路径。
     */
    private fun pureJavaProbe() {
        logLine("PHASE: 纯 Java 探针 开始（不加载 JSC）")
        val pressure = ArrayList<ByteArray>()
        var round = 0
        while (round < 5) {
            round++
            try {
                while (true)
                    pressure.add(ByteArray(8 * 1024 * 1024))
            } catch (_: OutOfMemoryError) {
                logLine("  探针 $round/5：堆压力触发（本轮到 OOM，累计 ${pressure.size * 8} MB）")
            }
            pressure.clear()
            System.gc()
            logLine("  探针 $round/5：完成")
        }
        logLine("APK-SUMMARY: 纯 Java 探针完成（未崩溃即说明与 JSC 无关）")
    }

    // ---------------------------------------------------------------- JS 能力自检

    private fun jsSelfCheck() {
        val ctx = jscContext ?: run { logLine("JSC 未就绪，跳过 JS 自检"); return }
        logLine("PHASE: JS 自检 开始")
        try {
            val checks = listOf(
                "JSON.stringify(['a', 1, true])" to "JSON/数组",
                "(function () { return 6 * 7; })()" to "闭包",
                "Intl.supportedValuesOf('timeZone').length" to "ICU 时区数据",
                "(function () { var s = 0; for (var i = 0; i < 100000; i++) s += i; return s; })()" to "JIT 热点循环",
                "new Intl.DateTimeFormat('zh-CN', {timeZone:'Asia/Shanghai'}).format(new Date(0))" to "时区格式化",
            )
            for ((expr, label) in checks) {
                logLine("  $label: ${ctx.evaluateScript(expr)}")
                logLine("PHASE: JS 自检 [$label] 完成")
            }
            logLine("APK-SUMMARY: JS 自检完成（${checks.size} 项）")
        } catch (t: Throwable) {
            logLine("❌ JS 自检失败：$t")
        }
    }

    // ---------------------------------------------------------------- 桥接回归

    /**
     * 直接调用移植工程里的 [JSCJniTest.main] —— 与开发期 `app_process` 形态**同一份源码**。
     *
     * 注意：`JSCJniTest` 的通过/失败计数是**静态累加**的，重跑会叠加，故重跑时同时打印本轮实际值。
     */
    private fun bridgeRegression(reRun: Boolean) {
        if (regressionStarted && reRun)
            logLine("（重跑：JSCJniTest 的计数为静态累加，下面的数字含此前各轮）")
        regressionStarted = true
        logLine("PHASE: 桥接回归 开始（线程 ${Thread.currentThread().name}）")

        val origin = System.out
        val started = System.currentTimeMillis()
        try {
            // ⚠️ 关键：把 System.out 直接接到本宿主的输出通道（UI + 文件日志），
            // **不再写原始 stdout**。
            //
            // 原因（本轮实测，Redmi K30 Pro / MIUI / API 31）：该 ROM 限制日志（`adb shell`
            // 读不到 logcat、`/dev/socket/logdw` Permission denied），App 的 stdout 管道写入会失败；
            // libopenjdk 的 `FileOutputStream.writeBytes` 随即抛 IOException 走 `jniLogException`，
            // 而 debug 包默认开启的 CheckJNI 在该路径上 abort 成 SIGSEGV —— 崩溃点在
            // FinalizerDaemon（`libopenjdk.so (jniLogException)` → `CheckJNI::NewObject`），
            // 栈里没有任何 JSC 帧。回归往 stdout 打 300+ 行，正好触发。
            // 对照实验：同一 App 的「纯 Java 探针」模式（不写 stdout）5 轮 248 MB 堆压力全过。
            System.setOut(PrintStream(SinkStream { emit(it) }, true, "UTF-8"))
            JSCJniTest.main(arrayOf<String>())
            logLine("APK-SUMMARY: 桥接回归完成，耗时 ${System.currentTimeMillis() - started} ms")
        } catch (t: Throwable) {
            logLine("❌ 桥接回归异常终止（${System.currentTimeMillis() - started} ms）：$t")
            t.stackTrace.take(15).forEach { logLine("    at $it") }
        } finally {
            System.setOut(origin)
        }
        logLine("PHASE: 桥接回归 结束")
    }

    // ---------------------------------------------------------------- L4 宿主契约

    /**
     * 跑移植工程的 [L4ContractTest]（与 `app_process` 形态**同一份源码**）：
     * 值/深嵌套往返、JS↔Java 双向异常、宿主异常隔离、JS→Java→JS 重入、
     * 微任务在 API 边界的执行时机（`JSLock::willReleaseLock` 排空）、GC 后生命周期。
     *
     * 这些是"宿主契约"层——真实 App 内验证，比开发期的 `app_process` 更贴近交付形态。
     */
    private fun l4Contract() {
        logLine("PHASE: L4 宿主契约 开始")
        val origin = System.out
        val started = System.currentTimeMillis()
        try {
            System.setOut(PrintStream(SinkStream { emit(it) }, true, "UTF-8"))
            L4ContractTest.main(arrayOf<String>())
            logLine("APK-SUMMARY: L4 宿主契约完成，耗时 ${System.currentTimeMillis() - started} ms")
        } catch (t: Throwable) {
            logLine("❌ L4 宿主契约异常终止（${System.currentTimeMillis() - started} ms）：$t")
            t.stackTrace.take(10).forEach { logLine("    at $it") }
        } finally {
            System.setOut(origin)
        }
        logLine("PHASE: L4 宿主契约 结束")
    }

    // ---------------------------------------------------------------- 内存压力

    private fun statusText(): String =
        "onTrimMemory 次数=$trimMemoryCalls / JSC 事件计数=${JSCVirtualMachine.memoryPressureEventCount()} / " +
            "isUnderMemoryPressure=${JSCVirtualMachine.isUnderMemoryPressure()}"

    private fun ownershipText(): String =
        "wrapper=${JSCContext.totalWrapperCount()} 弱值登记=${JSCContext.liveWeakValueCount()} " +
            "holder=${JSCContext.liveWrapperHolderCount()}"

    /**
     * 端到端链路：系统 trim 回调 → JSC 的 MemoryPressureHandler。
     *
     * `level` 是离散枚举（不是连续量级），故用精确匹配判定"严重"：
     * `TRIM_MEMORY_RUNNING_CRITICAL(15)` 与 `TRIM_MEMORY_COMPLETE(80)`；
     * 而 `TRIM_MEMORY_UI_HIDDEN(20)` / `BACKGROUND(40)` / `MODERATE(60)` 只表示"退到后台"，
     * 不代表内存紧张（数值更大反而更不紧张），不能用 `>=` 比较。
     */
    @Suppress("DEPRECATION")
    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        trimMemoryCalls++
        val critical = level == ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL ||
            level == ComponentCallbacks2.TRIM_MEMORY_COMPLETE
        virtualMachine?.notifyMemoryPressure(critical)
        logLine("onTrimMemory(level=$level, critical=$critical) → notifyMemoryPressure")
        logLine("  ${statusText()}")
    }

    /** 旧式回调（无 level）：按"严重"处理。 */
    override fun onLowMemory() {
        super.onLowMemory()
        virtualMachine?.notifyMemoryPressure(true)
        logLine("onLowMemory() → notifyMemoryPressure(true)")
        logLine("  ${statusText()}")
    }

    override fun onDestroy() {
        logLine("onDestroy：关闭 context / VM")
        jscContext?.close()
        virtualMachine?.close()
        try {
            fileOut?.close()
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }

    // ---------------------------------------------------------------- 工具

    private fun onBackground(block: () -> Unit) {
        Thread(block, "jsc-verify").start()
    }

    /** 同时写 UI、logcat、文件 —— 文件是"logcat 不可读"机型上的可靠通道。 */
    private fun logLine(text: String) {
        Log.i(TAG, text)
        emit(text)
    }

    private fun emit(text: String) {
        try {
            fileOut?.apply {
                write((text + "\n").toByteArray(Charsets.UTF_8))
                flush()
            }
        } catch (_: Throwable) {
        }
        runOnUiThread {
            uiLines.addLast(text)
            while (uiLines.size > MAX_UI_LINES) uiLines.removeFirst()
            output.text = uiLines.joinToString("\n")
        }
    }

    /**
     * 按行接收字节的 OutputStream —— 让 `System.out` 的输出改道到本宿主的日志通道。
     *
     * 注意按**字节**累积、按 UTF-8 解码（不能逐字节 `toChar()`，否则中文变乱码）。
     */
    private class SinkStream(private val sink: (String) -> Unit) : java.io.OutputStream() {
        private val line = java.io.ByteArrayOutputStream()

        override fun write(b: Int) {
            if (b == '\n'.code) {
                sink(String(line.toByteArray(), Charsets.UTF_8))
                line.reset()
            } else {
                line.write(b)
            }
        }

        override fun write(b: ByteArray, off: Int, len: Int) {
            for (i in off until off + len)
                write(b[i].toInt())
        }
    }

    private companion object {
        const val TAG = "JscApk"
        const val VERIFY_LOG = "verify.log"
        const val MAX_UI_LINES = 500
    }
}

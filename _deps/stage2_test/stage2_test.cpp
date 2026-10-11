// Stage-2 运行时验证程序（Android / arm64-v8a）
//
// 覆盖三项，均在真机上以「可观测输出」为准：
//   1) 栈回溯：WTFGetBacktrace / WTFPrintBacktrace —— 验证 _Unwind_Backtrace 路径（方案 §4.2）
//   2) 内存压力：WTF::MemoryPressureHandler::triggerMemoryPressureEvent —— 验证 Unix 实现被正确接线（方案 §4.3）
//   3) Gigacage / 地址空间：扫描 /proc/self/maps 的大段映射 —— 验证预留是否成功（方案 §4.6）
//
// 初始化要求（重要）：
//   必须先调用 JSC::initialize()。MemoryPressureHandler 的构造函数会做
//   `m_holdOffTimer(RunLoop::mainSingleton(), …)`，而 RunLoop::mainSingleton() 需要 WTF 主线程
//   已初始化；否则会在 RunLoop 的 ref() 里对空指针 +8 解引用（真机上表现为 SIGSEGV /
//   fault addr 0x8 / MemoryPressureHandler.cpp:67）。这与 Linux 端口行为一致 —— unix 版实现
//   一直有这个前提，只是 Android 从 Generic 版切过来后才暴露。
//
// 符号名与签名已用 llvm-nm 核对（均为导出符号）：
//   T WTFGetBacktrace / T WTFPrintBacktrace                              (C 链接)
//   T JSContextGroupCreate / T JSContextGroupRelease                     (JSC 公开 C API)
//   T _ZN3JSC10initializeEv                                              (JSC::initialize)
//   T _ZN3WTF21MemoryPressureHandler26triggerMemoryPressureEventEb      (C++ mangled)
//
// 构建：clang++ --target=aarch64-none-linux-android28 … -lJavaScriptCore -llog -landroid -ldl -lm -Wl,--export-dynamic

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void WTFGetBacktrace(void** stack, int* size);
extern "C" void WTFPrintBacktrace(void** stack, int size);

extern "C" void* JSContextGroupCreate(void);
extern "C" void JSContextGroupRelease(void* group);

namespace JSC {
void initialize();
}

namespace WTF {
void initialize();
void initializeMainThread();

class MemoryPressureHandler {
public:
    static MemoryPressureHandler& singleton();
    void install();
    void triggerMemoryPressureEvent(bool isCritical);
};
} // namespace WTF

static void dumpSigCgt()
{
    FILE* status = fopen("/proc/self/status", "r");
    if (!status) {
        printf("[signal] 无法读 /proc/self/status\n");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "SigCgt:", 7) != 0)
            continue;
        unsigned long long mask = strtoull(line + 7, nullptr, 16);
        printf("[signal] SigCgt=0x%llx；已安装 handler 的信号：", mask);
        for (int sig = 1; sig <= 64; ++sig) {
            if (mask & (1ull << (sig - 1)))
                printf("%d ", sig);
        }
        printf("\n[signal] 说明：Android 的 SIGRTMIN 通常为 32，实时信号用 34(SIGRTMIN+2) 起；\n");
        printf("[signal]       WTF 应已选中实时区间内一个未被占用的号码。\n");
        break;
    }
    fclose(status);
}

static void dumpLargeMappings(size_t thresholdMiB)
{
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        printf("[maps] 无法打开 /proc/self/maps\n");
        return;
    }

    char line[512];
    size_t count = 0;
    unsigned long long totalBytes = 0;
    printf("[maps] 大于 %zu MiB 的映射：\n", thresholdMiB);

    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        if (sscanf(line, "%llx-%llx", &start, &end) != 2)
            continue;
        unsigned long long bytes = end - start;
        if (bytes < (unsigned long long)thresholdMiB * 1024 * 1024)
            continue;

        char perms[8] = { 0 };
        char* p = strchr(line, ' ');
        if (p)
            sscanf(p + 1, "%7s", perms);

        // 打印整行（去掉换行）：这样既能看到路径，也能看到 Android 的 [anon:名字]
        //（PR_SET_VMA_ANON_NAME 命名，例如 JSC 的 JIT 池会带名字）。
        line[strcspn(line, "\r\n")] = 0;

        printf("  %6llu MiB  %s\n", bytes / (1024 * 1024), line);
        ++count;
        totalBytes += bytes;
    }
    fclose(maps);
    printf("[maps] 合计 %zu 段 / %llu MiB\n", count, totalBytes / (1024 * 1024));
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    printf("=== Stage-2 runtime checks ===\n");

    // ---------- 0) 初始化运行时（必须先做，见文件头说明） ----------
    // 序列与 jsc.cpp:4741-4761 保持一致：WTF::initializeMainThread() 内部会调用
    // RunLoop::initializeMain()，后者设置 s_mainRunLoop —— 这正是 RunLoop::mainSingleton()
    // 的前提（RunLoop.cpp:82 只有 ASSERT，release 构建下会静默解引用空指针）。
    WTF::initialize();
    WTF::initializeMainThread();
    JSC::initialize();
    printf("[init] WTF::initialize() + WTF::initializeMainThread() + JSC::initialize() done\n");
    void* group = JSContextGroupCreate();
    printf("[init] JSContextGroupCreate() -> %p\n", group);
    if (!group) {
        printf("[init] FAIL：JSC 运行时初始化失败\n");
        return 1;
    }

    // ---------- 0.5) 线程挂起信号：实证选号 ----------
    dumpSigCgt();

    // ---------- 1) 栈回溯 ----------
    {
        constexpr int kMaxFrames = 32;
        void* stack[kMaxFrames];
        int size = kMaxFrames;
        WTFGetBacktrace(stack, &size);
        printf("[backtrace] frames=%d\n", size);
        if (size > 0)
            WTFPrintBacktrace(stack, size);
        printf("[backtrace] %s\n", size > 0 ? "OK" : "FAIL（0 帧）");
    }

    // ---------- 2) 内存压力 ----------
    {
        auto& handler = WTF::MemoryPressureHandler::singleton();
        handler.install();
        printf("[memory-pressure] install() done\n");
        handler.triggerMemoryPressureEvent(false);
        printf("[memory-pressure] triggerMemoryPressureEvent(false) returned\n");
        handler.triggerMemoryPressureEvent(true);
        printf("[memory-pressure] triggerMemoryPressureEvent(true) returned（critical）\n");
        printf("[memory-pressure] 若上方出现 \"Got memory pressure notification\" 即为 OK\n");
    }

    // ---------- 3) Gigacage / 地址空间 ----------
    dumpLargeMappings(64);

    JSContextGroupRelease(group);
    printf("=== done ===\n");
    return 0;
}

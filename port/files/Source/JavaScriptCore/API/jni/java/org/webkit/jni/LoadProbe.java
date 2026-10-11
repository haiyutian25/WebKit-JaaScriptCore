/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package org.webkit.jni;

/**
 * 最小加载探针：定位 libJavaScriptCore.so 在某台设备上"卡在哪一步"。
 *
 * <p>用途背景：JSCJniTest 在 Redmi K30 Pro / Android 12（API 31）上打印标题后即无输出，
 * 进程被 KILL（exit 137）。纯 Java 的 {@link GcProbe} 在同设备上完全正常，因此问题锁定在
 * native 库的加载或 JSC 初始化阶段。本探针按步骤打印，逐步缩小范围：
 *
 * <pre>
 * A → 进程与 main 正常
 * B → java.library.path 是否正确
 * C/D → System.loadLibrary 是否成功（失败会打印 UnsatisfiedLinkError 详情）
 * E/F → JSCVirtualMachine 构造（JSC::initialize 初始化）
 * G   → JSCContext 创建（apiLock / 全局对象）
 * H/I → 求值 1+2
 * </pre>
 *
 * 用法（设备上）：
 * <pre>
 * cd /data/local/tmp
 * CLASSPATH=/data/local/tmp/jsc-jni-test.jar app_process \
 *     -Djava.library.path=/data/local/tmp /system/bin org.webkit.jni.LoadProbe
 * </pre>
 */
public final class LoadProbe {
    public static void main(String[] args) {
        System.out.println("A: main entered");
        System.out.flush();

        System.out.println("B: java.library.path=" + System.getProperty("java.library.path"));
        System.out.flush();

        // Android 10+ 的 linker namespace 不会从 /data/local/tmp 解析 .so 的依赖，
        // 因此必须先显式加载 libc++_shared.so（libJavaScriptCore.so 的依赖），
        // 之后主库的依赖解析就能命中已加载的同名库。
        try {
            System.out.println("C: System.load(\"/data/local/tmp/libc++_shared.so\") ...");
            System.out.flush();
            System.load("/data/local/tmp/libc++_shared.so");
            System.out.println("C2: libc++_shared loaded OK");
            System.out.flush();
        } catch (Throwable t) {
            System.out.println("C2: libc++_shared load FAILED: " + t);
            t.printStackTrace(System.out);
            System.out.flush();
            return;
        }

        try {
            System.out.println("C3: System.loadLibrary(\"JavaScriptCore\") ...");
            System.out.flush();
            System.loadLibrary("JavaScriptCore");
            System.out.println("D: loaded OK");
        } catch (Throwable t) {
            System.out.println("D: load FAILED: " + t);
            t.printStackTrace(System.out);
            System.out.flush();
            return;
        }
        System.out.flush();

        try {
            System.out.println("E: new JSCVirtualMachine() ...");
            System.out.flush();
            JSCVirtualMachine vm = new JSCVirtualMachine();
            System.out.println("F: vm created");
            System.out.flush();

            JSCContext context = new JSCContext(vm);
            System.out.println("G: context created");
            System.out.flush();

            Object result = context.evaluateScript("1 + 2");
            System.out.println("H: evaluate 1+2 = " + result);
            System.out.flush();

            context.close();
            vm.close();
            System.out.println("I: done");
        } catch (Throwable t) {
            System.out.println("E-H: JSC usage FAILED: " + t);
            t.printStackTrace(System.out);
        }
        System.out.flush();
    }
}

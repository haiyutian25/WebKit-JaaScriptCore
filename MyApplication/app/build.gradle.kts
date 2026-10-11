plugins {
    alias(libs.plugins.android.application)
}

// 直接引用移植工程的 JNI Java 源，而不是手工副本 —— **单一事实来源**：
// 这样 APK 中运行的 API 类与回归用例（JSCJniTest 的 90 条）与 app_process 形态完全同源，
// 不会再出现"改了源、忘了同步副本"的漂移。
// 前提：MyApplication 与 WebKit-JavaScriptCore 同级（本工作区即如此）。
val jscJniJavaDir = rootDir.parentFile.resolve("WebKit-JavaScriptCore/Source/JavaScriptCore/API/jni/java")

android {
    namespace = "com.example.myapplication"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.example.myapplication"
        // 28：libJavaScriptCore.so 以 --target=aarch64-none-linux-android28 构建（见方案 §3.5）
        minSdk = 28
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // JSC 产物只提供 arm64-v8a（ICU 亦为 arm64 交叉编译）
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    // 本地验证用 release：用 debug 签名，且**关闭 R8** ——
    //   · 关闭 R8：桥接层的 @JSExport 走反射、native 侧用 FindClass 拿 org.webkit.jni.*，
    //     混淆会破坏可达性（正式发布需补 keep 规则，本工程只作集成宿主，故直接关掉）。
    //   · 用途：release 非 debuggable ⇒ ART 默认**不开 CheckJNI**，可与 debug 包对照，
    //     判定"仅在 debug 包出现的 ART/libopenjdk abort"是否为工具链假阳性。
    signingConfigs {
        create("localRelease") {
            storeFile = File(System.getProperty("user.home"), ".android/debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("localRelease")
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    // JNI 桥接层 Java 源（org.webkit.jni.*，含 JSCJniTest 的 90 条回归用例）
    sourceSets {
        getByName("main") {
            java.srcDir(jscJniJavaDir)
        }
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}
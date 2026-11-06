import org.gradle.internal.os.OperatingSystem

plugins {
    id("com.android.application")
}

// O motor (C++ + Vulkan) é compilado pelo CMake do NDK; os shaders GLSL viram SPIR-V pelo glslc do NDK e entram no
// APK junto com os arquivos do jogo (personagem, texturas, fontes).
val engineDir = rootProject.projectDir.parentFile  // .../engine

android {
    namespace = "com.treta.penalti"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.treta.penalti"
        minSdk = 30          // Android 11+: Vulkan 1.3 nos aparelhos que têm
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += listOf("arm64-v8a") }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared", "-DANDROID_PLATFORM=android-30")
                cppFlags += listOf("-std=c++20", "-fexceptions", "-frtti")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")  // instala direto, sem loja
        }
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }
    sourceSets["main"].assets.srcDirs(layout.buildDirectory.dir("generated/penaltiAssets"))
    packaging { jniLibs.useLegacyPackaging = true }
}

// SPIR-V dos shaders + arquivos do jogo para a pasta de assets gerada.
val prepareAssets by tasks.registering {
    val outDir = layout.buildDirectory.dir("generated/penaltiAssets").get().asFile
    inputs.dir(engineDir.resolve("shaders"))
    inputs.dir(engineDir.resolve("game"))
    outputs.dir(outDir)
    doLast {
        val shaderOut = outDir.resolve("shaders").apply { mkdirs() }
        val host = when {
            OperatingSystem.current().isWindows -> "windows-x86_64"
            OperatingSystem.current().isMacOsX -> "darwin-x86_64"
            else -> "linux-x86_64"
        }
        val glslc = android.ndkDirectory.resolve("shader-tools/$host/glslc" + if (OperatingSystem.current().isWindows) ".exe" else "")
        engineDir.resolve("shaders").listFiles { f -> f.extension in setOf("vert", "frag", "comp", "task", "mesh") }!!.forEach { src ->
            exec {
                commandLine(glslc.absolutePath, "--target-env=vulkan1.3", "-O", src.absolutePath, "-o", shaderOut.resolve(src.name + ".spv").absolutePath)
            }
        }
        copy {
            from(engineDir.resolve("game"))
            into(outDir.resolve("game"))
        }
    }
}
tasks.named("preBuild") { dependsOn(prepareAssets) }

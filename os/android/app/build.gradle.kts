// LibertyRecomp Android — :app module.
//
// Gradle never runs CMake. The native side (libmain.so, librexruntime.so,
// the GPU plugins, libSDL3.so, libc++_shared.so) is millions of lines of
// recompiled PowerPC and is built by os/android/scripts/build_native.sh,
// which stages the libraries into src/main/jniLibs and the title resources
// into src/main/assets/Resources. This module only packages them.
//
// The SDL Java classes come from the same SDL3 tree the native library is
// built from: SDLActivity refuses to start when the versions differ.

plugins {
    id("com.android.application")
}

val sdlJavaDir = rootProject.file(
    "../../glue/rexglue-sdk-main/thirdparty/sdl3/android-project/app/src/main/java")

val isAlt = project.hasProperty("alt") || System.getenv("ALT_BUILD") == "1"
val altKeystore = file("alt.keystore")

android {
    namespace   = "com.libertyrecomp"
    compileSdk  = 35
    ndkVersion  = "30.0.16248370"

    defaultConfig {
        applicationId = if (isAlt) "com.libertyrecomp.alt" else "com.libertyrecomp"
        // 28: ASharedMemory (26) for guest memory and AAudio (27) for audio.
        minSdk        = 29
        targetSdk     = 35
        versionCode   = 18
        versionName   = if (isAlt) "0.5.9.3_alt_modded" else "0.5.9.3_modded"

        manifestPlaceholders["appName"] = if (isAlt) "Liberty Recompiled Alt" else "Liberty Recompiled"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    signingConfigs {
        if (altKeystore.exists()) {
            create("altKey") {
                storeFile     = altKeystore
                storePassword = "libertyrecomp"
                keyAlias      = "liberty_alt"
                keyPassword   = "libertyrecomp"
            }
        }
    }

    sourceSets["main"].java.srcDirs("src/main/java", sdlJavaDir)

    buildTypes {
        debug {
            isDebuggable    = true
            isJniDebuggable = true
            isMinifyEnabled = false
            if (isAlt && altKeystore.exists()) {
                signingConfig = signingConfigs.getByName("altKey")
            }
        }
        release {
            isMinifyEnabled = false
            if (isAlt && altKeystore.exists()) {
                signingConfig = signingConfigs.getByName("altKey")
            } else {
                signingConfig = signingConfigs.getByName("debug")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            // Unpack the libraries into nativeLibraryDir: the runtime finds its
            // GPU plugins by real path next to itself, and a Turnip driver
            // loaded through libadrenotools needs real files as well.
            useLegacyPackaging = false
            // build_native.sh decides what is stripped.
            if (!isAlt && !project.hasProperty("strip")) {
                keepDebugSymbols += "**/*.so"
            }
        }
    }

    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }
}

tasks.withType<JavaCompile> {
    options.encoding = "UTF-8"
}

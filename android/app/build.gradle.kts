plugins { id("com.android.application") }

android {
    namespace = "ch.plum.theforest"
    compileSdk = 35
    ndkVersion = "27.3.13750724"   // the NDK tools/build_android_so.sh uses
    buildToolsVersion = "35.0.0"
    defaultConfig {
        applicationId = "ch.plum.theforest"
        minSdk = 29
        targetSdk = 35
        versionCode = 3
        versionName = "0.2.1"
        ndk { abiFilters += "arm64-v8a" }
    }
    // libmain.so (runtime + lifted game code + Mono + DXVK) and libSDL3.so are prebuilt with the NDK
    // from the player's own The Forest files by tools/build_android_so.sh.
    packaging { jniLibs { useLegacyPackaging = true } }
    // Release signing: your own key via TF_KEYSTORE (or HP_KEYSTORE) (+ _PASSWORD, _KEY_ALIAS, _KEY_PASSWORD),
    // otherwise the SDK debug key, which is fine for sideloading.
    val ks = System.getenv("TF_KEYSTORE") ?: System.getenv("HP_KEYSTORE")
    signingConfigs {
        if (ks != null) create("own") {
            storeFile = file(ks)
            storePassword = System.getenv("TF_KEYSTORE_PASSWORD") ?: System.getenv("HP_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("TF_KEY_ALIAS") ?: System.getenv("HP_KEY_ALIAS")
            keyPassword = System.getenv("TF_KEY_PASSWORD") ?: System.getenv("HP_KEY_PASSWORD")
        }
    }
    buildTypes {
        getByName("debug") { isDebuggable = true }
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (ks != null) "own" else "debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

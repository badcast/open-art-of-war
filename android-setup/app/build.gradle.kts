plugins {
    id("com.android.application")
}

android {
    namespace = "org.openartofwar"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.openartofwar"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            abiFilters.addAll(listOf("arm64-v8a", "x86_64"))
        }

        externalNativeBuild {
            cmake {
                arguments.addAll(listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_BUILD_TYPE=Release"
                ))
                cppFlags.addAll(listOf("-std=c++17", "-O2", "-ffp-contract=off"))
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1+"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            // Do not put assets.blob here: libarchive opens it by file path, which cannot read inside an APK
        }
    }
}

dependencies {
    // SDL3 Android AAR library if available via Maven or submodule:
    // implementation("org.libsdl:SDL3:3.2.0")
}

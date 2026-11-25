#!/usr/bin/env bash
# Red Two para Android (arm64-v8a) en Linux: lo mismo que tools/build_android.ps1, en bash.
# Corre dentro de la imagen redtwo-android (Dockerfile), con ~/Red-Two montado en /work.
#   build_android.sh --disc /work/GSWE64.rvz [--env "R2_WATCHDOG=5;R2_NO_PRECOMPILE=1"] [--version 1.0.0] [--code 1]
# Resultado: /work/out/RedTwo-android-arm64.apk
set -euo pipefail
src=/work/red_two
disc=""; envs=""; vname="1.0.0"; vcode=1; config=RelWithDebInfo
while [ $# -gt 0 ]; do
    case "$1" in
    --disc) disc="$(realpath "$2")"; shift 2 ;;
    --env) envs="$2"; shift 2 ;;
    --version) vname="$2"; shift 2 ;;
    --code) vcode="$2"; shift 2 ;;
    *) echo "argumento desconocido: $1" >&2; exit 2 ;;
    esac
done
[ -f "$src/CMakeLists.txt" ] || { echo "no está el repo en $src (git clone --recursive ...)" >&2; exit 1; }
[ -n "$disc" ] && [ -f "$disc" ] || { echo "falta --disc <tu GSWE64 .rvz/.iso>" >&2; exit 1; }
sdk=$ANDROID_HOME; ndk=$ANDROID_NDK_HOME; bt=$sdk/build-tools/35.0.0
jar=$sdk/platforms/android-35/android.jar
minSdk=29; targetSdk=35
host=/work/build/host; build=/work/build/android; out=/work/out
mkdir -p "$out"

# submódulos y parches de Aurora (como tools/build_linux.sh)
git config --global --add safe.directory '*'
if [ ! -f "$src/extern/aurora/CMakeLists.txt" ] || [ ! -f "$src/extern/rcheevos/include/rc_client.h" ]; then
    git -C "$src" submodule update --init --recursive
fi
aurora="$src/extern/aurora"; clean=0
[ -z "$(git -C "$aurora" status --porcelain --untracked-files=no)" ] && clean=1
for patch in $(ls "$src"/patches/aurora/*.patch 2>/dev/null | sort); do
    if [ $clean -eq 0 ] && git -C "$aurora" apply --reverse --check "$patch" 2>/dev/null; then continue; fi
    if git -C "$aurora" apply "$patch" 2>/dev/null; then echo "aplicado $(basename "$patch")"; continue; fi
    if [ $clean -eq 1 ]; then echo "no se pudo aplicar $(basename "$patch")" >&2; exit 1; fi
    echo "nota: $(basename "$patch") se asume aplicado"
done

# 1) el recompilador para esta máquina (x86_64)
if [ ! -x "$host/r2recomp" ]; then
    cmake -S "$src" -B "$host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
    cmake --build "$host" --target r2recomp
fi

# 2) libmain.so con el NDK (el código del juego se genera desde tu disco)
if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=$config \
        -DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$minSdk -DANDROID_STL=c++_static \
        -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
        "-DCMAKE_CXX_FLAGS=-fexperimental-library -include $src/android/compat/libcxx_compat.h" \
        -DR2_HOST_RECOMP=$host/r2recomp -DR2_DOL="$disc" \
        -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static \
        -DAURORA_DAWN_PROVIDER=auto -DAURORA_DAWN_LINKAGE=static
fi
cmake --build "$build" --target red_two
so=$build/libmain.so; [ -f "$so" ] || { echo "no se generó $so" >&2; exit 1; }

# 3) APK sin Gradle: javac + d8, aapt2, zipalign, apksigner
stage=$build/apk; rm -rf "$stage"
mkdir -p "$stage/classes" "$stage/dex" "$stage/assets"
$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-debug -o "$stage/libmain.so" "$so"
sdlJava=$(sed -n 's/^AURORA_SDL3_JAVA_SOURCE_DIR:INTERNAL=//p' "$build/CMakeCache.txt")
auroraJava=$(sed -n 's/^AURORA_ANDROID_JAVA_SOURCE_DIR:INTERNAL=//p' "$build/CMakeCache.txt")
find "$sdlJava" "$auroraJava" "$src/android/java" -name '*.java' > "$stage/sources.txt"
javac -source 11 -target 11 -Xlint:-options -nowarn -encoding UTF-8 -classpath "$jar" -d "$stage/classes" @"$stage/sources.txt"
jar cf "$stage/classes.jar" -C "$stage/classes" .
$bt/d8 --release --min-api $minSdk --lib "$jar" --output "$stage/dex" "$stage/classes.jar"
cp "$src/LICENSE" "$stage/assets/LICENSE.txt"
python3 -I "$src/tools/third_party_notices.py" "$build" "$stage/assets/THIRD_PARTY_NOTICES.txt" || echo "nota: sin THIRD_PARTY_NOTICES"

manifest=$src/android/AndroidManifest.xml
if [ -n "$envs" ]; then   # variables de entorno para la app (meta-data SDL_ENV.*): diagnóstico
    meta=""
    IFS=';' read -ra kv <<< "$envs"
    for p in "${kv[@]}"; do
        [ -z "$p" ] && continue
        meta+="        <meta-data android:name=\"SDL_ENV.${p%%=*}\" android:value=\"${p#*=}\" />\n"
    done
    perl -pe "s#    </application>#${meta}    </application>#" "$manifest" > "$stage/AndroidManifest.xml"
    manifest=$stage/AndroidManifest.xml
    echo "entorno de la app: $envs"
fi
$bt/aapt2 compile --dir "$src/android/res" -o "$stage/res.zip"
$bt/aapt2 link -o "$stage/base.apk" -I "$jar" --manifest "$manifest" \
    --min-sdk-version $minSdk --target-sdk-version $targetSdk \
    --version-code $vcode --version-name "$vname" -A "$stage/assets" "$stage/res.zip"
mkdir -p "$stage/zip/lib/arm64-v8a"
cp "$stage/dex/classes.dex" "$stage/zip/"; cp "$stage/libmain.so" "$stage/zip/lib/arm64-v8a/"
(cd "$stage/zip" && zip -q -9 "$stage/base.apk" classes.dex lib/arm64-v8a/libmain.so)
$bt/zipalign -f -p 4 "$stage/base.apk" "$stage/aligned.apk"

# firma: la clave vive en /work/keystore (guárdala: Android solo actualiza con la misma)
ks=/work/keystore/redtwo-release.jks; pw=/work/keystore/redtwo-release.password.txt
mkdir -p /work/keystore
if [ ! -f "$ks" ]; then
    [ -f "$pw" ] || head -c 24 /dev/urandom | base64 | tr '/+' '_-' | tr -d '\n' > "$pw"
    keytool -genkeypair -keystore "$ks" -alias redtwo -keyalg RSA -keysize 4096 -validity 36500 \
        -dname "CN=Red Two" -storepass:file "$pw" -keypass:file "$pw"
fi
$bt/apksigner sign --ks "$ks" --ks-key-alias redtwo --ks-pass "file:$pw" --out "$out/RedTwo-android-arm64.apk" "$stage/aligned.apk"
$bt/apksigner verify "$out/RedTwo-android-arm64.apk"
ls -lh "$out/RedTwo-android-arm64.apk"

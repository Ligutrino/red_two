# Android build on Linux (Docker)

Builds `RedTwo-android-arm64.apk` on a Linux PC with Docker (no Android Studio, no Gradle).
As for every Red Two build, the game is recompiled from **your own disc image** (USA, GSWE64
rev 0): the APK contains code generated from it and is yours alone, never share it.

Layout (one work folder, mounted as `/work` in the container):

```
~/Red-Two/
  red_two/        this repository
  GSWE64.iso      your disc image (outside the repository)
  keystore/       signing key, created on the first build (keep it: updates need the same key)
  build/ out/     build files and the APK
```

```bash
cd ~/Red-Two
git clone --recursive <this repository> red_two
docker build -t redtwo-android red_two/tools/android-docker
docker run --rm -it --user $(id -u):$(id -g) -e HOME=/tmp -v ~/Red-Two:/work redtwo-android \
    bash /work/red_two/tools/android-docker/build_android.sh --disc /work/GSWE64.iso
adb install -r out/RedTwo-android-arm64.apk
```

Options: `--env "R2_WATCHDOG=5;R2_PAD_LOG=1"` (debug variables read at start), `--version`, `--code`.
Logs: `adb logcat -s RedTwo`.

# Imagen para compilar Red Two para Android (arm64-v8a) en Linux.
#   docker build -t redtwo-android .
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

# Compiladores y herramientas; dependencias de SDL3 para el build del recompilador del host
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip zip git python3 perl \
      clang lld cmake ninja-build pkg-config \
      openjdk-17-jdk-headless \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev \
      libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev \
      libdrm-dev libgbm-dev libvulkan-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*

# Android SDK: herramientas de línea de comandos, plataforma 35, build-tools y NDK r27
ENV ANDROID_HOME=/opt/android-sdk
RUN mkdir -p $ANDROID_HOME/cmdline-tools \
 && curl -fsSL -o /tmp/clt.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
 && unzip -q /tmp/clt.zip -d $ANDROID_HOME/cmdline-tools \
 && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
 && rm /tmp/clt.zip \
 && yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses > /dev/null \
 && $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager \
      "platforms;android-35" "build-tools;35.0.0" "ndk;27.3.13750724" \
 && chmod -R a+rX $ANDROID_HOME
ENV ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.3.13750724
ENV JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64

WORKDIR /work

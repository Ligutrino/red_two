# Red Two para Android: compilar en Linux con Docker

Genera `RedTwo-android-arm64.apk` (arm64-v8a, Android 10 o superior, Vulkan) en un PC con Linux
y Docker. No hace falta Android Studio ni Gradle: el SDK y el NDK quedan dentro de la imagen.

> **Sin datos del juego.** Este repositorio no trae nada del disco. Como en todo build de Red
> Two, el juego se recompila desde **tu propia imagen** de *Star Wars Rogue Squadron II: Rogue
> Leader* (USA, `GSWE64`, revisión 0; `.iso`, `.gcm`, `.rvz` o `.wia`). El APK resultante
> contiene código generado desde tu disco: es solo tuyo, **no lo compartas**.

Probado en Linux Mint 22 (x86_64) con un Galaxy Tab S9 (Snapdragon 8 Gen 2).

## 1. Requisitos (una vez)

- Linux x86_64 con unos **25 GB libres** y conexión a internet (la imagen descarga el SDK y el NDK).
- Docker y adb:

```bash
sudo apt install docker.io adb git
sudo usermod -aG docker $USER      # luego cierra sesión y vuelve a entrar
```

- En la tablet o el teléfono: *Opciones de desarrollador* > *Depuración USB* activada. Conéctalo
  por USB y acepta el aviso; `adb devices` debe mostrarlo como `device`.

## 2. Carpeta de trabajo

Todo vive en una carpeta, que el contenedor ve como `/work`. El repositorio **tiene que llamarse
`red_two`** dentro de ella:

```
~/Red-Two/
  red_two/        este repositorio (rama android)
  GSWE64.iso      tu imagen del disco (fuera del repositorio)
  keystore/       clave de firma, se crea en el primer build
  build/  out/    archivos de compilación y el APK
```

```bash
mkdir -p ~/Red-Two && cd ~/Red-Two
git clone --recursive -b android https://github.com/Ligutrino/red_two.git red_two
cp /ruta/a/tu/GSWE64.iso ~/Red-Two/GSWE64.iso
```

## 3. Imagen de Docker (una vez, unos 10 minutos)

```bash
docker build -t redtwo-android ~/Red-Two/red_two/tools/android-docker
```

## 4. Compilar

```bash
docker run --rm -it --user $(id -u):$(id -g) -e HOME=/tmp -v ~/Red-Two:/work redtwo-android \
    bash /work/red_two/tools/android-docker/build_android.sh --disc /work/GSWE64.iso
```

El primer build tarda bastante (recompila el juego y compila Dawn); los siguientes solo lo que
cambió. Termina con `-rw-r--r-- ... 22M ... /work/out/RedTwo-android-arm64.apk`.

Opciones: `--version 1.0.1 --code 2` (para instalar encima de una versión anterior, sube `--code`),
`--env "R2_WATCHDOG=5;R2_PAD_LOG=1"` (variables de diagnóstico que se leen al iniciar).

## 5. Instalar y jugar

```bash
adb install -r ~/Red-Two/out/RedTwo-android-arm64.apk
```

En el primer inicio el launcher pide el disco: **Choose the file** abre el selector de Android;
elige tu imagen (puede estar en Descargas, una tarjeta SD o un USB). Red Two la recuerda. La
imagen tiene que estar también en el dispositivo: cópiala, por ejemplo, con
`adb push ~/Red-Two/GSWE64.iso /sdcard/Download/`.

## Actualizar

```bash
cd ~/Red-Two/red_two && git pull --recurse-submodules
# y vuelve a compilar (paso 4) e instalar (paso 5)
```

## Problemas

- **Log:** `adb logcat -s RedTwo` (también queda en `Android/data/io.github.fhclimited.redtwo/files/logs/`).
- **`INSTALL_FAILED_UPDATE_INCOMPATIBLE`:** el APK se firmó con otra clave. Usa siempre la misma
  `~/Red-Two/keystore` (respáldala), o desinstala la versión anterior (se pierden sus partidas
  si no las exportas antes).
- **`permission denied` en `/work/...`:** alguna carpeta quedó de root por correr Docker sin
  `--user`; arréglalo con `sudo chown -R $USER: ~/Red-Two`.
- **Control Bluetooth que se queda pegado en una dirección:** es el enlace Bluetooth del control,
  no Red Two; por cable no pasa.

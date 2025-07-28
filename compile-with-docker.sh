#!/bin/sh

IMAGE_NAME="uvk5"
FIRMWARE_DIR="${PWD}/compiled-firmware"

# Create firmware output directory if it doesn't exist
mkdir -p "$FIRMWARE_DIR"

# Clean previously compiled firmware files
rm -f "$FIRMWARE_DIR"/*

# Build image only if it doesn't already exist
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "Docker image '$IMAGE_NAME' not found, building..."
    if ! docker build --build-arg BUILDPLATFORM=linux/amd64 -t "$IMAGE_NAME" .; then
        echo "❌ Failed to build docker image"
        exit 1
    fi
fi

# ------------------ BUILD VARIANTS ------------------

custom() {
    echo "🔧 Custom compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        EDITION_STRING=Custom \
        TARGET=f4hwn.custom \
        && mv f4hwn.custom* compiled-firmware/ \
        && make -s clean"
}

standard() {
    echo "📦 Standard compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        ENABLE_SPECTRUM=0 \
        ENABLE_FMRADIO=0 \
        ENABLE_AIRCOPY=0 \
        ENABLE_NOAA=0 \
        EDITION_STRING=Standard \
        TARGET=f4hwn.standard \
        && mv f4hwn.standard* compiled-firmware/ \
        && make -s clean"
}

bandscope() {
    echo "📺 Bandscope compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        ENABLE_SPECTRUM=1 \
        ENABLE_FMRADIO=0 \
        ENABLE_VOX=0 \
        ENABLE_AIRCOPY=1 \
        ENABLE_FEAT_F4HWN_SCREENSHOT=1 \
        ENABLE_FEAT_F4HWN_GAME=0 \
        ENABLE_FEAT_F4HWN_PMR=1 \
        ENABLE_FEAT_F4HWN_GMRS_FRS_MURS=1 \
        ENABLE_NOAA=0 \
        ENABLE_FEAT_F4HWN_RESCUE_OPS=0 \
        EDITION_STRING=Bandscope \
        TARGET=f4hwn.bandscope \
        && mv f4hwn.bandscope* compiled-firmware/ \
        && make -s clean"
}

broadcast() {
    echo "📻 Broadcast compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        ENABLE_SPECTRUM=0 \
        ENABLE_FMRADIO=1 \
        ENABLE_VOX=1 \
        ENABLE_AIRCOPY=1 \
        ENABLE_FEAT_F4HWN_SCREENSHOT=1 \
        ENABLE_FEAT_F4HWN_GAME=0 \
        ENABLE_FEAT_F4HWN_PMR=1 \
        ENABLE_FEAT_F4HWN_GMRS_FRS_MURS=1 \
        ENABLE_NOAA=0 \
        ENABLE_FEAT_F4HWN_RESCUE_OPS=0 \
        EDITION_STRING=Broadcast \
        TARGET=f4hwn.broadcast \
        && mv f4hwn.broadcast* compiled-firmware/ \
        && make -s clean"
}

basic() {
    echo "☘️ Basic compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        ENABLE_SPECTRUM=1 \
        ENABLE_FMRADIO=1 \
        ENABLE_VOX=0 \
        ENABLE_AIRCOPY=0 \
        ENABLE_FEAT_F4HWN_GAME=0 \
        ENABLE_FEAT_F4HWN_SPECTRUM=0 \
        ENABLE_FEAT_F4HWN_PMR=1 \
        ENABLE_FEAT_F4HWN_GMRS_FRS_MURS=1 \
        ENABLE_NOAA=0 \
        ENABLE_AUDIO_BAR=0 \
        ENABLE_FEAT_F4HWN_RESUME_STATE=0 \
        ENABLE_FEAT_F4HWN_CHARGING_C=0 \
        ENABLE_FEAT_F4HWN_INV=1 \
        ENABLE_FEAT_F4HWN_CTR=0 \
        ENABLE_FEAT_F4HWN_NARROWER=0 \
        ENABLE_FEAT_F4HWN_RESCUE_OPS=0 \
        EDITION_STRING=Basic \
        TARGET=f4hwn.basic \
        && mv f4hwn.basic* compiled-firmware/ \
        && make -s clean"
}

rescueops() {
    echo "🚨 RescueOps compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        ENABLE_SPECTRUM=0 \
        ENABLE_FMRADIO=0 \
        ENABLE_VOX=1 \
        ENABLE_AIRCOPY=1 \
        ENABLE_FEAT_F4HWN_SCREENSHOT=1 \
        ENABLE_FEAT_F4HWN_GAME=0 \
        ENABLE_FEAT_F4HWN_PMR=1 \
        ENABLE_FEAT_F4HWN_GMRS_FRS_MURS=1 \
        ENABLE_NOAA=1 \
        ENABLE_FEAT_F4HWN_RESCUE_OPS=1 \
        EDITION_STRING=RescueOps \
        TARGET=f4hwn.rescueops \
        && mv f4hwn.rescueops* compiled-firmware/ \
        && make -s clean"
}

game() {
    echo "🎮 Game compilation..."
    docker run --rm -v "${PWD}:/app" --user $(id -u):$(id -g) "$IMAGE_NAME" /bin/bash -c "\
        cd /app && git submodule update --init --recursive && make -s clean && make -s \
        ENABLE_SPECTRUM=0 \
        ENABLE_FMRADIO=1 \
        ENABLE_VOX=0 \
        ENABLE_AIRCOPY=1 \
        ENABLE_FEAT_F4HWN_GAME=1 \
        ENABLE_FEAT_F4HWN_PMR=1 \
        ENABLE_FEAT_F4HWN_GMRS_FRS_MURS=1 \
        ENABLE_NOAA=0 \
        ENABLE_FEAT_F4HWN_RESCUE_OPS=0 \
        EDITION_STRING=Game \
        TARGET=f4hwn.game \
        && mv f4hwn.game* compiled-firmware/ \
        && make -s clean"
}

# ------------------ MENU ------------------

case "$1" in
    custom) custom ;;
    standard) standard ;;
    bandscope) bandscope ;;
    broadcast) broadcast ;;
    basic) basic ;;
    rescueops) rescueops ;;
    game) game ;;
    all)
        bandscope
        broadcast
        basic
        rescueops
        game
        ;;
    *)
        echo "Usage: $0 {custom|standard|bandscope|broadcast|basic|rescueops|game|all}"
        exit 1
        ;;
esac

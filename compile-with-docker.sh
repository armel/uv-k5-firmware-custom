#!/bin/sh
#export DOCKER_DEFAULT_PLATFORM=linux/amd64
IMAGE_NAME="uvk5"
DEST_DIR="compiled-firmware"
docker build -t $IMAGE_NAME .
docker run --rm -v "${PWD}/${DEST_DIR}:/app/${DEST_DIR}" $IMAGE_NAME \
  /bin/bash -c "rm ./${DEST_DIR}/*; cd /app && make && cp f4hwn* ${DEST_DIR}/"

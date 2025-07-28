@echo off
docker build -t uvk5 .
docker run --rm -v %CD%:/app uvk5 /bin/bash -c "cd /app && git submodule update --init --recursive && make -s clean && make -s"
pause

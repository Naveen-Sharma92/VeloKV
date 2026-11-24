# Builds VeloKV in one image and copies only the binary into a small runtime
# image. Both stages use Ubuntu 22.04, the same compiler (GCC 11) and standard
# library the project is developed and tested with, so the binary's libstdc++
# matches at run time.
#
#   docker build -t velokv .
#   docker run --rm -p 6380:6380 velokv
#   docker run -d -p 6380:6380 -v velokv-data:/data velokv   # keep the AOF across containers

FROM ubuntu:22.04 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
# Tests need GoogleTest from the network and run in CI, so the image skips them.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVELOKV_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)"

FROM ubuntu:22.04
# A non-root user, and /data for the append-only file.
RUN useradd --system --no-create-home velokv \
    && mkdir /data && chown velokv /data
COPY --from=build /src/build/velokv /usr/local/bin/velokv
USER velokv
VOLUME /data
EXPOSE 6380
# Exec form: velokv runs as PID 1 and receives `docker stop`'s SIGTERM directly,
# so it shuts down cleanly and fsyncs the AOF. 0.0.0.0, because inside a
# container 127.0.0.1 is unreachable from the host.
CMD ["velokv", "--bind", "0.0.0.0", "--port", "6380", "--aof", "/data/velokv.aof", "--fsync", "everysec"]

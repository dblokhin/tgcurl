# tgcurl — Docker image (Debian-based, fully version-pinned).
#
# There is no prebuilt TDLib package in the Debian repositories (the only
# packaged distribution channel is the Fedora copr), so the build stage
# compiles TDLib from source at a PINNED commit — the exact commit the copr
# 1.8.63 package is built from — and the result is reproducible: same base
# image tag, same TDLib ref, same tgcurl sources in, same image out.
#
#   docker build -t tgcurl .
#   docker run -it --rm -v ~/tgcurl-data:/data tgcurl login
#   docker run     --rm -v ~/tgcurl-data:/data tgcurl send <chat_id> "hi"
#
# Note: compiling TDLib is demanding — give the build ~4 GB of RAM per job.

# Debian point-release tag, not a moving "bookworm-slim"/"12-slim": within a
# pinned point release the toolchain versions are frozen by Debian stable.
ARG DEBIAN_TAG=12.11-slim

# ---- Stage 1: build TDLib + tgcurl ------------------------------------------
FROM debian:${DEBIAN_TAG} AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake ninja-build gperf ca-certificates wget \
      libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# TDLib >= 1.8.63 is mandatory: Telegram rejects login on older MTProto layers
# with 406 UPDATE_APP_TO_LOGIN (see DESIGN.md -> Build). Pinned to commit
# 1677a0c — the source of the Fedora copr tdlib-1.8.63 package tgcurl is
# developed against. Bump deliberately:  --build-arg TDLIB_REF=<sha>
ARG TDLIB_REF=1677a0c
RUN wget -qO /td.tar.gz "https://github.com/tdlib/td/archive/${TDLIB_REF}.tar.gz" \
    && mkdir /td && tar -xzf /td.tar.gz -C /td --strip-components=1 && rm /td.tar.gz
RUN cmake -S /td -B /td/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /td/build --target install \
    && rm -rf /td/build

# tgcurl itself. TDLib is linked statically (Td::TdStatic) and the C++ runtime
# is bundled (TGCURL_STATIC); OpenSSL/zlib stay dynamic and are provided by the
# identical Debian base in the runtime stage. Only the tgcurl target is built —
# the test executables are not part of the image.
COPY CMakeLists.txt /src/
COPY src /src/src
COPY tests /src/tests
ARG TGCURL_VERSION=0.1.0
RUN cmake -S /src -B /src/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DTGCURL_VERSION="${TGCURL_VERSION}" \
      -DTGCURL_STATIC=ON \
    && cmake --build /src/build --target tgcurl \
    && strip /src/build/tgcurl

# ---- Stage 2: runtime --------------------------------------------------------
FROM debian:${DEBIAN_TAG}

# Runtime deps of the binary: OpenSSL and zlib (zlib is already in the base).
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 \
    && rm -rf /var/lib/apt/lists/*

# Run unprivileged; /data holds config.json + the TDLib session database
# (TGCURL_CONFIG_DIR), so it is the one path that must persist across runs.
RUN useradd --uid 1000 --user-group --home-dir /data --shell /usr/sbin/nologin tgcurl \
    && mkdir -p /data && chown tgcurl:tgcurl /data

COPY --from=build /src/build/tgcurl /usr/local/bin/tgcurl

USER tgcurl
ENV TGCURL_CONFIG_DIR=/data
VOLUME /data

ENTRYPOINT ["/usr/local/bin/tgcurl"]
# No arguments -> report the session state (harmless, shows the image works).
CMD ["status"]

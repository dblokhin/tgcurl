# tgcurl — Docker image.
#
# Two-stage build keeps the shipped image small: the heavy stage compiles
# TDLib from source and links tgcurl fully statically (musl), the runtime
# stage is bare Alpine (~8 MB) plus that one binary. The session lives in
# /data (TGCURL_CONFIG_DIR) — mount a volume there or the login is lost when
# the container exits.
#
#   docker build -t tgcurl .
#   docker run -it --rm -v tgcurl-data:/data tgcurl login
#   docker run     --rm -v tgcurl-data:/data tgcurl send <chat_id> "hi"
#
# Note: compiling TDLib is demanding — give the build ~4 GB of RAM per job.

ARG ALPINE_VERSION=3.22

# ---- Stage 1: build TDLib + tgcurl ------------------------------------------
FROM alpine:${ALPINE_VERSION} AS build

RUN apk add --no-cache \
    build-base cmake ninja gperf linux-headers \
    openssl-dev openssl-libs-static zlib-dev zlib-static

# TDLib >= 1.8.63 is mandatory: Telegram rejects login on older MTProto layers
# with 406 UPDATE_APP_TO_LOGIN (see DESIGN.md -> Build). TDLib tags releases
# rarely, so we build from master by default; pin a known-good commit or tag
# for a reproducible image:  docker build --build-arg TDLIB_REF=<sha> .
# (A source tarball works for a branch, tag or commit alike — no git needed.)
ARG TDLIB_REF=master
RUN wget -qO /td.tar.gz "https://github.com/tdlib/td/archive/${TDLIB_REF}.tar.gz" \
    && mkdir /td && tar -xzf /td.tar.gz -C /td --strip-components=1 && rm /td.tar.gz
RUN cmake -S /td -B /td/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /td/build --target install \
    && rm -rf /td/build

# tgcurl itself. -static against musl yields a fully self-contained binary
# (TDLib/OpenSSL/zlib are static archives already). Only the tgcurl target is
# built — the test executables are not part of the image.
COPY CMakeLists.txt /src/
COPY src /src/src
COPY tests /src/tests
ARG TGCURL_VERSION=0.1.0
RUN cmake -S /src -B /src/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DTGCURL_VERSION="${TGCURL_VERSION}" \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
    && cmake --build /src/build --target tgcurl \
    && strip /src/build/tgcurl

# ---- Stage 2: runtime --------------------------------------------------------
FROM alpine:${ALPINE_VERSION}

# Run unprivileged; /data holds config.json + the TDLib session database
# (TGCURL_CONFIG_DIR), so it is the one path that must persist across runs.
RUN adduser -D -u 1000 tgcurl && mkdir -p /data && chown tgcurl:tgcurl /data

COPY --from=build /src/build/tgcurl /usr/local/bin/tgcurl

USER tgcurl
ENV TGCURL_CONFIG_DIR=/data
VOLUME /data

ENTRYPOINT ["/usr/local/bin/tgcurl"]
# No arguments -> report the session state (harmless, shows the image works).
CMD ["status"]

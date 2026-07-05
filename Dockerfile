# tgcurl — Docker image.
#
# TDLib comes from a PREBUILT, version-pinned RPM — no TDLib compilation in
# the image build. Debian would be the preferred base, but no Debian/Ubuntu
# repository ships a TDLib package at all; the only packaged channel is the
# Fedora copr `stevenlin/tdlib-master`, so the image is Fedora-based. The
# build stage installs the pinned tdlib-static RPM and compiles only tgcurl
# itself (seconds, not the hour a TDLib build takes); the runtime stage is the
# same pinned Fedora tag plus the single static-TDLib binary.
#
#   docker build -t tgcurl .
#   docker run -it --rm -v ~/tgcurl-data:/data tgcurl login
#   docker run     --rm -v ~/tgcurl-data:/data tgcurl send <chat_id> "hi"

# Pinned release tag, not "latest": keeps glibc/OpenSSL identical between the
# stages and stable across rebuilds.
ARG FEDORA_TAG=44

# ---- Stage 1: build tgcurl against the packaged TDLib ------------------------
FROM fedora:${FEDORA_TAG} AS build

# Exact NEVR of the copr TDLib build (>= 1.8.63 is mandatory: Telegram rejects
# older MTProto layers with 406 UPDATE_APP_TO_LOGIN, see DESIGN.md -> Build).
# Pinning the full version keeps the image reproducible even as the copr moves;
# bump deliberately:  --build-arg TDLIB_PKG_VERSION=<new NEVR>
ARG TDLIB_PKG_VERSION=1.8.63-1.20260418.git1677a0c.fc44

RUN dnf -y install dnf5-plugins \
    && dnf -y copr enable stevenlin/tdlib-master \
    && dnf -y install \
         gcc-c++ cmake ninja-build openssl-devel zlib-devel \
         "tdlib-devel-${TDLIB_PKG_VERSION}" \
         "tdlib-static-${TDLIB_PKG_VERSION}" \
    && dnf clean all

# TDLib is linked statically (Td::TdStatic from tdlib-static); the C++
# runtime, OpenSSL and zlib stay dynamic and come from the identical Fedora
# base in the runtime stage. Only the tgcurl target is built — the test
# executables are not part of the image.
COPY CMakeLists.txt /src/
COPY src /src/src
COPY tests /src/tests
ARG TGCURL_VERSION=0.1.0
RUN cmake -S /src -B /src/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DTGCURL_VERSION="${TGCURL_VERSION}" \
    && cmake --build /src/build --target tgcurl \
    && strip /src/build/tgcurl

# ---- Stage 2: runtime --------------------------------------------------------
FROM fedora:${FEDORA_TAG}

# Run unprivileged; /data holds config.json + the TDLib session database
# (TGCURL_CONFIG_DIR), so it is the one path that must persist across runs.
# The binary's runtime deps (openssl-libs, zlib) are already in the base.
RUN useradd --uid 1000 --user-group --home-dir /data --shell /sbin/nologin tgcurl \
    && mkdir -p /data && chown tgcurl:tgcurl /data

COPY --from=build /src/build/tgcurl /usr/local/bin/tgcurl

USER tgcurl
ENV TGCURL_CONFIG_DIR=/data
VOLUME /data

ENTRYPOINT ["/usr/local/bin/tgcurl"]
# No arguments -> report the session state (harmless, shows the image works).
CMD ["status"]

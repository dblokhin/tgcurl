# tgcurl — developer Makefile.
#
# This is a thin, convenient wrapper around CMake (which does the real build).
# Common workflow:
#   make deps      # install build prerequisites (Fedora/Debian)
#   make build     # configure + compile into build/
#   make test      # run the test suite via ctest
#   make static    # build a self-contained binary (statically linked deps)
#   make release   # build static binary + produce .rpm and .deb packages
#   sudo make install
#
# Override defaults on the command line, e.g.:
#   make build BUILD_TYPE=Debug
#   make release VERSION=0.2.0

# ----------------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------------
NAME        := tgcurl
VERSION     ?= 0.1.0
BUILD_TYPE  ?= Release
BUILD_DIR   ?= build
DIST_DIR    ?= dist
PREFIX      ?= /usr/local
JOBS        ?= $(shell nproc 2>/dev/null || echo 2)

BIN         := $(BUILD_DIR)/$(NAME)
CMAKE_FLAGS ?=

# ----------------------------------------------------------------------------
# Phony targets
# ----------------------------------------------------------------------------
.PHONY: all help deps configure build static test clean distclean \
        install uninstall release rpm deb check-tools version

all: build

help:
	@echo "tgcurl make targets:"
	@echo "  deps       Install build prerequisites (needs sudo; detects dnf/apt)"
	@echo "  build      Configure and compile (BUILD_TYPE=$(BUILD_TYPE)) into $(BUILD_DIR)/"
	@echo "  static     Build a self-contained binary (bundled TDLib/OpenSSL/zlib/libstdc++)"
	@echo "  test       Run the test suite (ctest)"
	@echo "  install    Install to PREFIX=$(PREFIX) (needs sudo for system prefixes)"
	@echo "  uninstall  Remove installed files"
	@echo "  release    Build static binary and produce .rpm and .deb into $(DIST_DIR)/"
	@echo "  rpm / deb  Produce a single package format"
	@echo "  clean      Remove build artifacts"
	@echo "  distclean  Remove build/ and dist/ entirely"
	@echo ""
	@echo "Variables: VERSION=$(VERSION) BUILD_TYPE=$(BUILD_TYPE) PREFIX=$(PREFIX) JOBS=$(JOBS)"

version:
	@echo $(VERSION)

# ----------------------------------------------------------------------------
# Dependencies
# ----------------------------------------------------------------------------
# Build prerequisites: C++17 compiler, CMake, gperf, OpenSSL + zlib dev headers.
# TDLib itself is provided separately (system package, or vendored/from-source);
# see README. nfpm (for `release`) is fetched on demand by that target.
deps:
	@if command -v dnf >/dev/null 2>&1; then \
		echo ">> Installing build deps via dnf"; \
		sudo dnf install -y gcc-c++ cmake gperf openssl-devel zlib-devel make; \
	elif command -v apt-get >/dev/null 2>&1; then \
		echo ">> Installing build deps via apt-get"; \
		sudo apt-get update && sudo apt-get install -y g++ cmake gperf libssl-dev zlib1g-dev make; \
	else \
		echo "!! Unknown package manager. Install manually: a C++17 compiler, cmake, gperf, OpenSSL and zlib dev headers." >&2; \
		exit 1; \
	fi

# ----------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------
configure:
	cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
		-DTGCURL_VERSION=$(VERSION) \
		$(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) --parallel $(JOBS)

# Self-contained binary: statically link TDLib, OpenSSL, zlib and the C++
# runtime so the artifact runs on stock Linux hosts without those libraries
# installed. (glibc stays dynamic — fully-static glibc is not supported;
# this is the standard "portable Linux binary" trade-off.)
static:
	$(MAKE) build BUILD_TYPE=$(BUILD_TYPE) \
		CMAKE_FLAGS="-DTGCURL_STATIC=ON $(CMAKE_FLAGS)"
	@echo ">> Runtime dependencies of $(BIN):"
	@ldd $(BIN) || true

# ----------------------------------------------------------------------------
# Test
# ----------------------------------------------------------------------------
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# ----------------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------------
install: build
	cmake --install $(BUILD_DIR)

uninstall:
	@if [ -f $(BUILD_DIR)/install_manifest.txt ]; then \
		xargs rm -vf < $(BUILD_DIR)/install_manifest.txt; \
	else \
		echo "No install_manifest.txt; run a build/install first." >&2; \
	fi

# ----------------------------------------------------------------------------
# Release: static binary -> .rpm + .deb via nfpm
# ----------------------------------------------------------------------------
# nfpm produces both rpm and deb from one prebuilt binary, on any host, with no
# rpmbuild/dpkg toolchains required. See https://nfpm.goreleaser.com/
NFPM ?= nfpm

check-tools:
	@command -v $(NFPM) >/dev/null 2>&1 || { \
		echo "!! nfpm not found. Install it:"; \
		echo "     go install github.com/goreleaser/nfpm/v2/cmd/nfpm@latest"; \
		echo "   or download a release from https://github.com/goreleaser/nfpm/releases"; \
		exit 1; }

release: rpm deb
	@echo ">> Packages in $(DIST_DIR)/:"
	@ls -1 $(DIST_DIR)/*.rpm $(DIST_DIR)/*.deb 2>/dev/null || true

rpm: static check-tools
	@mkdir -p $(DIST_DIR)
	VERSION=$(VERSION) BIN=$(BIN) $(NFPM) pkg --config nfpm.yaml --packager rpm --target $(DIST_DIR)/

deb: static check-tools
	@mkdir -p $(DIST_DIR)
	VERSION=$(VERSION) BIN=$(BIN) $(NFPM) pkg --config nfpm.yaml --packager deb --target $(DIST_DIR)/

# ----------------------------------------------------------------------------
# Clean
# ----------------------------------------------------------------------------
clean:
	@if [ -d $(BUILD_DIR) ]; then cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true; fi
	rm -f $(BIN)

distclean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)

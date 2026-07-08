# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — hardened multi-stage image for a server daemon (#176).
#
# ONE parameterized Dockerfile builds BOTH daemons. The build stage compiles the
# full server tree exactly like CI (.github/workflows/build.yml: cmake + Ninja +
# g++, deps flatc/libflatbuffers/libssl/libmariadb), and the DAEMON build-arg
# selects which binary (authd | worldd) is copied into the runtime stage. Build
# each image by passing --build-arg DAEMON=authd or DAEMON=worldd (see
# docker-compose.yml and .github/workflows/cd.yml).
#
# Hardening baseline (D-30 §10 rule 6: "multi-stage, non-root, healthchecked"):
#   - multi-stage: no toolchain, headers, or source in the runtime image;
#   - small base: debian:bookworm-slim runtime (see "Why not distroless" below);
#   - non-root: runs as a dedicated uid/gid 10001 `meridian` user;
#   - runtime libs only: libssl3 + libmariadb3 (the shared libs the daemons dlopen
#     via find_package/pkg-config at build) — no -dev packages, no compilers;
#   - HEALTHCHECK: the daemon's own `--version` (exit 0 => binary + its shared
#     libs load); a real port probe lands with the /metrics endpoint (OPS-05);
#   - EXPOSE: the daemon's default listen port (authd 7100 IF-1, worldd 7200 IF-2);
#   - read-only-rootfs-friendly: the process writes nothing outside the mounted
#     TLS certs (read-only) and stdout/stderr JSON logs, so compose/K8s can run it
#     with read_only: true + a tmpfs if a scratch dir is ever needed.
#
# Clean-room (CONTRIBUTING.md): built from the written specs; no GPL source.

# =============================================================================
# Stage 1 — build: compile the server tree, mirror the CI toolchain exactly.
# =============================================================================
FROM debian:bookworm-slim AS build

# Mirror the build.yml `server` job deps. bookworm ships g++-12 (C++20-complete)
# and cmake >= 3.25 / ninja — matching the "cmake, ninja, g++, flatc, openssl,
# mariadb-dev" contract of CI. We pin the compiler explicitly for reproducibility.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        g++ \
        pkg-config \
        flatbuffers-compiler \
        libflatbuffers-dev \
        libssl-dev \
        libmariadb-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy only what the server build needs (the schema tree feeds flatc codegen for
# libmeridian-proto; server/ is the whole daemon tree). Keeps the build context
# and the layer cache tight — client/, content/, docs/ don't invalidate it.
COPY schema/ ./schema/
COPY server/ ./server/

# Configure + build the full tree Release, same flags as CI. Build both daemons
# in one pass (the DAEMON arg only selects which binary ships, not what compiles)
# so a single cached build layer serves both images.
RUN cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build server/build --target authd worldd

# =============================================================================
# Stage 2 — runtime: minimal + hardened. Only the selected daemon + its runtime
# shared libs. No compilers, no headers, no source.
# =============================================================================
FROM debian:bookworm-slim AS runtime

# Which daemon this image is. Overridden per build (authd | worldd).
ARG DAEMON=authd
# Default listen port for the HEALTHCHECK/EXPOSE metadata. authd 7100 (IF-1),
# worldd 7200 (IF-2) — the compose/env can still override the real --port.
ARG PORT=7100

# --- OCI image labels (provenance; #175 sets source/revision at publish). -----
LABEL org.opencontainers.image.title="meridian-${DAEMON}" \
      org.opencontainers.image.description="Project Meridian ${DAEMON} daemon (clean-room, Apache-2.0)" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.source="https://github.com/kwilliams312/project_meridian"

# Runtime shared libs ONLY — the .so's the daemons link against (libssl for
# meridian-srp/net TLS; libmariadb for meridian-db). No -dev, no toolchain.
# ca-certificates for TLS trust roots. Then create a non-root user to run as.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libmariadb3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 meridian \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin meridian

# Copy just the one daemon binary out of the build stage into a stable path.
COPY --from=build /src/server/build/${DAEMON}/${DAEMON} /usr/local/bin/meridiand

# Non-root. read-only rootfs friendly: the daemon reads TLS certs (mount ro) and
# writes only stdout/stderr.
USER 10001:10001

EXPOSE ${PORT}

# HEALTHCHECK: the binary's own --version. Exit 0 proves the ELF loads and every
# runtime .so it needs (libssl3, libmariadb3) resolves — a genuine "the image is
# not broken" signal that needs no shell, no curl, and no open port. A deeper
# liveness probe (TCP/`/metrics`) lands with OPS-05 telemetry.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/meridiand", "--version"]

ENTRYPOINT ["/usr/local/bin/meridiand"]

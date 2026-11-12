# cell-runtime: one image for every process in the cell (arm_sim, sensor_sim,
# plc_bridge, broker, plc-sim, cellctl). Compose selects the process via `command:`.
#
# Build from the repository root:
#   docker build -f deploy/docker/Dockerfile -t cell-runtime:$(cat VERSION) \
#     --build-arg CELL_VERSION=$(cat VERSION) .
#
# Behind a TLS-inspecting proxy, pass the proxy's CA bundle as a build secret (it is
# used for the git/pip downloads only and never ends up in an image layer):
#   docker build --network host --build-arg HTTPS_PROXY --build-arg NO_PROXY \
#     --secret id=ca_bundle,src=/path/to/ca-bundle.crt -f deploy/docker/Dockerfile .
#
# Builder and runtime share the same base on purpose: the binaries link against the
# distro's libzmq/libprotobuf/libmodbus/libstdc++, so the runtime must ship exactly
# the sonames (and a libstdc++ at least as new as) the builder compiled against.

ARG UBUNTU_VERSION=24.04

# ---------------------------------------------------------------------------
# builder: compiles the C++ processes and assembles the Python virtualenv
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      cppzmq-dev \
      git \
      libmodbus-dev \
      libprotobuf-dev \
      libsystemd-dev \
      libzmq3-dev \
      ninja-build \
      pkg-config \
      protobuf-compiler \
      python3 \
      python3-venv \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Python third-party dependencies first: they only change when pyproject.toml does.
# The venv is created at its final path so it can be copied to the runtime stage as-is.
COPY python/pyproject.toml python/pyproject.toml
RUN --mount=type=secret,id=ca_bundle,required=false \
    if [ -s /run/secrets/ca_bundle ]; then export PIP_CERT=/run/secrets/ca_bundle; fi \
 && python3 -m venv /opt/cell-runtime/venv \
 && python3 -c "import tomllib; print('\n'.join(tomllib.load(open('python/pyproject.toml', 'rb'))['project']['dependencies']))" \
      > /tmp/requirements-runtime.txt \
 && /opt/cell-runtime/venv/bin/pip install --no-cache-dir --disable-pip-version-check \
      -r /tmp/requirements-runtime.txt

# C++ build inputs only (tests/, bench/, docs/, deploy/ do not invalidate this layer).
COPY CMakeLists.txt VERSION ./
COPY cmake/ cmake/
COPY proto/ proto/
COPY config/ config/
COPY src/ src/

# Parallelism: defaults to all cores; override with --build-arg BUILD_JOBS=2.
ARG BUILD_JOBS=
RUN --mount=type=secret,id=ca_bundle,required=false \
    if [ -s /run/secrets/ca_bundle ]; then export GIT_SSL_CAINFO=/run/secrets/ca_bundle; fi \
 && cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCELL_BUILD_TESTS=OFF \
      -DCELL_BUILD_BENCH=OFF \
 && cmake --build build ${BUILD_JOBS:+--parallel ${BUILD_JOBS}} \
 && cmake --install build --prefix /opt/cell-runtime --strip

# Python tooling. cell_pb2.py is git-ignored, so generate it here with the image's
# protoc rather than trusting whatever happens to be in the build context.
COPY python/ python/
COPY scripts/gen_proto.sh scripts/gen_proto.sh
RUN --mount=type=secret,id=ca_bundle,required=false \
    if [ -s /run/secrets/ca_bundle ]; then export PIP_CERT=/run/secrets/ca_bundle; fi \
 && scripts/gen_proto.sh \
 && /opt/cell-runtime/venv/bin/pip install --no-cache-dir --disable-pip-version-check \
      --no-deps ./python \
 && /opt/cell-runtime/venv/bin/pip uninstall -y pip \
 && find /opt/cell-runtime/venv -depth -type d -name __pycache__ -exec rm -rf {} + \
 && mkdir -p /opt/cell-runtime/bin \
 && ln -s ../venv/bin/cellctl ../venv/bin/plc-sim /opt/cell-runtime/bin/

# ---------------------------------------------------------------------------
# runtime: shared libraries + python3 + the installed prefix; no toolchain
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Runtime counterparts of the -dev packages above. python3 must be the same distro
# interpreter the venv was created with (/usr/bin/python3).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libmodbus5 \
      libprotobuf32t64 \
      libsystemd0 \
      libzmq5 \
      python3 \
 && rm -rf /var/lib/apt/lists/*

# Fixed UID/GID so the ipc:// socket volume shared between the cell's containers has
# one owner. /tmp/cell exists in the image so a fresh named volume mounted there
# inherits that ownership.
ARG CELL_UID=10001
ARG CELL_GID=10001
RUN groupadd --gid ${CELL_GID} cell \
 && useradd --uid ${CELL_UID} --gid cell --no-create-home --home-dir /nonexistent \
      --shell /usr/sbin/nologin cell \
 && install -d -o cell -g cell -m 0750 /tmp/cell

COPY --from=builder /opt/cell-runtime /opt/cell-runtime
COPY --chmod=0755 deploy/docker/entrypoint.sh /usr/local/bin/cell-entrypoint

ENV PATH=/opt/cell-runtime/bin:${PATH} \
    CELL_CONFIG_DIR=/opt/cell-runtime/etc/cell-runtime \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

# broker: 5556 state/stats (XPUB), 5557 commands (ROUTER); plc-sim: 5020 Modbus TCP.
EXPOSE 5556 5557 5020

# No image-level HEALTHCHECK: what "healthy" means differs per process, so compose
# defines it per service. `cellctl health --json` (exit code 0/1) is on PATH for that.

USER cell
WORKDIR /opt/cell-runtime
# Every process handles SIGTERM itself (graceful shutdown); the entrypoint exec's, so
# the process is PID 1 and receives the signal directly.
STOPSIGNAL SIGTERM
ENTRYPOINT ["cell-entrypoint"]
CMD ["broker"]

ARG UBUNTU_VERSION
ARG CELL_VERSION=dev
ARG VCS_REF=unknown
ARG BUILD_DATE=unknown
LABEL org.opencontainers.image.title="cell-runtime" \
      org.opencontainers.image.description="Simulated industrial robot-cell runtime: device simulators, broker and diagnostics tooling" \
      org.opencontainers.image.version="${CELL_VERSION}" \
      org.opencontainers.image.revision="${VCS_REF}" \
      org.opencontainers.image.created="${BUILD_DATE}" \
      org.opencontainers.image.base.name="docker.io/library/ubuntu:${UBUNTU_VERSION}"

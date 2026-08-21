# Single-stage on purpose: the C++ binaries need data/, stats/, and
# web/frontend/ alongside them at runtime (their paths are baked in as
# absolute compile-time constants -- see CMakeLists.txt's
# SQL_OPTIMIZER_*_DIR definitions), and this is the simplest way to
# guarantee the runtime filesystem layout exactly matches what they were
# built against. A smaller multi-stage image is a reasonable follow-up, not
# a correctness requirement for a demo-scale project.
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j "$(nproc)"

# Which binary a given container runs is chosen by docker-compose.yml's
# `command:` per service -- this image just needs to have all of them built.

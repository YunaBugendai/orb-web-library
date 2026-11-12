FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential binutils make nodejs python3 \
    && rm -rf /var/lib/apt/lists/*

# syntax=docker/dockerfile:1.7
FROM debian:bookworm-slim

# Install Clang 21 + libc++ + tools (required for C++23 modules).
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates gnupg wget && \
    wget -qO - https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-21 main" \
        > /etc/apt/sources.list.d/llvm21.list && \
    rm -rf /var/lib/apt/lists/*

RUN DEBIAN_FRONTEND=noninteractive apt-get update && \
    apt-get install -y --no-install-recommends \
        git file jq make ninja-build \
        clang-21 libc++-21-dev libc++abi-21-dev lld-21 clang-tools-21 lldb-21 && \
    rm -rf /var/lib/apt/lists/*

# Bookworm ships CMake 3.25, and CMakeLists.txt needs 4.x: its `import std` gate is a
# UUID only the CMake release that minted it accepts. Same pin as the CI lane in
# .github/actions/setup-cmake.
ARG CMAKE_VERSION=4.1.2
RUN case "$(dpkg --print-architecture)" in \
        amd64) cmake_arch=x86_64 ;; \
        arm64) cmake_arch=aarch64 ;; \
        *) echo "no CMake build for $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac && \
    wget -qO /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${cmake_arch}.tar.gz" && \
    mkdir -p /opt/cmake && \
    tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 && \
    rm /tmp/cmake.tar.gz && \
    ln -s /opt/cmake/bin/cmake /usr/local/bin/cmake && \
    ln -s /opt/cmake/bin/ctest /usr/local/bin/ctest

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 100 && \
    update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 100 && \
    update-alternatives --install /usr/bin/lld lld /usr/bin/lld-21 100

ENV CC=clang-21 \
    CXX=clang++-21 \
    LDFLAGS="-Wl,--push-state,--no-as-needed -lc++ -lc++abi -lc++experimental -Wl,--pop-state -pthread -ldl"

ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN existing_group="$(getent group "$USER_GID" | cut -d: -f1 || true)" && \
    if [ -z "$existing_group" ]; then \
        groupadd --gid "$USER_GID" "$USERNAME"; \
    elif [ "$existing_group" != "$USERNAME" ]; then \
        groupmod -n "$USERNAME" "$existing_group"; \
    fi && \
    existing_user="$(getent passwd "$USER_UID" | cut -d: -f1 || true)" && \
    if [ -n "$existing_user" ] && [ "$existing_user" != "$USERNAME" ]; then \
        usermod -l "$USERNAME" "$existing_user" && \
        usermod -d "/home/$USERNAME" -m "$USERNAME"; \
    fi && \
    if ! id -u "$USERNAME" >/dev/null 2>&1; then \
        useradd --uid "$USER_UID" --gid "$USER_GID" -m "$USERNAME"; \
    fi

USER $USERNAME
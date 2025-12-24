FROM debian:bookworm-slim

RUN set -ex && \
    apt-get update && \
    apt-get install -y --no-install-recommends wget gnupg ca-certificates && \
    rm -rf /var/lib/apt/lists/*

RUN set -ex && \
    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/apt-llvm.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/apt-llvm.gpg] https://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-20 main" > /etc/apt/sources.list.d/apt.llvm.org.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends git cmake make clang-20 lldb-20 lld-20 libc++-20-dev libc++abi-20-dev file && \
    rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 100 && \
    update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 100 && \
    update-alternatives --install /usr/bin/lld lld /usr/bin/lld-20 100

# https://code.visualstudio.com/remote/advancedcontainers/add-nonroot-user
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

# Create the user (handle pre-existing gids/users in base image)
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

# [Optional] Set the default user. Omit if you want to keep the default as root.
USER $USERNAME

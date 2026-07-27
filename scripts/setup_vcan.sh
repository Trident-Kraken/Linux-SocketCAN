#!/usr/bin/env bash

set -euo pipefail

readonly INTERFACE="vcan0"

for command_name in ip modprobe; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'error: required command not found: %s\n' "${command_name}" >&2
        exit 1
    fi
done

if ((EUID == 0)); then
    SUDO=()
else
    if ! command -v sudo >/dev/null 2>&1; then
        printf 'error: run as root or install sudo\n' >&2
        exit 1
    fi
    SUDO=(sudo)
fi

if ! ip link show "${INTERFACE}" >/dev/null 2>&1; then
    "${SUDO[@]}" modprobe vcan
    "${SUDO[@]}" ip link add dev "${INTERFACE}" type vcan
fi

"${SUDO[@]}" ip link set up "${INTERFACE}"
ip -details link show "${INTERFACE}"

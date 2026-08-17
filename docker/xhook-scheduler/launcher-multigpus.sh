#!/bin/bash

set -Eeuo pipefail

CONFIG_DIR="${1:-/xpushare/scheduler/config}"
PORT_DIR="${2:-/xpushare/scheduler/podmanagerport}"
BASE_PORT="${BASE_PORT:-49901}"
SCAN_INTERVAL="${SCAN_INTERVAL:-3}"

ROOT_DIR="$(dirname "$CONFIG_DIR")"
STATE_DIR="${ROOT_DIR}/launcher-state"
STATE_FILE="${STATE_DIR}/ports"
HEALTH_FILE="${STATE_DIR}/health"

mkdir -p "$CONFIG_DIR" "$PORT_DIR" "$STATE_DIR"

declare -A LAUNCH_PIDS=()
declare -A PORT_MAP=()

log() {
    echo "[launcher-multigpus] $*" >&2
}

load_state() {
    if [[ -f "$STATE_FILE" ]]; then
        while read -r uuid port; do
            [[ -z "$uuid" ]] && continue
            PORT_MAP["$uuid"]="$port"
        done <"$STATE_FILE"
    fi
}

save_state() {
    local tmp
    tmp="$(mktemp "${STATE_DIR}/ports.XXXXXX")"
    for uuid in "${!PORT_MAP[@]}"; do
        echo "$uuid ${PORT_MAP[$uuid]}" >>"$tmp"
    done
    mv "$tmp" "$STATE_FILE"
}

port_in_use() {
    local candidate="$1"
    for used in "${PORT_MAP[@]}"; do
        if [[ "$used" -eq "$candidate" ]]; then
            return 0
        fi
    done
    return 1
}

assign_port() {
    local uuid="$1"
    if [[ -n "${PORT_MAP[$uuid]+x}" ]]; then
        echo "${PORT_MAP[$uuid]}"
        return
    fi
    local candidate="$BASE_PORT"
    while true; do
        if port_in_use "$candidate"; then
            candidate=$((candidate + 1))
            continue
        fi
        PORT_MAP["$uuid"]="$candidate"
        save_state
        echo "$candidate"
        return
    done
}

update_health() {
    date +%s >"$HEALTH_FILE"
}

start_backend() {
    local uuid="$1"
    local cfg="${CONFIG_DIR}/${uuid}"
    if [[ ! -f "$cfg" ]]; then
        log "Config ${cfg} not found, skip"
        return
    fi

    local port
    port="$(assign_port "$uuid")"

    if [[ ! -f "${PORT_DIR}/${uuid}" ]]; then
        echo 0 >"${PORT_DIR}/${uuid}"
    fi

    log "Starting scheduler for ${uuid} on port ${port}"
    python3 /launcher.py /xhook-schd /xhook-pmgr "$uuid" "$cfg" "$PORT_DIR" --port "$port" 1>&2 &
    local pid=$!
    LAUNCH_PIDS["$uuid"]=$pid
}

stop_backend() {
    local uuid="$1"
    if [[ -z "${LAUNCH_PIDS[$uuid]+x}" ]]; then
        return
    fi

    local pid="${LAUNCH_PIDS[$uuid]}"
    if kill -0 "$pid" 2>/dev/null; then
        log "Stopping scheduler for ${uuid} (pid ${pid})"
        kill -TERM "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    unset LAUNCH_PIDS["$uuid"]
}

cleanup() {
    log "Caught signal, cleaning up"
    for uuid in "${!LAUNCH_PIDS[@]}"; do
        stop_backend "$uuid"
    done
    exit 0
}

trap cleanup INT TERM

load_state
update_health

while true; do
    update_health
    shopt -s nullglob
    entries=("$CONFIG_DIR"/*)
    shopt -u nullglob
    declare -A CURRENT=()

    for path in "${entries[@]}"; do
        base="$(basename "$path")"
        [[ -z "$base" ]] && continue
        [[ "$base" == .* ]] && continue
        [[ -d "$path" ]] && continue
        CURRENT["$base"]=1

        if [[ -n "${LAUNCH_PIDS[$base]+x}" ]]; then
            pid="${LAUNCH_PIDS[$base]}"
            if kill -0 "$pid" 2>/dev/null; then
                continue
            fi
            log "Detected exited backend for ${base}; restarting"
            unset LAUNCH_PIDS["$base"]
        fi
        start_backend "$base"
    done

    for uuid in "${!LAUNCH_PIDS[@]}"; do
        if [[ -z "${CURRENT[$uuid]+x}" ]]; then
            log "Configuration for ${uuid} removed; stopping backend"
            stop_backend "$uuid"
        fi
    done

    sleep "$SCAN_INTERVAL"
done

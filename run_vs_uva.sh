#!/bin/bash

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEAM_CLIENT="$SCRIPT_DIR/src/rcssclient"
SERVER_BIN=${SERVER_BIN:-"$SCRIPT_DIR/src/rcssserver"}
MONITOR_BIN=${MONITOR_BIN:-"$HOME/tools/rcssmonitor/build/rcssmonitor"}
UVA_DIR=${UVA_DIR:-"$SCRIPT_DIR/../uva_base"}
UVA_PLAYER="$UVA_DIR/src/trilearn_player"
UVA_PLAYER_CONF="$UVA_DIR/src/player.conf"
UVA_FORMATION_CONF="$UVA_DIR/src/formations.conf"
LOG_DIR=${LOG_DIR:-"$SCRIPT_DIR/log"}

RCSS_SERVER=${RCSS_SERVER:-localhost}
RCSS_PORT=${RCSS_PORT:-6000}
TEAM_NAME=${TEAM_NAME:-team1}
OPPONENT_NAME=${OPPONENT_NAME:-UvaOpponent}
START_SERVER=${START_SERVER:-auto}
SHOW_MONITOR=${SHOW_MONITOR:-1}
CAPTURE_CLIENT_LOGS=${CAPTURE_CLIENT_LOGS:-0}
MATCH_HALF_TIME=${MATCH_HALF_TIME:-}
CONNECT_WAIT=${CONNECT_WAIT:-300}
KICK_OFF_WAIT=${KICK_OFF_WAIT:-100}
GAME_OVER_WAIT=${GAME_OVER_WAIT:-100}

if [ ! -x "$TEAM_CLIENT" ]; then
    echo "Error: team client is not executable: $TEAM_CLIENT" >&2
    exit 1
fi

if [ ! -x "$UVA_PLAYER" ]; then
    echo "Error: UvA opponent is not executable: $UVA_PLAYER" >&2
    echo "Set UVA_DIR to the extracted uva_base directory." >&2
    exit 1
fi

if [ "$SHOW_MONITOR" != "0" ] && [ ! -x "$MONITOR_BIN" ]; then
    echo "Error: live monitor is not executable: $MONITOR_BIN" >&2
    exit 1
fi

if [ "$SHOW_MONITOR" != "0" ] && [ -z "${DISPLAY:-}" ]; then
    echo "Error: DISPLAY is empty; the live monitor cannot open a window." >&2
    exit 1
fi

if [ "$START_SERVER" = "auto" ]; then
    START_SERVER=0
    case "$RCSS_SERVER" in
        localhost|127.0.0.1)
            if ss -H -lun "sport = :$RCSS_PORT" 2>/dev/null | grep -q .; then
                echo "Error: port $RCSS_PORT already has a soccer server." >&2
                echo "Stop the existing match, or set START_SERVER=0 to connect intentionally." >&2
                exit 1
            fi
            START_SERVER=1
            ;;
    esac
fi

if [ "$START_SERVER" != "0" ] && [ ! -x "$SERVER_BIN" ]; then
    echo "Error: soccer server is not executable: $SERVER_BIN" >&2
    exit 1
fi

mkdir -p "$LOG_DIR"
session_stamp=$(date +%Y%m%d-%H%M%S)
runtime_dir=$(mktemp -d /tmp/team1-debug.XXXXXX)
chmod 700 "$runtime_dir"
pids=()

cleanup() {
    trap - INT TERM EXIT
    if [ "${#pids[@]}" -gt 0 ]; then
        kill "${pids[@]}" 2>/dev/null || true
        wait "${pids[@]}" 2>/dev/null || true
    fi
    case "$runtime_dir" in
        /tmp/team1-debug.*)
            rm -rf -- "$runtime_dir"
            ;;
    esac
}

trap cleanup INT TERM EXIT

if [ "$START_SERVER" != "0" ]; then
    server_args=(
        "--server::port=$RCSS_PORT"
        "--server::auto_mode=true"
        "--server::connect_wait=$CONNECT_WAIT"
        "--server::kick_off_wait=$KICK_OFF_WAIT"
        "--server::game_over_wait=$GAME_OVER_WAIT"
        "--server::game_log_dir=$LOG_DIR"
        "--server::text_log_dir=$LOG_DIR"
    )
    if [ -n "$MATCH_HALF_TIME" ]; then
        server_args+=(
            "--server::half_time=$MATCH_HALF_TIME"
            "--server::nr_normal_halfs=2"
            "--server::nr_extra_halfs=0"
            "--server::penalty_shoot_outs=false"
        )
    fi

    "$SERVER_BIN" "${server_args[@]}" \
        >"$LOG_DIR/${session_stamp}-server-console.log" 2>&1 &
    pids+=("$!")

    sleep 1
    if ! kill -0 "${pids[0]}" 2>/dev/null; then
        echo "Error: soccer server failed to start." >&2
        tail -n 40 "$LOG_DIR/${session_stamp}-server-console.log" >&2
        exit 1
    fi
fi

if [ "$SHOW_MONITOR" != "0" ]; then
    XDG_RUNTIME_DIR="$runtime_dir" "$MONITOR_BIN" \
        --connect \
        --server-host "$RCSS_SERVER" \
        --server-port "$RCSS_PORT" \
        --auto-reconnect-mode true \
        --auto-reconnect-wait 1 \
        --maximize \
        --show-menu-bar true \
        --show-tool-bar true \
        --show-status-bar true \
        --show-player-number true \
        --show-stamina true \
        --show-offside-line true \
        --ball-size 0.45 \
        >"$LOG_DIR/${session_stamp}-monitor-console.log" 2>&1 &
    pids+=("$!")
    sleep 0.5
fi

# The libtool wrapper may need to relink .libs/lt-rcssclient after a build.
# Warm it up once so five parallel launches cannot race on the same output.
"$TEAM_CLIENT" \
    -id 1 \
    -team "$TEAM_NAME" \
    -server "$RCSS_SERVER" \
    -port "$RCSS_PORT" >/dev/null 2>&1 || true

# Start our team first so it is assigned to the left side.
for player_id in 1 2 3 4 5; do
    team_args=(
        -id "$player_id"
        -sidel
        -team "$TEAM_NAME"
        -server "$RCSS_SERVER"
        -port "$RCSS_PORT"
    )
    if [ "$CAPTURE_CLIENT_LOGS" = "0" ]; then
        "$TEAM_CLIENT" "${team_args[@]}" &
    else
        "$TEAM_CLIENT" "${team_args[@]}" \
            >"$LOG_DIR/${session_stamp}-team1-p${player_id}.log" 2>&1 &
    fi
    pids+=("$!")
    sleep 0.2
done

sleep 1

# Use mirrored pairs: goalie, left/right defenders and left/right attackers.
for formation_number in 1 2 5 10 11; do
    uva_args=(
        -number "$formation_number"
        -host "$RCSS_SERVER"
        -port "$RCSS_PORT"
        -team "$OPPONENT_NAME"
        -f "$UVA_FORMATION_CONF"
        -c "$UVA_PLAYER_CONF"
    )
    if [ "$CAPTURE_CLIENT_LOGS" = "0" ]; then
        "$UVA_PLAYER" "${uva_args[@]}" &
    else
        "$UVA_PLAYER" "${uva_args[@]}" \
            >"$LOG_DIR/${session_stamp}-uva-p${formation_number}.log" 2>&1 &
    fi
    pids+=("$!")
    sleep 0.2
done

if [ "$SHOW_MONITOR" = "0" ]; then
    echo "Headless match started: $TEAM_NAME vs $OPPONENT_NAME"
    echo "Press Ctrl+C to stop the match."
else
    echo "Live match started: $TEAM_NAME vs $OPPONENT_NAME"
    echo "Close the monitor window or press Ctrl+C here to stop the match."
fi
echo "Runtime logs: $LOG_DIR"
echo "Session: $session_stamp"

status=0
wait -n "${pids[@]}" || status=$?
exit "$status"

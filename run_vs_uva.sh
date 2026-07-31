#!/bin/bash

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEAM_CLIENT="$SCRIPT_DIR/src/rcssclient"
UVA_DIR=${UVA_DIR:-"$SCRIPT_DIR/../uva_base"}
UVA_PLAYER="$UVA_DIR/src/trilearn_player"
UVA_PLAYER_CONF="$UVA_DIR/src/player.conf"
UVA_FORMATION_CONF="$UVA_DIR/src/formations.conf"

RCSS_SERVER=${RCSS_SERVER:-localhost}
RCSS_PORT=${RCSS_PORT:-6000}
TEAM_NAME=${TEAM_NAME:-team1}
OPPONENT_NAME=${OPPONENT_NAME:-UvaOpponent}

if [ ! -x "$TEAM_CLIENT" ]; then
    echo "Error: team client is not executable: $TEAM_CLIENT" >&2
    exit 1
fi

if [ ! -x "$UVA_PLAYER" ]; then
    echo "Error: UvA opponent is not executable: $UVA_PLAYER" >&2
    echo "Set UVA_DIR to the extracted uva_base directory." >&2
    exit 1
fi

pids=()

cleanup() {
    if [ "${#pids[@]}" -gt 0 ]; then
        kill "${pids[@]}" 2>/dev/null || true
    fi
}

trap cleanup INT TERM EXIT

# Start our team first so it is assigned to the left side.
for player_id in 1 2 3 4 5; do
    "$TEAM_CLIENT" \
        -id "$player_id" \
        -sidel \
        -team "$TEAM_NAME" \
        -server "$RCSS_SERVER" \
        -port "$RCSS_PORT" &
    pids+=("$!")
    sleep 0.2
done

sleep 1

# Use a balanced five-player subset: goalie, two defenders and two attackers.
for formation_number in 1 2 3 9 10; do
    "$UVA_PLAYER" \
        -number "$formation_number" \
        -host "$RCSS_SERVER" \
        -port "$RCSS_PORT" \
        -team "$OPPONENT_NAME" \
        -f "$UVA_FORMATION_CONF" \
        -c "$UVA_PLAYER_CONF" &
    pids+=("$!")
    sleep 0.2
done

wait

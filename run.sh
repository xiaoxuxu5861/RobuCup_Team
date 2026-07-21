#!/bin/bash

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

MODE=${1:-left}
RCSS_SERVER=${RCSS_SERVER:-localhost}
RCSS_PORT=${RCSS_PORT:-6000}
PLAYER_COUNT=5

start_team() {
    side_flag=$1
    player_id=1

    while [ "$player_id" -le "$PLAYER_COUNT" ]; do
        ./src/rcssclient \
            -id "$player_id" \
            "$side_flag" \
            -server "$RCSS_SERVER" \
            -port "$RCSS_PORT" &
        player_id=$((player_id + 1))
        sleep 1
    done
}

case "$MODE" in
    left|l|-sidel)
        start_team -sidel
        ;;
    right|r|-sider)
        start_team -sider
        ;;
    both)
        start_team -sidel
        start_team -sider
        ;;
    *)
        echo "Usage: $0 [left|right|both]" >&2
        echo "Optional: RCSS_SERVER=host RCSS_PORT=6000 $0 left" >&2
        exit 2
        ;;
esac

wait

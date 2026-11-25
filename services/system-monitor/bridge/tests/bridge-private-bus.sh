#!/bin/sh
set -eu
exec dbus-run-session -- sh -c 'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$@"' sh "$@"

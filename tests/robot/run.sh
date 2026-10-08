#!/usr/bin/env bash
# Acceptance tests on the real board (Robot Framework).
#
#   tests/robot/run.sh                         # all suites (01-08)
#   tests/robot/run.sh --include users         # 01-05: users only, no update
#   tests/robot/run.sh --include update        # 06-08: firmware update
#   UPDATE_PORT=<UART4> SHELL_PORT=<app USB> tests/robot/run.sh
#
# Ports: prefer the /dev/serial/by-id paths. The app USB console leaves and
# comes back on every reboot, often with another ttyACM number.
#
# Needs: pip install -r tests/robot/requirements.txt
# Report: tests/robot/results/report.html
set -eu
cd "$(dirname "$0")"
UPDATE_PORT="${UPDATE_PORT:-/dev/serial/by-id/usb-1a86_USB_Single_Serial_5552003040-if00}"
SHELL_PORT="${SHELL_PORT:-/dev/serial/by-id/usb-GZM_Embedded_Systems_Do_Codigo_ao_Campo_Demo-if00}"

# Fail now instead of timing out in every test: two readers on one tty
# split the bytes (picocom left open makes each shell command wait 10 s).
check_port() {
	if [ ! -e "$2" ]; then
		echo "error: $1 $2 not found" >&2
		exit 1
	fi
	if command -v fuser >/dev/null; then
		pids="$(fuser "$2" 2>/dev/null | xargs || true)"
		if [ -n "${pids}" ]; then
			echo "error: $1 $2 is already open, close it first:" >&2
			ps -o pid=,args= -p "${pids// /,}" >&2
			exit 1
		fi
	fi
}
check_port UPDATE_PORT "${UPDATE_PORT}"
check_port SHELL_PORT "${SHELL_PORT}"
if [ "$(readlink -f "${UPDATE_PORT}")" = "$(readlink -f "${SHELL_PORT}")" ]; then
	echo "error: UPDATE_PORT and SHELL_PORT are the same device ($(readlink -f "${SHELL_PORT}"))." >&2
	echo "       UPDATE_PORT is the UART4 adapter, SHELL_PORT the app USB console." >&2
	exit 1
fi

exec robot --outputdir results \
	--variable "UPDATE_PORT:${UPDATE_PORT}" \
	--variable "SHELL_PORT:${SHELL_PORT}" \
	"$@" suites

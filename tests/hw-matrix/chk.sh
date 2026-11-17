#!/bin/bash
# usage: chk.sh label  (читает /tmp/serial.log)
printf "%-22s mounted=%s probes-ok=%s flush-fail=%s io-err=%s\n" "$1" "$(grep -ac '^\[AUTOMOUNT\] disk[0-9]*:' /tmp/serial.log)" "$(grep -ac 'write+readback OK' /tmp/serial.log)" "$(grep -ac 'flush FAILED' /tmp/serial.log)" "$(grep -ac 'I/O ERROR' /tmp/serial.log)"

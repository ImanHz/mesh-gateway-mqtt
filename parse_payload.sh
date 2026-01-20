
#!/usr/bin/bash

set -e
hex=$1
# --- Sanity check ---
[[ ${#hex} -ne 26 ]] && { echo "Invalid length"; exit 1; }
[[ ${hex:0:2} != "ff" ]] && { echo "Invalid header"; exit 1; }
[[ ${hex:24:2} != "fe" ]] && { echo "Invalid tail"; exit 1; }

# --- Time (5 bytes, LSB first, ignore MSB) ---
# bytes: t0 t1 t2 t3 t4 (LSB → MSB)
t0=${hex:2:2}
t1=${hex:4:2}
t2=${hex:6:2}
t3=${hex:8:2}
# t4 (MSB) ignored

time_sec=$(( 16#$t3$t2$t1$t0 ))

# Epoch offset: 2026-01-01 00:00:00 UTC
EPOCH_2026=1767225600
unix_time=$(( EPOCH_2026 + time_sec ))
rfc_time=$(date -u -d "@$unix_time" +"%Y-%m-%dT%H:%M:%SZ")
# --- Value (3 bytes, MSB first → uint32) ---
v0=${hex:12:2}
v1=${hex:14:2}
v2=${hex:16:2}
value=$(( 16#$v0$v1$v2 ))

# --- Remote address (2 bytes, LSB first) ---
a0=${hex:18:2}
a1=${hex:20:2}
addr=$(( 16#$a1$a0 ))

# --- RSSI ---
rssi_raw=$(( 16#${hex:22:2} ))
rssi=$(( rssi_raw - 128 ))

# --- Output ---
printf '{'
printf '"timestamp":%s,' "$rfc_time"
printf '"value":%d,' "$value"
printf '"address":%d,' "$addr"
printf '"rssi":%d' "$rssi"
printf '}\n'

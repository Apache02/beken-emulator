#!/usr/bin/env bash
# Runs the six firmware acceptance checks, each on a freshly started
# emulator. Exit status: 0 all passed, 77 skipped (BKLOADER unset or not
# executable), 1 otherwise.
set -eu
dir=$(cd "$(dirname "$0")" && pwd)

if [ -z "${BKLOADER:-}" ] || [ ! -x "$BKLOADER" ]; then
  echo "SKIP: set BKLOADER to the path of an executable bkloader to run the firmware checks"
  exit 77
fi
for image in bootloader_v0.1.0-2-g5ae564b-dirty_crc.bin app_crc.bin test_flash_sector_iram.bin test_gdma_iram.bin; do
  if [ ! -f "$dir/../../firmware/$image" ]; then
    echo "SKIP: firmware/$image is missing (the images are not part of the repository)"
    exit 77
  fi
done

failed=0
for check in check1_boot check2_shell check3_iram check4_flash check5_backup check6_gdma; do
  echo "==== $check"
  if "$dir/$check.sh"; then :; else
    rc=$?
    echo "==== $check: exit status $rc"
    failed=$((failed + 1))
  fi
done

if [ "$failed" -eq 0 ]; then
  echo "ALL PASS: 6 / 6 firmware checks"
else
  echo "FAILED: $failed of 6 firmware checks"
  exit 1
fi

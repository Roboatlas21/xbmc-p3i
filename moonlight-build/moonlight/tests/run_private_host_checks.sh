#!/usr/bin/env bash
# Run the recent private-branch regression checks locally before ARM32 builds.
# This script neither invokes nor changes any GitHub Actions workflows.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for name in \
  test_aml_guard_order.py \
  test_alsa_rate_guard.py \
  test_evdev_axis_geometry.py \
  test_evdev_sync.py \
  test_controller_mapping_wizard.py \
  test_stream_failure_exit.py \
  test_config_lookup_safety.py \
  test_udev_errors.py \
  test_native_input_safety.py \
  test_controller_registration.py \
  test_evdev_fd_dedup.py \
  test_osd_first_commit.py \
  test_evdev_mapping_failure.py \
  test_unique_id_persistence.py \
  test_session_config_precedence.py \
  test_private_credentials.py \
  test_client_identity_permissions.py \
  test_certificate_hex.py
do
  python3 -m unittest discover -s tests -p "$name" -v
done

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_controller_mapping_roundtrip.c src/input/mapping.c \
   -o "$tmp/test-controller-mapping"
"$tmp/test-controller-mapping"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_mapping_capture.c -o "$tmp/test-mapping-capture"
"$tmp/test-mapping-capture"

cc -std=gnu99 -Wall -Wextra -Werror \
   -fsanitize=undefined -fno-sanitize-recover=undefined \
   tests/test_motion_math.c -o "$tmp/test-motion-math"
"$tmp/test-motion-math"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_osd_plane_ownership.c -o "$tmp/test-osd-ownership"
"$tmp/test-osd-ownership"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_mapping_usable.c -o "$tmp/test-mapping-usable"
"$tmp/test-mapping-usable"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_aml_pacing_fault.c -o "$tmp/test-aml-pacing-fault"
"$tmp/test-aml-pacing-fault"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_alsa_rate.c -o "$tmp/test-alsa-rate"
"$tmp/test-alsa-rate"

cc -std=gnu99 -Wall -Wextra -Werror \
   -fsanitize=undefined -fno-sanitize-recover=undefined \
   tests/test_config_numeric.c -o "$tmp/test-config-numeric"
"$tmp/test-config-numeric"

cc -std=gnu99 -Wall -Wextra -Werror \
   -fsanitize=undefined -fno-sanitize-recover=undefined \
   tests/test_server_https_port.c -o "$tmp/test-server-https-port"
"$tmp/test-server-https-port"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_selected_names.c -o "$tmp/test-selected-names"
"$tmp/test-selected-names"

cc -std=gnu99 -Wall -Wextra -Werror -pedantic \
   tests/test_controller_count.c -o "$tmp/test-controller-count"
"$tmp/test-controller-count"

cc -std=gnu99 -Wall -Wextra -Werror -pthread \
   tests/test_loop_dispatch.c -o "$tmp/test-loop"
"$tmp/test-loop"

cc -std=gnu99 -Wall -Wextra -Werror \
   tests/test_aml_decode_unit_validate.c -o "$tmp/test-aml-units"
"$tmp/test-aml-units"

bash tests/test_native_xml.sh

echo "PASS: private host regressions (full ARM32 build still required)"

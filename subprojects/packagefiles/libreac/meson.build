# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# meson shim for libreac (FreeREAC/libreac ships only a hand Makefile). Dropped
# into the libreac checkout by reac-repacer's subprojects/libreac.wrap
# patch_directory. Exposes libreac_dep; the headers are <reac/*.h>.
#
# The version below must track libreac's own (packaging/libreac.spec,
# openwrt/libreac/Makefile) — the parent project's `>=0.4.0` floor is checked
# against it.
#
# Only the two translation units reac-repacer actually needs are built:
#   reac.c           reac_frame_clean_len() + the wire constants
#   reac_upstream.c  reac_upstream_channels(), used by the test suite to pin why
#                    the repacer cannot call it directly (it rejects the 40-ch
#                    downstream broadcast, which the inject paths do see)
# The decode/encode/capture/pcap units are left out: reac-repacer never decodes a
# frame, and pcap_source.c would drag in libpcap.

project('libreac', 'c',
  version : '0.4.0',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2'])

libreac_inc = include_directories('include')

libreac_lib = static_library('reac',
  ['src/reac.c', 'src/reac_upstream.c'],
  include_directories : libreac_inc,
  install : false)

libreac_dep = declare_dependency(
  link_with : libreac_lib,
  include_directories : libreac_inc)

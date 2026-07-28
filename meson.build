# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-repacer — native build (the daemon + the unit tests). The shipping builds
# stay where they were: OpenWrt via openwrt/reac-repacer/Makefile against the SDK,
# Fedora via packaging/reac-repacer.spec. This build exists so the test-only inject
# paths can be exercised off-rig, and so the libreac dependency is resolved the same
# way on a plain dev checkout as it is on the device.
#
# Reuses, does not reinvent:
#   - libreac  the REAC wire-format core: the frame geometry constants, the OHRCA
#              +2 trailer rule (reac_frame_clean_len) and the braid layout oracle
#              (reac_braid_pos). <reac/*.h>; system libreac-devel, or the bundled
#              subproject fallback. The relay path needs none of it — it does not
#              decode REAC — but the inject paths reach into the audio region and
#              must agree with the rest of the FreeREAC tree byte for byte.

project('reac-repacer', 'c',
  version : '0.2.3',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')

threads_dep = dependency('threads')
m_dep       = cc.find_library('m', required : true)   # sin() in the sine injector

# --- libreac: the wire-format core, pulled as a meson subproject --------------
# subprojects/libreac.wrap clones FreeREAC/libreac and drops a meson shim into the
# checkout (upstream libreac ships only a hand Makefile). Prefer a system libreac
# (Fedora libreac-devel, found via pkg-config); fall back to the bundled subproject
# on a plain dev checkout. Wired the same way reac-pw wires it.
#
# Floor 0.4.0: reac_frame_clean_len() + <reac/reac_braid.h> landed in 0.3.0, and
# 0.4.0 is the first release whose OpenWrt + rpm packages ship them.
libreac_dep = dependency('libreac',
  version : '>=0.4.0',
  fallback : ['libreac', 'libreac_dep'],
  required : true)

repacer_deps = [threads_dep, m_dep, libreac_dep]

executable('reac-repacer',
  'tools/reac_repacer.c',
  dependencies : repacer_deps,
  install : true)

executable('pacer-probe',
  'tools/pacer_probe.c',
  dependencies : [threads_dep, m_dep],
  install : false)

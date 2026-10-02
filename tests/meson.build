# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The tests include tools/reac_repacer.c directly (its inject paths are static
# and driven off file-scope state), so they carry the daemon's own dependencies.

test_inject = executable('test-inject',
  'test_inject.c',
  dependencies : repacer_deps,
  install : false)
test('inject-goldens', test_inject)

test_geometry = executable('test-geometry',
  'test_geometry.c',
  dependencies : repacer_deps,
  install : false)
test('frame-geometry', test_geometry)

# The README carries pitch + install; the build steps live in BUILDING.md.
python3 = find_program('python3')
readme_no_build = files('readme_no_build.py')
test('readme-no-build-command', python3,
  args : [readme_no_build, meson.project_source_root() / 'README.md'])
test('readme-no-build-self-test', python3,
  args : [readme_no_build, '--self-test'])

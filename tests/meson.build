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

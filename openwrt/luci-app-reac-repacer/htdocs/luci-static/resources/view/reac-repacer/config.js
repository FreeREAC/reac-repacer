// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

'use strict';
'require view';
'require form';

// Config view for the multiport REAC de-jitter re-pacer.
// Bound 1:1 to /etc/config/reac-repacer (section 'main', type 'repacer') — the
// same section the init reads to launch the daemon and the daemon re-reads on
// SIGHUP. Save & Apply writes UCI and the procd reload trigger relaunches the
// daemon; the hot params (target buffer, latency-reclaim rate) also retune the
// running daemon live, without a restart.
//
// Every option here maps 1:1 to a key the init's build_args/setup_etf reads
// (config_get ... main ...). The directional profile (clock source, PLL, ETF, …)
// is auto-filled per AP/STA at install by /etc/uci-defaults/97-reac-role; it is
// exposed here under Advanced for manual override only.
return view.extend({
	render: function () {
		let m, s, o;

		m = new form.Map('reac-repacer', _('REAC Wi-Fi Re-pacer'),
			_('De-jitter Roland REAC streams (EtherType 0x8819) carried over a bursty ' +
			  'Wi-Fi / WDS link. One daemon paces every port on a single recovered clock, ' +
			  'so the stageboxes stay sample-aligned. Pair with the reac-transport package ' +
			  'for the VLAN trunk + gretap fabric.'));

		s = m.section(form.NamedSection, 'main', 'repacer', _('Service'));
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enabled'),
			_('Master on/off for the whole re-pacer.'));
		o.rmempty = false;

		o = s.option(form.Value, 'wait_iface', _('Wait for interface'),
			_('Do not launch until this fabric interface exists (a gretap VLAN ' +
			  'sub-interface set up by reac-transport).'));
		o.placeholder = 'reactap.11';

		o = s.option(form.Value, 'prefill_ms', _('Target buffer (ms)'),
			_('De-jitter buffer depth (ms). The relay sizes the buffer to the observed ' +
			  'burst and drains toward the clean link floor from here. Save & Apply ' +
			  'retunes the running daemon live (no dropout). For instant by-ear tuning ' +
			  'without a save: ubus call reac_repacer set \'{"prefill_ms":150}\'.'));
		o.datatype = 'range(1,1000)';
		o.default = '30';

		o = s.option(form.Value, 'servo_clamp_ppm', _('Latency-reclaim rate (ppm)'),
			_('Maximum clock bias used to reclaim latency. 0 freezes the output clock ' +
			  '(the PLL still trims sub-ppm); raise for faster latency reduction. ' +
			  '~700 ppm (about 1 cent) is inaudible.'));
		o.datatype = 'range(0,3000)';
		o.default = '0';

		o = s.option(form.Value, 'detect_ms', _('REAC detect window (ms)'),
			_('How long to look for a REAC stream before declaring the port idle.'));
		o.datatype = 'range(100,10000)';
		o.default = '2000';
		o.modalonly = true;

		o = s.option(form.Value, 'clock_margin_ppm', _('PLL lock margin (ppm)'));
		o.datatype = 'range(0,1000)';
		o.default = '8';
		o.modalonly = true;

		o = s.option(form.Value, 'cpu', _('Real-time CPU'),
			_('CPU core to pin the pacing thread to. Isolate it from the core handling ' +
			  'the NIC interrupts.'));
		o.datatype = 'uinteger';
		o.default = '2';
		o.modalonly = true;

		// ---- directional profile: auto-filled per AP/STA by 97-reac-role ----
		o = s.option(form.ListValue, 'clock_source', _('Clock source'),
			_('Auto-set from the box Wi-Fi role at install. Override only if you know the ' +
			  'rig topology.'));
		o.value('local', _('local — AP / master side'));
		o.value('local-in', _('local-in — STA / box side'));
		o.default = 'local';
		o.modalonly = true;

		o = s.option(form.Flag, 'forward_only', _('Forward only'),
			_('De-jitter one direction only (the validated relay mode).'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Flag, 'bcast_only', _('Broadcast only'),
			_('De-jitter only the master broadcast (the audio carrier).'));
		o.default = '0';
		o.modalonly = true;

		o = s.option(form.Flag, 'pll', _('PLL'),
			_('Phase-lock the output clock (AP / master side).'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Flag, 'pace_by_downstream', _('Pace by downstream'),
			_('Drive the pacing clock from the downstream port occupancy (AP side).'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Flag, 'etf', _('ETF egress (TSN)'),
			_('Time the egress with the kernel ETF qdisc (SCM_TXTIME) on the stagebox ' +
			  'ports. HW offload needs an i226-class TSN NIC; otherwise software ETF.'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Value, 'etf_delta_us', _('ETF window (µs)'),
			_('Default ETF early-release window. A port pair may override it with a third ' +
			  'IN:OUT:µs field below.'));
		o.datatype = 'range(10,5000)';
		o.default = '300';
		o.depends('etf', '1');
		o.modalonly = true;

		// ---- port pairs: 'IN:OUT[:etf_µs]' strings, exactly as the init reads them ----
		o = s.option(form.DynamicList, 'port', _('REAC port pairs'),
			_('One entry per REAC zone: IN:OUT (tunnel-side iface : stagebox-side iface), ' +
			  'optionally IN:OUT:µs to set that port’s ETF window. ' +
			  'e.g. reactap.11:lan1 or reactap.12:lan2:80. A port with no input stays ' +
			  'dormant, so extra entries are harmless.'));
		o.placeholder = 'reactap.11:lan1';

		return m.render();
	}
});

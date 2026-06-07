// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

'use strict';
'require view';
'require form';
'require uci';

// Config view for the multiport REAC de-jitter re-pacer.
// Bound 1:1 to /etc/config/reac-repacer; Save&Apply writes UCI and reloads the
// service (LuCI apply + the procd reload trigger). One daemon, global settings
// plus one 'port' section per REAC zone.
return view.extend({
	render: function () {
		let m, s, o;

		m = new form.Map('reac-repacer', _('REAC Wi-Fi Re-pacer'),
			_('De-jitter Roland REAC streams (EtherType 0x8819) carried over a bursty ' +
			  'Wi-Fi link. One daemon paces every port on a single shared clock, so the ' +
			  'stageboxes stay sample-aligned. Latency self-tunes down to the link’s ' +
			  'clean floor by clock rate alone — frames are never dropped, so reducing ' +
			  'latency does not click. The sample rate is auto-detected unless pinned.'));

		// ---- global settings ----
		s = m.section(form.NamedSection, 'main', 'repacer', _('Service'));
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enabled'),
			_('Master on/off for the whole re-pacer.'));
		o.rmempty = false;

		o = s.option(form.Value, 'prefill_ms', _('Target buffer (ms)'),
			_('De-jitter buffer depth. The relay sizes the buffer to the observed ' +
			  'burst depth and drains down toward the clean floor from here. ' +
			  'Save &amp; Apply retunes the running daemon live (no audio dropout) — ' +
			  'the latency walks to the new value. For instant by-ear tuning without ' +
			  'a save, run: ubus call reac_repacer set \'{"prefill_ms":150}\'.'));
		o.datatype = 'range(1,1000)';
		o.default = '16';

		o = s.option(form.Flag, 'auto_rate', _('Auto rate'),
			_('Detect the sample rate (44.1 / 48 / 96 kHz) from the wire, at startup ' +
			  'and live. Turn off to pin it below.'));
		o.default = '1';

		// "Pin the freq in the website": only applies when auto-detection is off.
		o = s.option(form.ListValue, 'rate', _('Pinned rate'),
			_('Sample rate to lock to when auto rate is off.'));
		o.value('44100', '44.1 kHz');
		o.value('48000', '48 kHz');
		o.value('96000', '96 kHz');
		o.default = '48000';
		o.depends('auto_rate', '0');

		o = s.option(form.Value, 'servo_clamp_ppm', _('Latency-reclaim rate (ppm)'),
			_('Maximum clock bias used to reclaim latency. ~700 ppm (≈1 cent) is ' +
			  'inaudible; raise for faster latency reduction, lower for a wider ' +
			  'inaudibility margin.'));
		o.datatype = 'range(50,3000)';
		o.default = '700';
		o.modalonly = true;

		o = s.option(form.Flag, 'adapt', _('Adaptive buffer'),
			_('Size the buffer to the observed burst depth: grow at once to cover a ' +
			  'burst, ease down when the link is calm. Leave on.'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Value, 'cpu', _('Real-time CPU'),
			_('CPU core to pin the pacing thread to. Isolate it from the core handling ' +
			  'the NIC interrupts.'));
		o.datatype = 'uinteger';
		o.default = '3';
		o.modalonly = true;

		o = s.option(form.Flag, 'bcast_only', _('Broadcast only'),
			_('De-jitter only the master broadcast (the audio carrier).'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Flag, 'unbridge', _('Unbridge ports'),
			_('Take each port out of the bridge at start. Required: raw L2 transmit is ' +
			  'dropped by the switch on a VLAN-filtering bridge slave.'));
		o.default = '1';
		o.modalonly = true;

		// ---- per-port zones ----
		s = m.section(form.GridSection, 'port', _('REAC ports'),
			_('One row per REAC zone. A port with no input stays dormant until its ' +
			  'stream appears, so extra rows are harmless.'));
		s.addremove = true;
		s.anonymous = false;
		s.nodescriptions = true;

		o = s.option(form.Flag, 'enabled', _('On'));
		o.rmempty = false;
		o.default = '1';

		o = s.option(form.Value, 'in_iface', _('Tunnel iface'),
			_('Port the bursty master broadcast arrives on (the Wi-Fi/tunnel side).'));
		o.placeholder = 'reactap.11';

		o = s.option(form.Value, 'out_iface', _('Stagebox iface'),
			_('Port the clean, re-paced stream is sent to (the local stagebox).'));
		o.placeholder = 'lan1';

		return m.render();
	}
});

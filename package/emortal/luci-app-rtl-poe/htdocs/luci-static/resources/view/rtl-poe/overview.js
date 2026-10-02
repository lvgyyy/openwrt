/*
 * luci-app-rtl-poe — RTL8238B PoE Control
 * =======================================
 * LuCI JavaScript 控制器 (ucode)
 * 协议: RTL8238B 直接 I2C 寄存器读写 (地址 0x20), 非 Host Command
 */

'use strict';
'require form';
'require dom';
'require fs';
'require poll';
'require uci';
'require ui';
'require view';

const statusCommand = '/usr/libexec/rtl-poe-status';
const logCommand = '/usr/libexec/rtl-poe-log';
const clearLogCommand = '/usr/libexec/rtl-poe-log-clear';

function runStatus() {
	return L.resolveDefault(fs.exec_direct(statusCommand, [], 'json'), null);
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('rtl-poe'), runStatus() ]);
	},

	render: function(data) {
		let status = data[1];
		let summaryNode, logNode;
		let logGeneration = 0, clearingLog = false;
		let rows = [];

		const m = new form.Map('rtl-poe', _('RTL8238B PoE Control'),
			_('Use Save & Apply to apply RTL8238B PoE configuration changes.'));

		const s = m.section(form.NamedSection, 'main', 'poe');
		s.anonymous = true;

		const enabled = s.option(form.Flag, 'enabled', _('Enable RTL8238B PoE'));
		enabled.rmempty = false;
		enabled.default = '1';

		const budget = s.option(form.Value, 'budget_mw', _('PoE power budget (W)'),
			_('PoE power budget in Watts.'));
		budget.rmempty = false;
		budget.datatype = 'ufloat';

		const debug = s.option(form.Flag, 'debug', _('PoE debug logging'));
		debug.rmempty = false;
		debug.default = '0';

		const ports = [1, 2, 3, 4, 5, 6, 7]; /* LAN1..LAN7 */

		const outputs = ports.map(function(lan) {
			const option = 'port_lan%d'.format(lan);
			const o = s.option(form.ListValue, option, _('PoE output (LAN%d)').format(lan));
			o.value('1', _('PoE output enabled'));
			o.value('0', _('PoE output disabled'));
			o.rmempty = false;
			o.default = '1';
			return o;
		});

		function updateTelemetry() {
			if (!summaryNode)
				return;
			dom.content(summaryNode, [ status
				? _('PoE controller: %s, power consumption: %s W, power budget: %s W').format(
					status.controller || '--',
					status.total_power_mw == null ? '--' : (status.total_power_mw / 1000).toFixed(2),
					status.budget_mw == null ? '--' : (status.budget_mw / 1000).toFixed(2))
				: _('Unable to read PoE status. The controller may be unavailable or still initializing.') ]);

			rows.forEach(function(row) {
				const port = status && (status.ports || []).find(p => p.label === row.label);
				dom.content(row.state, [ port && port.enabled === false ? _('PoE power off') :
					port && port.power_mw > 0 ? _('PoE power good') : _('PoE not powered') ]);
				dom.content(row.power, [ port && port.power_mw != null ? '%.2f W'.format(port.power_mw / 1000) : '--' ]);
				dom.content(row.voltage, [ port && port.voltage_mv != null ? '%.1f V'.format(port.voltage_mv / 1000) : '--' ]);
				dom.content(row.current, [ port && port.current_ma != null ? '%d mA'.format(port.current_ma) : '--' ]);
			});
		}

		function updateLog() {
			const node = logNode, generation = logGeneration;
			if (!node || clearingLog)
				return Promise.resolve();
			return L.resolveDefault(fs.exec_direct(logCommand, []), null).then(function(data) {
				if (data == null || node !== logNode || generation !== logGeneration || clearingLog || node.value === data)
					return;
				const focused = document.activeElement === node;
				const follow = !focused && node.scrollTop + node.clientHeight >= node.scrollHeight - 4;
				const start = node.selectionStart, end = node.selectionEnd, top = node.scrollTop;
				node.value = data;
				if (focused)
					node.setSelectionRange(start, end);
				node.scrollTop = follow ? node.scrollHeight : top;
			});
		}

		function clearLog(ev) {
			const button = ev.currentTarget;
			button.disabled = true;
			clearingLog = true;
			logGeneration++;
			return fs.exec(clearLogCommand, []).then(function(result) {
				if (result.code !== 0)
					throw new Error(result.stderr || _('PoE log clear failed.'));
				logNode.value = '';
			}).catch(function(error) {
				ui.addNotification(null, E('p', {}, _('Unable to clear PoE log: %s').format(error.message)));
			}).finally(function() {
				clearingLog = false;
				button.disabled = m.readonly;
				return updateLog();
			});
		}

		s.render = function() {
			return Promise.all([
				enabled.render(0, 'main'), budget.render(1, 'main'), debug.render(2, 'main'),
				...outputs.map((o, i) => o.render(i + 3, 'main', true))
			]).then(function(nodes) {
				const headers = [ _('PoE LAN port'), _('PoE status'), _('PoE power'), _('PoE voltage'), _('PoE current') ]
					.map(title => E('th', { 'class': 'th' }, [ title ]));
				const table = E('table', { 'class': 'table cbi-section-table' }, [
					E('tr', { 'class': 'tr table-titles' }, headers)
				]);
				rows = ports.map(function(lan, i) {
					const row = { label: 'LAN%d'.format(lan) };
					[ [ 'state', _('PoE status') ], [ 'power', _('PoE power') ],
						[ 'voltage', _('PoE voltage') ], [ 'current', _('PoE current') ] ].forEach(function(cell) {
						row[cell[0]] = E('td', { 'class': 'td', 'data-title': cell[1] });
					});
					table.appendChild(E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td', 'data-title': _('PoE LAN port') }, [ 'LAN%d'.format(lan) ]),
						row.state, row.power, row.voltage, row.current, nodes[i + 3]
					]));
					return row;
				});
				summaryNode = E('p');
				const statusBox = E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, [ _('PoE status') ]), summaryNode, table
				]);
				logNode = E('textarea', {
					'class': 'cbi-input', 'readonly': '', 'rows': 12, 'spellcheck': 'false',
					'style': 'width:100%;font-family:monospace;resize:vertical'
				});
				const clear = E('button', {
					'type': 'button', 'class': 'cbi-button cbi-button-action',
					'disabled': m.readonly || null, 'click': clearLog
				}, [ _('Clear PoE log') ]);
				updateTelemetry();
				updateLog();
				return E('div', { 'data-section-id': 'main' }, [
					E('div', { 'class': 'cbi-section' }, [ nodes[0], nodes[1], nodes[2] ]),
					statusBox,
					E('div', { 'class': 'cbi-section', 'id': 'rtl-poe-log' }, [
						E('div', { 'class': 'rtl-poe-log-header' }, [
							E('h3', {}, [ _('PoE log') ]), clear
						]), logNode
					])
				]);
			});
		};

		poll.add(function() {
			return Promise.all([ runStatus().then(function(result) {
				status = result;
				updateTelemetry();
			}), updateLog() ]);
		}, 3);

		return m.render().then(function(node) {
			return E([], [ E('link', { 'rel': 'stylesheet',
				'href': L.resource('rtl-poe/overview.css') }), node ]);
		});
	}
});

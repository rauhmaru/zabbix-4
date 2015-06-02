<?php
/*
** Zabbix
** Copyright (C) 2001-2015 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/


require_once dirname(__FILE__).'/include/config.inc.php';
require_once dirname(__FILE__).'/include/hosts.inc.php';
require_once dirname(__FILE__).'/include/triggers.inc.php';
require_once dirname(__FILE__).'/include/items.inc.php';

$page['title'] = _('Trigger form');
$page['file'] = 'tr_logform.php';
$page['scripts'] = ['tr_logform.js'];
$page['type'] = detect_page_type(PAGE_TYPE_HTML);

define('ZBX_PAGE_NO_MENU', 1);

require_once dirname(__FILE__).'/include/page_header.php';

//		VAR			TYPE	OPTIONAL FLAGS	VALIDATION	EXCEPTION
$fields = [
	'description' =>	[T_ZBX_STR, O_OPT,  null,			NOT_EMPTY,			'isset({add}) || isset({update})'],
	'itemid' =>			[T_ZBX_INT, O_OPT,	 P_SYS,			DB_ID,				'isset({add}) || isset({update})'],
	'sform' =>			[T_ZBX_INT, O_OPT,  null,			IN('0,1'),			null],
	'sitems' =>			[T_ZBX_INT, O_OPT,  null,			IN('0,1'),			null],
	'triggerid' =>		[T_ZBX_INT, O_OPT,  P_SYS,			DB_ID,				null],
	'type' =>			[T_ZBX_INT, O_OPT,  null,			IN('0,1'),			null],
	'priority' =>		[T_ZBX_INT, O_OPT,  null,			IN('0,1,2,3,4,5'),	'isset({add}) || isset({update})'],
	'expressions' =>	[T_ZBX_STR, O_OPT,	 null,			NOT_EMPTY,			'isset({add}) || isset({update})'],
	'expr_type' =>		[T_ZBX_INT, O_OPT,  null,			IN('0,1'),			null],
	'comments' =>		[T_ZBX_STR, O_OPT,  null,			null,				null],
	'url' =>			[T_ZBX_STR, O_OPT,  null,			null,				null],
	'status' =>			[T_ZBX_INT, O_OPT,  null,			IN('0,1'),			null],
	'form_refresh' =>	[T_ZBX_INT, O_OPT,	 null,			null,				null],
	// actions
	'add' =>			[T_ZBX_STR, O_OPT,	 P_SYS|P_ACT,	null,				null],
	'update' =>			[T_ZBX_STR, O_OPT,	 P_SYS|P_ACT,	null,				null],
	'keys' => 			[T_ZBX_STR, O_OPT,  null,			null,				null],
];
check_fields($fields);

/*
 * Permissions
 */
if (getRequest('itemid') && !API::Item()->isWritable([$_REQUEST['itemid']])
		|| getRequest('triggerid') && !API::Trigger()->isWritable([$_REQUEST['triggerid']])) {
	access_deny();
}

$itemid = getRequest('itemid', 0);

$item = API::Item()->get([
	'output' => ['key_'],
	'selectHosts' => ['host'],
	'itemids' => $itemid,
	'limit' => 1
]);
$item = reset($item);
$host = reset($item['hosts']);

$constructor = new CTextTriggerConstructor(new CTriggerExpression());

/**
 * Save a trigger
 */
if (hasRequest('add') || hasRequest('update')) {
	show_messages();

	$exprs = getRequest('expressions', false);
	if ($exprs && ($expression = $constructor->getExpressionFromParts($host['host'], $item['key_'], $exprs))) {
		if (!check_right_on_trigger_by_expression(PERM_READ_WRITE, $expression)) {
			access_deny();
		}

		$now = time();
		$status = hasRequest('status') ? TRIGGER_STATUS_DISABLED : TRIGGER_STATUS_ENABLED;
		$type = TRIGGER_MULT_EVENT_ENABLED;

		if (hasRequest('triggerid')) {
			$triggerId = getRequest('triggerid');
			$description = getRequest('description', '');

			$triggersData = API::Trigger()->get([
				'triggerids' => [$triggerId],
				'output' => API_OUTPUT_EXTEND
			]);
			$triggerData = reset($triggersData);

			if ($triggerData['templateid']) {
				$description = $triggerData['description'];
				$expression = explode_exp($triggerData['expression']);
			}

			$trigger = [];
			$trigger['triggerid'] = $triggerId;
			$trigger['expression'] = $expression;
			$trigger['description'] = $description;
			$trigger['type'] = $type;
			$trigger['priority'] = getRequest('priority', 0);
			$trigger['status'] = $status;
			$trigger['comments'] = getRequest('comments', '');
			$trigger['url'] = getRequest('url', '');

			$result = (bool) API::Trigger()->update($trigger);

			$auditAction = AUDIT_ACTION_UPDATE;

			show_messages($result, _('Trigger updated'), _('Cannot update trigger'));
		}
		else {
			$trigger = [];
			$trigger['expression'] = $expression;
			$trigger['description'] = getRequest('description');
			$trigger['type'] = $type;
			$trigger['priority'] = getRequest('priority', 0);
			$trigger['status'] = $status;
			$trigger['comments'] = getRequest('comments', '');
			$trigger['url'] = getRequest('url', '');

			$result = (bool) API::Trigger()->create($trigger);
			if ($result) {
				$dbTriggers = API::Trigger()->get([
					'triggerids' => $result['triggerids'],
					'output' => ['triggerid']
				]);

				$dbTrigger = reset($dbTriggers);
				$triggerId = $dbTrigger['triggerid'];
			}

			$auditAction = AUDIT_ACTION_ADD;

			show_messages($result, _('Trigger added'), _('Cannot add trigger'));
		}

		if ($result) {
			DBstart();

			add_audit($auditAction, AUDIT_RESOURCE_TRIGGER,
				_('Trigger').' ['.$triggerId.'] ['.$trigger['description'].']'
			);

			DBend(true);

			unset($_REQUEST['sform']);

			zbx_add_post_js('closeForm("items.php");');
			require_once dirname(__FILE__).'/include/page_footer.php';
		}
	}
}

//------------------------ <FORM> ---------------------------

if (hasRequest('sform')) {
	$frmTRLog = new CFormTable(_('Trigger'), null, null, null, 'sform');
	$frmTRLog->setName('sform');
	$frmTRLog->setTableClass('formlongtable formtable');

	if (hasRequest('triggerid')) {
		$frmTRLog->addVar('triggerid', getRequest('triggerid'));
	}

	if (hasRequest('triggerid') && !hasRequest('form_refresh')) {
		$result = DBselect(
			'SELECT t.expression,t.description,t.priority,t.comments,t.url,t.status,t.type'.
			' FROM triggers t'.
			' WHERE t.triggerid='.zbx_dbstr(getRequest('triggerid')).
				' AND EXISTS ('.
					'SELECT NULL'.
					' FROM functions f,items i'.
					' WHERE t.triggerid=f.triggerid'.
						' AND f.itemid=i.itemid '.
						' AND i.value_type IN ('.
							ITEM_VALUE_TYPE_LOG.','.ITEM_VALUE_TYPE_TEXT.','.ITEM_VALUE_TYPE_STR.
						')'.
				')'
		);

		if ($row = DBfetch($result)) {
			$description = $row['description'];
			$expression = explode_exp($row['expression']);
			$type = $row['type'];
			$priority = $row['priority'];
			$comments = $row['comments'];
			$url = $row['url'];
			$status = $row['status'];
		}

		// break expression into parts
		$expressions = $constructor->getPartsFromExpression($expression);
	}
	else {
		$description = getRequest('description', '');
		$expressions = getRequest('expressions', []);
		$type = getRequest('type', 0);
		$priority = getRequest('priority', 0);
		$comments = getRequest('comments', '');
		$url = getRequest('url', '');
		$status = getRequest('status', 0);
	}

	$keys = getRequest('keys', []);

	$frmTRLog->addRow(_('Description'), new CTextBox('description', $description, 80));

	$itemName = '';

	$dbItems = DBfetchArray(DBselect(
		'SELECT itemid,hostid,name,key_,templateid'.
		' FROM items'.
		' WHERE itemid='.zbx_dbstr($itemid)
	));
	$dbItems = CMacrosResolverHelper::resolveItemNames($dbItems);
	$dbItem = reset($dbItems);

	if ($dbItem['templateid']) {
		$template = get_realhost_by_itemid($dbItem['templateid']);
		$itemName = $template['host'].NAME_DELIMITER.$dbItem['name_expanded'];
	}
	else {
		$host = get_host_by_hostid($dbItem['hostid']);
		$itemName = $host['name'].NAME_DELIMITER.$dbItem['name_expanded'];
	}

	$ctb = new CTextBox('item', $itemName, 80);
	$ctb->setId('item');
	$ctb->setAttribute('disabled', 'disabled');

	$script = "javascript: return PopUp('popup.php?dstfrm=".$frmTRLog->getName()."&dstfld1=itemid&dstfld2=item".
		"&srctbl=items&srcfld1=itemid&srcfld2=name');";
	$cbtn = (new CSubmit('select_item', _('Select')))->onCLick($script);

	$frmTRLog->addRow(_('Item'), [$ctb, $cbtn]);
	$frmTRLog->addVar('itemid', $itemid);


	$exp_select = new CComboBox('expr_type');
	$exp_select->setId('expr_type');
	$exp_select->addItem(CTextTriggerConstructor::EXPRESSION_TYPE_MATCH, _('Include'));
	$exp_select->addItem(CTextTriggerConstructor::EXPRESSION_TYPE_NO_MATCH, _('Exclude'));

	$ctb = new CTextBox('expression', '', 80);
	$ctb->setId('logexpr');

	$cb = (new CButton('add_exp', _('Add')))->onClick('javascript: add_logexpr();');
	$cbAdd = (new CButton('add_key_and', _('AND')))->onClick('javascript: add_keyword_and();');
	$cbOr = (new CButton('add_key_or', _('OR')))->onClick('javascript: add_keyword_or();');
	$cbIregexp = new CCheckBox('iregexp', 'no', null, 1);

	$frmTRLog->addRow(_('Expression'),
		[$ctb, BR(), $cbIregexp, 'iregexp', SPACE, $cbAdd, SPACE, $cbOr, SPACE, $exp_select, SPACE, $cb]
	);

	$keyTable = (new CTableInfo())
		->setId('key_list')
		->setHeader([_('Keyword'), _('Type'), _('Action')]);

	$table = (new CTableInfo())
		->setId('exp_list')
		->setHeader([_('Expression'), _('Type'), _('Position'), _('Action')]);

	$maxId = 0;
	foreach ($expressions as $id => $expr) {
		$imgup = new CImg('images/general/arrow_up.png', 'up', 12, 14);
		$imgup->onClick('javascript: element_up("logtr'.$id.'");');
		$imgup->onMouseover('javascript: this.style.cursor = "pointer";');
		$imgup->addClass('updown');

		$imgdn = new CImg('images/general/arrow_down.png', 'down', 12, 14);
		$imgdn->onClick('javascript: element_down("logtr'.$id.'");');
		$imgdn->onMouseover('javascript: this.style.cursor = "pointer";');
		$imgdn->addClass('updown');

		$del_url = (new CSpan(_('Delete')))
			->addClass('link')
			->onClick('javascript:'.
				' if (confirm('.CJs::encodeJson(_('Delete expression?')).')) remove_expression("logtr'.$id.'");'.
				' return false;'
			);

		$row = new CRow([
			htmlspecialchars($expr['value']),
			($expr['type'] == CTextTriggerConstructor::EXPRESSION_TYPE_MATCH) ? _('Include') : _('Exclude'),
			[$imgup, ' ', $imgdn],
			$del_url
		]);
		$row->setId('logtr'.$id);
		$table->addRow($row);

		$frmTRLog->addVar('expressions['.$id.'][value]', $expr['value']);
		$frmTRLog->addVar('expressions['.$id.'][type]', $expr['type']);

		$maxId = max($maxId, $id);
	}

	zbx_add_post_js('logexpr_count='.($maxId + 1).';');
	zbx_add_post_js('processExpressionList();');

	$maxId = 0;
	foreach ($keys as $id => $val) {
		$del_url = (new CLink(_('Delete'), '#'))
			->addClass('action')
			->onClick('javascript:'.
			' if (confirm('.CJs::encodeJson(_('Delete keyword?')).')) remove_keyword("keytr'.$id.'");'.
			' return false;');
		$row = new CRow([htmlspecialchars($val['value']), $val['type'], $del_url]);
		$row->setId('keytr'.$id);
		$keyTable->addRow($row);

		$frmTRLog->addVar('keys['.$id.'][value]', $val['value']);
		$frmTRLog->addVar('keys['.$id.'][type]', $val['type']);

		$maxId = max($maxId, $id);
	}

	zbx_add_post_js('key_count='.($maxId + 1).';');

	$frmTRLog->addRow(SPACE, $keyTable);
	$frmTRLog->addRow(SPACE, $table);

	$sev_select = new CComboBox('priority', $priority);

	$config = select_config();

	$severityNames = [];
	for ($severity = TRIGGER_SEVERITY_NOT_CLASSIFIED; $severity < TRIGGER_SEVERITY_COUNT; $severity++) {
		$severityNames[] = getSeverityName($severity, $config);
	}
	$sev_select->addItems($severityNames);

	$frmTRLog->addRow(_('Severity'), $sev_select);
	$frmTRLog->addRow(_('Comments'), new CTextArea('comments', $comments));
	$frmTRLog->addRow(_('URL'), new CTextBox('url', $url, 80));
	$frmTRLog->addRow(_('Disabled'),
		new CCheckBox('status', $status == TRIGGER_STATUS_DISABLED ? 'yes' : 'no', null, 1)
	);
	if (hasRequest('triggerid')) {
		$frmTRLog->addItemToBottomRow(new CSubmit('update', _('Update')));
	}
	else {
		$frmTRLog->addItemToBottomRow(new CSubmit('add', _('Add')));
	}
	$frmTRLog->addItemToBottomRow(SPACE);
	$frmTRLog->addItemToBottomRow(
		(new CButton('cancel', _('Cancel')))->onClick('javascript: self.close();')
	);

	$frmTRLog->show();
}

require_once dirname(__FILE__).'/include/page_footer.php';

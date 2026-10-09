/* Game servers: the RomM servers the console downloads games and BIOS files from and keeps
 * its saves on (src/remote/servers.h), set up step by step. */
'use strict';
const WIZARD_STEPS = ['server', 'features', 'account', 'done'];
let servers = [], wizard = null;
const FEATURE_NAMES = [['games', 'Games', 'download'], ['firmware', 'BIOS', 'file-text'], ['saves', 'Save sync', 'save']];

function serverPills(server) {
  const pills = element('div', undefined, 'server-pills');
  for (const [key, name, iconName] of FEATURE_NAMES) {
    if (!server[key]) continue;
    const pill = element('span', undefined, 'kind-pill got'); pill.append(uiIcon(iconName), document.createTextNode(key === 'saves' && server.states ? name + ' + states' : name));
    pills.append(pill);
  }
  if (!pills.childElementCount) pills.append(element('span', 'Not used', 'kind-pill had'));
  return pills;
}
function drawServers() {
  const list = $('#server-list'); list.replaceChildren();
  if (!servers.length) {
    const empty = element('div', undefined, 'empty-state'); empty.append(uiIcon('database'), element('p', 'No game server yet. Add your RomM to download games and BIOS files and keep your saves on it.'));
    list.append(empty); return;
  }
  for (const server of servers) {
    const row = element('div', undefined, 'server-row');
    const symbol = element('span', undefined, 'server-symbol'); symbol.append(uiIcon('database'));
    const about = element('div', undefined, 'server-about');
    about.append(element('strong', server.name), element('span', server.url + (server.user ? ' · signed in as ' + server.user : server.signed_in ? '' : ' · not signed in'), 'muted'), serverPills(server));
    const actions = element('div', undefined, 'server-actions');
    if (!server.save_sync_only) {
      const edit = element('button', undefined, 'secondary'); edit.type = 'button'; edit.append(uiIcon('pencil'), document.createTextNode('Edit'));
      edit.addEventListener('click', () => openWizard(server)); actions.append(edit);
    }
    const remove = element('button', undefined, 'secondary'); remove.type = 'button'; remove.append(uiIcon('trash-2'), document.createTextNode('Remove'));
    remove.setAttribute('aria-label', 'Remove ' + server.name);
    remove.addEventListener('click', () => removeServer(server)); actions.append(remove);
    row.append(symbol, about, actions); list.append(row);
  }
}
async function loadServers() {
  try { servers = (await api('/api/servers')).servers; drawServers(); }
  catch (error) { $('#server-list').replaceChildren(element('p', error.message, 'list-message inline-error')); }
}
async function removeServer(server) {
  const what = server.save_sync_only ? 'the save sync with ' + server.name : server.name;
  if (!confirm(`Remove ${what} from the console? Games already downloaded stay, and so do your saves on both sides.`)) return;
  try { servers = (await api('/api/servers?name=' + encodeURIComponent(server.name), { method: 'DELETE' })).servers; drawServers(); announce(`${server.name} was removed.`); }
  catch (error) { announce(error.message, true); }
}

/* The wizard: one step at a time, each checked before the next. */
function wizardMessage(text, failure = false) { $('#wizard-status').textContent = text; $('#wizard-status').classList.toggle('inline-error', failure); }
function wizardStep(step) {
  wizard.step = step;
  const index = WIZARD_STEPS.indexOf(step);
  $$('[data-wizard-step]').forEach(button => {
    const at = WIZARD_STEPS.indexOf(button.dataset.wizardStep);
    button.setAttribute('aria-pressed', String(at === index));
    button.disabled = at > wizard.reached || step === 'done';
  });
  $$('[data-wizard-panel]').forEach(panel => { panel.hidden = panel.dataset.wizardPanel !== step; });
  $('#wizard-back').hidden = index === 0 || step === 'done';
  $('#wizard-next').textContent = step === 'account' ? (wizard.editing ? 'Save' : 'Sign in and save') : step === 'done' ? 'Close' : 'Next';
  wizardMessage('');
  $('#wizard-states').disabled = !$('#wizard-saves').checked;
  if (step === 'server') $('#wizard-url').focus();
  if (step === 'account') {
    const keep = wizard.editing && wizard.editing.signed_in && wizard.url === wizard.editing.url;
    $('#wizard-keep').hidden = !keep;
    $('#wizard-keep').textContent = keep ? `Signed in${wizard.editing.user ? ' as ' + wizard.editing.user : ''}. Leave the password empty to keep that sign-in.` : '';
    $(keep || $('#wizard-user').value ? '#wizard-password' : '#wizard-user').focus();
  }
}
function openWizard(server = null) {
  wizard = { step: 'server', reached: server ? 2 : 0, editing: server, url: server?.url || '', busy: false };
  $('#wizard-title').textContent = server ? 'Edit ' + server.name : 'Add a game server';
  $('#wizard-url').value = server?.url || ''; $('#wizard-name').value = server?.name || '';
  $('#wizard-games').checked = server ? server.games : true; $('#wizard-firmware').checked = server ? server.firmware : true;
  $('#wizard-saves').checked = server ? server.saves : false; $('#wizard-states').checked = server ? server.states || !server.saves : true;
  $('#wizard-user').value = server?.user || ''; $('#wizard-password').value = '';
  $('#wizard-found').textContent = ''; $('#wizard-summary').replaceChildren();
  $('#server-wizard').showModal(); wizardStep('server');
}
function closeWizard() {
  if (wizard?.busy) return;
  $('#wizard-password').value = ''; $('#server-wizard').close();
  if (wizard?.step === 'done') loadServers();
  wizard = null;
}
function busy(state, text = '') {
  wizard.busy = state; $('#server-wizard').toggleAttribute('aria-busy', state);
  $('#wizard-next').disabled = $('#wizard-back').disabled = state; if (text) wizardMessage(text);
}
async function probeServer() {
  const url = $('#wizard-url').value.trim();
  if (!url) { wizardMessage('Enter your RomM’s address.', true); return false; }
  busy(true, 'Looking for RomM at ' + url + '…');
  try {
    const found = await api('/api/servers/probe', { method: 'POST', body: JSON.stringify({ type: 'romm', url }), headers: { 'Content-Type': 'application/json' }, signal: AbortSignal.timeout(30000) });
    if (!found.ok) { wizardMessage(found.error, true); return false; }
    wizard.url = found.url; $('#wizard-url').value = found.url;
    if (!$('#wizard-name').value.trim()) $('#wizard-name').value = new URL(found.url).hostname;
    $('#wizard-found').replaceChildren(checkNode(), element('span', `RomM ${found.version} answers at ${found.url}`));
    return true;
  } catch (error) { wizardMessage(error.message, true); return false; }
  finally { busy(false); }
}
async function saveServer() {
  const body = { type: 'romm', previous: wizard.editing?.name || '', name: $('#wizard-name').value.trim(), url: wizard.url,
    user: $('#wizard-user').value.trim(), password: $('#wizard-password').value,
    games: $('#wizard-games').checked, firmware: $('#wizard-firmware').checked, saves: $('#wizard-saves').checked, states: $('#wizard-states').checked };
  const keep = wizard.editing && wizard.editing.signed_in && wizard.url === wizard.editing.url;
  if (!keep && (!body.user || !body.password)) { wizardMessage('Enter your RomM name and password.', true); return false; }
  if (body.password && !body.user) { wizardMessage('Enter your RomM name too.', true); return false; }
  busy(true, body.password ? 'Signing in to RomM…' : 'Saving…');
  try {
    servers = (await api('/api/servers', { method: 'POST', body: JSON.stringify(body), headers: { 'Content-Type': 'application/json' }, signal: AbortSignal.timeout(60000) })).servers;
    $('#wizard-password').value = '';
    const saved = servers.find(s => s.name === (body.name || new URL(body.url).hostname)) || body;
    const rows = [['Server', `${saved.name} · ${saved.url}`], ['Signed in as', saved.user || body.user || '—'],
      ['Game downloads', saved.games ? 'On' : 'Off'], ['BIOS downloads', saved.firmware ? 'On' : 'Off'],
      ['Save sync', saved.saves ? (saved.states ? 'On, with save states' : 'On, saves only') : 'Off']];
    $('#wizard-summary').replaceChildren(...rows.flatMap(([k, v]) => [element('dt', k), element('dd', v)]));
    drawServers(); announce(`${saved.name} is set up.`);
    return true;
  } catch (error) { wizardMessage(error.message, true); return false; }
  finally { busy(false); }
}
$('#wizard-next').addEventListener('click', async () => {
  if (!wizard || wizard.busy) return;
  const step = wizard.step;
  if (step === 'done') { closeWizard(); return; }
  if (step === 'server' && !await probeServer()) return;
  if (step === 'features' && !['games', 'firmware', 'saves'].some(k => $('#wizard-' + k).checked)) { wizardMessage('Choose at least one thing the server is for.', true); return; }
  if (step === 'account' && !await saveServer()) return;
  const next = WIZARD_STEPS[WIZARD_STEPS.indexOf(step) + 1];
  wizard.reached = Math.max(wizard.reached, WIZARD_STEPS.indexOf(next)); wizardStep(next);
});
$('#wizard-back').addEventListener('click', () => { if (wizard && !wizard.busy) wizardStep(WIZARD_STEPS[Math.max(0, WIZARD_STEPS.indexOf(wizard.step) - 1)]); });
$$('[data-wizard-step]').forEach(button => button.addEventListener('click', () => { if (wizard && !wizard.busy && !button.disabled) wizardStep(button.dataset.wizardStep); }));
$('#wizard-url').addEventListener('input', () => { if (!wizard) return; wizard.reached = 0; $('#wizard-found').textContent = ''; wizardStep('server'); });
$('#wizard-url').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); $('#wizard-next').click(); } });
$('#wizard-password').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); $('#wizard-next').click(); } });
$('#wizard-saves').addEventListener('change', () => { $('#wizard-states').disabled = !$('#wizard-saves').checked; });
$('#wizard-close').addEventListener('click', closeWizard);
$('#server-wizard').addEventListener('cancel', event => { event.preventDefault(); closeWizard(); });
$('#add-server').addEventListener('click', () => openWizard());
if (pageFromHash() === 'servers') loadServers();

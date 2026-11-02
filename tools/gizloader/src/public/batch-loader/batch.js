// Shared Browser/Desktop workbench. Transport clients own package parsing and I/O.
import { H2LoaderCommands, commandAvailable } from './protocol.js';
import { H2LoaderController } from './controller.js';
import { renderDeviceProgress } from './progress.js';
import { pickFile, downloadFile, ForgePickerError } from '@gizclaw/forge-picker';
import { downloadFirmwarePackage, firmwareUrl } from './download.js';

export function mountBatchLoader(root, { language, t, mode, createClient }) {
  const desktop = mode === 'desktop';
  const abort = new AbortController();
  let disposed = false;
  let packageInspection = 0;
  let refreshPending = false;
  let connectionTimer;
  let packageSelection = null;
  const on = (element, type, listener) => element.addEventListener(type, listener, { signal: abort.signal });
  const $ = (selector) => root.querySelector(selector);
  const notice = $('#batch-notice');
  const packageResult = $('#batch-package-result');
  const deviceRows = $('#device-rows');
  const progressBoxes = new Map();
  const operationButtons = [...root.querySelectorAll('[data-operation]')];
  const rebooting = new Set(['install', 'rebootApp', 'rebootLoader', 'restart']);
  const state = { client: null, devices: [], packageFile: null, packageInfo: null, initializing: true, scanning: false };
  if ($('#batch-package-file')) $('#batch-package-file').value = '';
  packageResult.textContent = t('尚未选择固件包');

  function say(message, tone = '') {
    if (disposed) return;
    notice.textContent = message;
    notice.dataset.tone = tone;
  }

  function hex(value) {
    return Number.isInteger(value) ? value.toString(16).toUpperCase().padStart(4, '0') : '----';
  }

  function hasCommand(device, command) {
    try { return commandAvailable(device.commandAvailability, command); }
    catch { return false; }
  }

  function isEligible(device, operation) {
    if (!device.port || controller.isBusy(device.portId)) return false;
    if (desktop && operation === 'disconnect') return device.active;
    if (desktop && operation === 'connect') return device.present && !device.active;
    if (desktop && (!device.present || !device.connected)) return false;
    if (operation === 'status') return true;
    if (operation === 'rebootLoader') return device.role === 'app' && hasCommand(device, H2LoaderCommands.REBOOT_LOADER);
    if (operation === 'rebootApp') return device.role === 'h2loader' && hasCommand(device, H2LoaderCommands.REBOOT_APP);
    if (operation === 'restart') return device.role === 'app'
      ? hasCommand(device, H2LoaderCommands.REBOOT_APP)
      : device.role === 'h2loader' && hasCommand(device, H2LoaderCommands.REBOOT_LOADER);
    return hasCommand(device, H2LoaderCommands.STAGE_PAYLOAD) && Boolean(state.packageFile);
  }

  function projectStatus(result) {
    const active = result?.active && typeof result.active === 'object' ? result.active : {};
    return {
      board: typeof result?.board === 'string' ? result.board : undefined,
      target: typeof result?.target === 'string' ? result.target : undefined,
      capabilities: Number.isInteger(result?.capabilities) ? result.capabilities : undefined,
      commandAvailability: Number.isInteger(result?.commandAvailability) ? result.commandAvailability : undefined,
      role: active.role === 'loader' ? 'h2loader' : active.role === 'app' ? 'app' : undefined,
      version: typeof active.version === 'string' ? active.version : undefined,
      statusSnapshot: result,
    };
  }

  function cell(row, content) {
    const td = row.insertCell();
    if (content instanceof Node) td.append(content);
    else td.textContent = content ?? '—';
    return td;
  }

  function smallLine(primary, secondary) {
    const box = document.createElement('span');
    const strong = document.createElement('strong');
    strong.textContent = primary ?? '—';
    const small = document.createElement('small');
    small.textContent = secondary ?? '';
    box.append(strong, small);
    return box;
  }

  function renderRows() {
    deviceRows.replaceChildren();
    progressBoxes.clear();
    $('#device-count').textContent = language === 'en' ? `${state.devices.filter((item) => item.port).length} devices` : `${state.devices.filter((item) => item.port).length} 台`;
    if (!state.devices.length) {
      const row = deviceRows.insertRow();
      let empty = state.initializing ? t('正在读取已授权端口…') : t('尚无已授权设备。点击“授权端口”选择设备。');
      if (desktop) {
        empty = root.ownerDocument.createElement('span');
        empty.className = 'desktop-empty-copy';
        empty.textContent = t('尚无串口设备。连接设备后点击“扫描串口”。');
      }
      cell(row, empty).colSpan = desktop ? 7 : 8;
      row.firstElementChild.className = 'batch-empty';
    }
    for (const device of state.devices) {
      const row = deviceRows.insertRow();
      const busy = controller.isBusy(device.portId);
      if (!desktop) {
      const checked = document.createElement('input');
      checked.type = 'checkbox';
      checked.checked = device.selected;
      checked.disabled = busy;
      checked.setAttribute('aria-label', language === 'en' ? `Select ${device.portId}` : `选择 ${device.portId}`);
      on(checked, 'change', () => { device.selected = checked.checked; renderControls(); });
      cell(row, checked);
      }
      cell(row, smallLine(desktop ? device.portId : device.name, desktop ? `${device.name} · USB ${hex(device.usbVendorId)}:${hex(device.usbProductId)}${device.remembered ? " · 上次列表" : ""}${device.present === false ? " · 未检测到" : ""}` : `${device.portId} · USB ${hex(device.usbVendorId)}:${hex(device.usbProductId)}`));
      cell(row, smallLine(device.board, device.target));
      cell(row, smallLine(device.role === 'app' ? 'App' : device.role === 'h2loader' ? 'Loader' : t('未识别'), device.version));
      const pill = document.createElement('span');
      pill.className = 'batch-pill';
      const displayState = desktop && !busy && !device.error ? device.connected ? 'connected' : 'disconnected' : device.state;
      pill.dataset.state = displayState;
      pill.textContent = ({ready:t('就绪'),queued:t('排队中'),busy:t('处理中'),stale:t('待同步状态'),success:t('完成'),error:t('失败'),cancelled:t('已取消'),offline:t('离线')})[displayState] ?? ({ connected: '已连接', disconnected: device.present === false ? '未检测到' : '未连接' })[displayState] ?? displayState;
      cell(row, pill);
      const available = [
        hasCommand(device, H2LoaderCommands.STATUS) && t('状态'),
        hasCommand(device, H2LoaderCommands.STAGE_PAYLOAD) && t('发送'),
        hasCommand(device, H2LoaderCommands.REBOOT_UPGRADE) && t('升级'),
        (hasCommand(device, H2LoaderCommands.REBOOT_APP) || hasCommand(device, H2LoaderCommands.REBOOT_LOADER)) && t('重启'),
      ].filter(Boolean).join(' · ');
      cell(row, available || '—');
      const progressBox = renderDeviceProgress(device, { language, t });
      progressBoxes.set(device.portId, progressBox);
      cell(row, progressBox);
      const actions = document.createElement('div');
      actions.className = 'batch-row-actions';
      const statusButton = document.createElement('button');
      statusButton.type = 'button';
      statusButton.textContent = t('状态');
      statusButton.disabled = busy;
      if (!desktop) on(statusButton, 'click', () => void run('status', [device.portId]));
      const cancelButton = document.createElement('button');
      cancelButton.type = 'button';
      cancelButton.textContent = t('取消');
      cancelButton.disabled = !busy;
      on(cancelButton, 'click', () => cancelOne(device.portId));
      const forgetButton = document.createElement('button');
      forgetButton.type = 'button';
      forgetButton.textContent = t('移除授权');
      forgetButton.disabled = busy;
      on(forgetButton, 'click', () => void forgetOne(device.portId));
      if (desktop) {
        const toggle = document.createElement('button'); toggle.type = 'button';
        toggle.textContent = device.active ? '断开' : '连接';
        toggle.disabled = busy || state.scanning || (device.present === false && !device.active);
        on(toggle, 'click', () => device.active ? void disconnectOne(device.portId) : void run('connect', [device.portId]));
        forgetButton.textContent = '移除'; forgetButton.disabled = busy || state.scanning;
        actions.append(toggle, forgetButton); if (busy) actions.append(cancelButton);
      } else actions.append(statusButton, cancelButton, forgetButton);
      cell(row, actions);
    }
    const all = $('#select-all');
    if (all) { all.checked = state.devices.length > 0 && state.devices.every((item) => item.selected);
      all.indeterminate = state.devices.some((item) => item.selected) && !all.checked; }
  }

  function renderControls() {
    if (disposed) return;
    const selected = state.devices.filter((device) => desktop || device.selected);
    if ($('#desktop-connection-count')) $('#desktop-connection-count').textContent = `已连接 ${state.devices.filter(device => device.connected).length} 台`;
    const connectionAction = $('#connect-ports');
    if (connectionAction) {
      const active = state.devices.some(device => device.active);
      connectionAction.dataset.operation = active ? 'disconnect' : 'connect';
      $('#connection-action-label').textContent = active ? '断开串口' : '连接串口';
    }
    for (const button of operationButtons) {
      button.disabled = !state.client || state.scanning || !selected.some((device) => isEligible(device, button.dataset.operation));
    }
    if (connectionAction && controller.busyIds().length > 0) connectionAction.disabled = true;
    const discovery = desktop ? $('#scan-ports') : $('#authorize-port');
    discovery.disabled = !state.client || !state.client.supported() || state.scanning || controller.busyIds().length > 0;
    if ($('#batch-package-choose')) $('#batch-package-choose').disabled = !state.client;
    $('#batch-forge-picker').disabled = !state.client || Boolean(packageSelection);
    $('#batch-url-toggle').disabled = !state.client;
    $('#batch-url-download').disabled = !state.client;
    $('#batch-forge-cancel').hidden = !packageSelection;
    $('#cancel-all').disabled = controller.busyIds().length === 0;
  }

  function render() {
    if (disposed) return;
    renderRows();
    renderControls();
  }

  const controller = new H2LoaderController((portId, patch) => {
    const device = state.devices.find((item) => item.portId === portId);
    if (!device) return;
    const progressOnly = device.state === 'busy' && patch.state === 'busy' && patch.phase !== undefined;
    const { statusSnapshot, ...delta } = patch;
    for (const [key, value] of Object.entries(delta)) if (value !== undefined) device[key] = value;
    if (statusSnapshot) device.status = statusSnapshot;
    const progressBox = progressBoxes.get(portId);
    if (progressOnly && progressBox) renderDeviceProgress(device, { language, t }, progressBox);
    else render();
  }, 16);

  function replacePorts(ports) {
    const previous = new Map(state.devices.map((item) => [item.portId, item]));
    const seen = new Set();
    state.devices = ports.filter((port) => {
      if (seen.has(port.id)) return false;
      seen.add(port.id); return true;
    }).map((port, index) => ({
      ...previous.get(port.id),
      ...(desktop && Object.hasOwn(port, 'initialStatus') ? projectStatus(port.initialStatus) : {}),
      port, portId: port.id,
      name: port.name ?? previous.get(port.id)?.name ?? (language === 'en' ? `Device ${index + 1}` : `设备 ${index + 1}`),
      selected: desktop || (previous.get(port.id)?.selected ?? port.selectedByDefault ?? true),
      present: port.present !== false, remembered: Boolean(port.remembered), connected: Boolean(port.connected), active: Boolean(port.active),
      state: desktop && port.initialStatus ? 'success' : desktop && port.initialError ? 'error' : 'ready',
      status: desktop && Object.hasOwn(port, 'initialStatus') ? port.initialStatus : previous.get(port.id)?.status,
      error: desktop ? port.initialError ?? null : null,
      progress: desktop && port.initialStatus ? 100 : 0,
      usbVendorId: port.usbVendorId, usbProductId: port.usbProductId,
    }));
  }
  async function pollConnections() {
    if (disposed || !state.client?.readConnections) return;
    try {
      const states = await state.client.readConnections(); const byId = new Map(states.map(item => [item.id,item]));
      let changed = false;
      for (const device of state.devices) {
        const current = byId.get(device.portId); const connected = Boolean(current?.connected);
        if (device.active !== Boolean(current?.active)) { device.active = Boolean(current?.active); changed = true; }
        if (device.connected !== connected) { device.connected = connected; changed = true; }
        const status = current?.status;
        if (connected && status && JSON.stringify(device.status) !== JSON.stringify(status)) { Object.assign(device, projectStatus(status)); device.status = status; changed = true; }
      }
      if (changed) render();
    } catch { /* A closing client stops this poll through dispose(). */ }
    if (!disposed) connectionTimer = setTimeout(pollConnections, 500);
  }
  async function disconnectOne(portId) {
    const device = state.devices.find(item => item.portId === portId);
    if (!device || controller.isBusy(portId)) return;
    try { await state.client.disconnect(device.port); device.connected = false; device.active = false; device.error = null; device.state = 'ready'; say(`${portId} 已断开。`); render(); }
    catch(error) { say(String(error), 'error'); }
  }
  async function scanPorts() {
    if (!state.client?.supported() || state.scanning) return;
    state.scanning = true;
    renderControls();
    if (desktop) say('正在扫描串口…');
    try {
      const ports = await (state.client.getPorts?.() ?? state.client.getAuthorizedPorts());
      replacePorts(ports);
      state.initializing = false; render();
      if (desktop) say(`列表内 ${state.devices.length} 个串口，点击“连接串口”建立长连接。`);
      else say(ports.length ? language === 'en' ? `Found ${state.devices.length} authorized ports. Syncing device status.` : `已读取 ${state.devices.length} 个授权端口，正在同步设备状态。` : t('尚无已授权端口。'));
      const selected = state.devices.filter((device) => device.selected);
      if (!desktop && selected.length) await run('status', selected.map((device) => device.portId));
    } catch (error) {
      state.initializing = false;
      say(language === 'en' ? `Scan failed: ${error instanceof Error ? error.message : String(error)}` : `扫描失败：${error instanceof Error ? error.message : String(error)}`, 'error');
      render();
    } finally {
      state.scanning = false;
      render();
    }
  }

  async function run(operation, ids) {
    const requested = state.devices.filter((device) => ids ? ids.includes(device.portId) : desktop || device.selected);
    const eligible = requested.filter((device) => isEligible(device, operation));
    if (!eligible.length) { say(t('所选设备当前不支持这项操作，或需要先选择固件包。'), 'error'); return; }
    say(language === 'en' ? `Running the operation on ${eligible.length} devices…` : `正在对 ${eligible.length} 台设备执行操作…`);
    const packageFile = state.packageFile;
    const releaseLease = ['stage', 'install'].includes(operation) ? state.client.retainPackage?.(packageFile) : undefined;
    let ok;
    try { ok = await controller.run(eligible, async (device, options) => {
      let result;
      if (operation === 'disconnect') {
        await state.client.disconnect(device.port);
        return { connected: false, active: false, error: null, detail: '串口已断开' };
      }
      if (operation === 'connect') result = await state.client.connect(device.port, options);
      else if (operation === 'status') result = await state.client.status(device.port, options);
      else if (operation === 'stage') result = await state.client.stage(device.port, packageFile, options);
      else if (operation === 'install') {
        const staged = await state.client.stage(device.port, packageFile, {
          ...options,
          onProgress: (progress) => options.onProgress({ ...progress, progress: Math.round((progress.progress ?? progress.percent ?? 0) * .75) }),
        });
        if (!commandAvailable(staged.commandAvailability, H2LoaderCommands.REBOOT_UPGRADE)) {
          throw new Error(t('包已发送，但设备未开放升级切换命令。'));
        }
        await state.client.rebootUpgrade(device.port, {
          ...options,
          onProgress: (progress) => options.onProgress({ ...progress, progress: 75 + Math.round((progress.progress ?? progress.percent ?? 0) * .25) }),
        });
        result = staged;
      } else if (operation === 'restart') {
        result = device.role === 'app'
          ? await state.client.rebootApp(device.port, options)
          : await state.client.rebootLoader(device.port, options);
      } else result = await state.client[operation](device.port, options);
      return { ...projectStatus(result), ...(desktop ? { connected: true, active: true, error: null } : {}), stale: !desktop && rebooting.has(operation), detail: operation === 'stage' ? t('固件包已发送') : undefined };
    }, { concurrency: operation === 'status' && !desktop ? eligible.length : undefined }); }
    finally { releaseLease?.(); }
    const completed = desktop ? operation === 'connect' ? '连接完成，日志和设备操作共用此长连接。' : operation === 'disconnect' ? '串口已全部断开。' : '任务完成。可刷新状态核对运行镜像。' : t('任务完成。请同步状态并核对运行镜像。');
    say(ok ? completed : t('部分设备操作未完成，请查看每行详情。'), ok ? '' : 'error');
    render();
    if (refreshPending && controller.busyIds().length === 0) { refreshPending = false; await scanPorts(); }
  }

  async function forgetOne(portId) {
    const device = state.devices.find((item) => item.portId === portId);
    if (!device || controller.isBusy(portId)) return;
    try {
      await state.client.forgetPort(device.port);
      state.devices = state.devices.filter((item) => item.portId !== portId);
      say(desktop ? `${portId} 已移除；再次扫描会重新加入当前可用串口。` : language === 'en' ? `${portId} was removed from the browser's authorized ports.` : `${portId} 已从浏览器授权列表移除。`);
      render();
    } catch (error) { say(language === 'en' ? `Removal failed: ${error instanceof Error ? error.message : String(error)}` : `移除失败：${error instanceof Error ? error.message : String(error)}`, 'error'); }
  }

  function cancelOne(portId) {
    controller.cancelDevice(portId);
    const device = state.devices.find((item) => item.portId === portId);
    if (device) { device.state = 'cancelled'; device.detail = t('任务已取消'); }
    render();
  }

  if (!desktop) on($('#select-all'), 'change', (event) => {
    for (const device of state.devices) if (!controller.isBusy(device.portId)) device.selected = event.target.checked;
    render();
  });
  if (desktop) {
    on($('#scan-ports'), 'click', () => void scanPorts());
  }
  else on($('#authorize-port'), 'click', async () => {
    try { await state.client.requestPort(); await scanPorts(); }
    catch (error) {
      if (error instanceof DOMException && error.name === 'NotFoundError') return;
      say(language === 'en' ? `Authorization failed: ${error instanceof Error ? error.message : String(error)}` : `授权失败：${error instanceof Error ? error.message : String(error)}`, 'error');
    }
  });
  on($('#cancel-all'), 'click', () => {
    const busy = new Set(controller.busyIds());
    controller.cancel();
    for (const device of state.devices) if (busy.has(device.portId)) { device.state = 'cancelled'; device.detail = t('任务已取消'); }
    say(t('已取消所有排队和进行中的任务。'));
    render();
  });
  for (const button of operationButtons) on(button, 'click', () => void run(button.dataset.operation));
  function beginSelection() {
    packageSelection?.abort();
    const selection = new AbortController();
    packageSelection = selection;
    packageInspection += 1;
    state.client?.releasePackage?.(state.packageFile);
    state.packageFile = null;
    state.packageInfo = null;
    renderControls();
    return selection;
  }
  const currentSelection = selection => !disposed && packageSelection === selection && !selection.signal.aborted;
  function finishSelection(selection) {
    if (packageSelection !== selection) return;
    packageSelection = null;
    renderControls();
  }
  function renderPackageInfo(file, info) {
    const role = info.role === 'h2loader' ? 'Loader' : 'App';
    const board = info.board ?? t('未知板型');
    const target = info.target ?? t('未知 Target');
    const version = `${language === 'en' ? 'Version' : '版本'} ${info.version ?? t('未标版本')}`;
    const size = `${(file.size / 1048576).toFixed(2)} MiB`;
    const verified = language === 'en' ? 'Verified' : '已校验';
    if (!desktop) {
      packageResult.textContent = `${file.name} · ${role} · ${board} · ${target} · ${version} · ${size} · ${verified}`;
      return;
    }
    const element = (className, text) => {
      const node = root.ownerDocument.createElement('span');
      node.className = className;
      node.textContent = text;
      return node;
    };
    const name = element('desktop-package-name', file.name);
    name.title = file.name;
    const metadata = element('desktop-package-meta', '');
    metadata.append(
      element('desktop-package-role', role),
      element('desktop-package-target', `${board} · ${target}`),
      element('desktop-package-version', version),
      element('desktop-package-size', size),
      element('desktop-package-verified', verified),
    );
    packageResult.replaceChildren(name, metadata);
  }
  async function inspectFile(file, selection = beginSelection()) {
    const inspection = packageInspection;
    if (!file) { packageResult.textContent = t('尚未选择固件包'); finishSelection(selection); return; }
    packageResult.textContent = t('正在校验固件包…');
    try {
      const info = await state.client.inspectPackage(file);
      if (!currentSelection(selection) || inspection !== packageInspection) { state.client.releasePackage?.(file); return; }
      const role = info.role;
      if (role !== 'app' && role !== 'h2loader') throw new Error(t('无法识别包类型，请选择 App 或 Loader 固件包。'));
      state.packageFile = file;
      state.packageInfo = info;
      renderPackageInfo(file, info);
      say(t('固件包校验通过，可选择设备执行发送。'));
    } catch (error) {
      state.client.releasePackage?.(file);
      if (!currentSelection(selection) || inspection !== packageInspection) return;
      packageResult.textContent = language === 'en' ? `Validation failed: ${error instanceof Error ? error.message : String(error)}` : `校验失败：${error instanceof Error ? error.message : String(error)}`;
      say(t('固件包不可用。'), 'error');
    }
    finishSelection(selection);
  }
  if (desktop) on($('#batch-package-choose'), 'click', async () => {
    const selection = beginSelection();
    try {
      const file = await state.client.choosePackage();
      if (!currentSelection(selection)) { if (file) state.client.releasePackage?.(file); return; }
      if (file) await inspectFile(file, selection);
      else packageResult.textContent = t('已取消固件选择。');
    } catch (error) {
      if (currentSelection(selection)) say(String(error instanceof Error ? error.message : error), 'error');
    } finally { finishSelection(selection); }
  });
  else on($('#batch-package-file'), 'change', (event) => void inspectFile(event.target.files?.[0]));
  on($('#batch-forge-picker'), 'click', async () => {
    const selection = beginSelection();
    if ($('#batch-package-file')) $('#batch-package-file').value = '';
    packageResult.textContent = t('请在 Forge 登录并选择固件包…');
    try {
      let file;
      if (desktop) file = await state.client.chooseForgePackage({
        signal: selection.signal,
        onProgress: progress => { if (currentSelection(selection)) packageResult.textContent = `${t('正在下载 Forge 固件包…')} ${Math.round(progress.percent)}%`; },
      });
      else {
        // Stay inside the user's click when opening the browser picker.
        const descriptor = await pickFile({ callbackUrl: `${location.origin}/tools/forge-picker/callback`, initialPath: 'releases/h2vivi/firmwares/', extensions: ['.zlib'], signal: selection.signal });
        if (!currentSelection(selection)) return;
        if (descriptor) {
          packageResult.textContent = `${descriptor.name} · ${t('正在下载 Forge 固件包…')}`;
          file = await downloadFile(descriptor, { signal: selection.signal });
        }
      }
      if (!currentSelection(selection)) { if (file) state.client.releasePackage?.(file); return; }
      if (file) await inspectFile(file, selection);
      else packageResult.textContent = t('已取消固件选择。');
    } catch (error) {
      if (!currentSelection(selection)) return;
      const messages = { PopupBlocked: '请允许弹出窗口，然后重新从 Forge 选择。', Timeout: 'Forge 选择已超时，请重新选择。', Unsupported: '当前浏览器无法使用 Forge 选择器。请使用 Chrome 或 Edge。', DownloadFailed: 'Forge 固件包下载失败，请重新选择。' };
      packageResult.textContent = error instanceof ForgePickerError && messages[error.code] ? t(messages[error.code]) : String(error instanceof Error ? error.message : error);
      say(t('固件包不可用。'), 'error');
    } finally { finishSelection(selection); }
  });
  on($('#batch-forge-cancel'), 'click', () => {
    packageSelection?.abort();
    packageSelection = null;
    packageInspection += 1;
    packageResult.textContent = t('已取消固件选择。');
    renderControls();
  });
  const urlForm = $('#batch-package-url-form');
  function showUrlForm(visible) {
    urlForm.hidden = !visible;
    $('#batch-url-toggle').setAttribute('aria-expanded', String(visible));
    if (visible) $('#batch-package-url').focus();
  }
  on($('#batch-url-toggle'), 'click', () => showUrlForm(urlForm.hidden));
  on($('#batch-url-close'), 'click', () => showUrlForm(false));
  const firmwareMenu = $('#batch-firmware-menu');
  if (firmwareMenu) {
    on(document, 'pointerdown', event => { if (!firmwareMenu.contains(event.target)) firmwareMenu.open = false; });
    on(document, 'keydown', event => {
      if (event.key === 'Escape' && firmwareMenu.open) { firmwareMenu.open = false; firmwareMenu.querySelector('summary').focus(); }
    });
  }
  on(urlForm, 'submit', async event => {
    event.preventDefault();
    let url;
    try { url = firmwareUrl($('#batch-package-url').value.trim()).href; }
    catch (error) { say(t(error.message), 'error'); return; }
    const selection = beginSelection();
    if ($('#batch-package-file')) $('#batch-package-file').value = '';
    packageResult.textContent = t('正在下载 URL 固件包…');
    const options = { signal: selection.signal, onProgress: progress => {
      if (!currentSelection(selection)) return;
      packageResult.textContent = `${t('正在下载 URL 固件包…')} ${Number.isFinite(progress.percent) ? `${Math.round(progress.percent)}%` : `${((progress.receivedBytes ?? 0) / 1048576).toFixed(2)} MiB`}`;
    } };
    try {
      const file = await (desktop ? state.client.downloadPackage(url, options) : downloadFirmwarePackage(url, options));
      if (currentSelection(selection)) await inspectFile(file, selection);
      else state.client.releasePackage?.(file);
    } catch (error) {
      if (!currentSelection(selection)) return;
      packageResult.textContent = t(error instanceof Error ? error.message : String(error));
      say(t('固件包不可用。'), 'error');
    } finally { finishSelection(selection); }
  });

  function dispose() {
    if (disposed) return;
    disposed = true;
    packageSelection?.abort();
    clearTimeout(connectionTimer);
    abort.abort();
    controller.cancel();
    void state.client?.close();
  }
  render();
  void (async () => {
    try {
      const client = await createClient();
      if (disposed) { await client.close(); return; }
      state.client = client;
      state.initializing = false;
      if (!desktop) client.onPortsChanged?.(() => {
        if (disposed) return;
        if (controller.busyIds().length) refreshPending = true;
        else void scanPorts();
      });
      if (desktop) {
        if (client.getSavedPorts) replacePorts(await client.getSavedPorts());
        say(state.devices.length ? `已恢复上次的 ${state.devices.length} 个串口，点击“连接串口”建立长连接。` : t('点击“扫描串口”读取设备。')); render();
        void pollConnections();
      }
      else if (client.supported()) await scanPorts();
      else { say(t('当前浏览器不支持 Web Serial。请使用安全 HTTPS 环境中的 Chrome 或 Edge；仍可校验本地固件包。')); render(); }
    } catch (error) {
      if (disposed) return;
      state.initializing = false;
      say(language === 'en' ? `Gizloader initialization failed: ${error instanceof Error ? error.message : String(error)}` : `Gizloader 初始化失败：${error instanceof Error ? error.message : String(error)}`, 'error');
      render();
    }
  })();
  return dispose;
}

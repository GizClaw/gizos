// Measure acknowledged payload bytes, independently of the task's weighted percent.
export function createTransferTracker(now = () => performance.now()) {
  let samples = [];
  let transfer = null;
  return (progress) => {
    if (progress.phase !== 'stage') {
      samples = [];
      if (transfer) transfer = { ...transfer, bytesPerSecond: null };
      return transfer;
    }
    const totalBytes = Number(progress.totalBytes);
    const acknowledgedBytes = Number(progress.acknowledgedBytes);
    if (!Number.isFinite(totalBytes) || totalBytes <= 0 || !Number.isFinite(acknowledgedBytes) || acknowledgedBytes < 0) {
      samples = [];
      transfer = null;
      return null;
    }
    const at = now();
    const bytes = Math.min(acknowledgedBytes, totalBytes);
    const previous = samples.at(-1);
    if (previous && (bytes < previous.bytes || totalBytes !== transfer.totalBytes || at < previous.at)) samples = [];
    samples.push({ at, bytes });
    // Retain the sample just before the one-second window for a stable rate.
    while (samples.length > 2 && samples[1].at <= at - 1000) samples.shift();
    const elapsed = at - samples[0].at;
    transfer = {
      acknowledgedBytes: bytes,
      totalBytes,
      bytesPerSecond: elapsed >= 250 ? (bytes - samples[0].bytes) * 1000 / elapsed : null,
    };
    return transfer;
  };
}

const byteUnits = ['B', 'KiB', 'MiB', 'GiB'];
function byteUnit(bytes) {
  return Math.min(byteUnits.length - 1, Math.max(0, Math.floor(Math.log2(Math.max(1, bytes)) / 10)));
}

function formatRate(bytesPerSecond) {
  const unit = byteUnit(bytesPerSecond);
  return `${(bytesPerSecond / 1024 ** unit).toFixed(unit ? 1 : 0)} ${byteUnits[unit]}/s`;
}

function formatTransferred({ acknowledgedBytes, totalBytes }) {
  const unit = byteUnit(totalBytes);
  const digits = unit >= 2 ? 2 : 0;
  return `${(acknowledgedBytes / 1024 ** unit).toFixed(digits)} / ${(totalBytes / 1024 ** unit).toFixed(digits)} ${byteUnits[unit]}`;
}

export function renderDeviceProgress(device, { language, t }, existingBox) {
  const working = device.state === 'busy';
  const active = working || device.state === 'queued';
  const phases = {
    idle: t('等待操作'), connect: t('正在连接'), precheck: t('检查设备'),
    stage: t('正在发送'), activate: t('正在切换'), rediscover: t('重新连接'),
    'final-verify': t('校验结果'), complete: t('完成'),
  };
  const title = active
    ? device.state === 'queued' ? t('排队中') : phases[device.phase] ?? t('处理中')
    : device.state === 'error' ? t('操作失败')
    : device.state === 'cancelled' ? t('已取消')
    : device.state === 'stale' ? t('设备已重启，请同步状态')
    : device.detail || (device.state === 'success' ? t('完成') : t('等待操作'));
  const sending = working && device.phase === 'stage' && device.transfer;
  const completed = device.transfer && (device.state === 'success' || device.state === 'stale');
  const value = Math.max(0, Math.min(100, Number(device.progress ?? device.percent) || 0));
  const measured = sending || completed;
  const view = JSON.stringify([device.state, device.phase, title, Boolean(device.transfer), device.error]);
  const box = existingBox ?? document.createElement('div');
  // Reuse the live bar between SDK polls so its animation and width transition persist.
  if (box.dataset.view === view) {
    updateMeasurements();
    return box;
  }
  box.replaceChildren();
  box.className = 'batch-task';
  box.dataset.state = device.state;
  box.dataset.view = view;
  const header = document.createElement('div');
  header.className = 'batch-task-head';
  const label = document.createElement('span');
  label.className = 'batch-task-phase';
  label.textContent = title;
  header.append(label);
  box.append(header);

  if (active || completed) {
    if (measured) {
      const percent = document.createElement('span');
      percent.className = 'batch-task-percent';
      header.append(percent);
    }
    const bar = document.createElement('div');
    bar.className = 'batch-progress';
    bar.setAttribute('role', 'progressbar');
    bar.setAttribute('aria-label', language === 'en' ? `${device.portId} task progress` : `${device.portId} 任务进度`);
    bar.setAttribute('aria-valuemin', '0');
    bar.setAttribute('aria-valuemax', '100');
    bar.dataset.indeterminate = String(working && !measured);
    const fill = document.createElement('span');
    fill.className = 'batch-progress-fill';
    bar.append(fill);
    box.append(bar);
  }
  if (device.transfer) {
    const metrics = document.createElement('div');
    metrics.className = 'batch-transfer';
    if (sending) {
      const rate = document.createElement('span');
      rate.className = 'batch-transfer-rate';
      rate.title = t('传输速度');
      metrics.append(rate);
    }
    const size = document.createElement('span');
    size.className = 'batch-transfer-size';
    metrics.append(size);
    box.append(metrics);
  }
  if (device.error) {
    const detail = document.createElement('span');
    detail.className = 'batch-task-error';
    detail.textContent = device.error;
    box.append(detail);
  }
  updateMeasurements();
  return box;

  function updateMeasurements() {
    const percent = box.querySelector('.batch-task-percent');
    if (percent) percent.textContent = `${Math.round(value)}%`;
    const bar = box.querySelector('.batch-progress');
    if (bar) {
      if (measured) bar.setAttribute('aria-valuenow', String(value));
      bar.setAttribute('aria-valuetext', measured ? `${title} ${Math.round(value)}%` : title);
      bar.firstElementChild.style.width = `${measured ? value : 0}%`;
    }
    const rate = box.querySelector('.batch-transfer-rate');
    if (rate) {
      rate.textContent = device.transfer.bytesPerSecond === null ? t('测速中…') : formatRate(device.transfer.bytesPerSecond);
      rate.setAttribute('aria-label', `${t('传输速度')} ${rate.textContent}`);
    }
    const size = box.querySelector('.batch-transfer-size');
    if (size) {
      size.textContent = formatTransferred(device.transfer);
      size.setAttribute('aria-label', `${t('已发送 / 总大小')} ${size.textContent}`);
    }
  }
}

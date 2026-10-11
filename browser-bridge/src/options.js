'use strict';

const DEFAULTS = {
  enabled: true,
  extFilter: '',
  fallbackToBrowser: true,
  keepHistory: true,
};

const $ = (id) => document.getElementById(id);

async function loadSettings() {
  const s = await chrome.storage.sync.get(DEFAULTS);
  const merged = { ...DEFAULTS, ...s };
  $('enabled').checked = !!merged.enabled;
  $('extFilter').value = merged.extFilter || '';
  $('fallbackToBrowser').checked = !!merged.fallbackToBrowser;
  $('keepHistory').checked = !!merged.keepHistory;
}

async function saveSettings() {
  await chrome.storage.sync.set({
    enabled: $('enabled').checked,
    extFilter: $('extFilter').value.trim(),
    fallbackToBrowser: $('fallbackToBrowser').checked,
    keepHistory: $('keepHistory').checked,
  });
}

async function refreshStatus() {
  const data = await chrome.runtime.sendMessage({ action: 'getStatus' });
  if (data && data.lastError) {
    $('status').textContent =
      '上次错误：' + data.lastError + (data.lastErrorAt ? '（' + new Date(data.lastErrorAt).toLocaleString() + '）' : '');
    $('status').className = 'status error';
  } else if (data && data.hostOnline === true) {
    $('status').textContent = '本地桥已连接';
    $('status').className = 'status ok';
  } else {
    $('status').textContent = '本地桥未连接或未安装';
    $('status').className = 'status error';
  }
}

// 从宿主读当前 exe 路径, 填入输入框
async function loadHostPath() {
  try {
    const res = await chrome.runtime.sendMessage({ action: 'ping' });
    if (res && res.ok && res.executable) {
      $('exePath').value = res.executable;
      $('pathStatus').textContent = res.executable_ready
        ? '当前路径有效'
        : '当前路径无效（文件不存在）';
      $('pathStatus').className = res.executable_ready ? 'status ok' : 'status error';
    } else {
      $('pathStatus').textContent = '无法读取当前路径（本地桥未连接）';
      $('pathStatus').className = 'status error';
    }
  } catch (err) {
    $('pathStatus').textContent = '无法读取当前路径（本地桥未安装）';
    $('pathStatus').className = 'status error';
  }
}

// 把输入框里的路径发给宿主, 宿主校验并写入 bridge_config.json
async function saveHostPath() {
  const btn = $('savePath');
  const status = $('pathStatus');
  const p = $('exePath').value.trim();
  btn.disabled = true;
  status.textContent = '正在保存…';
  status.className = 'status';
  try {
    const res = await chrome.runtime.sendMessage({ action: 'setConfig', executable: p });
    if (res && res.ok) {
      status.textContent = '已保存：' + res.executable;
      status.className = 'status ok';
      await testConnection();
    } else {
      status.textContent = '保存失败：' + ((res && res.error) || '未知错误');
      status.className = 'status error';
    }
  } catch (err) {
    status.textContent = '保存异常：' + String((err && err.message) || err);
    status.className = 'status error';
  } finally {
    btn.disabled = false;
  }
}

async function testConnection() {
  const btn = $('test');
  const status = $('status');
  btn.disabled = true;
  status.textContent = '正在测试…';
  status.className = 'status';
  try {
    const res = await chrome.runtime.sendMessage({ action: 'ping' });
    if (res && res.ok) {
      status.textContent = '连接成功：本地桥已就绪';
      status.className = 'status ok';
    } else {
      const reason = (res && res.error) || '未知错误';
      status.textContent =
        '连接失败：' + reason + '。请先运行 native-host 目录下的 install.bat，再重新加载扩展。';
      status.className = 'status error';
    }
  } catch (err) {
    status.textContent = '连接异常：' + String((err && err.message) || err);
    status.className = 'status error';
  } finally {
    btn.disabled = false;
  }
}

document.addEventListener('DOMContentLoaded', async () => {
  await loadSettings();
  await refreshStatus();
  await loadHostPath();

  ['enabled', 'extFilter', 'fallbackToBrowser', 'keepHistory'].forEach((id) => {
    $(id).addEventListener('change', saveSettings);
  });

  $('test').addEventListener('click', testConnection);
  $('savePath').addEventListener('click', saveHostPath);
  $('openPanel').addEventListener('click', () => {
    chrome.runtime.sendMessage({ action: 'openPanel' });
    window.close();   // popup 场景下关闭弹窗
  });
});

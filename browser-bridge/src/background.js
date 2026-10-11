// MThDownloader 下载桥接 - background service worker (Manifest V3)
// 职责：拦截浏览器下载 -> 取消并擦除 -> 通过 Native Messaging 交给本地桥 -> 失败可回退为浏览器下载。
'use strict';

const HOST_NAME = 'com.mthdownloader.bridge';

const DEFAULTS = Object.freeze({
  enabled: true,
  // 逗号/空格分隔的扩展名白名单，例如 "zip,rar,7z,iso,exe,apk,mp4"；留空表示拦截所有下载。
  extFilter: '',
  // 桥接失败时是否回退为浏览器自带下载。
  fallbackToBrowser: true,
  // 拦截后在浏览器下载历史保留一条记录 (默认开)。
  // Chrome 不允许外部程序伪造"已完成"记录, 保留的是"已取消"记录 (含原始 URL, 可点重试再次转发)。
  keepHistory: true,
});

// 只转发可被外部程序直接处理的协议；blob/data 等留在浏览器内下载。
function isForwardableUrl(url) {
  if (typeof url !== 'string' || !url) return false;
  const m = /^([a-zA-Z][a-zA-Z0-9+.-]*):/.exec(url);
  if (!m) return false;
  const scheme = m[1].toLowerCase();
  return ['http', 'https', 'ftp', 'magnet'].includes(scheme);
}

// 从文件名（优先）或 URL 中提取小写扩展名。
function extOf(filename, url) {
  const candidate = filename || url || '';
  const clean = candidate.split(/[?#]/)[0];
  const base = clean.slice(clean.lastIndexOf('/') + 1);
  const dot = base.lastIndexOf('.');
  return dot >= 0 ? base.slice(dot + 1).toLowerCase() : '';
}

function matchesExtFilter(ext, filter) {
  const list = String(filter || '')
    .split(/[\s,;]+/)
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean);
  return list.length === 0 || list.includes(ext);
}

async function getSettings() {
  const stored = await chrome.storage.sync.get(DEFAULTS);
  return { ...DEFAULTS, ...stored };
}

async function setBadge(text) {
  try {
    if (text) {
      await chrome.action.setBadgeBackgroundColor({ color: '#e5484d' });
      await chrome.action.setBadgeText({ text });
    } else {
      await chrome.action.setBadgeText({ text: '' });
    }
  } catch (_) {
    // 忽略徽章错误（不影响主流程）
  }
}

async function recordError(error) {
  await chrome.storage.local.set({
    lastError: String(error || ''),
    lastErrorAt: Date.now(),
  });
}

function sendToHost(message) {
  return chrome.runtime
    .sendNativeMessage(HOST_NAME, message)
    .then((res) => {
      if (res && res.ok) return { ok: true, data: res };
      return { ok: false, error: (res && res.error) || '本地桥返回失败' };
    })
    .catch((err) => ({
      ok: false,
      error: (err && err.message) || String(err),
    }));
}

async function forwardDownload(url, extra) {
  const settings = await getSettings();
  const result = await sendToHost({
    action: 'download',
    url,
    filename: extra.filename || '',
    referrer: extra.referrer || '',
    mime: extra.mime || '',
  });

  if (result.ok) {
    await setBadge('');
  } else {
    await setBadge('!');
    await recordError(result.error);
    console.error('[mth-bridge] 转发失败:', result.error);
  }
  return result;
}

// 拦截浏览器下载：onCreated 触发即取消，避免浏览器占用带宽。
chrome.downloads.onCreated.addListener(async (item) => {
  const url = item.url || item.finalUrl || '';
  try {
    // 忽略本扩展自己发起的回退下载，避免死循环。
    if (item.byExtensionId && item.byExtensionId === chrome.runtime.id) return;

    const settings = await getSettings();
    if (!settings.enabled) return;
    if (!isForwardableUrl(url)) return;

    const ext = extOf(item.filename, url);
    if (!matchesExtFilter(ext, settings.extFilter)) return;

    await chrome.downloads.cancel(item.id).catch(() => {});
    // keepHistory 开启时保留下载历史记录 (状态"已取消", 含原始 URL, 可点重试再次转发);
    // 关闭时擦除记录, 历史里完全不留痕
    if (!settings.keepHistory) {
      await chrome.downloads.erase({ id: item.id }).catch(() => {});
    }

    const result = await forwardDownload(url, {
      filename: item.filename || '',
      referrer: item.referrer || '',
      mime: item.mime || '',
    });

    if (!result.ok && settings.fallbackToBrowser) {
      // 桥接失败时恢复浏览器下载，尽量保留原始文件名。
      const opts = { url };
      const name = item.filename || '';
      if (name && !/^[a-zA-Z]:[\\/]/.test(name) && !/^[\\/]/.test(name)) {
        opts.filename = name;
      }
      await chrome.downloads.download(opts).catch((err) => {
        console.error('[mth-bridge] 回退下载失败:', err);
      });
    }
  } catch (err) {
    console.error('[mth-bridge] onCreated 处理失败:', err);
  }
});

// 右键菜单：直接把链接地址交给 MThDownloader（不经过浏览器下载）。
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: 'mth-download-link',
      title: '使用 MThDownloader 下载',
      contexts: ['link', 'image', 'audio', 'video'],
    });
  });
});

chrome.contextMenus.onClicked.addListener(async (info) => {
  const url = info.linkUrl || info.srcUrl || '';
  if (!isForwardableUrl(url)) return;
  const settings = await getSettings();
  if (!settings.enabled) return;

  await forwardDownload(url, {
    filename: '',
    referrer: info.pageUrl || '',
    mime: '',
  });
});

// 供 options 页面查询状态 / 测试连接 / 设置 exe 路径。
chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.action === 'ping') {
    sendToHost({ action: 'ping' }).then((res) => {
      chrome.storage.local
        .set({ hostOnline: !!res.ok, hostError: res.error || '' })
        .then(() => sendResponse(res));
    });
    return true; // 异步响应
  }

  if (msg && msg.action === 'setConfig') {
    // 转发给宿主: 宿主校验路径存在后写入 bridge_config.json
    sendToHost({
      action: 'set_config',
      executable: String(msg.executable || '').trim(),
    }).then((res) => sendResponse(res));
    return true;
  }

  if (msg && msg.action === 'list') {
    // 下载面板: 读取 MThDownloader 的下载历史记录
    sendToHost({ action: 'list' }).then((res) => sendResponse(res));
    return true;
  }

  if (msg && msg.action === 'openPanel') {
    chrome.tabs.create({ url: chrome.runtime.getURL('panel.html') });
    sendResponse({ ok: true });
    return false;
  }

  if (msg && msg.action === 'getStatus') {
    chrome.storage.local
      .get(['hostOnline', 'hostError', 'lastError', 'lastErrorAt'])
      .then((data) => sendResponse(data || {}));
    return true;
  }

  return false;
});

// 启动自检：若宿主未安装/异常，在图标上提示。
(async () => {
  const res = await sendToHost({ action: 'ping' });
  await setBadge(res.ok ? '' : '!');
  await chrome.storage.local.set({
    hostOnline: !!res.ok,
    hostError: res.error || '',
  });
})();

'use strict';

// MThDownloader 下载面板: 通过本地桥读取 MThDownloader.history.jsonl, 2 秒轮询刷新。

const $ = (id) => document.getElementById(id);

function fmtSize(n) {
  if (!n || n <= 0) return '';
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  if (n < 1073741824) return (n / 1048576).toFixed(2) + ' MB';
  return (n / 1073741824).toFixed(2) + ' GB';
}

function fmtDur(s) {
  if (!s || s <= 0) return '';
  const m = Math.floor(s / 60), r = s % 60;
  return m > 0 ? `${m} 分 ${r} 秒` : `${r} 秒`;
}

function fmtTime(t) {
  if (!t) return '';
  return new Date(t * 1000).toLocaleString('zh-CN', { hour12: false });
}

// 从文件路径/URL 提取显示名
function nameOf(item) {
  if (item.file) {
    const base = item.file.split(/[\\/]/).filter(Boolean).pop();
    if (base && base !== '.part') return base;
  }
  const m = /^[a-z]+:(\/)?([^?#]*)/i.exec(item.url || '');
  if (m) {
    const base = decodeURIComponent(m[2] || '').split('/').filter(Boolean).pop();
    if (base) return base;
  }
  return item.url || '(未知任务)';
}

// 把事件行按 id 聚合成任务 (同 id 取最后事件为状态)
function groupById(items) {
  const map = new Map();
  for (const it of items) {
    const id = String(it.id ?? '?');
    const cur = map.get(id) || { events: [] };
    cur.events.push(it);
    map.set(id, cur);
  }
  const tasks = [];
  for (const [id, g] of map) {
    const last = g.events[g.events.length - 1];
    const startedAt = g.events[0].time || 0;
    tasks.push({
      id,
      state: last.event,          // start / done / fail / cancel
      url: last.url || '',
      file: last.file || '',
      size: Number(last.size) || 0,
      secs: Number(last.secs) || 0,
      err: last.err || '',
      time: last.time || startedAt,
      startTime: startedAt,
    });
  }
  tasks.sort((a, b) => b.startTime - a.startTime || Number(b.id) - Number(a.id));
  return tasks;
}

const BADGE = {
  start: ['run', '下载中'],
  done:  ['done', '已完成'],
  fail:  ['fail', '失败'],
  cancel:['stop', '已取消'],
};

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

function render(items) {
  const list = $('list');
  if (!Array.isArray(items) || items.length === 0) {
    list.innerHTML = '<div class="empty">还没有下载记录 —— 在浏览器里下载一个文件试试。</div>';
    return;
  }
  const tasks = groupById(items);
  list.innerHTML = tasks.map((t) => {
    const [cls, label] = BADGE[t.state] || ['stop', t.state];
    const bits = [];
    if (t.size > 0) bits.push(fmtSize(t.size));
    if (t.secs > 0) bits.push('耗时 ' + fmtDur(t.secs));
    if (t.err) bits.push(esc(t.err));
    bits.push(esc(fmtTime(t.startTime)));
    const sub = esc(t.url) + (bits.length ? ' · ' + bits.join(' · ') : '');
    const file = t.file
      ? `<span class="copy" data-copy="${esc(t.file)}" title="点击复制路径">复制路径</span>`
      : '';
    return `<div class="item">
      <span class="badge ${cls}">${label}</span>
      <span class="name">${esc(nameOf(t))}</span>
      <span class="sub">${sub}${file}</span>
    </div>`;
  }).join('');
}

async function refresh() {
  try {
    const res = await chrome.runtime.sendMessage({ action: 'list' });
    if (res && res.ok) {
      $('bridge').innerHTML = '<span class="dot on"></span>本地桥已连接';
      render(res.items);
    } else {
      $('bridge').innerHTML = '<span class="dot off"></span>本地桥未连接';
      $('list').innerHTML =
        '<div class="empty">本地桥未连接。请先运行 native-host\\install.bat，' +
        '然后在扩展选项页点"测试本地桥连接"。</div>';
    }
  } catch (err) {
    $('bridge').innerHTML = '<span class="dot off"></span>本地桥异常';
  }
}

document.addEventListener('click', (e) => {
  const el = e.target.closest('.copy');
  if (!el) return;
  navigator.clipboard.writeText(el.dataset.copy).then(() => {
    const old = el.textContent;
    el.textContent = '已复制';
    setTimeout(() => { el.textContent = old; }, 1200);
  });
});

refresh();
setInterval(refresh, 2000);

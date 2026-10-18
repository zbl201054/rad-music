// Rad Music - 共享工具：API 调用 / 格式化 / 错误文案 / 歌词解析
export const API_BASE = location.protocol.startsWith('http') ? location.origin : 'http://127.0.0.1:8765';

export const CODE_MESSAGES = {
  0: '成功',
  1001: '平台错误',
  1002: 'SHA 无效',
  1003: '时间戳无效',
  1004: '文件不存在',
  1005: '尚未加载曲目',
  1006: '解密失败',
  1007: '元数据不合法',
  1008: '版本不兼容',
  1009: '搜索无可用源',
  1010: '音源超时',
  1011: '请求失败',
  1012: '不支持的能力',
  1013: '插件已禁用',
  1014: '请求过于频繁',
  1015: '暂无歌词',
  1016: 'SMTC 不可用',
  1017: 'DSP 初始化失败',
  1018: '下载失败',
  1019: '磁盘空间不足',
  1020: '任务不存在',
  1021: '不支持该音质',
  1022: '校验和不匹配',
  1023: 'base 冲突',
  1024: '标签解析失败',
  1025: '不支持的格式',
  1026: '曲库已锁定',
  1027: '音质未定义',
  1028: 'base 重建失败',
  1029: 'meta 路径不一致',
  1030: '歌单不存在',
  1031: '专辑不存在',
  1032: 'MV 不存在'
};

export async function api(method, path, body) {
  try {
    const options = {method: method || 'GET', headers: {}};
    if (body instanceof Uint8Array || body instanceof Blob) {
      options.body = body;
      options.headers['Content-Type'] = 'application/octet-stream';
    } else if (body !== undefined && body !== null) {
      options.body = JSON.stringify(body);
      options.headers['Content-Type'] = 'application/json';
    }
    const response = await fetch(API_BASE + path, options);
    const text = await response.text();
    if (!text) return {code: 1011, message: `空响应 (${response.status})`, data: {}};
    try {
      return JSON.parse(text);
    } catch {
      return {code: 1011, message: `响应不是 JSON (${response.status})`, data: {}};
    }
  } catch (error) {
    return {code: 1011, message: '网络错误：' + (error && error.message ? error.message : String(error)), data: {}};
  }
}

export function errText(json) {
  if (!json) return '未知错误';
  const code = json.code;
  if (code === 0) return '成功';
  const base = CODE_MESSAGES[code] || '错误 ' + code;
  return json.message ? `${base}：${json.message}` : base;
}

export function esc(value) {
  return String(value == null ? '' : value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

export function fmtTime(ms) {
  if (ms == null || isNaN(ms)) return '00:00';
  const total = Math.max(0, Math.floor(ms / 1000));
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  if (minutes >= 60) {
    const hours = Math.floor(minutes / 60);
    return `${hours}:${String(minutes % 60).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
  }
  return `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
}

export function fmtDuration(seconds) {
  if (!seconds) return '--:--';
  return fmtTime(seconds * 1000);
}

export function fmtBytes(bytes) {
  const value = Number(bytes) || 0;
  if (value < 1024) return value + ' B';
  if (value < 1024 * 1024) return (value / 1024).toFixed(1) + ' KB';
  if (value < 1024 * 1024 * 1024) return (value / 1024 / 1024).toFixed(1) + ' MB';
  return (value / 1024 / 1024 / 1024).toFixed(2) + ' GB';
}

let toastTimer = null;
export function toast(message, kind = 'info') {
  let element = document.getElementById('toast');
  if (!element) {
    element = document.createElement('div');
    element.id = 'toast';
    document.body.appendChild(element);
  }
  element.textContent = message;
  element.className = 'toast show ' + kind;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    element.className = 'toast ' + kind;
  }, 2800);
}

export function debounce(fn, ms) {
  let timer = null;
  return function (...args) {
    clearTimeout(timer);
    timer = setTimeout(() => fn.apply(this, args), ms);
  };
}

// ---------------- 音质 ----------------
export const QUALITY_RANK = {hires: 6, lossless: 5, high: 4, medium: 3, low: 2, unknown: 1};

export function qualityRank(level) {
  return QUALITY_RANK[level] != null ? QUALITY_RANK[level] : 0;
}

// base.json 的 bitrate 单位是 kbps（见 quality.hpp）；插件清单里偶有 bps 写法（≥10000 视为 bps）
export function fmtBitrate(bitrate) {
  const value = Number(bitrate) || 0;
  if (!value) return '';
  return (value >= 10000 ? Math.round(value / 1000) : value) + 'kbps';
}

export function qualityLabel(quality) {
  if (!quality) return '未知音质';
  if (quality.label) return quality.label;
  const map = {hires: 'Hi-Res', lossless: '无损', high: '高音质', medium: '标准', low: '低音质', unknown: '未知'};
  if (quality.level && map[quality.level]) return map[quality.level];
  if (quality.bitrate) return fmtBitrate(quality.bitrate);
  return '未知音质';
}

export function qualityDetail(quality) {
  if (!quality) return '';
  const parts = [];
  if (quality.codec) parts.push(String(quality.codec).toUpperCase());
  if (quality.bitrate) parts.push(fmtBitrate(quality.bitrate));
  if (quality.sample_rate) parts.push((quality.sample_rate / 1000) + 'kHz');
  if (quality.bit_depth) parts.push(quality.bit_depth + 'bit');
  if (quality.channels) parts.push(quality.channels + '声道');
  return parts.join(' · ');
}

export function qualityBadge(quality, extraClass = '') {
  const label = qualityLabel(quality);
  const level = (quality && quality.level) || 'unknown';
  return `<span class="q-badge q-${esc(level)} ${extraClass}">${esc(label)}</span>`;
}

export function sortQualities(list) {
  return (list || []).slice().sort((a, b) => qualityRank(b && b.level) - qualityRank(a && a.level));
}

// ---------------- 封面 ----------------
// 曲库条目的 pic 可能是：在线 URL（http/https）、data: URL、或相对路径 covers/<hash>.<ext>。
// 相对路径需要经宿主 /music/cover/ 路由读取，否则浏览器会 404。
export function coverUrl(pic) {
  const value = String(pic || '');
  if (!value) return '';
  if (/^(https?:|data:|blob:)/i.test(value)) return value;
  if (value.indexOf('covers/') === 0) return API_BASE + '/music/cover/' + value;
  return value;
}

// ---------------- LRC ----------------
export function parseLrc(text) {
  const lines = [];
  if (!text) return lines;
  const timeExp = /\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/g;
  for (const raw of String(text).split(/\r?\n/)) {
    let match;
    let lastIndex = 0;
    const times = [];
    while ((match = timeExp.exec(raw)) !== null) {
      const minutes = parseInt(match[1], 10);
      const seconds = parseInt(match[2], 10);
      const fraction = match[3] ? parseInt(match[3].padEnd(3, '0'), 10) : 0;
      times.push(minutes * 60000 + seconds * 1000 + fraction);
      lastIndex = timeExp.lastIndex;
    }
    if (!times.length) continue;
    const content = raw.slice(lastIndex).trim();
    if (!content) continue;
    for (const time of times) lines.push({time_ms: time, text: content});
  }
  lines.sort((a, b) => a.time_ms - b.time_ms);
  return lines;
}

export function lyricLinesFromData(data) {
  if (!data) return [];
  const lyric = data.lyric;
  if (!lyric) return [];
  if (typeof lyric === 'string') return parseLrc(lyric);
  if (Array.isArray(lyric.lines)) {
    return lyric.lines
      .map((line) => ({
        time_ms: Number(line.time_ms != null ? line.time_ms : line.time || 0),
        text: line.text != null ? line.text : line.content || '',
        tr: line.tr || line.translation || '',
        roma: line.roma || line.roman || ''
      }))
      .filter((line) => line.text)
      .sort((a, b) => a.time_ms - b.time_ms);
  }
  if (typeof lyric.text === 'string') return parseLrc(lyric.text);
  return [];
}

export function formatDate(value) {
  if (value === undefined || value === null || value === '') return '';
  // 后端时间戳统一为秒级 epoch，也兼容毫秒级与可解析字符串
  const date = typeof value === 'number'
    ? new Date(value < 100000000000 ? value * 1000 : value)
    : new Date(value);
  if (isNaN(date.getTime())) return '';
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
}

// Rad Music - 应用外壳：状态 / 播放器 / 路由 / 通用 UI
import { API_BASE, api, esc, errText, fmtTime, fmtDuration, toast as apiToast,
  qualityLabel, qualityDetail, qualityBadge, sortQualities, qualityRank,
  lyricLinesFromData, coverUrl } from './api.js';
import { icon } from './icons.js';
import { VIEWS } from './views.js';
import { renderSettingsView, THEMES } from './settings.js';

export const $ = (selector, root = document) => root.querySelector(selector);
export const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

const safeArray = (value) => (Array.isArray(value) ? value : []);

export function el(tag, attrs = {}, html = '') {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs || {})) {
    if (value == null || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'dataset') Object.assign(node.dataset, value);
    else if (key.startsWith('on') && typeof value === 'function') node.addEventListener(key.slice(2).toLowerCase(), value);
    else node.setAttribute(key, value === true ? '' : String(value));
  }
  if (html) node.innerHTML = html;
  return node;
}

export const toast = apiToast;

// ---------------------------------------------------------------- 全局状态
export const state = {
  route: 'library',
  routeParams: {},
  conn: 'unknown',
  settings: {
    theme: 'violet', color_scheme: 'dark', player_radius: 24, compact_player: false,
    preferred_quality: 'auto', volume: 0.8, autoplay_next: true, mv_source: '',
    hide_unavailable: false, fold_results: true, plugin_enabled: {}
  },
  library: [],
  libraryLoaded: false,
  search: {
    keyword: '', data: null, loading: false,
    all: [], statuses: [], page: 1, total: 0, tab: 'all',
    filters: null, filtersOpen: true, mv: null
  },
  playlists: [], albums: [], mvs: [], downloads: [], plugins: [],
  pluginsLoaded: false, mvPlatforms: [],
  cache: null, sources: [], platforms: [],
  viewState: {}
};

export const player = {
  audio: null,
  queue: [],
  index: -1,
  current: null,
  variants: [],
  quality: 'auto',
  playing: false,
  loading: false,
  position: 0,
  duration: 0,
  volume: 0.8,
  muted: false,
  // 默认列表循环：队列播完最后一首自动回到开头继续播（按需求设定）
  loop: 'all',
  shuffle: false,
  lyric: [],
  lyricActive: -1,
  pageOpen: false,
  pageTab: 'lyric',
  base: null,
  serverLoaded: false,
  token: 0
};

function audioEl() {
  if (!player.audio) {
    const audio = new Audio();
    audio.preload = 'metadata';
    audio.volume = player.volume;
    audio.addEventListener('timeupdate', onAudioProgress);
    audio.addEventListener('loadedmetadata', onAudioProgress);
    audio.addEventListener('durationchange', onAudioProgress);
    audio.addEventListener('play', () => { player.playing = true; renderPlayer(); syncControl('resume'); });
    audio.addEventListener('pause', () => {
      if (player.audio && player.audio.ended) return;
      player.playing = false; renderPlayer();
    });
    audio.addEventListener('ended', onAudioEnded);
    audio.addEventListener('error', onAudioError);
    player.audio = audio;
  }
  return player.audio;
}

export function audioUrl(sha) {
  return API_BASE + '/music/audio/' + encodeURIComponent(sha);
}

// ---------------------------------------------------------------- 曲目统一结构
// 音源显示名：优先取插件清单里的平台中文名，退化用平台标识
export function platformLabel(platform) {
  if (!platform) return '';
  if (platform === 'local') return '本地';
  const meta = (state.platformMeta || {})[platform];
  if (meta && meta.name) return meta.name;
  const known = (state.platforms || []).find((entry) => entry && entry.platform === platform);
  return (known && known.name) || platform;
}

export function itemFromLocal(track) {
  return {
    key: 'local:' + track.sha,
    platform: 'local',
    itemId: track.sha,
    sha: track.sha,
    title: track.name || '(未知标题)',
    artist: track.author || '未知艺术家',
    album: track.album || '',
    duration: Number(track.duration) || 0,
    pic: track.pic || '',
    quality: track.quality || null,
    qualities: [track.quality].filter(Boolean),
    groupKey: track.group_key || '',
    songKey: track.song_key || '',
    sourceLabel: '本地'
  };
}

// 曲库条目 → 播放条目：按 platform 区分本地文件与在线曲目，在线曲目必须带 item_id
export function itemFromTrack(track) {
  if (!track) return null;
  const platform = track.platform || 'local';
  if (platform === 'local') return itemFromLocal(track);
  const itemId = String(track.item_id || track.sha || '');
  return {
    key: 'online:' + platform + ':' + itemId,
    platform,
    itemId,
    sha: track.sha || '',
    title: track.name || '(未知标题)',
    artist: track.author || '未知艺术家',
    album: track.album || '',
    duration: Number(track.duration) || 0,
    pic: track.pic || '',
    quality: track.quality || null,
    qualities: [track.quality].filter(Boolean),
    groupKey: track.group_key || '',
    songKey: track.song_key || '',
    sourceLabel: platformLabel(platform)
  };
}

export function itemFromResult(result, source, quality) {
  const base = source && source.base ? source.base : {};
  const chosen = quality || source.quality || null;
  const qList = sortQualities((source && source.qualities) || (source && source.quality ? [source.quality] : []));
  return {
    key: 'online:' + source.platform + ':' + source.id,
    platform: source.platform,
    itemId: String(source.id),
    sha: base.sha || '',
    title: base.name || result.title || '(未知标题)',
    artist: base.author || (result.artists || []).join(' / ') || '未知艺术家',
    album: result.album || '',
    duration: Number(result.duration || base.duration || 0) || 0,
    pic: base.pic || '',
    quality: chosen,
    qualities: qList,
    groupKey: result.group_key || '',
    songKey: '',
    sourceLabel: source.platform,
    hidden: !!(source && source.available === false)
  };
}

export function trackVariantKey(item) {
  return item.platform === 'local' ? 'local:' + item.sha : 'online:' + item.platform + ':' + item.itemId;
}

// ---------------------------------------------------------------- 通用 UI：弹窗 / 菜单
let modalCleanup = null;

export function openModal({title, body = '', footer = '', wide = false, onMount = null, onClose = null}) {
  closeContextMenu();
  const backdrop = $('#modal-backdrop');
  const box = $('#modal');
  if (!backdrop || !box) return;
  if (modalCleanup) modalCleanup();
  box.className = 'modal' + (wide ? ' wide' : '');
  box.innerHTML =
    '<div class="modal-head"><h3>' + esc(title) + '</h3><span class="spacer"></span>' +
    '<button class="icon-btn" data-modal-close title="关闭">' + icon('close', 20) + '</button></div>' +
    '<div class="modal-body">' + body + '</div>' +
    (footer ? '<div class="modal-foot">' + footer + '</div>' : '');
  backdrop.hidden = false;
  const close = () => closeModal();
  const onKey = (event) => { if (event.key === 'Escape') close(); };
  const onBackdrop = (event) => { if (event.target === backdrop) close(); };
  backdrop.addEventListener('click', onBackdrop);
  document.addEventListener('keydown', onKey);
  $$('[data-modal-close]', box).forEach((button) => button.addEventListener('click', close));
  modalCleanup = () => {
    backdrop.removeEventListener('click', onBackdrop);
    document.removeEventListener('keydown', onKey);
    if (onClose) onClose();
  };
  if (onMount) onMount(box, close);
  return {box, close};
}

export function closeModal() {
  const backdrop = $('#modal-backdrop');
  if (backdrop) backdrop.hidden = true;
  if (modalCleanup) { const cleanup = modalCleanup; modalCleanup = null; cleanup(); }
  $$('.modal-body .lyric-line.active').forEach((node) => node.classList.remove('active'));
}

export function confirmDialog({title, message, okLabel = '确定', cancelLabel = '取消', danger = false}) {
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (settled) return; settled = true; resolve(value); closeModal(); };
    openModal({
      title,
      body: '<p class="note" style="font-size:13.5px;line-height:1.7">' + esc(message || '') + '</p>',
      footer: '<button class="btn ghost" data-cancel>' + esc(cancelLabel) + '</button>' +
              '<button class="btn ' + (danger ? 'danger' : 'primary') + '" data-ok>' + esc(okLabel) + '</button>',
      onMount: (box) => {
        $('[data-ok]', box).addEventListener('click', () => finish(true));
        $('[data-cancel]', box).addEventListener('click', () => finish(false));
      },
      onClose: () => finish(false)
    });
  });
}

export function promptDialog({title, label, value = '', placeholder = '', multiline = false, okLabel = '保存'}) {
  return new Promise((resolve) => {
    let settled = false;
    const finish = (result) => { if (settled) return; settled = true; resolve(result); if ($('#modal-backdrop') && !$('#modal-backdrop').hidden) closeModal(); };
    const field = multiline
      ? '<textarea data-input placeholder="' + esc(placeholder) + '">' + esc(value) + '</textarea>'
      : '<input type="text" data-input value="' + esc(value) + '" placeholder="' + esc(placeholder) + '" />';
    openModal({
      title,
      body: '<label class="col" style="gap:8px"><span class="note">' + esc(label || '') + '</span>' + field + '</label>',
      footer: '<button class="btn ghost" data-cancel>取消</button><button class="btn primary" data-ok>' + esc(okLabel) + '</button>',
      onMount: (box) => {
        const input = $('[data-input]', box);
        if (input) { input.focus(); input.select && input.select(); }
        $('[data-ok]', box).addEventListener('click', () => finish(input ? input.value : ''));
        $('[data-cancel]', box).addEventListener('click', () => finish(null));
        if (input && !multiline) input.addEventListener('keydown', (event) => { if (event.key === 'Enter') finish(input.value); });
      },
      onClose: () => finish(null)
    });
  });
}

export function contextMenu(x, y, items) {
  const menu = $('#ctx-menu');
  if (!menu) return;
  menu.innerHTML = (items || []).map((item, index) => {
    if (item.divider) return '<div class="divider" style="margin:5px 8px"></div>';
    return '<button class="ctx-item' + (item.danger ? ' danger' : '') + '" data-idx="' + index + '">' +
      (item.icon ? icon(item.icon, 17) : '') + '<span>' + esc(item.label) + '</span></button>';
  }).join('');
  menu.hidden = false;
  menu.style.left = '0px';
  menu.style.top = '0px';
  const rect = menu.getBoundingClientRect();
  const left = Math.min(x, window.innerWidth - rect.width - 10);
  const top = Math.min(y, window.innerHeight - rect.height - 10);
  menu.style.left = Math.max(8, left) + 'px';
  menu.style.top = Math.max(8, top) + 'px';
  $$('[data-idx]', menu).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const item = items[Number(button.dataset.idx)];
      closeContextMenu();
      if (item && item.onClick) item.onClick();
    });
  });
}

export function closeContextMenu() {
  const menu = $('#ctx-menu');
  if (menu) menu.hidden = true;
}

// ---------------------------------------------------------------- 设置
export function applySettings(settings) {
  if (!settings) return;
  state.settings = Object.assign({}, state.settings, settings);
  const root = document.documentElement;
  root.dataset.theme = state.settings.theme || 'violet';
  root.dataset.scheme = state.settings.color_scheme === 'light' ? 'light' : 'dark';
  root.style.setProperty('--player-radius', (Number(state.settings.player_radius) || 0) + 'px');
  const bar = $('#player-bar');
  if (bar) bar.classList.toggle('compact', !!state.settings.compact_player);
  const volume = Number(state.settings.volume);
  if (!isNaN(volume) && !player.playing) player.volume = Math.min(1, Math.max(0, volume));
  player.volume = Math.min(1, Math.max(0, Number(state.settings.volume) || player.volume));
  if (player.audio) player.audio.volume = player.muted ? 0 : player.volume;
  renderPlayer();
}

export async function loadSettings() {
  const result = await api('GET', '/music/settings');
  if (result.code === 0 && result.data) {
    const data = result.data.settings || result.data;
    applySettings(data);
  }
  return state.settings;
}

export async function saveSettings(patch, {silent = true} = {}) {
  const next = Object.assign({}, state.settings, patch || {});
  applySettings(next);
  const result = await api('POST', '/music/settings', patch || {});
  if (result.code !== 0) {
    if (!silent) toast(errText(result), 'error');
    return result;
  }
  if (result.data) applySettings(result.data.settings || result.data);
  return result;
}

// ---------------------------------------------------------------- 连接状态
export async function refreshConnection() {
  const result = await api('GET', '/music/status');
  const dot = $('#conn-dot');
  const text = $('#conn-text');
  if (result.code === 0) {
    state.conn = 'ok';
    const status = result.data || {};
    const map = {playing: '播放中', paused: '已暂停', stopped: '空闲', ended: '空闲', buffering: '缓冲中'};
    if (text) text.textContent = '已连接 · ' + (map[status.state] || '空闲');
    if (dot) dot.className = 'dot ok';
  } else {
    state.conn = 'err';
    if (text) text.textContent = '连接失败';
    if (dot) dot.className = 'dot err';
  }
  return state.conn;
}

// ---------------------------------------------------------------- 路由
const ROUTE_META = {
  library: {title: '曲库', subtitle: '本地音乐与在线曲目', icon: 'library'},
  search: {title: '搜索', subtitle: '跨音源搜索并折叠同一首歌', icon: 'search'},
  playlists: {title: '歌单', subtitle: '从本地曲库或在线音源同步', icon: 'playlist'},
  albums: {title: '专辑', subtitle: '专辑收藏与在线同步', icon: 'album'},
  artists: {title: '歌手', subtitle: '收藏的歌手与快速搜索', icon: 'star'},
  mv: {title: 'MV', subtitle: '视频源与音乐视频', icon: 'video'},
  downloads: {title: '下载', subtitle: '按音质下载并管理任务', icon: 'download'},
  import: {title: '导入', subtitle: '扫描、导入本地文件并查看状态', icon: 'upload'},
  settings: {title: '设置', subtitle: '主题、音源、插件与播放体验', icon: 'settings'}
};

export function navigate(route, params = {}) {
  if (!VIEWS[route] && route !== 'settings') route = 'library';
  state.route = route;
  state.routeParams = params || {};
  $$('.nav-item').forEach((button) => button.classList.toggle('active', button.dataset.route === route));
  const meta = ROUTE_META[route] || {title: route};
  const title = $('#page-title');
  const subtitle = $('#page-subtitle');
  if (title) title.textContent = params.title || meta.title;
  if (subtitle) subtitle.textContent = params.subtitle || meta.subtitle || '';
  setTopbarActions('');
  const root = $('#view-root');
  if (root) root.innerHTML = '<div class="empty"><strong>加载中…</strong></div>';
  const view = route === 'settings' ? renderSettingsView : VIEWS[route];
  try {
    view(root, params);
  } catch (error) {
    console.error(error);
    if (root) root.innerHTML = '<div class="empty"><strong>页面渲染失败</strong><span>' + esc(String(error && error.message || error)) + '</span></div>';
  }
  const sidebar = $('#sidebar');
  if (sidebar) sidebar.classList.remove('open');
  syncHash();
}

export function setTopbarActions(html, onMount = null) {
  const host = $('#topbar-actions');
  if (!host) return;
  host.innerHTML = html || '';
  if (onMount) onMount(host);
}

function syncHash() {
  const target = '#' + state.route + (state.routeParams && state.routeParams.id ? '/' + state.routeParams.id : '');
  if (location.hash !== target) history.replaceState(null, '', target);
}

// 解析 #route/id 形式的地址（详情页可深链、可刷新、可前进后退）
function parseHash() {
  const raw = (location.hash || '').replace(/^#\/?/, '');
  const parts = raw.split('/').filter(Boolean).map((part) => {
    try {
      return decodeURIComponent(part);
    } catch {
      return part;
    }
  });
  const route = parts[0] && (VIEWS[parts[0]] || parts[0] === 'settings') ? parts[0] : 'library';
  const params = parts[1] ? {id: parts[1]} : {};
  return {route, params};
}

// ---------------------------------------------------------------- 播放器
function defaultQualityFor(item) {
  const preferred = state.settings.preferred_quality || 'auto';
  if (preferred === 'auto') return 'auto';
  const has = (item.qualities || []).some((quality) => quality && (quality.quality_id === preferred || quality.level === preferred));
  return has ? preferred : 'auto';
}

function syncLoad(body) {
  api('POST', '/music/load', body).then((result) => {
    player.serverLoaded = result.code === 0;
    if (result.code === 0) player.base = result.data.base || player.base;
  }).catch(() => {});
}

function syncControl(action, extra) {
  const body = extra || {};
  api('POST', '/music/' + action, body).catch(() => {});
}

function onAudioProgress() {
  const audio = player.audio;
  if (!audio) return;
  player.position = (audio.currentTime || 0) * 1000;
  player.duration = (audio.duration && isFinite(audio.duration)) ? audio.duration * 1000 : player.duration;
  renderProgress();
  highlightLyric();
}

let lastEndedAt = 0;
function onAudioEnded() {
  if (Date.now() - lastEndedAt < 400) return;
  lastEndedAt = Date.now();
  player.playing = false;
  renderPlayer();
  if (player.loop === 'one') {
    player.audio.currentTime = 0;
    player.audio.play().catch(() => {});
    return;
  }
  if (state.settings.autoplay_next === false) return;
  if (player.loop === 'off' && !player.shuffle && player.index >= player.queue.length - 1) {
    player.position = player.duration;
    renderProgress();
    return;
  }
  next();
}

function onAudioError() {
  if (!player.current || player.loading) return;
  toast('音频加载失败：' + (player.current.title || '未知曲目'), 'error');
  player.playing = false;
  renderPlayer();
}

async function onlineUrlFor(item, qualityId) {
  if (!qualityId || qualityId === 'auto') return '';
  const result = await api('POST', '/music/request', {
    source: item.platform,
    op: 'get_url',
    params: {id: item.itemId, item_id: item.itemId, quality: qualityId}
  });
  if (result.code !== 0) return '';
  const value = result.data && result.data.result;
  if (!value) return '';
  if (typeof value === 'string') return value;
  const url = value.url || value.location || value.play_url || value.src || '';
  return typeof url === 'string' ? url : '';
}

export async function playItem(item, {quality = undefined, autoplay = true} = {}) {
  if (!item) return false;
  const token = ++player.token;
  player.loading = true;
  player.current = item;
  player.base = null;
  player.quality = quality === undefined ? defaultQualityFor(item) : quality;
  player.lyric = [];
  player.lyricActive = -1;
  player.position = 0;
  player.duration = item.duration ? item.duration * 1000 : 0;
  renderPlayer();
  renderProgress();
  renderLyrics();

  let url = '';
  try {
    if (item.platform === 'local') {
      if (!item.sha) throw new Error('缺少本地曲目 sha');
      url = audioUrl(item.sha);
      player.duration = item.duration ? item.duration * 1000 : 0;
      syncLoad({sha: item.sha});
    } else {
      const loaded = await api('POST', '/music/load', {platform: item.platform, id: item.itemId});
      if (token !== player.token) return false;
      if (loaded.code === 0) {
        player.serverLoaded = true;
        player.base = loaded.data.base || null;
        const pp = String(loaded.data.play_path || '');
        if (/^https?:/i.test(pp)) url = pp;
        else if (loaded.data.base && loaded.data.base.sha) url = audioUrl(loaded.data.base.sha);
        if (loaded.data.base) {
          if (!item.title || item.platform === 'local') item.title = loaded.data.base.name || item.title;
          if (loaded.data.base.pic) item.pic = loaded.data.base.pic;
          if (loaded.data.base.quality && !item.quality) item.quality = loaded.data.base.quality;
        }
      }
      const remote = await onlineUrlFor(item, player.quality);
      if (token !== player.token) return false;
      if (remote) url = remote;
      if (!url) {
        throw new Error(loaded.code !== 0 ? errText(loaded) : '该音源没有返回可播放地址');
      }
      syncControl('start');
    }
  } catch (error) {
    player.loading = false;
    if (token !== player.token) return false;
    toast(String(error && error.message || error), 'error');
    player.playing = false;
    renderPlayer();
    return false;
  }

  if (token !== player.token) return false;
  player.variants = await variantsFor(item);
  const audio = audioEl();
  audio.pause();
  audio.src = url;
  audio.volume = player.muted ? 0 : player.volume;
  if (autoplay) {
    try {
      await audio.play();
      player.playing = true;
    } catch (error) {
      player.playing = false;
      toast('无法开始播放：' + String(error && error.message || error), 'error');
    }
  }
  if (token !== player.token) return false;
  player.loading = false;
  renderPlayer();
  renderQualityRow();
  loadLyric(item);
  return true;
}

export async function variantsFor(item) {
  if (!item) return [];
  if (!state.libraryLoaded) await loadLibrary();

  // 曲库里「同一首歌」的全部版本：合并后同一 song_key 下可能混着本地文件与多个在线音源，
  // 不能按 platform 分流，否则播放页看不到另一半版本。
  const anchor = state.library.find((track) =>
    (item.sha && track.sha === item.sha) ||
    (item.itemId && track.item_id && String(track.item_id) === String(item.itemId) && track.platform === item.platform));
  const songKey = item.songKey || (anchor && anchor.song_key) || '';
  const libTracks = songKey
    ? state.library.filter((track) => track.song_key === songKey)
    : state.library.filter((track) => track.sha === item.sha);

  const seen = new Set();
  const variants = [];
  const push = (variant) => {
    if (!variant.id || seen.has(variant.id)) return;
    seen.add(variant.id);
    variants.push(variant);
  };
  for (const track of libTracks) {
    const platform = track.platform || 'local';
    push({
      kind: 'library',
      id: track.sha,
      sha: track.sha,
      platform,
      label: qualityLabel(track.quality),
      detail: [platformLabel(platform), qualityDetail(track.quality)].filter(Boolean).join(' · '),
      quality: track.quality,
      track
    });
  }

  if (item.platform === 'local') {
    variants.sort((a, b) => qualityRank(b.quality && b.quality.level) - qualityRank(a.quality && a.quality.level));
    return variants;
  }
  // 在线音源：叠加该源可选的其它音质（库里已有实体的不再重复列出）
  let qualities = item.qualities && item.qualities.length ? item.qualities.slice() : [];
  if (!qualities.length) {
    const result = await api('POST', '/music/request', {
      source: item.platform, op: 'get_qualities', params: {id: item.itemId, item_id: item.itemId}
    });
    if (result.code === 0) {
      const value = result.data && result.data.result;
      const list = Array.isArray(value) ? value : (value && value.qualities) || [];
      qualities = list;
      item.qualities = list;
    }
  }
  for (const quality of sortQualities(qualities)) {
    const id = quality.quality_id || quality.id || quality.level;
    if (!id || seen.has(id)) continue;
    // 库里已存在同音源同音质的实体版本 → 用库里那条（可直接播放），不重复列
    const duplicated = variants.some((entry) =>
      entry.kind === 'library' && entry.platform === item.platform &&
      entry.quality && (entry.quality.quality_id || entry.quality.level) === id);
    if (duplicated) continue;
    push({kind: 'online', id, label: qualityLabel(quality), detail: qualityDetail(quality), quality});
  }
  variants.sort((a, b) => qualityRank(b.quality && b.quality.level) - qualityRank(a.quality && a.quality.level));
  return variants;
}

export function setQuality(qualityId) {
  closeQualityMenu();
  if (!player.current) return;
  const variant = player.variants.find((entry) => entry.id === qualityId);
  if (!variant) return;
  // 曲库里的具体版本（本地文件 / 在线条目，合并后可能跨音源）
  if (variant.kind === 'local' || variant.kind === 'library') {
    const track = variant.track;
    if (!track) return;
    if (track.sha === player.current.sha) { player.quality = qualityId; renderQualityRow(); renderQualityButton(); return; }
    const item = itemFromTrack(track) || itemFromLocal(track);
    item.key = player.current.key;
    if (player.index >= 0 && player.index < player.queue.length) player.queue[player.index] = item;
    player.quality = qualityId;
    playItem(item, {quality: 'auto'});
    return;
  }
  player.quality = qualityId;
  playItem(player.current, {quality: qualityId});
}

export function renderPlayer() {
  const current = player.current;
  const title = current ? current.title : '未在播放';
  const artist = current ? [current.artist, current.sourceLabel].filter(Boolean).join(' · ') : 'Rad Music';
  const coverHtml = current && current.pic
    ? '<img src="' + esc(coverUrl(current.pic)) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />'
    : icon('music', 22);

  const pbCover = $('#pb-cover');
  if (pbCover) pbCover.innerHTML = coverHtml;
  const pbTitle = $('#pb-title');
  if (pbTitle) pbTitle.textContent = title;
  const pbArtist = $('#pb-artist');
  if (pbArtist) pbArtist.textContent = artist;
  const playIcon = player.playing ? icon('pause', 22) : icon('play', 22);
  const pbPlay = $('#pb-play');
  if (pbPlay) pbPlay.innerHTML = playIcon;
  const pbPrev = $('#pb-prev');
  if (pbPrev) pbPrev.innerHTML = icon('prev', 20);
  const pbNext = $('#pb-next');
  if (pbNext) pbNext.innerHTML = icon('next', 20);
  const pbLyric = $('#pb-lyric');
  if (pbLyric) pbLyric.innerHTML = icon('lyric', 20);
  const pbQueue = $('#pb-queue');
  if (pbQueue) pbQueue.innerHTML = icon('queue', 20);
  const pbExpand = $('#pb-expand');
  if (pbExpand) pbExpand.innerHTML = icon('expand', 20);
  const volIcon = $('#pb-vol-icon');
  if (volIcon) volIcon.innerHTML = icon(player.muted || player.volume === 0 ? 'mute' : 'volume', 18);
  renderLoopButtons();
  const pbVolume = $('#pb-volume');
  if (pbVolume && document.activeElement !== pbVolume) pbVolume.value = String(Math.round((player.muted ? 0 : player.volume) * 100));
  renderQualityButton();
  renderPlayerPage();
}

function renderQualityButton() {
  const button = $('#pb-quality');
  if (!button) return;
  const current = player.current;
  if (!current) { button.style.display = 'none'; return; }
  button.style.display = '';
  const variant = player.variants.find((entry) => entry.id === player.quality)
    || player.variants.find((entry) => entry.sha && entry.sha === current.sha)
    || player.variants.find((entry) => current.quality && entry.id === (current.quality.quality_id || current.quality.level));
  const label = player.quality === 'auto'
    ? (current.quality ? qualityLabel(current.quality) : '自动')
    : (variant ? variant.label : qualityLabel(current.quality));
  button.textContent = label || '音质';
  button.title = '音质：' + (label || '自动') + '（点击切换）';
}

export function renderProgress() {
  const duration = player.duration || 0;
  const position = Math.min(player.position || 0, duration || player.position || 0);
  const ratio = duration > 0 ? Math.min(1, position / duration) : 0;
  const curText = fmtTime(position);
  const durText = fmtTime(duration);
  const seats = [
    {seek: '#pb-seek', cur: '#pb-cur', dur: '#pb-dur'},
    {seek: '#pp-seek', cur: '#pp-cur', dur: '#pp-dur'}
  ];
  for (const seat of seats) {
    const seek = $(seat.seek);
    const cur = $(seat.cur);
    const dur = $(seat.dur);
    if (cur) cur.textContent = curText;
    if (dur) dur.textContent = durText;
    if (seek && document.activeElement !== seek) {
      seek.value = String(Math.round(ratio * 1000));
      seek.style.setProperty('--fill', (ratio * 100) + '%');
    }
  }
}

export function highlightLyric() {
  if (!player.lyric.length) return;
  const ms = player.position || 0;
  let index = -1;
  for (let i = 0; i < player.lyric.length; i += 1) {
    if (player.lyric[i].time_ms <= ms + 60) index = i; else break;
  }
  if (index === player.lyricActive) return;
  player.lyricActive = index;
  const nodes = $$('.lyric-line[data-line]');
  nodes.forEach((node) => {
    const active = Number(node.dataset.line) === index;
    node.classList.toggle('active', active);
  });
  const activeNode = $('.pp-lyrics .lyric-line.active');
  if (activeNode && player.pageOpen && player.pageTab === 'lyric') {
    const host = $('#pp-lyrics');
    if (host) {
      const target = activeNode.offsetTop - host.clientHeight * 0.42;
      host.scrollTo({top: Math.max(0, target), behavior: 'smooth'});
    }
  }
}

export async function loadLibrary(force = false) {
  if (state.libraryLoaded && !force) return state.library;
  const result = await api('GET', '/music/library');
  if (result.code === 0) {
    state.library = (result.data && result.data.tracks) || [];
    state.libraryLoaded = true;
  } else if (!state.library.length) {
    state.library = [];
  }
  return state.library;
}

export async function loadLyric(item) {
  player.lyric = [];
  player.lyricActive = -1;
  if (!item) { renderLyrics(); return; }
  let data = null;
  const request = item.platform === 'local'
    ? api('GET', '/music/lyric?sha=' + encodeURIComponent(item.sha || ''))
    : api('GET', '/music/lyric?source=' + encodeURIComponent(item.platform) + '&id=' + encodeURIComponent(item.itemId));
  const result = await request;
  if (result.code === 0) data = result.data;
  if ((!data || !data.lyric) && item.platform !== 'local') {
    const fallback = await api('POST', '/music/request', {
      source: item.platform, op: 'get_lyric', params: {id: item.itemId, item_id: item.itemId}
    });
    if (fallback.code === 0) {
      const value = fallback.data && fallback.data.result;
      if (value) {
        if (typeof value === 'string') data = {lyric: value, timed: true, offset_ms: 0};
        else if (value.lrc) data = {lyric: value.lrc, timed: true, offset_ms: value.offset_ms || 0};
        else if (value.lyric) data = value;
      }
    }
  }
  let lines = lyricLinesFromData(data);
  const offset = Number(data && data.offset_ms) || 0;
  if (offset) lines = lines.map((line) => Object.assign({}, line, {time_ms: line.time_ms + offset}));
  player.lyric = lines;
  player.lyricActive = -1;
  renderLyrics();
}

export function renderLyrics() {
  const host = $('#pp-lyrics');
  const lines = player.lyric || [];
  const markup = lines.length
    ? lines.map((line, index) =>
      '<div class="lyric-line" data-line="' + index + '">' + esc(line.text || '') +
      (line.tr ? '<span class="tr">' + esc(line.tr) + '</span>' : '') + '</div>').join('')
    : '<div class="lyric-placeholder">' +
      (player.current ? '暂无歌词，或歌词还在获取中。' : '选择一首歌开始播放，歌词会显示在这里。') + '</div>';
  if (host) host.innerHTML = markup;
  if (host) {
    $$('.lyric-line', host).forEach((node) => {
      node.addEventListener('click', () => {
        const index = Number(node.dataset.line);
        const line = player.lyric[index];
        if (line) seek(line.time_ms / 1000);
      });
    });
  }
  const panel = $('.lyric-panel');
  if (panel) {
    panel.innerHTML = lines.length
      ? lines.map((line, index) => '<div class="lyric-line" data-line="' + index + '">' + esc(line.text || '') + '</div>').join('')
      : '<div class="lyric-placeholder">暂无歌词</div>';
  }
  highlightLyric();
}

export function renderQualityRow() {
  const host = $('#pp-quality-row');
  if (!host) return;
  const current = player.current;
  if (!current) { host.innerHTML = ''; return; }
  const parts = [];
  if (current.quality) parts.push(qualityBadge(current.quality));
  parts.push('<span class="tag">' + esc(current.platform === 'local' ? '本地曲库' : platformLabel(current.platform)) + '</span>');
  if (player.variants.length > 1) parts.push('<span class="tag accent">' + player.variants.length + ' 个音质版本</span>');
  if (current.album) parts.push('<span class="tag">' + esc(current.album) + '</span>');
  host.innerHTML = parts.join('');
}

export function renderPlayerPage() {
  const page = $('#player-page');
  if (!page || page.hidden) return;
  const current = player.current;
  const cover = $('#pp-cover');
  if (cover) {
    cover.innerHTML = current && current.pic
      ? '<img src="' + esc(coverUrl(current.pic)) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />'
      : icon('music', 56);
  }
  const bg = $('#pp-bg');
  if (bg) {
    // 相对路径的封面（covers/xxx）必须走宿主 /music/cover/ 路由，否则浏览器会 404
    const bgUrl = current && current.pic ? coverUrl(current.pic) : '';
    if (bgUrl) bg.style.backgroundImage = 'url("' + bgUrl.replace(/"/g, '%22') + '")';
    else bg.style.removeProperty('background-image');
  }
  const title = $('#pp-title');
  if (title) title.textContent = current ? current.title : '未在播放';
  const artist = $('#pp-artist');
  if (artist) {
    if (current && current.artist) {
      artist.innerHTML = '<span class="pp-artist-name">' + esc(current.artist) + '</span>' + icon('chevronRight', 15, 'pp-artist-caret');
      artist.classList.add('linkable');
      artist.title = '查看歌手：' + current.artist;
    } else {
      artist.textContent = current ? '未知歌手' : '选择一首歌开始';
      artist.classList.remove('linkable');
      artist.removeAttribute('title');
    }
  }
  const source = $('#pp-source');
  if (source) source.textContent = current ? (current.platform === 'local' ? '本地曲库' : platformLabel(current.platform)) : '—';
  const play = $('#pp-play');
  if (play) play.innerHTML = player.playing ? icon('pause', 26) : icon('play', 26);
  const prev = $('#pp-prev');
  if (prev) prev.innerHTML = icon('prev', 24);
  const nextBtn = $('#pp-next');
  if (nextBtn) nextBtn.innerHTML = icon('next', 24);
  const close = $('#pp-close');
  if (close) close.innerHTML = icon('chevronDown', 22);
  const queueToggle = $('#pp-queue-toggle');
  if (queueToggle) queueToggle.innerHTML = icon('queue', 20);
  const vol = $('#pp-vol-icon');
  if (vol) vol.innerHTML = icon(player.muted || player.volume === 0 ? 'mute' : 'volume', 18);
  renderLoopButtons();
  const volInput = $('#pp-volume');
  if (volInput && document.activeElement !== volInput) volInput.value = String(Math.round((player.muted ? 0 : player.volume) * 100));
  const lyricToggle = $('#pp-lyric-toggle');
  if (lyricToggle) lyricToggle.classList.toggle('active', player.pageTab === 'lyric');
  const tabButtons = $$('#pp-quality-btn, #pp-add-playlist');
  tabButtons.forEach((button) => { button.classList.remove('active'); });
  renderQualityRow();
  renderQueue();
}

export function renderQueue() {
  const host = $('#pp-queue');
  if (!host) return;
  const showQueue = player.pageTab === 'queue';
  host.hidden = !showQueue;
  const lyricsHost = $('#pp-lyrics');
  if (lyricsHost) lyricsHost.hidden = showQueue;
  if (!showQueue) return;
  if (!player.queue.length) {
    host.innerHTML = '<div class="lyric-placeholder">播放队列为空</div>';
    return;
  }
  host.innerHTML = '<h3>播放队列（' + player.queue.length + '）</h3>' +
    player.queue.map((item, index) =>
      '<div class="track-row' + (index === player.index ? ' playing' : '') + '" data-queue="' + index + '">' +
      '<div class="track-index">' + (index === player.index ? icon('play', 12) : (index + 1)) + '</div>' +
      '<div class="track-main"><div class="track-title-line"><span class="track-name">' + esc(item.title) + '</span></div>' +
      '<div class="track-sub"><span>' + esc(item.artist) + '</span></div></div>' +
      (item.quality ? qualityBadge(item.quality) : '') +
      '<div class="track-duration">' + fmtDuration(item.duration) + '</div></div>').join('');
  $$('[data-queue]', host).forEach((row) => {
    row.addEventListener('dblclick', () => {
      const index = Number(row.dataset.queue);
      player.queueIndex = index;
      player.index = index;
      playItem(player.queue[index]);
    });
  });
}

export function openPlayerPage() {
  const page = $('#player-page');
  if (!page) return;
  page.hidden = false;
  player.pageOpen = true;
  renderPlayerPage();
  renderProgress();
  renderLyrics();
}

export function closePlayerPage() {
  const page = $('#player-page');
  if (page) page.hidden = true;
  player.pageOpen = false;
}

// 播放页点歌手名 → 联网解析歌手 → 打开歌手页（歌手页再拉详情）
async function artistSearchSource(preferred) {
  const result = await api('GET', '/music/plugins');
  const plugins = result.code === 0 ? safeArray(result.data && result.data.plugins) : [];
  const candidates = [];
  const seen = new Set();
  for (const plugin of plugins) {
    if (plugin.enabled === false || plugin.has_implementation === false) continue;
    if (!safeArray(plugin.capabilities).includes('search_artist')) continue;
    for (const platform of safeArray(plugin.platforms)) {
      if (!platform || seen.has(platform)) continue;
      seen.add(platform);
      candidates.push(platform);
    }
  }
  if (preferred && candidates.includes(preferred)) return preferred;
  return candidates[0] || '';
}

async function openArtistByName(name, platformHint) {
  const keyword = String(name || '').trim();
  if (!keyword) { toast('未知歌手'); return; }
  const source = await artistSearchSource(platformHint);
  if (!source) { toast('没有可搜索歌手的音源', 'error'); return; }
  toast('正在联网查找歌手「' + keyword + '」…');
  const result = await api('POST', '/music/request', {
    source, op: 'search_artist', params: {keyword, page: 1, page_size: 8}
  });
  if (result.code !== 0) { toast('查找歌手失败：' + errText(result), 'error'); return; }
  const payload = (result.data && result.data.result) || result.data || {};
  const artists = safeArray(payload.artists);
  if (!artists.length) { toast('未找到歌手「' + keyword + '」', 'error'); return; }
  const lower = keyword.toLowerCase();
  const best = artists.find((entry) => String(entry.name || '').trim().toLowerCase() === lower) || artists[0];
  if (!best.id) { toast('未找到歌手「' + keyword + '」', 'error'); return; }
  navigate('artist', {id: source + ':' + best.id, title: best.name || keyword});
  if (player.pageOpen) closePlayerPage();
}

function openArtistFromCurrent(anchor) {
  const current = player.current;
  if (!current) return;
  const platform = current.platform;
  const raw = String(current.artist || '').trim();
  if (!raw) { toast('未知歌手'); return; }
  const names = raw.split(/\s*\/\s*/).map((part) => part.trim()).filter(Boolean);
  if (names.length <= 1) { openArtistByName(raw, platform); return; }
  let x = 120;
  let y = 120;
  if (anchor && anchor.getBoundingClientRect) {
    const rect = anchor.getBoundingClientRect();
    x = rect.left;
    y = rect.bottom + 6;
  }
  contextMenu(x, y, names.map((name) => ({label: name, icon: 'star', onClick: () => openArtistByName(name, platform)})));
}

function openQualityMenu() {
  const menu = $('#quality-menu');
  if (!menu) return;
  if (!menu.hidden) { closeQualityMenu(); return; }
  const inPage = player.pageOpen;
  const host = inPage ? $('#player-page') : $('#player-bar');
  if (host && menu.parentElement !== host) host.appendChild(menu);
  menu.style.right = inPage ? '24px' : '';
  menu.style.bottom = inPage ? '96px' : '';
  menu.style.left = 'auto';
  const current = player.current;
  if (!current) { toast('先选择一首歌'); return; }
  const variants = player.variants || [];
  const rows = [];
  rows.push('<div class="qm-head">音质与版本 · ' + esc(current.platform === 'local' ? '本地' : platformLabel(current.platform)) + '</div>');
  rows.push('<button class="qm-item' + (player.quality === 'auto' ? ' active' : '') + '" data-quality="auto">' +
    '<div class="qm-main"><span class="qm-label">自动（最高可用）</span>' +
    '<span class="qm-detail">由音源决定实际音质</span></div>' +
    (player.quality === 'auto' ? icon('check', 18) : '') + '</button>');
  if (!variants.length) {
    rows.push('<div class="qm-empty">该曲目没有更多可选音质</div>');
  }
  for (const variant of variants) {
    const active = (player.quality !== 'auto' && player.quality === variant.id) ||
      (player.quality === 'auto' && !!variant.sha && variant.sha === current.sha);
    rows.push('<button class="qm-item' + (active ? ' active' : '') + '" data-quality="' + esc(variant.id) + '">' +
      '<div class="qm-main"><span class="qm-label">' + esc(variant.label) + '</span>' +
      '<span class="qm-detail">' + esc(variant.detail || (variant.kind === 'library' ? '曲库版本' : '在线音质')) + '</span></div>' +
      (active ? icon('check', 18) : '') + '</button>');
  }
  menu.innerHTML = rows.join('');
  menu.hidden = false;
  $$('[data-quality]', menu).forEach((button) => {
    button.addEventListener('click', () => {
      const value = button.dataset.quality;
      if (value === 'auto') { closeQualityMenu(); player.quality = 'auto'; if (player.current) playItem(player.current, {quality: 'auto'}); return; }
      setQuality(value);
    });
  });
}

export function closeQualityMenu() {
  const menu = $('#quality-menu');
  if (menu) menu.hidden = true;
}

// ---------------------------------------------------------------- 播放控制
export function togglePlay() {
  if (!player.current) {
    if (player.queue.length) { playQueue(player.queue, 0); return; }
    toast('播放队列为空');
    return;
  }
  if (player.playing) pause(); else resume();
}

export function resume() {
  if (!player.current) { togglePlay(); return; }
  const audio = audioEl();
  if (!audio.src) { playItem(player.current); return; }
  audio.play().then(() => { player.playing = true; renderPlayer(); syncControl('resume'); })
    .catch((error) => toast('播放失败：' + String(error && error.message || error), 'error'));
}

export function pause() {
  if (player.audio) player.audio.pause();
  player.playing = false;
  renderPlayer();
  syncControl('pause');
}

export function stop() {
  if (player.audio) { player.audio.pause(); player.audio.removeAttribute('src'); player.audio.load(); }
  player.playing = false;
  player.position = 0;
  player.duration = 0;
  syncControl('stop');
  renderPlayer();
  renderProgress();
}

// ---------------------------------------------------------------- MV 全窗口播放
// MV 铺满整个窗口（不是弹窗）：打开时自动暂停正在播放的音乐，关闭时先停掉视频再恢复音乐
export const mvStage = {open: false, mv: null, resumeMusic: false, keyHandler: null};

export function isMvStageOpen() { return !!mvStage.open; }

export function openMvStage(mv) {
  const stage = $('#mv-stage');
  if (!stage || !mv) return;
  if (mvStage.open) closeMvStage({restore: false});
  // 记下「打开 MV 之前音乐是否在播」，关闭时才知道要不要恢复
  mvStage.resumeMusic = !!(player.audio && !player.audio.paused) || (!!player.playing && !!player.current);
  if (mvStage.resumeMusic) pause();
  mvStage.open = true;
  mvStage.mv = mv;
  stage.hidden = false;
  renderMvStage();
  if (!mvStage.keyHandler) {
    mvStage.keyHandler = (event) => {
      if (event.key !== 'Escape') return;
      const tag = (event.target && event.target.tagName) || '';
      if (/INPUT|TEXTAREA|SELECT/.test(tag)) return;
      closeMvStage();
    };
    document.addEventListener('keydown', mvStage.keyHandler);
  }
}

export function closeMvStage({restore = true} = {}) {
  // 先彻底停掉 MV：暂停 + 卸掉 src，避免关掉后还在后台缓冲/出声
  const video = $('#mvs-video');
  if (video) {
    try { video.pause(); } catch (error) { /* 忽略 */ }
    video.removeAttribute('src');
    try { video.load(); } catch (error) { /* 忽略 */ }
  }
  const stage = $('#mv-stage');
  if (stage) stage.hidden = true;
  mvStage.open = false;
  mvStage.mv = null;
  if (mvStage.keyHandler) {
    document.removeEventListener('keydown', mvStage.keyHandler);
    mvStage.keyHandler = null;
  }
  const shouldResume = restore && mvStage.resumeMusic;
  mvStage.resumeMusic = false;
  if (shouldResume && player.current) resume();
}

function renderMvStage() {
  const mv = mvStage.mv || {};
  const setText = (selector, value) => { const node = $(selector); if (node) node.textContent = value || ''; };
  setText('#mvs-source', mv.platform ? platformLabel(mv.platform) : '本地');
  setText('#mvs-title', mv.title || '未命名 MV');
  setText('#mvs-artist', mv.artist || '');

  const cover = $('#mvs-cover');
  if (cover) {
    cover.innerHTML = mv.pic
      ? '<img src="' + esc(mv.pic) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />'
      : icon('video', 28);
  }

  const rows = [];
  const duration = Number(mv.duration_seconds || mv.duration || 0) || 0;
  if (duration) rows.push(['时长', fmtDuration(duration)]);
  rows.push(['来源', mv.platform ? platformLabel(mv.platform) : '本地']);
  if (mv.item_id) rows.push(['来源 ID', String(mv.item_id)]);
  if (Number(mv.width) && Number(mv.height)) rows.push(['分辨率', mv.width + ' × ' + mv.height]);
  if (mv.sha) rows.push(['SHA', String(mv.sha)]);
  const address = String(mv.location || '(空)');
  rows.push(['地址', address.length > 96 ? address.slice(0, 96) + '…' : address, address]);
  const info = $('#mvs-info');
  if (info) {
    info.innerHTML = rows.map((row) => '<dt>' + esc(row[0]) + '</dt><dd' +
      (row[2] ? ' title="' + esc(row[2]) + '"' : '') + '>' + esc(row[1]) + '</dd>').join('');
  }

  const location = String(mv.location || '');
  const isHttp = /^https?:/i.test(location);
  const video = $('#mvs-video');
  const box = $('#mvs-video-box');
  const note = $('#mvs-note');
  if (box) {
    const fallback = $('.mv-stage-fallback', box);
    if (fallback) fallback.remove();
    if (!isHttp) {
      box.insertAdjacentHTML('beforeend',
        '<div class="mv-stage-fallback">视频地址不是 http(s) 直链，浏览器无法直接播放：<br />' + esc(location || '(空)') + '</div>');
    }
  }
  if (note) note.textContent = '';
  if (!video) return;
  if (isHttp) {
    video.hidden = false;
    video.src = location;
    video.play().catch(() => {
      if (note) note.textContent = '浏览器拦截了自动播放，点一下视频里的播放键开始。';
    });
  } else {
    video.hidden = true;
    video.removeAttribute('src');
  }
}

async function reopenMv() {
  const mv = mvStage.mv;
  if (!mv || !mv.sha) { toast('没有可解析的 MV'); return; }
  const opened = await api('POST', '/music/mv/' + encodeURIComponent(mv.sha) + '/open', {});
  if (opened.code !== 0) { toast(errText(opened), 'error'); return; }
  const next = (opened.data && (opened.data.mv || opened.data)) || {};
  mvStage.mv = Object.assign({}, mv, next);
  const list = safeArray(state.mvs);
  const index = list.findIndex((entry) => entry.sha === mvStage.mv.sha);
  if (index >= 0) list[index] = Object.assign({}, list[index], mvStage.mv);
  else list.push(mvStage.mv);
  state.mvs = list;
  toast('已重新解析视频地址', 'ok');
  renderMvStage();
}

async function copyMvLink() {
  const location = String((mvStage.mv && mvStage.mv.location) || '');
  if (!location) { toast('没有视频地址'); return; }
  try {
    await navigator.clipboard.writeText(location);
    toast('已复制视频地址', 'ok');
  } catch (error) {
    toast('复制失败：' + String(error && error.message || error), 'error');
  }
}

export async function playMvStage(mv, {resolve = false} = {}) {
  if (!mv || !mv.sha) return;
  let target = mv;
  if (resolve || !mv.location) {
    const opened = await api('POST', '/music/mv/' + encodeURIComponent(mv.sha) + '/open', {});
    if (opened.code !== 0) { toast(errText(opened), 'error'); return; }
    target = Object.assign({}, mv, (opened.data && (opened.data.mv || opened.data)) || {});
    const list = safeArray(state.mvs);
    const index = list.findIndex((entry) => entry.sha === target.sha);
    if (index >= 0) list[index] = Object.assign({}, list[index], target);
    else list.push(target);
    state.mvs = list;
  }
  openMvStage(target);
}

function bindMvStage() {
  const close = $('#mvs-close');
  if (close) { close.innerHTML = icon('close', 20); close.addEventListener('click', () => closeMvStage()); }
  const reopen = $('#mvs-reopen');
  if (reopen) { reopen.innerHTML = icon('refresh', 19); reopen.addEventListener('click', () => reopenMv()); }
  const refresh = $('#mvs-refresh');
  if (refresh) refresh.addEventListener('click', () => reopenMv());
  const copy = $('#mvs-copy');
  if (copy) copy.addEventListener('click', () => copyMvLink());
  const video = $('#mvs-video');
  if (video) {
    video.addEventListener('error', () => {
      if (!video.src) return;
      const note = $('#mvs-note');
      if (note) note.textContent = '视频加载失败，可能是地址已过期，试试「重新解析」。';
    });
  }
}

// 从头重播当前曲目：不动队列、不动索引，也不重新解析音源（除非音频还没加载过）
async function restartCurrent() {
  const item = (player.index >= 0 && player.index < player.queue.length) ? player.queue[player.index] : player.current;
  if (!item) return false;
  const audio = player.audio;
  if (audio && audio.src && player.current === item) {
    audio.currentTime = 0;
    player.position = 0;
    renderProgress();
    try {
      await audio.play();
      player.playing = true;
      renderPlayer();
      syncControl('resume');
      return true;
    } catch (error) {
      // 浏览器拒绝自动播放时退化成重新加载，保证至少是「从头开始」的状态
    }
  }
  return playItem(item);
}

export function next() {
  if (!player.queue.length) return;
  // 单曲循环：手动切下一首也只是重播当前这首，不换曲目、不改队列
  if (player.loop === 'one') { restartCurrent(); return; }
  let index;
  if (player.shuffle && player.queue.length > 1) {
    do { index = Math.floor(Math.random() * player.queue.length); } while (index === player.index);
  } else {
    index = player.index + 1;
    if (index >= player.queue.length) {
      if (player.loop === 'all') index = 0;
      else {
        // 顺序播放到了最后一首：真正暂停音频（此前只改了状态标记，声音还在放、图标却变了）
        pause();
        renderProgress();
        toast('已经是最后一首');
        return;
      }
    }
  }
  player.index = index;
  playItem(player.queue[index]);
}

export function prev() {
  if (!player.queue.length) return;
  // 单曲循环：与「下一首」对称，只重播当前这首
  if (player.loop === 'one') { restartCurrent(); return; }
  if (player.position > 4000) { seek(0); return; }
  let index = player.index - 1;
  if (index < 0) index = player.loop === 'all' ? player.queue.length - 1 : 0;
  player.index = index;
  playItem(player.queue[index]);
}

export function seek(seconds) {
  if (!player.audio || !player.current) return;
  const audio = player.audio;
  if (audio.readyState > 0) {
    audio.currentTime = Math.max(0, seconds);
    player.position = audio.currentTime * 1000;
    renderProgress();
    syncControl('seek', {seconds: Number(seconds) || 0});
  }
}

export function setVolume(value) {
  const volume = Math.min(1, Math.max(0, Number(value) || 0));
  player.volume = volume;
  player.muted = volume === 0;
  if (player.audio) player.audio.volume = volume;
  renderPlayer();
  saveSettings({volume}, {silent: true});
}

export function toggleMute() {
  player.muted = !player.muted;
  if (player.audio) player.audio.volume = player.muted ? 0 : player.volume;
  renderPlayer();
}

const LOOP_LABELS = {off: '顺序播放', all: '列表循环', one: '单曲循环'};

// 渲染循环模式按钮：顺序播放=灰显，列表循环=点亮，单曲循环=点亮+角标 1
function renderLoopButton(selector, size) {
  const button = $(selector);
  if (!button) return;
  const mode = player.loop;
  const badge = mode === 'one' ? '<span class="loop-badge">1</span>' : '';
  button.innerHTML = '<span class="loop-ico">' + icon('repeat', size) + '</span>' + badge;
  button.classList.toggle('active', mode !== 'off');
  button.title = '循环：' + LOOP_LABELS[mode] + '（点击切换）';
}

function renderLoopButtons() {
  renderLoopButton('#pb-loop', 18);
  renderLoopButton('#pp-loop', 20);
}

export function toggleLoop() {
  const order = ['off', 'all', 'one'];
  player.loop = order[(order.indexOf(player.loop) + 1) % order.length];
  toast(LOOP_LABELS[player.loop]);
  renderLoopButtons();
}

export function toggleShuffle() {
  player.shuffle = !player.shuffle;
  toast(player.shuffle ? '随机播放已开启' : '随机播放已关闭');
}

export function playQueue(items, index = 0) {
  player.queue = (items || []).slice();
  player.index = Math.min(Math.max(0, index), Math.max(0, player.queue.length - 1));
  if (player.queue.length) playItem(player.queue[player.index]);
}

export function enqueue(items, {playNext = false} = {}) {
  const list = items || [];
  if (!list.length) return;
  if (playNext) player.queue.splice(player.index + 1, 0, ...list);
  else player.queue.push(...list);
  toast('已加入队列（' + list.length + ' 首）');
  renderQueue();
}

// 播放单曲：不清空播放队列，把曲目插入到「当前正在播放的曲目」之后并立即播放；
// 若当前没有正在播放的曲目（队列为空或未开始播放），则插入到队列开头。
// 播放歌单 / 专辑 / 播放全部等整集合请继续使用 playQueue()（会清空队列）。
export async function enqueueAndPlay(items, {quality = undefined} = {}) {
  const list = items || [];
  if (!list.length) return;
  const hasCurrent = !!player.current && player.index >= 0 && player.index < player.queue.length;
  const at = hasCurrent ? player.index + 1 : 0;
  player.queue.splice(at, 0, ...list);
  player.index = at;
  renderQueue();
  await playItem(player.queue[at], {quality});
}

// ---------------------------------------------------------------- 绑定 & 初始化
function bindRange(input, onInput) {
  if (!input) return;
  input.addEventListener('input', () => {
    const ratio = Number(input.value) / Number(input.max || 1000);
    input.style.setProperty('--fill', (ratio * 100) + '%');
    onInput(input);
  });
  input.addEventListener('change', () => onInput(input));
}

function bindPlayerUi() {
  const pbPlay = $('#pb-play');
  if (pbPlay) pbPlay.addEventListener('click', togglePlay);
  const pbPrev = $('#pb-prev');
  if (pbPrev) pbPrev.addEventListener('click', prev);
  const pbNext = $('#pb-next');
  if (pbNext) pbNext.addEventListener('click', next);
  const bindLoop = (selector) => {
    const button = $(selector);
    if (button) button.addEventListener('click', (event) => { event.stopPropagation(); toggleLoop(); });
  };
  bindLoop('#pb-loop');
  bindLoop('#pp-loop');
  const pbQuality = $('#pb-quality');
  if (pbQuality) pbQuality.addEventListener('click', (event) => { event.stopPropagation(); openQualityMenu(); });
  const pbExpand = $('#pb-expand');
  if (pbExpand) pbExpand.addEventListener('click', openPlayerPage);
  const pbCover = $('#pb-cover');
  if (pbCover) pbCover.addEventListener('click', openPlayerPage);
  const pbTitle = $('#pb-title');
  if (pbTitle) pbTitle.addEventListener('click', openPlayerPage);
  const pbLyric = $('#pb-lyric');
  if (pbLyric) pbLyric.addEventListener('click', () => { openPlayerPage(); player.pageTab = 'lyric'; renderQueue(); renderPlayerPage(); });
  const pbQueue = $('#pb-queue');
  if (pbQueue) pbQueue.addEventListener('click', () => { openPlayerPage(); player.pageTab = 'queue'; renderQueue(); renderPlayerPage(); });

  const volumeHandler = (input) => setVolume(Number(input.value) / 100);
  bindRange($('#pb-volume'), volumeHandler);
  bindRange($('#pp-volume'), volumeHandler);
  const seekHandler = (input) => {
    const ratio = Number(input.value) / Number(input.max || 1000);
    seek(ratio * (player.duration || 0) / 1000);
  };
  bindRange($('#pb-seek'), seekHandler);
  bindRange($('#pp-seek'), seekHandler);

  const page = $('#player-page');
  if (page) {
    $('#pp-close') && $('#pp-close').addEventListener('click', closePlayerPage);
    $('#pp-play') && $('#pp-play').addEventListener('click', togglePlay);
    $('#pp-prev') && $('#pp-prev').addEventListener('click', prev);
    $('#pp-next') && $('#pp-next').addEventListener('click', next);
    $('#pp-quality-btn') && $('#pp-quality-btn').addEventListener('click', () => {
      // 打开音质弹层：在播放页里复用播放栏的音质菜单
      const menu = $('#quality-menu');
      if (menu) { openPlayerPage(); openQualityMenu(); }
    });
    $('#pp-lyric-toggle') && $('#pp-lyric-toggle').addEventListener('click', () => {
      player.pageTab = player.pageTab === 'lyric' ? 'queue' : 'lyric';
      renderQueue(); renderPlayerPage();
    });
    $('#pp-queue-toggle') && $('#pp-queue-toggle').addEventListener('click', () => {
      player.pageTab = player.pageTab === 'queue' ? 'lyric' : 'queue';
      renderQueue(); renderPlayerPage();
    });
    $('#pp-add-playlist') && $('#pp-add-playlist').addEventListener('click', () => {
      if (player.current) addToPlaylistFlow([player.current]);
    });
    $('#pp-artist') && $('#pp-artist').addEventListener('click', (event) => {
      event.stopPropagation();
      openArtistFromCurrent(event.currentTarget);
    });
    $('#pp-vol-icon') && $('#pp-vol-icon').addEventListener('click', toggleMute);
    $('#pp-lyrics') && $('#pp-lyrics').addEventListener('dblclick', openQualityMenu);
  }
  const pbVolIcon = $('#pb-vol-icon');
  if (pbVolIcon) pbVolIcon.addEventListener('click', toggleMute);

  document.addEventListener('click', (event) => {
    if (!event.target.closest('#quality-menu') && !event.target.closest('#pb-quality') && !event.target.closest('#pp-quality-btn')) closeQualityMenu();
    if (!event.target.closest('#ctx-menu')) closeContextMenu();
  });
  document.addEventListener('keydown', (event) => {
    // MV 全窗口播放时，快捷键交给视频自己（空格 = 视频播放/暂停，Esc = 关 MV）
    if (mvStage.open) return;
    const tag = (event.target && event.target.tagName) || '';
    if (/INPUT|TEXTAREA|SELECT/.test(tag)) return;
    if (event.code === 'Space') { event.preventDefault(); togglePlay(); }
    else if (event.code === 'Escape') { closeQualityMenu(); if (player.pageOpen) closePlayerPage(); }
    else if (event.code === 'ArrowRight' && event.altKey) next();
    else if (event.code === 'ArrowLeft' && event.altKey) prev();
    else if (event.code === 'ArrowRight' && event.shiftKey) seek((player.position + 5000) / 1000);
    else if (event.code === 'ArrowLeft' && event.shiftKey) seek(Math.max(0, player.position - 5000) / 1000);
  });
}

function bindNav() {
  const nav = $('#nav');
  if (!nav) return;
  const icons = {library: 'library', search: 'search', artists: 'star', playlists: 'playlist', albums: 'album', mv: 'video', downloads: 'download', import: 'upload', settings: 'settings'};
  $$('.nav-item', nav).forEach((button) => {
    const route = button.dataset.route;
    button.innerHTML = icon(icons[route] || 'music', 19) + '<span>' + esc(ROUTE_META[route] ? ROUTE_META[route].title : route) + '</span>';
    button.addEventListener('click', () => navigate(route));
  });
  const toggle = $('#btn-nav');
  if (toggle) toggle.addEventListener('click', () => { const sidebar = $('#sidebar'); if (sidebar) sidebar.classList.toggle('open'); });
}

function bindChrome() {
  const brand = $('.brand-mark');
  if (brand) brand.textContent = 'Rad';
  const boot = $('#boot-error');
  if (boot) boot.hidden = true;
}

// 队列管理弹窗：把多个曲目加入歌单 / 收藏
export async function addToPlaylistFlow(items) {
  const list = items || [];
  if (!list.length) return;
  const payloadOf = () => list.map((item) => ({
    platform: item.platform, item_id: item.itemId, sha: item.sha || '',
    name: item.title, author: item.artist, duration: item.duration || 0, group_key: item.groupKey || ''
  }));
  const result = await api('GET', '/music/playlist');
  const playlists = (result.code === 0 && result.data && result.data.playlists) || [];
  const rows = playlists.length
    ? playlists.map((list2) => '<button class="ctx-item" data-playlist="' + esc(list2.id) + '">' +
      icon('playlist', 17) + '<span>' + esc(list2.name) + '</span><span class="spacer"></span><span class="muted">' + (list2.track_count || 0) + '</span></button>').join('')
    : '<div class="qm-empty">还没有歌单，先创建一个</div>';
  openModal({
    title: '加入歌单（' + list.length + ' 首）',
    body: '<div class="col" style="gap:2px">' + rows + '</div>' +
      '<div class="divider"></div><button class="btn primary" data-new-playlist style="width:100%">' + icon('plus', 16) + '新建歌单</button>',
    onMount: (box) => {
      $$('[data-playlist]', box).forEach((button) => {
        button.addEventListener('click', async () => {
          const playlistId = button.dataset.playlist;
          const added = await api('POST', '/music/playlist/' + encodeURIComponent(playlistId) + '/add_many', {items: payloadOf()});
          if (added.code === 0) {
            const count = added.data && added.data.track_count;
            toast('已加入歌单' + (count != null ? '（共 ' + count + ' 首）' : ''), 'ok');
            closeModal();
          } else {
            toast(errText(added), 'error');
          }
        });
      });
      const create = $('[data-new-playlist]', box);
      if (create) create.addEventListener('click', async () => {
        const name = await promptDialog({title: '新建歌单', label: '歌单名称', placeholder: '例如：深夜循环'});
        if (!name) return;
        const created = await api('POST', '/music/playlist', {name});
        const playlist = created.code === 0 && created.data ? (created.data.playlist || created.data) : null;
        if (playlist && playlist.id) {
          closeModal();
          const added = await api('POST', '/music/playlist/' + encodeURIComponent(playlist.id) + '/add_many', {items: payloadOf()});
          toast(added.code === 0 ? '歌单已创建并加入曲目' : errText(added), added.code === 0 ? 'ok' : 'error');
        } else {
          toast(errText(created), 'error');
        }
      });
    }
  });
}

// ---------------------------------------------------------------- 启动
async function init() {
  bindChrome();
  bindNav();
  bindPlayerUi();
  bindMvStage();
  player.volume = Number(state.settings.volume) || 0.8;
  renderProgress();
  renderPlayer();
  loadSettings().then(() => { renderPlayer(); renderProgress(); });
  refreshConnection();
  setInterval(refreshConnection, 15000);
  const parsed = parseHash();
  navigate(parsed.route, parsed.params);
  window.addEventListener('hashchange', () => {
    const next = parseHash();
    if (next.route !== state.route || String(next.params.id || '') !== String((state.routeParams && state.routeParams.id) || '')) {
      navigate(next.route, next.params);
    }
  });
  loadLibrary().catch(() => {});
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) refreshConnection();
  });
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
else init();

export const THEME_OPTIONS = THEMES;

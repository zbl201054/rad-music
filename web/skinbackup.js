// Rad Music - 皮肤自动备份（把当前自定义外观存成 .rsk 落到本机曲库目录）
//
// 为什么落到磁盘而不是 localStorage：一张壁纸就一两 MB，浏览器配额撑不住几份；
// 而且用户真正的诉求是「改崩了能找回来」，文件在 <曲库>/skins/ 下更符合直觉，
// 还能直接发给别人。
//
// 防刷：外观没变就不写；同一分钟内反复改只覆盖同一份文件；服务端只保留最近 12 份。
import { API_BASE, api } from './api.js';
import { normalizeAppearance, readLocalBg } from './appearance.js';
import { buildSkinFile } from './skinfile.js';

const LAST_KEY = 'radmusic.backup.key';
const AUTHOR_KEY = 'radmusic.skinAuthor';

let timer = null;
let pending = null;
let running = false;

export function getAuthor() {
  try {
    return localStorage.getItem(AUTHOR_KEY) || '';
  } catch (error) {
    return '';
  }
}

export function setAuthor(value) {
  try {
    const text = String(value || '').trim();
    if (text) localStorage.setItem(AUTHOR_KEY, text);
    else localStorage.removeItem(AUTHOR_KEY);
  } catch (error) {
    /* ignore */
  }
}

// 外观指纹：主题/明暗/所有外观字段 + 壁纸内容（用长度 + 尾段做粗指纹，够区分不同图）
function fingerprint(settings, imageDataUrl) {
  const ap = normalizeAppearance(settings.appearance);
  const bg = String(imageDataUrl || '');
  return [
    settings.theme || '',
    settings.color_scheme || '',
    JSON.stringify(ap),
    bg.length,
    bg.slice(-80)
  ].join('|');
}

function stampName() {
  const now = new Date();
  const pad = (value) => String(value).padStart(2, '0');
  return '自动备份-' + now.getFullYear() + pad(now.getMonth() + 1) + pad(now.getDate()) +
    '-' + pad(now.getHours()) + pad(now.getMinutes()) + '.rsk';
}

function skinAppearance(ap, imageDataUrl) {
  return Object.assign({}, ap, {
    bg_image: imageDataUrl ? '' : ap.bg_image,
    bg_use_local: !!imageDataUrl
  });
}

/**
 * 生成并上传一份 .rsk 备份。
 * @returns {Promise<{ok: boolean, skipped?: boolean, name?: string, dir?: string, at?: number, error?: string}>}
 */
export async function backupNow(settings, {name = '', force = false} = {}) {
  if (!settings) return {ok: false, error: '设置还没加载好'};
  const ap = normalizeAppearance(settings.appearance);
  if (!ap.enabled) return {ok: false, skipped: true, error: '没有开启自定义外观，不需要备份'};
  const imageDataUrl = readLocalBg() || (/^data:/.test(ap.bg_image) ? ap.bg_image : '');
  const key = fingerprint(settings, imageDataUrl);
  if (!force) {
    try {
      if (localStorage.getItem(LAST_KEY) === key) return {ok: true, skipped: true};
    } catch (error) {
      /* ignore */
    }
  }
  const fileName = name || stampName();
  let bytes;
  try {
    bytes = await buildSkinFile({
      name: fileName.replace(/\.rsk$/i, ''),
      author: getAuthor(),
      theme: settings.theme || 'custom',
      colorScheme: settings.color_scheme || 'dark',
      appearance: skinAppearance(ap, imageDataUrl),
      imageDataUrl
    });
  } catch (error) {
    return {ok: false, error: String((error && error.message) || error)};
  }
  const result = await api('POST', '/music/skin/backup?filename=' + encodeURIComponent(fileName), bytes);
  if (result.code !== 0) {
    return {ok: false, error: result.message || '备份失败'};
  }
  try {
    localStorage.setItem(LAST_KEY, key);
  } catch (error) {
    /* ignore */
  }
  return Object.assign({ok: true}, result.data || {});
}

// 外观变了就排一次备份（默认 6 秒后），拖动滑块期间的中间状态不会各存一份
export function queueAutoBackup(settings, delay = 6000) {
  if (!settings) return;
  pending = settings;
  if (timer !== null) clearTimeout(timer);
  timer = setTimeout(() => {
    timer = null;
    const snapshot = pending;
    pending = null;
    if (running || !snapshot) return;
    running = true;
    backupNow(snapshot).catch(() => {}).finally(() => { running = false; });
  }, delay);
}

export function cancelAutoBackup() {
  if (timer !== null) clearTimeout(timer);
  timer = null;
  pending = null;
}

export async function listBackups() {
  const result = await api('GET', '/music/skin/backups');
  if (result.code !== 0) return {ok: false, items: [], error: result.message || '读取备份列表失败'};
  const data = result.data || {};
  return {ok: true, items: Array.isArray(data.items) ? data.items : [], dir: data.dir || '', keep: Number(data.keep) || 0};
}

export async function readBackup(name) {
  const response = await fetch(API_BASE + '/music/skin/backup?name=' + encodeURIComponent(name));
  if (!response.ok) throw new Error('读取备份失败（HTTP ' + response.status + '）');
  return new Uint8Array(await response.arrayBuffer());
}

export async function deleteBackup(name) {
  const result = await api('POST', '/music/skin/backups', {action: 'delete', name});
  return result.code === 0;
}

export async function openBackupFolder() {
  const list = await listBackups();
  if (!list.ok || !list.dir) return false;
  const result = await api('POST', '/music/open-folder', {path: list.dir});
  return result.code === 0;
}

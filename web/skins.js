// Rad Music - 皮肤库（主题栏里的「我的皮肤」）
//
// 导入或导出的 .rsk 会落进本机皮肤库，主题栏里以卡片形式列出，点一下即可整套切换。
// 每条皮肤 = 元数据（名称/主题/外观）+ 壁纸 + 缩略图：
//   * 壁纸是原图（压缩到长边 1600），用于切换时铺满全屏；
//   * 缩略图是 200px 的小图，只给主题卡片显示，别拿它当壁纸。
// 都放 localStorage（单条几百 KB，限制条数防止撑爆配额）。

const SKINS_KEY = 'radmusic.skins';
const CURRENT_KEY = 'radmusic.currentSkin';
export const MAX_SKINS = 8;

function readAll() {
  try {
    const raw = localStorage.getItem(SKINS_KEY);
    const list = raw ? JSON.parse(raw) : [];
    return Array.isArray(list) ? list.filter((item) => item && item.id) : [];
  } catch (error) {
    return [];
  }
}

function writeAll(list) {
  try {
    localStorage.setItem(SKINS_KEY, JSON.stringify(list));
    return true;
  } catch (error) {
    return false; // 配额满
  }
}

export function listSkins() {
  return readAll();
}

export function getSkin(id) {
  return readAll().find((item) => item.id === id) || null;
}

export function currentSkinId() {
  try {
    return localStorage.getItem(CURRENT_KEY) || '';
  } catch (error) {
    return '';
  }
}

export function setCurrentSkinId(id) {
  try {
    if (id) localStorage.setItem(CURRENT_KEY, id);
    else localStorage.removeItem(CURRENT_KEY);
  } catch (error) {
    /* ignore */
  }
}

export function removeSkin(id) {
  const next = readAll().filter((item) => item.id !== id);
  writeAll(next);
  if (currentSkinId() === id) setCurrentSkinId('');
  return next;
}

/**
 * 保存/更新一条皮肤。同名同 id 会覆盖。
 * @returns {{ok: boolean, id?: string, error?: string}}
 */
export function saveSkin(skin) {
  const list = readAll();
  const name = String(skin.name || '').trim() || '未命名皮肤';
  const existing = list.find((item) => item.id === skin.id) ||
    list.find((item) => item.name === name);
  const id = existing ? existing.id : 'skin_' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
  const entry = {
    id,
    name,
    author: String(skin.author || '').trim(),
    note: String(skin.note || '').trim(),
    theme: skin.theme || 'custom',
    color_scheme: skin.color_scheme || 'dark',
    appearance: skin.appearance || {},
    wallpaper: skin.wallpaper || '',
    thumb: skin.thumb || '',
    encrypted: !!skin.encrypted,
    created: existing ? existing.created : Date.now(),
    updated: Date.now()
  };
  const index = list.findIndex((item) => item.id === id);
  if (index >= 0) list[index] = entry;
  else list.unshift(entry);

  while (list.length > MAX_SKINS) list.pop();
  if (!writeAll(list)) {
    return {ok: false, error: '本机存储已满，先删掉一两个旧皮肤再试'};
  }
  return {ok: true, id};
}

/**
 * 改一条皮肤的元数据（改名 / 换署名 / 改备注），不动配色与壁纸。
 * 名称与别的皮肤重名时直接拒绝，避免 saveSkin 的「同名覆盖」把别人的皮肤吃掉。
 * @returns {{ok: boolean, error?: string}}
 */
export function updateSkinMeta(id, patch) {
  const list = readAll();
  const index = list.findIndex((item) => item.id === id);
  if (index < 0) return {ok: false, error: '这条皮肤已经不在皮肤栏里了'};
  const entry = list[index];
  const nextName = patch.name === undefined ? entry.name : String(patch.name || '').trim();
  if (!nextName) return {ok: false, error: '皮肤名不能为空'};
  const duplicated = list.some((item) => item.id !== id && item.name === nextName);
  if (duplicated) return {ok: false, error: '皮肤栏里已经有同名的皮肤了，换个名字'};
  entry.name = nextName;
  if (patch.author !== undefined) entry.author = String(patch.author || '').trim();
  if (patch.note !== undefined) entry.note = String(patch.note || '').trim();
  entry.updated = Date.now();
  if (!writeAll(list)) return {ok: false, error: '本机存储已满，写入失败'};
  return {ok: true};
}

/** 只更新某条皮肤的配色/壁纸（「用当前外观覆盖」）。 */
export function updateSkinContent(id, content) {
  const list = readAll();
  const index = list.findIndex((item) => item.id === id);
  if (index < 0) return {ok: false, error: '这条皮肤已经不在皮肤栏里了'};
  const entry = list[index];
  if (content.theme) entry.theme = content.theme;
  if (content.color_scheme) entry.color_scheme = content.color_scheme;
  if (content.appearance) entry.appearance = content.appearance;
  if (content.wallpaper !== undefined) entry.wallpaper = content.wallpaper;
  if (content.thumb !== undefined) entry.thumb = content.thumb;
  entry.updated = Date.now();
  if (!writeAll(list)) return {ok: false, error: '本机存储已满，写入失败'};
  return {ok: true};
}

// 皮肤 → .rsk 需要的字段（壁纸用库里存的压缩版，直接当 imageDataUrl 打包）
export function skinFilePayload(skin) {
  const ap = Object.assign({}, skin.appearance || {});
  const wallpaper = skin.wallpaper || '';
  if (wallpaper) {
    ap.bg_use_local = true;
    ap.bg_image = '';
  }
  return {
    name: skin.name,
    author: skin.author || '',
    note: skin.note || '',
    theme: skin.theme || 'custom',
    colorScheme: skin.color_scheme || 'dark',
    appearance: ap,
    imageDataUrl: wallpaper
  };
}

// ---------------------------------------------------------------- 图片处理
function loadImage(dataUrl) {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error('图片解码失败'));
    image.src = dataUrl;
  });
}

/**
 * 把 dataURL 图片压到长边 maxEdge，输出 jpeg dataURL。
 * 跨域图片（http 链接）会让 canvas 变“污染”，此时抛出，由调用方退回渐变缩略图。
 */
export async function shrinkDataUrl(dataUrl, maxEdge = 1600, quality = 0.82) {
  if (!dataUrl) return '';
  const image = await loadImage(dataUrl);
  const ratio = Math.min(1, maxEdge / Math.max(image.width, image.height));
  const width = Math.max(1, Math.round(image.width * ratio));
  const height = Math.max(1, Math.round(image.height * ratio));
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  ctx.drawImage(image, 0, 0, width, height);
  return canvas.toDataURL('image/jpeg', quality);
}

export async function makeThumb(dataUrl, maxEdge = 200, quality = 0.75) {
  if (!dataUrl) return '';
  try {
    return await shrinkDataUrl(dataUrl, maxEdge, quality);
  } catch (error) {
    return '';
  }
}

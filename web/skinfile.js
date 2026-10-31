// Rad Music - 皮肤包（.rsk）
//
// .rsk 就是一个 zip：里面 `skin.json`（主题 + 外观设置）+ 可选的壁纸图片文件。
// 浏览器没有内置 zip 读写，这里手写最小实现：
//   * 写：只用 store（不压缩）。壁纸本身是 jpg/png，已有压缩，再 deflate 意义不大。
//   * 读：支持 store 与 deflate（别人用系统/第三方工具重新打包也能读），deflate 走 DecompressionStream。
// 不引第三方库，保持前端零依赖。

const LOCAL_SIG = 0x04034b50;
const CENTRAL_SIG = 0x02014b50;
const EOCD_SIG = 0x06054b50;
const MIME_BY_EXT = {jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', webp: 'image/webp', gif: 'image/gif'};

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[i] = c >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc = (crc >>> 8) ^ CRC_TABLE[(crc ^ bytes[i]) & 0xff];
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function concatBytes(chunks) {
  let total = 0;
  for (const chunk of chunks) total += chunk.length;
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

function dosStamp(date) {
  const time = ((date.getHours() & 31) << 11) | ((date.getMinutes() & 63) << 5) | ((date.getSeconds() / 2) & 31);
  const day = (((date.getFullYear() - 1980) & 127) << 9) | (((date.getMonth() + 1) & 15) << 5) | (date.getDate() & 31);
  return {time: time & 0xffff, date: day & 0xffff};
}

/**
 * 打一个 store 模式的 zip。
 * @param {{name: string, data: Uint8Array}[]} entries
 * @returns {Uint8Array}
 */
export function buildZip(entries) {
  const encoder = new TextEncoder();
  const stamp = dosStamp(new Date());
  const locals = [];
  const centrals = [];
  let offset = 0;

  for (const entry of entries) {
    const nameBytes = encoder.encode(entry.name);
    const data = entry.data;
    const crc = crc32(data);

    const local = new Uint8Array(30 + nameBytes.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, LOCAL_SIG, true);
    lv.setUint16(4, 20, true);        // version needed
    lv.setUint16(6, 0, true);         // flags
    lv.setUint16(8, 0, true);         // method: store
    lv.setUint16(10, stamp.time, true);
    lv.setUint16(12, stamp.date, true);
    lv.setUint32(14, crc, true);
    lv.setUint32(18, data.length, true);
    lv.setUint32(22, data.length, true);
    lv.setUint16(26, nameBytes.length, true);
    lv.setUint16(28, 0, true);
    local.set(nameBytes, 30);

    const central = new Uint8Array(46 + nameBytes.length);
    const cv = new DataView(central.buffer);
    cv.setUint32(0, CENTRAL_SIG, true);
    cv.setUint16(4, 20, true);        // version made by
    cv.setUint16(6, 20, true);        // version needed
    cv.setUint16(8, 0, true);         // flags
    cv.setUint16(10, 0, true);        // method
    cv.setUint16(12, stamp.time, true);
    cv.setUint16(14, stamp.date, true);
    cv.setUint32(16, crc, true);
    cv.setUint32(20, data.length, true);
    cv.setUint32(24, data.length, true);
    cv.setUint16(28, nameBytes.length, true);
    cv.setUint16(30, 0, true);        // extra
    cv.setUint16(32, 0, true);        // comment
    cv.setUint16(34, 0, true);        // disk
    cv.setUint16(36, 0, true);        // internal attrs
    cv.setUint32(38, 0, true);        // external attrs
    cv.setUint32(42, offset, true);   // local header offset
    central.set(nameBytes, 46);

    locals.push(local, data);
    centrals.push(central);
    offset += local.length + data.length;
  }

  const centralBlob = concatBytes(centrals);
  const eocd = new Uint8Array(22);
  const ev = new DataView(eocd.buffer);
  ev.setUint32(0, EOCD_SIG, true);
  ev.setUint16(4, 0, true);
  ev.setUint16(6, 0, true);
  ev.setUint16(8, centrals.length, true);
  ev.setUint16(10, centrals.length, true);
  ev.setUint32(12, centralBlob.length, true);
  ev.setUint32(16, offset, true);
  ev.setUint16(20, 0, true);

  return concatBytes(locals.concat([centralBlob, eocd]));
}

async function inflateRaw(bytes) {
  if (typeof DecompressionStream === 'undefined') {
    throw new Error('当前环境不支持解压 zip，请用未压缩的皮肤包');
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/**
 * 读 zip（store / deflate）。
 * @param {Uint8Array} bytes
 * @returns {Promise<{name: string, data: Uint8Array}[]>}
 */
export async function readZip(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let eocd = -1;
  const min = Math.max(0, bytes.length - 65558);
  for (let i = bytes.length - 22; i >= min; i--) {
    if (view.getUint32(i, true) === EOCD_SIG) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error('不是有效的 .rsk 文件（找不到 zip 目录）');

  const count = view.getUint16(eocd + 10, true);
  let pointer = view.getUint32(eocd + 16, true);
  const entries = [];

  for (let i = 0; i < count; i++) {
    if (pointer + 46 > bytes.length || view.getUint32(pointer, true) !== CENTRAL_SIG) break;
    const method = view.getUint16(pointer + 10, true);
    const compSize = view.getUint32(pointer + 20, true);
    const nameLen = view.getUint16(pointer + 28, true);
    const extraLen = view.getUint16(pointer + 30, true);
    const commentLen = view.getUint16(pointer + 32, true);
    const localOffset = view.getUint32(pointer + 42, true);
    const name = new TextDecoder().decode(bytes.subarray(pointer + 46, pointer + 46 + nameLen));

    const localNameLen = view.getUint16(localOffset + 26, true);
    const localExtraLen = view.getUint16(localOffset + 28, true);
    const start = localOffset + 30 + localNameLen + localExtraLen;
    const raw = bytes.subarray(start, start + compSize);
    const data = method === 0 ? raw : await inflateRaw(raw);
    entries.push({name, data});
    pointer += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

// ---------------------------------------------------------------- data URL 互转
export function dataUrlToBytes(dataUrl) {
  const match = /^data:([^;,]+)(;base64)?,(.*)$/s.exec(String(dataUrl || ''));
  if (!match) return null;
  const mime = match[1] || 'application/octet-stream';
  if (!match[2]) return null;   // 只处理 base64 形式的 data URL
  const binary = atob(match[3]);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return {mime, bytes};
}

export function bytesToDataUrl(bytes, mime) {
  let binary = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
  }
  return 'data:' + (mime || 'application/octet-stream') + ';base64,' + btoa(binary);
}

function extFromMime(mime) {
  if (/png/i.test(mime)) return 'png';
  if (/webp/i.test(mime)) return 'webp';
  if (/gif/i.test(mime)) return 'gif';
  return 'jpg';
}

// ---------------------------------------------------------------- 皮肤包读写
export const SKIN_FORMAT = 'radmusic.skin';
export const SKIN_VERSION = 1;

/**
 * 生成 .rsk 的字节。
 * @param {{name: string, theme: string, colorScheme: string, appearance: object, imageDataUrl?: string}} skin
 * @returns {Uint8Array}
 */
export function buildSkinFile(skin) {
  const meta = {
    format: SKIN_FORMAT,
    version: SKIN_VERSION,
    name: skin.name || '未命名皮肤',
    created: new Date().toISOString(),
    app: 'Rad Music',
    theme: skin.theme || 'custom',
    color_scheme: skin.colorScheme || 'dark',
    appearance: skin.appearance || {}
  };
  const entries = [];
  let imageName = '';

  if (skin.imageDataUrl) {
    const parsed = dataUrlToBytes(skin.imageDataUrl);
    if (parsed) {
      imageName = 'wallpaper.' + extFromMime(parsed.mime);
      entries.push({name: imageName, data: parsed.bytes});
      meta.image = imageName;
    }
  }
  // skin.json 先算好（要带上 image 名）再放进 zip
  entries.unshift({name: 'skin.json', data: new TextEncoder().encode(JSON.stringify(meta, null, 2))});
  return buildZip(entries);
}

/**
 * 解析 .rsk。
 * @param {Uint8Array} bytes
 * @returns {Promise<{meta: object, imageDataUrl: string}>}
 */
export async function parseSkinFile(bytes) {
  const entries = await readZip(bytes);
  const metaEntry = entries.find((entry) => /(^|\/)skin\.json$/i.test(entry.name));
  if (!metaEntry) throw new Error('皮肤包里没有 skin.json');
  let meta;
  try {
    meta = JSON.parse(new TextDecoder().decode(metaEntry.data));
  } catch (error) {
    throw new Error('skin.json 解析失败');
  }
  if (!meta || typeof meta !== 'object' || !meta.appearance) {
    throw new Error('skin.json 内容不符合皮肤格式');
  }
  let imageDataUrl = '';
  if (meta.image) {
    const imageEntry = entries.find((entry) => entry.name === meta.image);
    if (imageEntry) {
      const ext = String(meta.image).split('.').pop().toLowerCase();
      imageDataUrl = bytesToDataUrl(imageEntry.data, MIME_BY_EXT[ext] || 'image/jpeg');
    }
  }
  return {meta, imageDataUrl};
}

// 触发浏览器下载（webview 里会走默认下载行为）
export function downloadBytes(bytes, filename) {
  const blob = new Blob([bytes], {type: 'application/octet-stream'});
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
}

export function safeFileName(name) {
  const clean = String(name || '').replace(/[\\/:*?"<>|]/g, '').trim();
  return clean || 'radmusic-skin';
}

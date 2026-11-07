// Rad Music - 皮肤包（.rsk）
//
// .rsk 就是一个 zip：
//   v1（旧）  skin.json（主题 + 外观设置）+ 可选壁纸 image.jpg/png/webp
//   v2（本版）skin.json 里多了「署名 author」「签名 signature」，可选整包加密：
//              skin.json（明文头：名字/署名/KDF 参数）+ payload.enc + wallpaper.enc
//              加密算法 AES-256-GCM，密钥 PBKDF2-HMAC-SHA256(password, salt, 150000)；
//              只有知道密码的人才能读出外观与壁纸，头里留名字是方便导入时先显示「谁的皮肤」。
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

// ---------------------------------------------------------------- SHA-256 / Base64
// 签名要用 SHA-256，但 WebCrypto 在非安全上下文（file:// 打开）里没有 crypto.subtle，
// 所以这里带一份纯 JS 实现：签名永远可用，加密才依赖 WebCrypto。
const SHA_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
]);

function rotr(value, bits) {
  return ((value >>> bits) | (value << (32 - bits))) >>> 0;
}

function sha256Bytes(data) {
  const length = data.length;
  const padded = new Uint8Array((((length + 9 + 63) >> 6) << 6));
  padded.set(data);
  padded[length] = 0x80;
  const pv = new DataView(padded.buffer);
  pv.setUint32(padded.length - 8, Math.floor(length / 536870912));
  pv.setUint32(padded.length - 4, (length << 3) >>> 0);

  let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
  let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
  const w = new Uint32Array(64);

  for (let offset = 0; offset < padded.length; offset += 64) {
    for (let i = 0; i < 16; i++) w[i] = pv.getUint32(offset + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, hh = h7;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + SHA_K[i] + w[i]) >>> 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0;
    h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0; h6 = (h6 + g) >>> 0; h7 = (h7 + hh) >>> 0;
  }

  const out = new Uint8Array(32);
  const ov = new DataView(out.buffer);
  [h0, h1, h2, h3, h4, h5, h6, h7].forEach((value, index) => ov.setUint32(index * 4, value));
  return out;
}

function bytesToHex(bytes) {
  let text = '';
  for (const byte of bytes) text += byte.toString(16).padStart(2, '0');
  return text;
}

export function sha256Hex(text) {
  return bytesToHex(sha256Bytes(new TextEncoder().encode(String(text))));
}

function bytesToBase64(bytes) {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

function base64ToBytes(text) {
  const binary = atob(String(text || ''));
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

// ---------------------------------------------------------------- 加密（AES-256-GCM）
const KDF_ITERATIONS = 150000;

function webCrypto() {
  const subtle = typeof crypto !== 'undefined' ? crypto.subtle : null;
  if (!subtle || !crypto.getRandomValues) {
    throw new Error('当前环境不支持加密（浏览器需以 http://127.0.0.1 或 https 打开），只能导出未加密的皮肤包');
  }
  return subtle;
}

async function deriveKey(password, salt, iterations) {
  const subtle = webCrypto();
  const base = await subtle.importKey('raw', new TextEncoder().encode(password), 'PBKDF2', false, ['deriveKey']);
  return subtle.deriveKey(
    {name: 'PBKDF2', salt, iterations, hash: 'SHA-256'},
    base,
    {name: 'AES-GCM', length: 256},
    false,
    ['encrypt', 'decrypt']
  );
}

async function aesEncrypt(key, plainBytes) {
  const subtle = webCrypto();
  const nonce = crypto.getRandomValues(new Uint8Array(12));
  const data = await subtle.encrypt({name: 'AES-GCM', iv: nonce}, key, plainBytes);
  return {nonce, data: new Uint8Array(data)};
}

async function aesDecrypt(key, nonce, cipherBytes) {
  const subtle = webCrypto();
  try {
    const plain = await subtle.decrypt({name: 'AES-GCM', iv: nonce}, key, cipherBytes);
    return new Uint8Array(plain);
  } catch (error) {
    throw new Error('密码错误，或皮肤包被改动过');
  }
}

// ---------------------------------------------------------------- 皮肤包读写
export const SKIN_FORMAT = 'radmusic.skin';
export const SKIN_VERSION = 2;
export const SKIN_SIG_PREFIX = 'rsk1-';

/**
 * 署名签名：把名字 / 署名 / 主题 / 外观指纹拼成规范串再 SHA-256。
 * 只防手改（导入时能发现「名字或外观被换过」），不防伪造——真正防别人看内容靠密码加密。
 * image 只取「有没有图」：加密包里图片叫 wallpaper.enc，明文包里叫 wallpaper.png，
 * 若把文件名算进去，同一个外观加密前后签名就不一致了。
 */
export function skinSignature({name, author, theme, colorScheme, appearance, image}) {
  const canonical = ['rsk1', String(name || ''), String(author || ''), String(theme || ''),
    String(colorScheme || ''), sha256Hex(JSON.stringify(appearance || {})), image ? 'image' : ''].join('\n');
  return SKIN_SIG_PREFIX + sha256Hex(canonical).slice(0, 32);
}

/**
 * 生成 .rsk 的字节。
 * @param {{name: string, author?: string, note?: string, theme: string, colorScheme: string,
 *          appearance: object, imageDataUrl?: string, password?: string}} skin
 * @returns {Promise<Uint8Array>}
 */
export async function buildSkinFile(skin) {
  const name = skin.name || '未命名皮肤';
  const author = String(skin.author || '').trim();
  const note = String(skin.note || '').trim();
  const theme = skin.theme || 'custom';
  const colorScheme = skin.colorScheme || 'dark';
  const appearance = skin.appearance || {};
  const password = String(skin.password || '');

  let imageName = '';
  let imageBytes = null;
  let imageMime = '';
  if (skin.imageDataUrl) {
    const parsed = dataUrlToBytes(skin.imageDataUrl);
    if (parsed) {
      imageMime = parsed.mime;
      imageName = 'wallpaper.' + extFromMime(parsed.mime);
      imageBytes = parsed.bytes;
    }
  }

  if (!password) {
    const signature = skinSignature({name, author, theme, colorScheme, appearance, image: imageName});
    const meta = {
      format: SKIN_FORMAT,
      version: SKIN_VERSION,
      name,
      author,
      note,
      created: new Date().toISOString(),
      app: 'Rad Music',
      theme,
      color_scheme: colorScheme,
      appearance,
      signature,
      encrypted: false
    };
    const entries = [];
    if (imageBytes) {
      meta.image = imageName;
      entries.push({name: imageName, data: imageBytes});
    }
    entries.unshift({name: 'skin.json', data: new TextEncoder().encode(JSON.stringify(meta, null, 2))});
    return buildZip(entries);
  }

  // 加密：明文头只放名字/署名 + KDF 参数，外观与壁纸各一份密文
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const key = await deriveKey(password, salt, KDF_ITERATIONS);
  const signature = skinSignature({name, author, theme, colorScheme, appearance, image: imageName});
  const payload = new TextEncoder().encode(JSON.stringify({theme, color_scheme: colorScheme, appearance, signature, note}));
  const sealedPayload = await aesEncrypt(key, payload);

  const meta = {
    format: SKIN_FORMAT,
    version: SKIN_VERSION,
    name,
    author,
    note,
    created: new Date().toISOString(),
    app: 'Rad Music',
    encrypted: true,
    cipher: 'aes-256-gcm',
    kdf: {name: 'pbkdf2-sha256', iterations: KDF_ITERATIONS, salt: bytesToBase64(salt)},
    payload: 'payload.enc',
    payload_nonce: bytesToBase64(sealedPayload.nonce),
    image: '',
    image_nonce: '',
    image_mime: ''
  };
  const entries = [
    {name: 'payload.enc', data: sealedPayload.data}
  ];
  if (imageBytes) {
    const sealedImage = await aesEncrypt(key, imageBytes);
    entries.push({name: 'wallpaper.enc', data: sealedImage.data});
    meta.image = 'wallpaper.enc';
    meta.image_nonce = bytesToBase64(sealedImage.nonce);
    meta.image_mime = imageMime;
  }
  entries.unshift({name: 'skin.json', data: new TextEncoder().encode(JSON.stringify(meta, null, 2))});
  return buildZip(entries);
}

/**
 * 解析 .rsk（支持 v1 明文 / v2 明文 / v2 加密）。
 * @param {Uint8Array} bytes
 * @param {{password?: string}} [options]
 * @returns {Promise<{meta: object, imageDataUrl: string, signatureOk: boolean|null, encrypted: boolean,
 *                    needPassword: boolean, author: string}>}
 */
export async function parseSkinFile(bytes, {password = ''} = {}) {
  const entries = await readZip(bytes);
  const metaEntry = entries.find((entry) => /(^|\/)skin\.json$/i.test(entry.name));
  if (!metaEntry) throw new Error('皮肤包里没有 skin.json');
  let meta;
  try {
    meta = JSON.parse(new TextDecoder().decode(metaEntry.data));
  } catch (error) {
    throw new Error('skin.json 解析失败');
  }
  if (!meta || typeof meta !== 'object') throw new Error('skin.json 内容不符合皮肤格式');

  const author = String(meta.author || '');
  // ---- 加密包：先拿到外观与壁纸，之后走同一条校验路径
  if (meta.encrypted) {
    if (!password) {
      return {meta, imageDataUrl: '', signatureOk: null, encrypted: true, needPassword: true, author};
    }
    const kdf = meta.kdf || {};
    const salt = base64ToBytes(kdf.salt);
    const iterations = Number(kdf.iterations) || KDF_ITERATIONS;
    if (!salt.length) throw new Error('加密皮肤包缺少 salt，文件可能已损坏');
    const key = await deriveKey(password, salt, iterations);
    const payloadEntry = entries.find((entry) => entry.name === (meta.payload || 'payload.enc'));
    if (!payloadEntry) throw new Error('加密皮肤包里没有外观数据（payload.enc）');
    const plain = await aesDecrypt(key, base64ToBytes(meta.payload_nonce), payloadEntry.data);
    let inner;
    try {
      inner = JSON.parse(new TextDecoder().decode(plain));
    } catch (error) {
      throw new Error('加密内容解析失败（密码不对？）');
    }
    meta = Object.assign({}, meta, {
      theme: inner.theme,
      color_scheme: inner.color_scheme,
      appearance: inner.appearance,
      signature: inner.signature,
      note: inner.note || meta.note || ''
    });
    let imageDataUrl = '';
    if (meta.image) {
      const imageEntry = entries.find((entry) => entry.name === meta.image);
      if (imageEntry) {
        const imageBytes = await aesDecrypt(key, base64ToBytes(meta.image_nonce), imageEntry.data);
        imageDataUrl = bytesToDataUrl(imageBytes, meta.image_mime || 'image/jpeg');
      }
    }
    const expected = skinSignature({
      name: meta.name, author: meta.author, theme: meta.theme,
      colorScheme: meta.color_scheme, appearance: meta.appearance, image: meta.image || ''
    });
    return {
      meta,
      imageDataUrl,
      signatureOk: meta.signature ? meta.signature === expected : false,
      encrypted: true,
      needPassword: false,
      author
    };
  }

  // ---- 明文包（v1 没有 appearance 时视为非法）
  if (!meta.appearance) throw new Error('skin.json 内容不符合皮肤格式');
  let imageDataUrl = '';
  if (meta.image) {
    const imageEntry = entries.find((entry) => entry.name === meta.image);
    if (imageEntry) {
      const ext = String(meta.image).split('.').pop().toLowerCase();
      imageDataUrl = bytesToDataUrl(imageEntry.data, MIME_BY_EXT[ext] || 'image/jpeg');
    }
  }
  let signatureOk = null;   // v1 皮肤包没有签名，不判断
  if (meta.signature) {
    const expected = skinSignature({
      name: meta.name, author: meta.author, theme: meta.theme,
      colorScheme: meta.color_scheme, appearance: meta.appearance, image: meta.image || ''
    });
    signatureOk = meta.signature === expected;
  }
  return {meta, imageDataUrl, signatureOk, encrypted: false, needPassword: false, author};
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

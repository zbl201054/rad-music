// Rad Music - 自定义外观引擎
//
// 职责：把「自定义外观」设置对象翻译成一组 CSS 变量 + 文档级 data-* 属性。
//  * 颜色：每个区域（侧边栏 / 顶栏 / 内容卡片 / 播放栏 / 背景）各自一份颜色与不透明度，
//          输出为 rgb(r g b / a) 形式，背景图开启时 opacility 会自动按系数降低 → 控件成为
//          「颜色遮罩」，让图片透出来。
//  * 形状：卡片 / 播放栏 / 侧边栏圆角、控件圆角（药丸 ↔ 方形）。
//  * 背景图：URL（可跨设备同步）或本地上传（压缩后存 localStorage，体积大不进 settings.json）。
//
// 所有变量都写在 <html> 的行内 style 上：行内样式优先级高于 styles.css 里的 :root 与
// html[data-*] 规则，因此切回预设主题时只要把变量恢复成空串即可，不会互相污染。

const LOCAL_BG_KEY = 'radmusic.bgImage';
const IMAGE_ALPHA_FACTOR = 0.72;   // 有背景图时各区域不透明度乘这个系数

export const DEFAULT_APPEARANCE = {
  enabled: false,
  // 背景
  bg_color: '#141317',
  bg_image: '',
  bg_use_local: false,
  bg_fit: 'cover',
  bg_blur: 0,
  bg_dim: 0.28,
  // 文字 / 强调色
  accent: '#9a7bff',
  // 留空 = 自动：跟随由背景亮度决定的明暗方案取色（避免浅底浅字）
  text: '',
  // 各区域
  sidebar_color: '#191820',
  sidebar_alpha: 0.95,
  sidebar_radius: 0,
  topbar_color: '#141317',
  topbar_alpha: 0.85,
  card_color: '#1e1c25',
  card_radius: 12,
  card_alpha: 0.95,
  bar_color: '#25232d',
  bar_radius: 28,
  bar_alpha: 0.96,
  // 形状
  btn_radius: 999,
  panel_blur: 16,
  // 炫彩：绕色环一周的速度档位（1 最慢 … 5 最快，见 RAINBOW_PERIODS）
  rainbow_speed: 3
};

// 外观预设（设置页「一键填充」用）
// 只保留两种取向：图片（控件变成颜色遮罩）/ 纯色（整块纯色，不用图片）
export const APPEARANCE_PRESETS = [
  {
    id: 'photo', name: '图片',
    patch: {
      bg_color: '#101014', bg_fit: 'cover', bg_blur: 2, bg_dim: 0.34,
      accent: '#8fd0ff', text: '#ffffff',
      sidebar_color: '#000000', sidebar_alpha: 0.5, sidebar_radius: 22,
      topbar_color: '#000000', topbar_alpha: 0.4,
      card_color: '#000000', card_radius: 18, card_alpha: 0.44,
      bar_color: '#000000', bar_radius: 28, bar_alpha: 0.52,
      btn_radius: 999, panel_blur: 22
    }
  },
  {
    id: 'solid', name: '纯色',
    patch: {
      bg_color: '#101014', bg_image: '', bg_use_local: false, bg_blur: 0, bg_dim: 0.2,
      accent: '#c6c9d4', text: '#eceaf2',
      sidebar_color: '#16161b', sidebar_alpha: 1, sidebar_radius: 0,
      topbar_color: '#101014', topbar_alpha: 1,
      card_color: '#1b1b21', card_radius: 8, card_alpha: 1,
      bar_color: '#1b1b21', bar_radius: 14, bar_alpha: 1,
      btn_radius: 10, panel_blur: 0
    }
  }
];

// ---------------------------------------------------------------- 颜色工具
function hexToRgb(hex) {
  let value = String(hex || '').trim().replace(/^#/, '');
  if (value.length === 3) value = value.split('').map((c) => c + c).join('');
  if (value.length === 8) value = value.slice(0, 6);
  if (!/^[0-9a-fA-F]{6}$/.test(value)) return null;
  return {
    r: parseInt(value.slice(0, 2), 16),
    g: parseInt(value.slice(2, 4), 16),
    b: parseInt(value.slice(4, 6), 16)
  };
}

function clamp(value, lo, hi) { return Math.min(hi, Math.max(lo, value)); }

export function maskColor(hex, alpha) {
  const rgb = hexToRgb(hex);
  if (!rgb) return '';
  const a = clamp(Number(alpha), 0, 1);
  return 'rgba(' + rgb.r + ', ' + rgb.g + ', ' + rgb.b + ', ' + (Math.round(a * 1000) / 1000) + ')';
}

/**
 * 把「颜色 + 不透明度」压平到底色上，得到一个不透明色。
 * 没有背景图时用它：遮罩的意义是压住图片，无图还留半透明的话，下层文字/封面会直接透上来
 * （设置页播放栏里那团白糊就是这么来的）。
 */
function flattenOver(hex, alpha, baseHex) {
  const c = hexToRgb(hex) || {r: 0, g: 0, b: 0};
  const b = hexToRgb(baseHex) || {r: 0, g: 0, b: 0};
  const a = clamp(Number(alpha), 0, 1);
  const mix = (x, y) => Math.round(x * a + y * (1 - a));
  return 'rgb(' + mix(c.r, b.r) + ', ' + mix(c.g, b.g) + ', ' + mix(c.b, b.b) + ')';
}

function rgbToHsl(hex) {
  const rgb = hexToRgb(hex);
  if (!rgb) return {h: 0, s: 0, l: 50};
  const r = rgb.r / 255, g = rgb.g / 255, b = rgb.b / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b);
  const l = (max + min) / 2;
  let h = 0, s = 0;
  if (max !== min) {
    const d = max - min;
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    if (max === r) h = ((g - b) / d + (g < b ? 6 : 0));
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
  }
  return {h, s: s * 100, l: l * 100};
}

function hslToHex(h, s, l) {
  const hue = ((h % 360) + 360) % 360 / 360;
  const sat = clamp(s, 0, 100) / 100;
  const light = clamp(l, 0, 100) / 100;
  const hue2rgb = (p, q, t) => {
    let tt = t;
    if (tt < 0) tt += 1;
    if (tt > 1) tt -= 1;
    if (tt < 1 / 6) return p + (q - p) * 6 * tt;
    if (tt < 1 / 2) return q;
    if (tt < 2 / 3) return p + (q - p) * (2 / 3 - tt) * 6;
    return p;
  };
  let r, g, b;
  if (sat === 0) {
    r = g = b = light;
  } else {
    const q = light < 0.5 ? light * (1 + sat) : light + sat - light * sat;
    const p = 2 * light - q;
    r = hue2rgb(p, q, hue + 1 / 3);
    g = hue2rgb(p, q, hue);
    b = hue2rgb(p, q, hue - 1 / 3);
  }
  const to = (v) => Math.round(clamp(v, 0, 1) * 255).toString(16).padStart(2, '0');
  return '#' + to(r) + to(g) + to(b);
}

// 由强调色派生出 MD3 的三个 seed（主色 / 次色 / 第三色），让预设色板跟着强调色走
function seedTriplet(accent) {
  const {h, s, l} = rgbToHsl(accent);
  return {
    seed: accent,
    seed2: hslToHex(h + 38, clamp(s * 0.94, 12, 96), clamp(l - 6, 18, 78)),
    seed3: hslToHex(h - 46, clamp(s * 0.82, 10, 92), clamp(l + 14, 30, 88))
  };
}

// 相对亮度（WCAG 公式），用来判断外观是深色还是浅色基调
function luminance(hex) {
  const rgb = hexToRgb(hex);
  if (!rgb) return 0;
  const channel = (v) => {
    const c = v / 255;
    return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(rgb.r) + 0.7152 * channel(rgb.g) + 0.0722 * channel(rgb.b);
}

/**
 * 自定义外观的明暗基调：由背景色（没有则用卡片色）的亮度决定。
 * 必须由外观自己决定，否则「深色遮罩 + 浅色方案」会让 MD3 的容器/描边仍是浅色，
 * 出现白底白字、满屏白线与白色占位块（v0.12 第一版就是这个毛病）。
 */
export function appearanceScheme(ap) {
  const bg = hexToRgb(ap.bg_color) ? ap.bg_color : (hexToRgb(ap.card_color) ? ap.card_color : '#141317');
  return luminance(bg) > 0.42 ? 'light' : 'dark';
}

// 文字色留「自动」时，用于推导描边色的近似文字色
const AUTO_INK = {dark: '#e7e3ee', light: '#1a1b21'};

// ---------------------------------------------------------------- 整体配色（一键派生）
/**
 * 由「一个主色 + 明暗」派生整套区域色。
 * 各区块只取主色的色相，饱和度压到很低的百分比，靠亮度分层：
 *   深色：bg 7% → 顶栏 8% → 侧边栏 10% → 卡片 13% → 播放栏 17%
 *   浅色：bg 96% → 顶栏 98% → 侧边栏 99% → 卡片/播放栏 100%
 * 这样一键出来就是协调的一套，不会出现某个区域跑调。
 * @param {string} seedColor 主色（#rrggbb）
 * @param {'dark'|'light'} scheme
 * @returns {object} 可直接 merge 进 appearance 的颜色字段
 */
export function derivePalette(seedColor, scheme) {
  const base = hexToRgb(seedColor) ? seedColor : DEFAULT_APPEARANCE.accent;
  const {h, s, l} = rgbToHsl(base);
  const sat = clamp(s, 0, 100);
  const dark = scheme !== 'light';
  const tint = (lightness, satScale = 0.3) => hslToHex(h, clamp(sat * satScale, 3, 46), lightness);

  // 强调色：深色底上太暗看不清、浅色底上太亮糊掉，都拉回可用区间
  const accent = dark
    ? hslToHex(h, clamp(sat, 8, 96), clamp(l < 45 ? 62 : l, 45, 80))
    : hslToHex(h, clamp(sat, 8, 96), clamp(l > 72 ? 46 : l, 32, 66));

  if (dark) {
    return {
      bg_color: tint(7),
      sidebar_color: tint(10, 0.34),
      topbar_color: tint(8, 0.28),
      card_color: tint(13, 0.34),
      bar_color: tint(17, 0.34),
      accent
    };
  }
  return {
    bg_color: tint(96, 0.22),
    sidebar_color: tint(99, 0.16),
    topbar_color: tint(98, 0.16),
    card_color: tint(100, 0.12),
    bar_color: tint(100, 0.18),
    accent
  };
}

// ---------------------------------------------------------------- 本地缓存
// 首屏先用缓存把配色画出来（服务器设置是异步到达的，否则会闪一下默认色），
// 服务器数据回来后以服务器为准并回写缓存。
const SKIN_CACHE_KEY = 'radmusic.skinCache';

export function writeSkinCache(settings) {
  if (!settings) return;
  try {
    localStorage.setItem(SKIN_CACHE_KEY, JSON.stringify({
      theme: settings.theme || '',
      color_scheme: settings.color_scheme || '',
      appearance: normalizeAppearance(settings.appearance),
      saved_at: Date.now()
    }));
  } catch (error) {
    // 配额满/隐私模式：缓存可有可无，不影响主流程
  }
}

export function readSkinCache() {
  try {
    const raw = localStorage.getItem(SKIN_CACHE_KEY);
    if (!raw) return null;
    const data = JSON.parse(raw);
    if (!data || typeof data !== 'object' || !data.appearance) return null;
    return {
      theme: data.theme || 'violet',
      color_scheme: data.color_scheme || 'dark',
      appearance: normalizeAppearance(data.appearance)
    };
  } catch (error) {
    return null;
  }
}

// ---------------------------------------------------------------- 炫彩（光谱循环）
// 不用 CSS animation 动画 @property：Chromium 在根元素上不保证每帧重算依赖它的 --seed，
// 且系统打开「减少动画」时 animation-duration 会被 `!important` 压成 .01ms，炫彩直接静止。
// 这里用定时器把 --rad-hue 写成内联变量，稳定、可暂停，也不受 media query 影响。
const RAINBOW_TICK = 100;          // 10fps：周期最短时每步 12°，肉眼仍连续
const RAINBOW_PERIODS = {1: 24000, 2: 14000, 3: 8000, 4: 5000, 5: 3000};   // 绕一圈的毫秒数

export const RAINBOW_SPEED_OPTIONS = [
  {value: 2, label: '慢'},
  {value: 3, label: '标准'},
  {value: 4, label: '快'},
  {value: 5, label: '极快'}
];

let rainbowTimer = null;
let rainbowHue = 0;
let rainbowSpeed = 0;

export function stopRainbow() {
  if (rainbowTimer) {
    clearInterval(rainbowTimer);
    rainbowTimer = null;
  }
  rainbowSpeed = 0;
  rainbowHue = 0;
  document.documentElement.style.removeProperty('--rad-hue');
}

export function startRainbow(speed) {
  const next = Math.min(5, Math.max(1, Math.round(Number(speed) || DEFAULT_APPEARANCE.rainbow_speed)));
  if (rainbowTimer && next === rainbowSpeed) return;
  // 换速度只重启定时器，不清色相，避免画面回跳到红色
  if (rainbowTimer) {
    clearInterval(rainbowTimer);
    rainbowTimer = null;
  }
  rainbowSpeed = next;
  const root = document.documentElement;
  const period = RAINBOW_PERIODS[next] || RAINBOW_PERIODS[3];
  const stepDeg = 360 * RAINBOW_TICK / period;
  const tick = () => {
    if (document.hidden) return;   // 后台标签页不烧 CPU
    rainbowHue = (rainbowHue + stepDeg) % 360;
    root.style.setProperty('--rad-hue', rainbowHue.toFixed(2));
  };
  tick();
  rainbowTimer = setInterval(tick, RAINBOW_TICK);
}

// ---------------------------------------------------------------- 本地背景图
export function readLocalBg() {
  try {
    return localStorage.getItem(LOCAL_BG_KEY) || '';
  } catch (error) {
    return '';
  }
}

export function writeLocalBg(dataUrl) {
  try {
    if (!dataUrl) localStorage.removeItem(LOCAL_BG_KEY);
    else localStorage.setItem(LOCAL_BG_KEY, dataUrl);
    return true;
  } catch (error) {
    return false;
  }
}

// 图片压缩：长边限制到 2400px，转 JPEG 质量 0.86。壁纸类图片 5MB → 通常 400KB 左右，
// 既能塞进 localStorage（多数环境 5MB 配额），也足够大屏显示。
export function shrinkImage(file, maxEdge = 2400, quality = 0.86) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error('读取图片失败'));
    reader.onload = () => {
      const image = new Image();
      image.onerror = () => reject(new Error('图片解码失败'));
      image.onload = () => {
        const ratio = Math.min(1, maxEdge / Math.max(image.width, image.height));
        const width = Math.max(1, Math.round(image.width * ratio));
        const height = Math.max(1, Math.round(image.height * ratio));
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(image, 0, 0, width, height);
        resolve(canvas.toDataURL('image/jpeg', quality));
      };
      image.src = String(reader.result || '');
    };
    reader.readAsDataURL(file);
  });
}

// ---------------------------------------------------------------- 归一化
export function normalizeAppearance(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  const out = Object.assign({}, DEFAULT_APPEARANCE);
  const colorKeys = ['bg_color', 'accent', 'sidebar_color', 'topbar_color', 'card_color', 'bar_color'];
  for (const key of colorKeys) {
    if (typeof source[key] === 'string' && hexToRgb(source[key])) out[key] = source[key];
  }
  // 文字色：空串 = 自动（跟随明暗方案）。v0.12 第一版把 #e7e3ec 当默认值写进了 settings，
  // 那是个只在深色底上成立的浅色，浅色方案下会变成白底白字 —— 这里统一归一成「自动」。
  const text = typeof source.text === 'string' ? source.text.trim() : '';
  out.text = (text === '#e7e3ec' || text === '#e7e3ee') ? '' : (hexToRgb(text) ? text : '');
  for (const key of ['bg_blur', 'bg_dim', 'sidebar_alpha', 'sidebar_radius', 'topbar_alpha',
    'card_alpha', 'card_radius', 'bar_alpha', 'bar_radius', 'btn_radius', 'panel_blur', 'rainbow_speed']) {
    const value = Number(source[key]);
    if (Number.isFinite(value)) out[key] = value;
  }
  out.bg_image = typeof source.bg_image === 'string' ? source.bg_image : '';
  out.bg_fit = ['cover', 'contain', 'fill', 'center'].indexOf(source.bg_fit) >= 0 ? source.bg_fit : 'cover';
  out.bg_use_local = !!source.bg_use_local;
  out.enabled = !!source.enabled;
  return out;
}

export function appearanceImage(ap) {
  if (!ap) return '';
  if (ap.bg_use_local) return readLocalBg();
  return ap.bg_image || '';
}

// ---------------------------------------------------------------- 应用
const MANAGED_VARS = [
  '--c-bg', '--c-sidebar', '--c-topbar', '--c-card', '--c-bar', '--c-blur',
  '--r-sidebar', '--r-card', '--r-bar', '--shape-full',
  '--seed', '--seed-2', '--seed-3',
  '--md-on-surface', '--md-on-surface-variant', '--md-outline-variant', '--text', '--text-dim'
];

function clearVars(root) {
  for (const name of MANAGED_VARS) root.style.removeProperty(name);
}

/**
 * 把外观设置写进 CSS 变量。
 * @param {object} settings 完整设置对象（读 theme / color_scheme / appearance）
 * @param {object} [override] 传入即可「只预览不保存」：直接用它作为 appearance
 */
export function applyAppearance(settings, override) {
  const root = document.documentElement;
  const ap = normalizeAppearance(override || (settings && settings.appearance));
  const theme = (settings && settings.theme) || 'violet';

  // 明暗基调：外观开启时由背景亮度决定（见 appearanceScheme 注释），关闭时回到用户选定的方案
  const scheme = ap.enabled
    ? appearanceScheme(ap)
    : ((settings && settings.color_scheme) === 'light' ? 'light' : 'dark');
  root.dataset.scheme = scheme;

  applyBackground(ap);
  if (!ap.enabled) {
    root.dataset.appearance = 'off';
    clearVars(root);
    return ap;
  }
  root.dataset.appearance = 'on';

  // 有背景图时降低各区域不透明度 → 控件变成「颜色遮罩」
  const image = appearanceImage(ap);
  const factor = image ? IMAGE_ALPHA_FACTOR : 1;

  const {seed, seed2, seed3} = seedTriplet(ap.accent);
  if (theme === 'rainbow') {
    // 炫彩皮肤：色相由 CSS 动画驱动，用户选的强调色此时不生效（避免相互覆盖）
    root.style.removeProperty('--seed');
    root.style.removeProperty('--seed-2');
    root.style.removeProperty('--seed-3');
  } else {
    root.style.setProperty('--seed', seed);
    root.style.setProperty('--seed-2', seed2);
    root.style.setProperty('--seed-3', seed3);
  }

  // 有背景图时是「半透明遮罩 + 磨砂」，没有图时压平到底色（避免下层内容透上来）
  const surface = (hex, alpha) => (image
    ? maskColor(hex, alpha * factor)
    : flattenOver(hex, alpha, ap.bg_color));

  root.style.setProperty('--c-bg', ap.bg_color);
  root.style.setProperty('--c-sidebar', surface(ap.sidebar_color, ap.sidebar_alpha));
  root.style.setProperty('--c-topbar', surface(ap.topbar_color, ap.topbar_alpha));
  root.style.setProperty('--c-card', surface(ap.card_color, ap.card_alpha));
  root.style.setProperty('--c-bar', surface(ap.bar_color, ap.bar_alpha));
  root.style.setProperty('--c-blur', (image ? Math.max(0, ap.panel_blur) : 0) + 'px');

  root.style.setProperty('--r-sidebar', Math.max(0, ap.sidebar_radius) + 'px');
  root.style.setProperty('--r-card', Math.max(0, ap.card_radius) + 'px');
  root.style.setProperty('--r-bar', Math.max(0, ap.bar_radius) + 'px');
  root.style.setProperty('--shape-full', Math.max(0, ap.btn_radius) + 'px');

  // 文字色：留「自动」时不覆盖，交给明暗方案（深底浅字 / 浅底深字），不会出现浅底浅字
  const text = ap.text || '';
  if (text) {
    root.style.setProperty('--md-on-surface', text);
    root.style.setProperty('--md-on-surface-variant', 'color-mix(in srgb, ' + text + ' 74%, transparent)');
    root.style.setProperty('--text', text);
    root.style.setProperty('--text-dim', 'color-mix(in srgb, ' + text + ' 74%, transparent)');
  } else {
    root.style.removeProperty('--md-on-surface');
    root.style.removeProperty('--md-on-surface-variant');
    root.style.removeProperty('--text');
    root.style.removeProperty('--text-dim');
  }
  // 描边跟着文字色走：否则「深色遮罩 + 浅色方案」时 outline-variant 仍是浅色，满屏白线
  const ink = text || AUTO_INK[scheme] || AUTO_INK.dark;
  root.style.setProperty('--md-outline-variant', 'color-mix(in srgb, ' + ink + ' 16%, transparent)');
  return ap;
}

function applyBackground(ap) {
  const root = document.documentElement;
  const box = document.getElementById('app-bg');
  const layer = document.getElementById('app-bg-img');
  const dim = document.getElementById('app-bg-dim');
  const url = ap.enabled ? appearanceImage(ap) : '';
  if (!box || !layer || !dim) {
    root.dataset.bgimg = url ? '1' : '0';
    return;
  }
  if (!url) {
    box.hidden = true;
    layer.style.backgroundImage = 'none';
    root.dataset.bgimg = '0';
    return;
  }
  box.hidden = false;
  layer.style.backgroundImage = 'url("' + String(url).replace(/"/g, '%22').replace(/\n/g, '') + '")';
  layer.style.backgroundSize = ap.bg_fit;
  layer.style.backgroundPosition = 'center';
  layer.style.backgroundRepeat = 'no-repeat';
  layer.style.filter = ap.bg_blur > 0 ? 'blur(' + ap.bg_blur + 'px) saturate(1.05)' : 'none';
  dim.style.background = 'rgba(0, 0, 0, ' + clamp(Number(ap.bg_dim) || 0, 0, 0.9) + ')';
  root.dataset.bgimg = '1';
}

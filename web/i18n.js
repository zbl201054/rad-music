// Rad Music - 多语言（i18n）
//
// 设计要点（为什么不是「给每处文案加 key」）：
//   这个前端是零框架 + innerHTML 拼字符串，界面文案散在 16000 行代码里、还大量是
//   模板拼接出来的。逐个改成 t('key') 既不现实，也会让后续新代码很容易漏。
//   所以这里用「中文原文当 key」的两层结构：
//     1) DOM 翻译层：监听 DOM 变化，把文本节点与 title / placeholder 等属性的中文
//        换成当前语言；切语言就是把原文重新过一遍，代码里不用写任何翻译调用。
//     2) t() 函数：给新代码用（后端消息、非 DOM 场景）。
//   语言包是 .rlan（JSON）：{ 中文原文: 译文 }，并可声明 inherit（继承另一个包）
//   与 rules（逐条替换规则，「萌猫语」这类整体改造语言用规则比逐个词条省事）。
//
// 兜底原则：任何一条查不到译文，就原样保留中文——绝不显示 key 或空白。

import { API_BASE, api, setTextTranslator } from './api.js';

// 内置语言包：静态文件，随 web/ 一起发布。服务端 <root>/langs/ 里的同名包会覆盖它
// （方便不发版就更新官方语言包）。
export const BUILTIN_PACKS = [
  {id: 'zh-CN', file: './langs/zh-CN.rlan'},
  {id: 'zh-TW', file: './langs/zh-TW.rlan'},
  {id: 'en-US', file: './langs/en-US.rlan'},
  {id: 'en-GB', file: './langs/en-GB.rlan'},
  {id: 'zh-Moe', file: './langs/zh-Moe.rlan'}
];

const LANG_KEY = 'radmusic.lang';           // 当前语言（同步读，避免首屏闪一下中文）
const CACHE_PREFIX = 'radmusic.i18n.';      // 已解析词表的本地缓存
const DOC_LANG = {'zh-CN': 'zh-CN', 'zh-TW': 'zh-TW', 'zh-Moe': 'zh-CN', 'en-US': 'en-US', 'en-GB': 'en-GB'};

// 这些容器里的文字属于「用户内容」（歌名、歌词、文件名、路径、原始日志），不是界面文案，
// 翻译它们会把歌名弄坏。整棵子树跳过。
// 注意不要把 .track-sub 也列进来：它除了歌手名还夹着「已合并 2 条」「无损」这类界面标签，
// 而歌手名基本不会和词条撞上（词条都是按钮/提示文案），所以照常翻译更划算。
const SKIP_SELECTOR = [
  '[data-i18n-skip]', 'script', 'style', 'noscript', 'textarea', 'code', 'pre',
  '.lyrics', '.pp-lyrics', '.lyric-line', '.tr', '.pb-title', '.pb-artist',
  '.track-name',
  '#boot-error-text', '.log-line', '.raw-json'
].join(',');

const ATTRS = ['title', 'placeholder', 'aria-label', 'alt'];

// 中文句读（含全角分号/感叹/问号）：长提示拆句用，保留标点在句尾
const SENTENCE = /[。！？]/;
const SENTENCE_SPLIT = /(?<=[。！？])/;

// ---------------------------------------------------------------- 状态
const packCache = new Map();      // id -> 原始 .rlan 对象
const metaCache = new Map();      // id -> 列表用的元信息
let dictionary = emptyDictionary();
let activeId = 'zh-CN';
let enabled = false;              // 当前语言是不是「非简体中文」——否则不碰 DOM
let observer = null;
const listeners = new Set();

const textState = new WeakMap();  // Text -> {src, out}
const attrState = new WeakMap();  // Element -> {attr: {src, out}}
let applying = false;             // 正在整树重刷（期间忽略自身改动）

function emptyDictionary() {
  return {exact: new Map(), patterns: [], rules: [], rulesFallback: false, id: 'zh-CN', name: '中文（简体）'};
}

const isPlainObject = (value) => !!value && typeof value === 'object' && !Array.isArray(value);
const escapeRegExp = (text) => String(text).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

function storageGet(key) {
  try { return localStorage.getItem(key); } catch (error) { return null; }
}
function storageSet(key, value) {
  try { localStorage.setItem(key, value); } catch (error) { /* 隐私模式/配额满：降级为不缓存 */ }
}
function storageDel(key) {
  try { localStorage.removeItem(key); } catch (error) { /* 忽略 */ }
}

// ---------------------------------------------------------------- 语言包格式
// 只保留合法字段，导入来的包可能缺字段、类型也不对，全部在这里收敛。
export function normalizePack(raw) {
  if (!isPlainObject(raw)) return null;
  const id = String(raw.id || '').trim();
  if (!/^[A-Za-z0-9_-]{2,32}$/.test(id)) return null;
  const pack = {
    format: String(raw.format || 'rlan'),
    format_version: Number(raw.format_version) || 1,
    id,
    name: String(raw.name || id).slice(0, 80),
    native_name: String(raw.native_name || raw.name || id).slice(0, 80),
    locale: String(raw.locale || id).slice(0, 32),
    author: String(raw.author || '').slice(0, 80),
    version: String(raw.version || '1.0.0').slice(0, 32),
    description: String(raw.description || '').slice(0, 300),
    direction: raw.direction === 'rtl' ? 'rtl' : 'ltr',
    inherit: typeof raw.inherit === 'string' ? raw.inherit.trim() : '',
    strings: {}, patterns: [], rules: [], rules_fallback: !!raw.rules_fallback
  };
  if (isPlainObject(raw.strings)) {
    for (const [key, value] of Object.entries(raw.strings)) {
      if (typeof value !== 'string') continue;
      if (!key || key.length > 600 || value.length > 900) continue;
      pack.strings[key] = value;
    }
  }
  if (Array.isArray(raw.patterns)) {
    for (const entry of raw.patterns) {
      if (!Array.isArray(entry) || entry.length < 2) continue;
      const from = String(entry[0] || '');
      const to = String(entry[1] || '');
      if (!from || !to || from.length > 600 || to.length > 900) continue;
      pack.patterns.push([from, to]);
    }
  }
  if (Array.isArray(raw.rules)) {
    for (const entry of raw.rules) {
      if (!Array.isArray(entry) || entry.length < 2) continue;
      const from = String(entry[0] || '');
      if (!from || from.length > 40) continue;
      pack.rules.push([from, String(entry[1] == null ? '' : entry[1])]);
    }
  }
  return pack;
}

// 「{0}」这种占位符编译成捕获组：来源里第 n 个 {n} 对应译文里的 {n}
function compilePattern(from, to) {
  const pieces = String(from).split(/(\{\d+\})/);
  let source = '^';
  let groups = 0;
  for (const piece of pieces) {
    if (/^\{\d+\}$/.test(piece)) {
      source += '(.+?)';
      groups += 1;
    } else if (piece) {
      source += escapeRegExp(piece);
    }
  }
  if (!groups) return null;
  source += '$';
  try {
    return {re: new RegExp(source), to};
  } catch (error) {
    return null;
  }
}

function buildDictionary(pack) {
  const dict = emptyDictionary();
  dict.id = pack.id;
  dict.name = pack.name || pack.id;
  for (const [key, value] of Object.entries(pack.strings)) dict.exact.set(key, value);
  for (const [from, to] of pack.patterns) {
    const compiled = compilePattern(from, to);
    if (compiled) dict.patterns.push(compiled);
  }
  dict.rules = pack.rules.map(([from, to]) => [from, to])
    // 长规则优先：单遍扫描时先试「正在播放」再试「正在」，避免整词被拆成两段
    .sort((a, b) => b[0].length - a[0].length);
  dict.rulesFallback = !!pack.rules_fallback;
  return dict;
}

// 继承链：en-GB 继承 en-US 再覆盖少量拼写差异，避免整份词表复制两遍。
// 环状依赖直接截断（导入来的包什么都可能写）。
function resolveDictionary(id, seen = new Set()) {
  const pack = packCache.get(id);
  if (!pack || seen.has(id)) return emptyDictionary();
  seen.add(id);
  const base = pack.inherit ? resolveDictionary(pack.inherit, seen) : emptyDictionary();
  const own = buildDictionary(pack);
  const merged = emptyDictionary();
  merged.id = own.id;
  merged.name = own.name;
  merged.exact = new Map(base.exact);
  for (const [key, value] of own.exact) merged.exact.set(key, value);
  merged.patterns = base.patterns.concat(own.patterns);
  merged.rules = base.rules.concat(own.rules);
  merged.rulesFallback = own.rulesFallback || base.rulesFallback;
  return merged;
}

// ---------------------------------------------------------------- 翻译
// 顺序：精确词条 → 占位符模式 → 替换规则（仅在包声明 rules_fallback 时）→ 逐句兜底。
// 规则类语言（萌猫语）没有逐条词条也能整段变味，这是它比词表省事的关键。
export function translate(text) {
  if (!enabled || !text) return text;
  const hit = lookup(text);
  if (hit != null) return hit;
  if (dictionary.rulesFallback && dictionary.rules.length) {
    return applyRules(text, dictionary.rules);
  }
  // 长段落兜底：设置页的长提示是「一句话一句话」拼起来的，词表按句收录。
  // 整段匹配不上（中间夹了数字、路径之类）时，拆成句子逐句翻，能翻的都翻掉，
  // 翻不到的句子保持原文——比整段放弃要好得多。
  const bySentence = sentenceFallback(text);
  return bySentence != null ? bySentence : text;
}

// 先按句号/问号/感叹号切句；某句查不到时再按分号切一次（长提示常是「半句；半句」）
function sentenceFallback(text) {
  if (text.length < 10 || !SENTENCE.test(text)) return null;
  const parts = text.split(SENTENCE_SPLIT);
  if (parts.length < 2) return null;
  let changed = false;
  const out = parts.map((part) => {
    const translated = translateClause(part);
    if (translated !== part) changed = true;
    return translated;
  });
  return changed ? joinSentences(out) : null;
}

// 中文句子之间不需要空格，英文需要：译完的那一句以 . ! ? 结尾时补一个空格
function joinSentences(parts) {
  return parts.map((part, index) => {
    const next = parts[index + 1];
    if (!next || !next.trim()) return part;
    return /[.!?:;]$/.test(part.trimEnd()) ? part.trimEnd() + ' ' : part;
  }).join('');
}

function translateClause(clause) {
  const body = clause.trim();
  if (!body) return clause;
  const direct = lookup(body);
  if (direct != null) return clause.replace(body, direct);
  if (body.indexOf('；') >= 0) {
    const pieces = body.split(/(?<=；)/);
    if (pieces.length > 1) {
      let changed = false;
      const mapped = pieces.map((piece) => {
        const translated = translateClause(piece);
        if (translated !== piece) changed = true;
        return translated;
      });
      if (changed) return clause.replace(body, joinSentences(mapped));
    }
  }
  return clause;
}

// 精确词条 → 占位符模式；查不到返回 null（区别于「译文恰好等于原文」）
function lookup(text) {
  const exact = dictionary.exact.get(text);
  if (exact != null) return exact;
  // 句首「· 」是排版用的分隔符，词表里按正文收录，去掉再查一次
  if (text.startsWith('· ')) {
    const rest = dictionary.exact.get(text.slice(2));
    if (rest != null) return '· ' + rest;
  }
  for (const pattern of dictionary.patterns) {
    const match = pattern.re.exec(text);
    if (!match) continue;
    return pattern.to.replace(/\{(\^?)(\d+)\}/g, (whole, translateCapture, index) => {
      const value = match[Number(index) + 1];
      if (value == null) return whole;
      // {^0} = 捕获到的部分本身也是界面词（如「当前：视频」里的「视频」），一并翻掉；
      // 不带 ^ 的一律原样保留，避免把歌名、关键词这类用户内容翻坏
      if (translateCapture === '^') {
        const inner = lookup(value.trim());
        return inner != null ? value.replace(value.trim(), inner) : value;
      }
      return value;
    });
  }
  return null;
}

// 替换规则一遍过：从左到右扫描，命中就整段替换并跳过已消费的字符，
// 绝不再回头处理自己产出的文字。否则「正在」→「正在喵」之后再套「播放」→「播放喵」，
// 同一句话会被叠成两个喵。长规则优先，所以「正在播放」这种整词不会被拆开。
function applyRules(text, rules) {
  let out = '';
  let i = 0;
  while (i < text.length) {
    let hit = null;
    for (const rule of rules) {
      const from = rule[0];
      if (from && text.startsWith(from, i)) { hit = rule; break; }
    }
    if (hit) {
      out += hit[1];
      i += hit[0].length;
    } else {
      out += text[i];
      i += 1;
    }
  }
  return out;
}

// 给代码用的翻译入口：t('已复制') / t('共 {0} 首', 12)
// 先把参数填进中文原文（凑成运行时真正会出现的句子），再交给词表——
// 这样同一套占位符模式既能翻 DOM 文本，也能翻代码里拼出来的消息。
export function t(text, ...args) {
  let source = String(text);
  if (args.length) {
    source = source.replace(/\{(\d+)\}/g, (whole, index) =>
      (args[Number(index)] != null ? String(args[Number(index)]) : whole));
  }
  return translate(source);
}

// ---------------------------------------------------------------- DOM 翻译层
function skipped(node) {
  let element = node && node.nodeType === 1 ? node : node && node.parentElement;
  return !!(element && element.closest && element.closest(SKIP_SELECTOR));
}

function localizeText(node) {
  if (!enabled || !node || node.nodeType !== 3) return;
  if (skipped(node)) return;
  const current = node.nodeValue || '';
  const state = textState.get(node);
  // 当前值等于上次我们写进去的译文 → 这次变化是我们自己造成的，跳过（防死循环）
  const source = state && current === state.out ? state.src : current;
  const match = /^(\s*)([\s\S]*?)(\s*)$/.exec(source);
  if (!match) return;
  const [, lead, body, tail] = match;
  if (!body) return;
  const translated = translate(body);
  const out = lead + translated + tail;
  textState.set(node, {src: source, out});
  if (node.nodeValue !== out) node.nodeValue = out;
}

function localizeAttrs(element) {
  if (!enabled || !element || element.nodeType !== 1) return;
  if (skipped(element)) return;
  for (const name of ATTRS) {
    const current = element.getAttribute(name);
    if (!current) continue;
    const store = attrState.get(element) || {};
    const state = store[name];
    const source = state && current === state.out ? state.src : current;
    const translated = translate(source);
    store[name] = {src: source, out: translated};
    attrState.set(element, store);
    if (translated !== current) element.setAttribute(name, translated);
  }
}

export function localizeTree(root) {
  if (!enabled || !root) return;
  if (root.nodeType === 3) { localizeText(root); return; }
  if (root.nodeType !== 1 && root.nodeType !== 9 && root.nodeType !== 11) return;
  if (root.nodeType === 1) localizeAttrs(root);
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT, null);
  let node = walker.nextNode();
  while (node) {
    if (node.nodeType === 3) localizeText(node);
    else if (node.nodeType === 1) {
      if (node.closest && node.closest(SKIP_SELECTOR)) {
        // 跳过整棵子树，别在用户内容里翻出歌名
        node = skipSubtree(walker, node);
        continue;
      }
      localizeAttrs(node);
    }
    node = walker.nextNode();
  }
}

function skipSubtree(walker, element) {
  let node = walker.nextNode();
  while (node && element.contains(node)) node = walker.nextNode();
  return node;
}

// 整棵树按「原文 → 当前语言」重刷一遍。切语言时用；不需要记住哪些节点翻译过，
// 因为每个节点都记着自己的原文（textState）。
export function relocalizeDocument() {
  if (applying) return;
  applying = true;
  try {
    if (!enabled) {
      restoreDocument();
      return;
    }
    localizeTree(document.body);
  } finally {
    applying = false;
  }
}

// 关闭翻译（切回简体中文）：把节点写回原文，避免中英混排
function restoreDocument() {
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT, null);
  let node = walker.nextNode();
  while (node) {
    if (node.nodeType === 3) {
      const state = textState.get(node);
      if (state && node.nodeValue === state.out && state.src !== state.out) node.nodeValue = state.src;
    } else if (node.nodeType === 1) {
      const store = attrState.get(node);
      if (store) {
        for (const name of ATTRS) {
          const state = store[name];
          if (!state) continue;
          if (node.getAttribute(name) === state.out && state.src !== state.out) node.setAttribute(name, state.src);
        }
      }
    }
    node = walker.nextNode();
  }
}

function startObserver() {
  if (observer) return;
  observer = new MutationObserver((records) => {
    if (applying || !enabled) return;
    for (const record of records) {
      if (record.type === 'characterData') localizeText(record.target);
      else if (record.type === 'attributes') localizeAttrs(record.target);
      else {
        for (const node of record.addedNodes) {
          if (node.nodeType === 3) localizeText(node);
          else if (node.nodeType === 1) localizeTree(node);
        }
      }
    }
  });
  observer.observe(document.documentElement, {
    childList: true, subtree: true, characterData: true,
    attributes: true, attributeFilter: ATTRS
  });
}

function applyDocumentLang() {
  const pack = packCache.get(activeId) || {id: activeId};
  const lang = DOC_LANG[pack.id] || pack.locale || pack.id;
  document.documentElement.setAttribute('lang', lang);
  document.documentElement.setAttribute('dir', pack.direction === 'rtl' ? 'rtl' : 'ltr');
}

// ---------------------------------------------------------------- 语言包加载
async function fetchBuiltin(id) {
  const entry = BUILTIN_PACKS.find((item) => item.id === id);
  if (!entry) return null;
  try {
    const response = await fetch(new URL(entry.file, location.href).toString(), {cache: 'no-cache'});
    if (!response.ok) return null;
    return normalizePack(await response.json());
  } catch (error) {
    return null;
  }
}

// 服务端导入的语言包优先于内置同名包
async function fetchServerPack(id) {
  try {
    const result = await api('GET', '/music/langs/content?id=' + encodeURIComponent(id));
    if (!result || result.code !== 0 || !result.data || !result.data.pack) return null;
    return normalizePack(result.data.pack);
  } catch (error) {
    return null;
  }
}

export async function ensurePack(id) {
  if (!id) return null;
  if (packCache.has(id)) return packCache.get(id);
  let pack = await fetchServerPack(id);
  if (!pack) pack = await fetchBuiltin(id);
  if (!pack) return null;
  packCache.set(pack.id, pack);
  metaCache.set(pack.id, packMeta(pack));
  return pack;
}

// 继承链上的包都要先就位（en-GB 依赖 en-US）
async function ensureChain(id, seen = new Set()) {
  if (!id || seen.has(id)) return;
  seen.add(id);
  const pack = await ensurePack(id);
  if (pack && pack.inherit) await ensureChain(pack.inherit, seen);
}

function packMeta(pack) {
  return {
    id: pack.id,
    name: pack.name,
    native_name: pack.native_name,
    author: pack.author,
    version: pack.version,
    description: pack.description,
    inherit: pack.inherit,
    builtin: BUILTIN_PACKS.some((item) => item.id === pack.id),
    count: Object.keys(pack.strings).length + pack.patterns.length
  };
}

// 已解析词表的本地缓存：首屏要同步拿到词表，否则刷新时会先闪一帧中文
function cacheDictionary(id, dict) {
  const payload = {
    v: 1,
    id: dict.id,
    name: dict.name,
    exact: Array.from(dict.exact.entries()),
    patterns: dict.patterns.map((item) => [item.re.source, item.to]),
    rules: dict.rules,
    rulesFallback: dict.rulesFallback
  };
  storageSet(CACHE_PREFIX + id, JSON.stringify(payload));
}

function hydrateFromCache(id) {
  const raw = storageGet(CACHE_PREFIX + id);
  if (!raw) return false;
  try {
    const payload = JSON.parse(raw);
    if (!payload || payload.v !== 1 || payload.id !== id) return false;
    const dict = emptyDictionary();
    dict.id = payload.id;
    dict.name = payload.name || id;
    dict.exact = new Map(payload.exact || []);
    dict.patterns = (payload.patterns || []).map(([source, to]) => {
      try { return {re: new RegExp(source), to}; } catch (error) { return null; }
    }).filter(Boolean);
    dict.rules = payload.rules || [];
    dict.rulesFallback = !!payload.rulesFallback;
    dictionary = dict;
    return true;
  } catch (error) {
    return false;
  }
}

// api.js 里那些「拼出来再渲染」的文案（音质标签、声道、错误码）走这个钩子翻译
setTextTranslator(translate);

function setEnabled(next) {
  enabled = !!next;
  if (enabled) startObserver();
  else if (observer) { observer.disconnect(); observer = null; }
}

// ---------------------------------------------------------------- 对外 API
export function currentLanguage() { return activeId; }
export function isTranslated() { return enabled; }
export function languageName(id) {
  const meta = metaCache.get(id);
  return meta ? meta.name : id;
}

export function onLanguageChange(callback) {
  listeners.add(callback);
  return () => listeners.delete(callback);
}

// 内置包清单 + 服务端已导入包（服务端同名覆盖内置）
export async function listLanguages() {
  const items = new Map();
  for (const entry of BUILTIN_PACKS) {
    const pack = packCache.get(entry.id);
    if (pack) items.set(entry.id, packMeta(pack));
    else items.set(entry.id, {id: entry.id, name: entry.id, native_name: entry.id, builtin: true, count: 0});
  }
  const result = await api('GET', '/music/langs');
  if (result && result.code === 0 && result.data && Array.isArray(result.data.items)) {
    for (const item of result.data.items) {
      if (!item || !item.id) continue;
      items.set(item.id, Object.assign({builtin: false}, item));
    }
  }
  // 内置包名以实际包内容为准（列表接口拿不到内置包的语言名）
  await Promise.all(BUILTIN_PACKS.map((entry) => ensurePack(entry.id).catch(() => null)));
  for (const entry of BUILTIN_PACKS) {
    const pack = packCache.get(entry.id);
    if (!pack) continue;
    const existing = items.get(entry.id);
    if (existing && existing.builtin) items.set(entry.id, Object.assign(packMeta(pack), {builtin: true}));
  }
  const order = BUILTIN_PACKS.map((entry) => entry.id);
  return Array.from(items.values()).sort((a, b) => {
    const ia = order.indexOf(a.id);
    const ib = order.indexOf(b.id);
    if (ia >= 0 || ib >= 0) return (ia < 0 ? 999 : ia) - (ib < 0 ? 999 : ib);
    return String(a.name).localeCompare(String(b.name));
  });
}

// 启动时调用：先用本地缓存把语言装上（同步、不闪），再异步取最新包
export async function initI18n() {
  const saved = storageGet(LANG_KEY) || 'zh-CN';
  activeId = saved;
  if (saved !== 'zh-CN') {
    // 缓存里的词表是「已解析」的，能立刻生效
    if (hydrateFromCache(saved)) {
      setEnabled(true);
      applyDocumentLang();
    } else {
      await ensureChain(saved);
      if (packCache.has(saved)) {
        dictionary = resolveDictionary(saved);
        cacheDictionary(saved, dictionary);
        setEnabled(true);
        applyDocumentLang();
      } else {
        activeId = 'zh-CN';
        storageSet(LANG_KEY, activeId);
      }
    }
    relocalizeDocument();
  }
  // 后台把包更新到最新版本（离线/服务端没起来时忽略）
  ensureChain(activeId).then(() => {
    if (activeId === 'zh-CN' || !packCache.has(activeId)) return;
    const fresh = resolveDictionary(activeId);
    cacheDictionary(activeId, fresh);
    if (fresh.exact.size !== dictionary.exact.size) {
      dictionary = fresh;
      relocalizeDocument();
    }
  }).catch(() => {});
  return activeId;
}

// 切换语言：落 localStorage（首屏同步）+ 服务端设置（跨设备），并立刻重刷界面
export async function setLanguage(id, {persist = true} = {}) {
  const target = String(id || 'zh-CN');
  if (target !== 'zh-CN') {
    await ensureChain(target);
    if (!packCache.has(target)) {
      // 包不存在：不动当前语言，让调用方提示
      throw new Error('language pack not found: ' + target);
    }
  }
  activeId = target;
  dictionary = target === 'zh-CN' ? emptyDictionary() : resolveDictionary(target);
  if (target !== 'zh-CN') cacheDictionary(target, dictionary);
  setEnabled(target !== 'zh-CN');
  applyDocumentLang();
  relocalizeDocument();
  if (persist) {
    storageSet(LANG_KEY, target);
    try {
      const {saveSettings} = await import('./app.js');
      saveSettings({language: target});
    } catch (error) { /* app.js 还没加载完（启动早期）：设置由调用方补 */ }
  }
  for (const callback of listeners) {
    try { callback(target); } catch (error) { /* 回调挂了不影响切换 */ }
  }
  return target;
}

// 导入 .rlan：文本 → 校验 → 存服务端 → 注册到内存
export async function importLanguageFile(file) {
  const text = await file.text();
  let raw;
  try {
    raw = JSON.parse(text);
  } catch (error) {
    throw new Error('不是合法的 JSON 文件');
  }
  const pack = normalizePack(raw);
  if (!pack) throw new Error('语言包缺少合法的 id / name 字段');
  const result = await api('POST', '/music/langs/import', {filename: file.name || pack.id + '.rlan', pack: raw});
  if (!result || result.code !== 0) {
    throw new Error((result && result.message) || '服务端拒绝了这次导入');
  }
  packCache.set(pack.id, pack);
  metaCache.set(pack.id, packMeta(pack));
  return packMeta(pack);
}

export async function removeLanguage(id) {
  const result = await api('POST', '/music/langs/remove', {id});
  if (!result || result.code !== 0) throw new Error((result && result.message) || '删除失败');
  packCache.delete(id);
  metaCache.delete(id);
  storageDel(CACHE_PREFIX + id);
  if (activeId === id) await setLanguage('zh-CN');
}

export async function exportLanguage(id) {
  const target = id || activeId;
  const pack = packCache.get(target) || await ensurePack(target);
  if (!pack) throw new Error('找不到语言包');
  return JSON.stringify(pack, null, 2);
}

// 供设置页显示的统计
export function languageStats() {
  return {
    id: activeId,
    name: dictionary.name,
    exact: dictionary.exact.size,
    patterns: dictionary.patterns.length,
    rules: dictionary.rules.length
  };
}

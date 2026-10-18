// Rad Music - 设置页（主题 / 播放 / 音源 / 插件 / 调音 / 缓存 / 日志 / 数据 / API）
import { api, esc, errText, fmtBytes, fmtTime, toast,
  qualityLabel, qualityDetail, sortQualities } from './api.js';
import { icon } from './icons.js';
import {
  state, player, $, $$, openModal, closeModal, confirmDialog, promptDialog,
  saveSettings, loadSettings, applySettings, refreshConnection, loadLibrary,
  stop as stopPlayback
} from './app.js';

export const THEMES = [
  {id: 'violet', name: '紫罗兰', colors: ['#9a7bff', '#6a5bff', '#c9a6ff']},
  {id: 'ocean', name: '深海', colors: ['#45b3ff', '#2f7cf6', '#8fdcff']},
  {id: 'sunset', name: '日落', colors: ['#ff8059', '#ff4d79', '#ffbe8f']},
  {id: 'forest', name: '森林', colors: ['#46d39a', '#1fa97a', '#a4f0cd']},
  {id: 'sakura', name: '樱花', colors: ['#ff8ab5', '#e0508c', '#ffd0e2']},
  {id: 'mono', name: '极简', colors: ['#c3c9d4', '#8b93a1', '#e6eaf1']}
];

const TABS = [
  {id: 'appearance', name: '外观', icon: 'palette'},
  {id: 'playback', name: '播放', icon: 'play'},
  {id: 'sources', name: '音源', icon: 'globe'},
  {id: 'plugins', name: '插件', icon: 'plugin'},
  {id: 'audio', name: '调音', icon: 'equalizer'},
  {id: 'cache', name: '缓存', icon: 'folder'},
  {id: 'logs', name: '日志', icon: 'list'},
  {id: 'data', name: '数据', icon: 'trash'},
  {id: 'about', name: '关于 / API', icon: 'info'}
];

let activeTab = 'appearance';
let migrateTimer = null;   // 迁移进度轮询定时器

function card(title, hint, content) {
  return '<div class="set-card"><h4>' + esc(title) + '</h4>' +
    (hint ? '<p class="hint">' + hint + '</p>' : '') + content + '</div>';
}

function fieldRow(name, desc, control) {
  return '<div class="field"><div class="field-info"><strong>' + esc(name) + '</strong>' +
    (desc ? '<span>' + desc + '</span>' : '') + '</div><div class="field-control">' + control + '</div></div>';
}

function themeSwatches() {
  return '<div class="theme-grid">' + THEMES.map((theme) => {
    const active = state.settings.theme === theme.id;
    return '<button class="theme-swatch' + (active ? ' active' : '') + '" data-theme-id="' + theme.id + '">' +
      '<span class="swatch-bar" style="background:linear-gradient(135deg,' + theme.colors[0] + ',' + theme.colors[1] + ')"></span>' +
      '<span>' + esc(theme.name) + '</span></button>';
  }).join('') + '</div>';
}

export async function renderSettingsView(root) {
  root.innerHTML = '<div class="settings-layout">' +
    '<div class="settings-tabs" id="settings-tabs">' + TABS.map((tab) =>
      '<button class="settings-tab' + (tab.id === activeTab ? ' active' : '') + '" data-tab="' + tab.id + '">' +
      icon(tab.icon, 18) + '<span>' + esc(tab.name) + '</span></button>').join('') + '</div>' +
    '<div class="settings-panel" id="settings-panel"></div></div>';
  $$('[data-tab]', root).forEach((button) => {
    button.addEventListener('click', () => {
      activeTab = button.dataset.tab;
      $$('[data-tab]', root).forEach((entry) => entry.classList.toggle('active', entry.dataset.tab === activeTab));
      renderTab(root);
    });
  });
  renderTab(root);
}

function renderTab(root) {
  const host = $('#settings-panel', root);
  if (!host) return;
  host.innerHTML = '<div class="empty"><strong>加载中…</strong></div>';
  const renderer = TABS.find((tab) => tab.id === activeTab);
  if (!renderer) return;
  const map = {
    appearance: renderAppearance,
    playback: renderPlayback,
    sources: renderSources,
    plugins: renderPlugins,
    audio: renderAudio,
    cache: renderCache,
    logs: renderLogs,
    data: renderData,
    about: renderAbout
  };
  map[activeTab](host, root);
}

// ---------------------------------------------------------------- 外观
function renderAppearance(host) {
  host.innerHTML =
    card('主题', '切换应用配色，立即生效并保存到服务器设置。', themeSwatches()) +
    card('明暗模式', '',
      fieldRow('配色方案', '深色更适合夜间听歌', '<select id="set-scheme" style="width:150px">' +
        '<option value="dark"' + (state.settings.color_scheme !== 'light' ? ' selected' : '') + '>深色</option>' +
        '<option value="light"' + (state.settings.color_scheme === 'light' ? ' selected' : '') + '>浅色</option></select>')) +
    card('播放栏', '底部悬浮播放栏的样式。',
      fieldRow('圆角大小', '拖动调整播放栏圆角半径（0–48px），实时预览',
        '<input type="range" id="set-radius" class="range" min="0" max="48" step="1" value="' + (Number(state.settings.player_radius) || 0) + '" style="width:180px" />' +
        '<span class="tag" id="set-radius-value">' + (Number(state.settings.player_radius) || 0) + 'px</span>') +
      fieldRow('紧凑模式', '降低播放栏高度，隐藏进度条',
        '<label class="switch"><input type="checkbox" id="set-compact"' + (state.settings.compact_player ? ' checked' : '') + ' /><span></span></label>')) +
    card('界面预览', '这些设置不需要保存按钮，修改即生效。',
      '<div class="note">当前主题：<strong>' + esc((THEMES.find((theme) => theme.id === state.settings.theme) || {}).name || state.settings.theme) + '</strong> · ' +
      '配色：<strong>' + (state.settings.color_scheme === 'light' ? '浅色' : '深色') + '</strong> · ' +
      '播放栏圆角：<strong>' + (Number(state.settings.player_radius) || 0) + 'px</strong></div>');

  $$('[data-theme-id]', host).forEach((button) => {
    button.addEventListener('click', async () => {
      await saveSettings({theme: button.dataset.themeId});
      $$('[data-theme-id]', host).forEach((entry) => entry.classList.toggle('active', entry.dataset.themeId === button.dataset.themeId));
      renderAppearance(host);
    });
  });
  $('#set-scheme', host).addEventListener('change', async (event) => {
    await saveSettings({color_scheme: event.target.value});
    renderAppearance(host);
  });
  const radius = $('#set-radius', host);
  const radiusValue = $('#set-radius-value', host);
  radius.addEventListener('input', () => {
    const value = Number(radius.value);
    document.documentElement.style.setProperty('--player-radius', value + 'px');
    if (radiusValue) radiusValue.textContent = value + 'px';
    radius.style.setProperty('--fill', (value / 48 * 100) + '%');
  });
  radius.addEventListener('change', () => saveSettings({player_radius: Number(radius.value)}));
  radius.style.setProperty('--fill', (Number(radius.value) / 48 * 100) + '%');
  $('#set-compact', host).addEventListener('change', async (event) => {
    await saveSettings({compact_player: event.target.checked});
  });
}

// ---------------------------------------------------------------- 播放
async function renderPlayback(host) {
  const qualityResult = await api('GET', '/music/quality');
  const sources = (qualityResult.code === 0 && qualityResult.data && qualityResult.data.sources) || [];
  const levelOptions = [];
  const seen = new Set();
  for (const source of sources) {
    for (const level of (source.levels || [])) {
      const id = level.quality_id || level.id || level.level;
      if (!id || seen.has(id)) continue;
      seen.add(id);
      levelOptions.push({id, label: qualityLabel(level), detail: qualityDetail(level), platform: source.platform});
    }
  }
  host.innerHTML =
    card('音质偏好', '播放时默认选择的音质（在当前曲目/音源支持时生效）。',
      fieldRow('默认音质', '自动表示由音源返回其最高可用音质',
        '<select id="set-quality" style="width:220px"><option value="auto"' +
        (state.settings.preferred_quality === 'auto' ? ' selected' : '') + '>自动（最高可用）</option>' +
        levelOptions.map((option) => '<option value="' + esc(option.id) + '"' +
          (state.settings.preferred_quality === option.id ? ' selected' : '') + '>' +
          esc(option.platform + ' · ' + option.label + (option.detail ? ' · ' + option.detail : '')) + '</option>').join('') +
        '</select>')) +
    card('播放行为', '',
      fieldRow('自动播放下一首', '当前曲目结束后自动继续',
        '<label class="switch"><input type="checkbox" id="set-autoplay"' + (state.settings.autoplay_next !== false ? ' checked' : '') + ' /><span></span></label>') +
      fieldRow('默认音量', '启动时的播放音量',
        '<input type="range" id="set-volume" class="range" min="0" max="100" step="1" style="width:160px" value="' + Math.round((Number(state.settings.volume) || 0.8) * 100) + '" />' +
        '<span class="tag" id="set-volume-value">' + Math.round((Number(state.settings.volume) || 0.8) * 100) + '%</span>')) +
    card('搜索与折叠', '',
      fieldRow('折叠同一首歌', '跨音源折叠 group_key 相同的曲目',
        '<label class="switch"><input type="checkbox" id="set-fold"' + (state.settings.fold_results !== false ? ' checked' : '') + ' /><span></span></label>') +
      fieldRow('隐藏不可用音源', '搜索时隐藏 available=false 的源',
        '<label class="switch"><input type="checkbox" id="set-hide"' + (state.settings.hide_unavailable ? ' checked' : '') + ' /><span></span></label>')) +
    card('键盘快捷键', '',
      '<div class="note"><strong>Space</strong> 播放/暂停　<strong>Esc</strong> 关闭播放页/菜单　' +
      '<strong>Alt+←/→</strong> 上一首/下一首　<strong>Shift+←/→</strong> 后退/前进 5 秒</div>');

  $('#set-quality', host).addEventListener('change', (event) => saveSettings({preferred_quality: event.target.value}));
  $('#set-autoplay', host).addEventListener('change', (event) => saveSettings({autoplay_next: event.target.checked}));
  const volume = $('#set-volume', host);
  const volumeValue = $('#set-volume-value', host);
  volume.addEventListener('input', () => {
    const value = Number(volume.value);
    if (volumeValue) volumeValue.textContent = value + '%';
    volume.style.setProperty('--fill', value + '%');
    if (player.audio) { player.audio.volume = value / 100; player.volume = value / 100; }
  });
  volume.addEventListener('change', () => saveSettings({volume: Number(volume.value) / 100}));
  volume.style.setProperty('--fill', volume.value + '%');
  $('#set-fold', host).addEventListener('change', (event) => saveSettings({fold_results: event.target.checked}));
  $('#set-hide', host).addEventListener('change', (event) => saveSettings({hide_unavailable: event.target.checked}));
}

// ---------------------------------------------------------------- 音源
async function renderSources(host) {
  const [qualityResult, pluginsResult] = await Promise.all([
    api('GET', '/music/quality'),
    api('GET', '/music/plugins')
  ]);
  const sources = (qualityResult.code === 0 && qualityResult.data && qualityResult.data.sources) || [];
  const pluginList = (pluginsResult.code === 0 && pluginsResult.data && pluginsResult.data.plugins) || [];
  const priority = (pluginsResult.code === 0 && pluginsResult.data && pluginsResult.data.source_priority) || [];
  const platforms = [];
  for (const source of sources) if (source.platform && !platforms.includes(source.platform)) platforms.push(source.platform);
  for (const plugin of pluginList) {
    for (const platform of (plugin.platforms || [])) if (!platforms.includes(platform)) platforms.push(platform);
  }
  const mvSources = pluginList.filter((plugin) => (plugin.capabilities || []).some((cap) =>
    cap === 'search_mv' || cap === 'get_mv'));
  const mvPlatform = [];
  for (const plugin of mvSources) for (const platform of (plugin.platforms || [])) if (!mvPlatform.includes(platform)) mvPlatform.push(platform);
  const currentMv = String(state.settings.mv_source || '');

  host.innerHTML =
    card('视频源（MV）', '在「搜索」页把类型选成 MV 时，默认使用哪个视频源搜索与解析。',
      fieldRow('默认视频源', '留空则自动选择第一个支持 search_mv / get_mv 的插件',
        '<select id="set-mv-source" style="width:220px"><option value="">自动选择</option>' +
        mvPlatform.map((platform) => '<option value="' + esc(platform) + '"' +
          (currentMv === platform ? ' selected' : '') + '>' + esc(platform) + '</option>').join('') +
        (mvPlatform.length === 0 ? '<option value="" disabled>（当前没有支持 MV 的插件）</option>' : '') +
        (currentMv && !mvPlatform.includes(currentMv)
          ? '<option value="' + esc(currentMv) + '" disabled selected>' + esc(currentMv) + '（不可用，请更换）</option>' : '') +
        '</select>')) +
    card('音源优先级', '服务端在搜索与折叠时按此顺序优先选择音源（读取自服务器配置）。',
      priority.length
        ? '<div class="row wrap" style="gap:8px">' + priority.map((id, index) =>
          '<span class="tag accent">' + (index + 1) + '. ' + esc(id) + '</span>').join('') + '</div>'
        : '<div class="note">服务器没有设置音源优先级。</div>') +
    card('音源与音质', '所有已声明平台的音质清单（由插件声明，宿主不编造）。',
      sources.length ? sources.map((source) =>
        '<div class="field"><div class="field-info"><strong>' + esc(source.platform || '') +
        '</strong><span>插件：' + esc(source.plugin_id || '') + '</span></div>' +
        '<div class="plugin-meta" style="flex:none">' +
        sortQualities(source.levels || []).map((level) => '<span class="q-badge q-' + esc(level.level || 'unknown') + '">' +
          esc(qualityLabel(level)) + '</span>').join('') + '</div></div>').join('')
        : '<div class="note">还没有已启用的音源插件。</div>') +
    card('操作', '',
      '<div class="row" style="gap:10px">' +
      '<button class="btn" id="set-refresh-sources">' + icon('refresh', 16) + '刷新音源信息</button>' +
      '<button class="btn ghost" id="set-goto-plugins">' + icon('plugin', 16) + '管理插件</button></div>');

  $('#set-mv-source', host).addEventListener('change', (event) => saveSettings({mv_source: event.target.value}));
  $('#set-refresh-sources', host).addEventListener('click', () => renderSources(host));
  $('#set-goto-plugins', host).addEventListener('click', () => {
    activeTab = 'plugins';
    const root = $('#view-root');
    $$('#settings-tabs [data-tab]', document).forEach((entry) => entry.classList.toggle('active', entry.dataset.tab === 'plugins'));
    if (root) renderTab(root);
  });
}

// ---------------------------------------------------------------- 插件
async function renderPlugins(host) {
  const result = await api('GET', '/music/plugins');
  const plugins = (result.code === 0 && result.data && result.data.plugins) || [];
  const rows = plugins.map((plugin) => {
    const type = plugin.type === 'function' ? '功能插件' : '播放源';
    const caps = (plugin.capabilities || []).join(' / ');
    return '<div class="plugin-item">' +
      '<div class="plugin-icon">' + icon(plugin.type === 'function' ? 'equalizer' : 'globe', 20) + '</div>' +
      '<div class="plugin-main">' +
        '<div class="plugin-name">' + esc(plugin.name || plugin.id) +
          '<span class="tag">' + esc(plugin.id) + '</span>' +
          '<span class="tag">' + type + '</span>' +
          (plugin.has_implementation ? '<span class="tag accent">内置</span>' : '<span class="tag">声明式</span>') +
          (plugin.crashed ? '<span class="tag" style="color:var(--danger)">已崩溃</span>' : '') +
        '</div>' +
        '<div class="plugin-desc">' + esc(caps || '未声明 capabilities') +
          ((plugin.platforms || []).length ? '　平台：' + esc(plugin.platforms.join(' / ')) : '') +
          (plugin.version ? '　版本：' + esc(plugin.version) : '') + '</div>' +
      '</div>' +
      '<div class="plugin-actions">' +
        '<label class="switch"><input type="checkbox" data-plugin-toggle="' + esc(plugin.id) + '"' +
          (plugin.enabled ? ' checked' : '') + ' /><span></span></label>' +
        '<button class="icon-btn" data-plugin-settings="' + esc(plugin.id) + '" title="插件设置">设置</button>' +
        '<button class="icon-btn" data-plugin-view="' + esc(plugin.id) + '" title="查看清单">' + icon('eye', 18) + '</button>' +
        (plugin.has_implementation ? '' : '<button class="icon-btn" data-plugin-delete="' + esc(plugin.id) + '" title="删除">' + icon('trash', 18) + '</button>') +
      '</div></div>';
  }).join('');
  host.innerHTML =
    card('已安装插件', '播放源插件无需界面，功能插件可提供面板/设置/菜单。通过 manifest 声明能力与权限。',
      plugins.length ? rows : '<div class="note">还没有加载任何插件。可以安装内置演示插件或在下方添加声明式插件。</div>' +
      '<div class="row" style="gap:10px;margin-top:10px"><button class="btn" id="plugin-demo">' +
      icon('plus', 16) + '载入内置演示播放源说明</button></div>') +
    card('添加插件（声明式）', '粘贴 plugin.json 内容（type=source 或 function）。' +
      '源码插件需要以 dll/so 形式随服务端加载；声明式插件会保存到 &lt;root&gt;/plugins/&lt;id&gt;/plugin.json。',
      '<label class="col" style="gap:6px"><textarea id="plugin-json" style="min-height:170px" placeholder=\'' +
      esc(JSON.stringify({id: 'my.source', name: '我的音源', type: 'source', platforms: ['my'], version: '1.0.0',
        capabilities: ['search', 'get_song', 'get_url', 'get_lyric'], quality_levels: [{id: 'lossless', level: 'lossless', codec: 'flac'}],
        permissions: ['network']}, null, 2)) + '\'></textarea></label>' +
      '<div class="row" style="gap:10px;margin-top:10px">' +
      '<button class="btn primary" id="plugin-add">' + icon('plus', 16) + '注册插件</button>' +
      '<button class="btn ghost" id="plugin-template">填入模板</button></div>') +
    card('插件标准提示', '',
      '<div class="note">播放源插件声明 platforms / capabilities / quality_levels；功能插件声明 ui.panel / ui.settings / ui.menu。' +
      '宿主会校验 api_version 与权限（network、filesystem、audio_dsp 等）。</div>');

  $$('[data-plugin-toggle]', host).forEach((input) => {
    input.addEventListener('change', async () => {
      const id = input.dataset.pluginToggle;
      const response = await api('POST', '/music/plugins/' + encodeURIComponent(id) + '/enabled', {enabled: input.checked});
      if (response.code === 0) {
        toast(input.checked ? '插件已启用' : '插件已禁用', 'ok');
      } else {
        input.checked = !input.checked;
        toast(errText(response), 'error');
      }
    });
  });
  $$('[data-plugin-view]', host).forEach((button) => {
    button.addEventListener('click', async () => {
      const response = await api('GET', '/music/plugins/' + encodeURIComponent(button.dataset.pluginView));
      if (response.code !== 0) { toast(errText(response), 'error'); return; }
      openModal({
        title: '插件清单 · ' + button.dataset.pluginView,
        body: '<pre class="note mono" style="white-space:pre-wrap;font-size:12px">' + esc(JSON.stringify(response.data, null, 2)) + '</pre>'
      });
    });
  });
  $$('[data-plugin-settings]', host).forEach((button) => {
    button.addEventListener('click', () => openPluginSettings(button.dataset.pluginSettings, host));
  });
  $$('[data-plugin-delete]', host).forEach((button) => {
    button.addEventListener('click', async () => {
      const id = button.dataset.pluginDelete;
      if (!(await confirmDialog({title: '删除插件', message: '确定删除插件「' + id + '」？清单文件也会被移除。', okLabel: '删除', danger: true}))) return;
      const response = await api('DELETE', '/music/plugins/' + encodeURIComponent(id));
      if (response.code === 0) { toast('插件已删除', 'ok'); renderPlugins(host); }
      else toast(errText(response), 'error');
    });
  });
  const templateButton = $('#plugin-template', host);
  if (templateButton) templateButton.addEventListener('click', () => {
    $('#plugin-json', host).value = JSON.stringify({
      id: 'my.source', name: '我的音源', type: 'source', version: '1.0.0', api_version: '1',
      platforms: ['my'], capabilities: ['search', 'get_song', 'get_url', 'get_qualities', 'get_lyric'],
      quality_levels: [{id: 'high', level: 'high', codec: 'mp3', bitrate: 320000},
                       {id: 'lossless', level: 'lossless', codec: 'flac', bitrate: 1411200}],
      permissions: ['network'], limits: {max_concurrency: 4, qps: 5}
    }, null, 2);
  });
  const addButton = $('#plugin-add', host);
  if (addButton) addButton.addEventListener('click', async () => {
    const text = String($('#plugin-json', host).value || '').trim();
    if (!text) { toast('请先粘贴插件清单 JSON'); return; }
    let parsed;
    try { parsed = JSON.parse(text); } catch (error) { toast('JSON 解析失败：' + error.message, 'error'); return; }
    const response = await api('POST', '/music/plugins', parsed);
    if (response.code === 0) { toast('插件已注册', 'ok'); renderPlugins(host); }
    else toast(errText(response), 'error');
  });
  const demoButton = $('#plugin-demo', host);
  if (demoButton) demoButton.addEventListener('click', () => {
    openModal({
      title: '内置演示播放源',
      body: '<div class="note">演示插件（demo）仅在服务端以 <code>--demo</code> 启动时可用，' +
        '提供搜索/歌词/音质清单，但不提供真实音频流。<br><br>' +
        '要让真实音源工作，需要实现 ISourcePlugin 并随服务端编译，或编写声明式清单 + 宿主侧适配实现。</div>'
    });
  });
}

// ---------------------------------------------------------------- 调音（EQ / DSP）
async function renderAudio(host) {
  const [eqResult, presetsResult, dspResult] = await Promise.all([
    api('GET', '/music/eq'),
    api('GET', '/music/eq/presets'),
    api('GET', '/music/dsp/nodes')
  ]);
  const eq = (eqResult.code === 0 && eqResult.data) || {enabled: false, bands: [], preamp_db: 0, engine: 'builtin'};
  const presets = (presetsResult.code === 0 && presetsResult.data && presetsResult.data.presets) || [];
  const nodes = (dspResult.code === 0 && dspResult.data && dspResult.data.nodes) || [];
  const bands = Array.isArray(eq.bands) && eq.bands.length ? eq.bands : defaultBands();
  const bandHtml = bands.map((band, index) =>
    '<div class="eq-band"><span class="gain" id="eq-gain-' + index + '">' + (band.gain || band.gain_db || 0) + 'dB</span>' +
    '<input type="range" class="range" data-eq-band="' + index + '" min="-24" max="24" step="1" value="' + (band.gain || band.gain_db || 0) + '" />' +
    '<span class="freq">' + formatFreq(band.freq) + '</span></div>').join('');

  host.innerHTML =
    card('均衡器（EQ）', '使用后端内置 EQ 引擎，实时作用于播放链路。',
      fieldRow('启用 EQ', '关闭时不改变音频',
        '<label class="switch"><input type="checkbox" id="eq-enabled"' + (eq.enabled ? ' checked' : '') + ' /><span></span></label>') +
      fieldRow('预置', '选择服务器 presets/eq 目录下的预置，或手动调整',
        '<select id="eq-preset" style="width:180px"><option value="">自定义</option>' +
        presets.map((name) => '<option value="' + esc(name) + '"' + (eq.preset === name ? ' selected' : '') + '>' + esc(name) + '</option>').join('') +
        '</select>') +
      fieldRow('前级增益', '单位 dB，范围 -24 ~ 24',
        '<input type="range" id="eq-preamp" class="range" min="-24" max="24" step="1" style="width:160px" value="' + (Number(eq.preamp_db) || 0) + '" />' +
        '<span class="tag" id="eq-preamp-value">' + (Number(eq.preamp_db) || 0) + 'dB</span>') +
      '<div class="divider"></div>' +
      '<div class="eq-layout">' + bandHtml + '</div>' +
      '<div class="row" style="gap:10px;margin-top:10px">' +
      '<button class="btn primary" id="eq-apply">' + icon('check', 16) + '应用 EQ</button>' +
      '<button class="btn" id="eq-flat">恢复 0dB</button>' +
      '<span class="note" id="eq-status">引擎：' + esc(eq.engine || '内置') + (eq.latency_ms ? ' · 延迟 ' + eq.latency_ms + 'ms' : '') + '</span></div>') +
    card('DSP 节点', '由声明了 audio_dsp 权限的功能插件提供。',
      nodes.length ? nodes.map((node) =>
        '<div class="field"><div class="field-info"><strong>' + esc(node.name || node.plugin_id) + '</strong>' +
        '<span>插件：' + esc(node.plugin_id) + '</span></div>' +
        '<div class="field-control"><button class="btn small" data-dsp-node="' + esc(node.plugin_id) + '">切换旁路</button></div></div>').join('')
        : '<div class="note">没有可用的 DSP 节点（需要功能插件声明 audio_dsp 权限）。</div>');

  const collectBands = () => $$('[data-eq-band]', host).map((input, index) => ({
    type: (bands[index] && bands[index].type) || 'peak',
    freq: (bands[index] && bands[index].freq) || 1000,
    gain: Number(input.value),
    q: (bands[index] && bands[index].q) || 1
  }));
  const updateBandLabels = () => {
    $$('[data-eq-band]', host).forEach((input, index) => {
      const label = $('#eq-gain-' + index, host);
      if (label) label.textContent = input.value + 'dB';
      input.style.setProperty('--fill', ((Number(input.value) + 24) / 48 * 100) + '%');
    });
  };
  $$('[data-eq-band]', host).forEach((input) => input.addEventListener('input', updateBandLabels));
  updateBandLabels();
  const preamp = $('#eq-preamp', host);
  const preampValue = $('#eq-preamp-value', host);
  preamp.addEventListener('input', () => {
    if (preampValue) preampValue.textContent = preamp.value + 'dB';
    preamp.style.setProperty('--fill', ((Number(preamp.value) + 24) / 48 * 100) + '%');
  });
  preamp.style.setProperty('--fill', ((Number(preamp.value) + 24) / 48 * 100) + '%');
  const apply = async () => {
    const body = {
      enabled: $('#eq-enabled', host).checked,
      preset: $('#eq-preset', host).value || '',
      preamp_db: Number(preamp.value) || 0,
      bands: collectBands()
    };
    const response = await api('POST', '/music/eq', body);
    if (response.code === 0) {
      const info = response.data || {};
      const status = $('#eq-status', host);
      if (status) status.textContent = '引擎：' + (info.engine || '内置') + (info.latency_ms ? ' · 延迟 ' + info.latency_ms + 'ms' : '') + ' · 已应用';
      toast('EQ 已应用', 'ok');
    } else {
      toast(errText(response), 'error');
    }
  };
  $('#eq-apply', host).addEventListener('click', apply);
  $('#eq-flat', host).addEventListener('click', () => {
    $$('[data-eq-band]', host).forEach((input) => { input.value = '0'; });
    preamp.value = '0';
    if (preampValue) preampValue.textContent = '0dB';
    updateBandLabels();
    apply();
  });
  $('#eq-enabled', host).addEventListener('change', apply);
  $('#eq-preset', host).addEventListener('change', apply);
  $$('[data-dsp-node]', host).forEach((button) => {
    button.addEventListener('click', async () => {
      const response = await api('POST', '/music/dsp/bypass', {node: button.dataset.dspNode, bypassed: true});
      toast(response.code === 0 ? '已切换旁路' : errText(response), response.code === 0 ? 'ok' : 'error');
    });
  });
}

function defaultBands() {
  return [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000].map((freq) => ({type: 'peak', freq, gain: 0, q: 1}));
}

function formatFreq(freq) {
  const value = Number(freq) || 0;
  return value >= 1000 ? (value / 1000) + 'k' : String(value);
}

// ---------------------------------------------------------------- 缓存
async function renderCache(host) {
  const result = await api('GET', '/music/cache');
  const data = (result.code === 0 && result.data) || {};
  const layers = data.layer_list || [];
  const limit = Number(data.limit_bytes) || 0;
  const usage = Number(data.usage_bytes) || 0;
  host.innerHTML =
    card('缓存概览', '分层缓存：meta / audio / lyric / cover / tmp，可清理可重建。',
      '<div class="layer-bar">' +
        '<div class="layer-row"><span class="name">总计</span><span class="bar"><i style="width:' +
          (limit > 0 ? Math.min(100, usage * 100 / limit) : 0) + '%"></i></span>' +
          '<span class="num">' + fmtBytes(usage) + ' / ' + fmtBytes(limit) + '</span></div>' +
        layers.map((layer) =>
          '<div class="layer-row"><span class="name">' + esc(layer.name) + '</span><span class="bar"><i style="width:' +
          (usage > 0 ? Math.min(100, (Number(layer.bytes) || 0) * 100 / usage) : 0) + '%"></i></span>' +
          '<span class="num">' + (layer.entries || 0) + ' 项 · ' + fmtBytes(layer.bytes) + '</span></div>').join('') +
      '</div>' +
      (data.broken_meta ? '<p class="note" style="margin-top:10px;color:var(--danger)">发现 ' + data.broken_meta + ' 个损坏的 meta 文件</p>' : '') +
      (data.pending_eviction ? '<p class="note">待清理条目：' + data.pending_eviction + '</p>' : '') +
      '<div class="row" style="gap:10px;margin-top:12px">' +
      '<button class="btn" id="cache-clear-all">' + icon('trash', 16) + '清理全部缓存</button>' +
      '<button class="btn ghost" id="cache-refresh">' + icon('refresh', 16) + '刷新</button></div>') +
    card('清理单层缓存', '',
      '<div class="row wrap" style="gap:8px">' + ['meta', 'audio', 'lyric', 'cover', 'tmp'].map((name) =>
        '<button class="chip-btn" data-cache-clear="' + name + '">清理 ' + name + '</button>').join('') + '</div>') +
    card('缓存配置', '容量以 MB 为单位（0 表示不限制）。',
      '<div class="field-grid">' +
        '<label>meta (MB)<input type="number" id="cache-limit-meta" min="0" value="' + toMb(data.limit_bytes) + '" /></label>' +
        '<label>audio (MB)<input type="number" id="cache-limit-audio" min="0" value="512" /></label>' +
        '<label>lyric (MB)<input type="number" id="cache-limit-lyric" min="0" value="64" /></label>' +
        '<label>cover (MB)<input type="number" id="cache-limit-cover" min="0" value="128" /></label>' +
        '<label>预加载<select id="cache-preload"><option value="0">关闭</option><option value="1" selected>开启</option></select></label>' +
      '</div>' +
      '<div class="row" style="gap:10px;margin-top:12px">' +
      '<button class="btn primary" id="cache-save">' + icon('check', 16) + '保存配置</button></div>') +
    card('重建缓存条目', '按 song_key + quality_key 重建 base.json。',
      '<div class="field-grid">' +
        '<label>song_key<input type="text" id="cache-song-key" placeholder="song_key" /></label>' +
        '<label>quality_key<input type="text" id="cache-quality-key" placeholder="例如 local.lossless-flac-1411" /></label>' +
      '</div><div class="row" style="gap:10px;margin-top:12px">' +
      '<button class="btn" id="cache-rebuild">' + icon('refresh', 16) + '重建</button></div>');

  const clear = async (type) => {
    const response = await api('POST', '/music/cache/clear', {type});
    if (response.code === 0) {
      toast('已清理：' + (type === 'all' ? '全部缓存' : type) + '（' + ((response.data && response.data.cleared) || 0) + ' 项）', 'ok');
      renderCache(host);
    } else toast(errText(response), 'error');
  };
  $('#cache-clear-all', host).addEventListener('click', async () => {
    if (!(await confirmDialog({title: '清理全部缓存', message: '缓存中的 meta、音频缓存与歌词都会删除，曲库文件不受影响。', okLabel: '清理', danger: true}))) return;
    clear('all');
  });
  $$('[data-cache-clear]', host).forEach((button) =>
    button.addEventListener('click', () => clear(button.dataset.cacheClear)));
  $('#cache-refresh', host).addEventListener('click', () => renderCache(host));
  $('#cache-save', host).addEventListener('click', async () => {
    const body = {
      limit_meta: Number($('#cache-limit-meta', host).value || 0) * 1024 * 1024,
      limit_audio: Number($('#cache-limit-audio', host).value || 0) * 1024 * 1024,
      limit_lyric: Number($('#cache-limit-lyric', host).value || 0) * 1024 * 1024,
      limit_cover: Number($('#cache-limit-cover', host).value || 0) * 1024 * 1024,
      preload: $('#cache-preload', host).value === '1'
    };
    const response = await api('POST', '/music/cache/config', body);
    toast(response.code === 0 ? '缓存配置已保存' : errText(response), response.code === 0 ? 'ok' : 'error');
    if (response.code === 0) renderCache(host);
  });
  $('#cache-rebuild', host).addEventListener('click', async () => {
    const songKey = String($('#cache-song-key', host).value || '').trim();
    const qualityKey = String($('#cache-quality-key', host).value || '').trim();
    if (!songKey || !qualityKey) { toast('请填写 song_key 与 quality_key'); return; }
    const response = await api('POST', '/music/cache/rebuild', {song_key: songKey, quality_key: qualityKey});
    toast(response.code === 0 ? '重建成功' : errText(response), response.code === 0 ? 'ok' : 'error');
  });
}

function toMb(bytes) {
  return Math.round((Number(bytes) || 0) / 1024 / 1024);
}

// ---------------------------------------------------------------- 日志
async function renderLogs(host) {
  const lines = 200;
  const level = state.viewState && state.viewState.logLevel ? state.viewState.logLevel : '';
  const category = state.viewState && state.viewState.logCategory ? state.viewState.logCategory : '';
  const query = '/music/logs?lines=' + lines + (level ? '&level=' + encodeURIComponent(level) : '') +
    (category ? '&category=' + encodeURIComponent(category) : '');
  const result = await api('GET', query);
  const data = (result.code === 0 && result.data) || {};
  const records = data.records || [];
  host.innerHTML =
    card('日志级别', '调整服务端日志输出级别（即时生效）。',
      fieldRow('当前级别', 'trace / debug / info / warn / error / off',
        '<select id="log-level" style="width:140px">' +
        ['trace', 'debug', 'info', 'warn', 'error', 'off'].map((name) =>
          '<option value="' + name + '"' + (data.level === name ? ' selected' : '') + '>' + name + '</option>').join('') +
        '</select>')) +
    card('最近日志', '最多返回 500 条。',
      '<div class="row" style="gap:10px;margin-bottom:10px">' +
        '<select id="log-filter-level" style="width:130px"><option value="">全部级别</option>' +
        ['trace', 'debug', 'info', 'warn', 'error'].map((name) =>
          '<option value="' + name + '"' + (level === name ? ' selected' : '') + '>' + name + '</option>').join('') + '</select>' +
        '<input type="text" id="log-filter-category" placeholder="分类，例如 api" value="' + esc(category) + '" style="width:170px" />' +
        '<button class="btn" id="log-refresh">' + icon('refresh', 16) + '刷新</button>' +
      '</div>' +
      (records.length ? '<div class="log-list">' + records.slice().reverse().map((record) =>
        '<div class="log-row"><span class="t">' + esc((record.time || '').split(' ').pop() || '') + '</span>' +
        '<span class="lvl ' + esc(String(record.level || '').toLowerCase()) + '">' + esc(record.level || '') + '</span>' +
        '<span class="cat">' + esc(record.category || '') + '</span>' +
        '<span class="msg">' + esc(record.message || '') + '</span></div>').join('') + '</div>'
        : '<div class="note">没有日志记录。</div>'));

  $('#log-level', host).addEventListener('change', async (event) => {
    const response = await api('POST', '/music/logs/level', {level: event.target.value});
    toast(response.code === 0 ? '日志级别已更新' : errText(response), response.code === 0 ? 'ok' : 'error');
  });
  $('#log-refresh', host).addEventListener('click', () => {
    state.viewState = state.viewState || {};
    state.viewState.logLevel = $('#log-filter-level', host).value;
    state.viewState.logCategory = $('#log-filter-category', host).value;
    renderLogs(host);
  });
}

// ---------------------------------------------------------------- 数据
// 分类由后端 /music/data 下发，勾选后按 targets 清除（二次确认 + 输入确认短语）
async function renderData(host) {
  stopMigratePoll();
  const [result, locResult] = await Promise.all([
    api('GET', '/music/data'),
    api('GET', '/music/data/location')
  ]);
  if (result.code !== 0) {
    host.innerHTML = card('数据管理', '', '<div class="note">读取数据概览失败：' + esc(errText(result)) + '</div>');
    return;
  }
  const data = result.data || {};
  const groups = data.groups || [];
  const total = Number(data.usage_bytes) || 0;
  const loc = (locResult.code === 0 && locResult.data) || {};
  const migration = loc.migration || {};

  const migrateCard = card('存储位置 / 迁移数据',
    '更改数据（曲目 + 缓存 + 设置）的存放目录。整个数据目录会一起复制到新位置，随后应用自动重启切换；旧数据会保留，确认无误后可自行删除回收空间。',
    '<dl class="kv">' +
      '<dt>当前目录</dt><dd class="mono" style="word-break:break-all">' + esc(data.root || '') + '</dd>' +
      '<dt>配置保存于</dt><dd class="mono" style="word-break:break-all">' + esc(loc.config_path || '') + '</dd>' +
    '</dl>' +
    '<div class="row" style="gap:8px;margin-top:12px">' +
      '<input type="text" id="migrate-target" placeholder="新的数据目录（绝对路径）" style="flex:1;min-width:0" />' +
      '<button class="btn primary" id="migrate-start">迁移并切换</button>' +
    '</div>' +
    '<div id="migrate-status" class="note" style="margin-top:10px"></div>');

  host.innerHTML = migrateCard +
    card('数据概览', '曲目、缓存与配置都保存在服务端数据目录中，清除后无法恢复。',
      '<dl class="kv">' +
        '<dt>数据目录</dt><dd class="mono" style="word-break:break-all">' + esc(data.root || '') + '</dd>' +
        '<dt>总占用</dt><dd>' + fmtBytes(total) + ' · ' + (Number(data.total_files) || 0) + ' 个文件</dd>' +
        '<dt>曲库条目</dt><dd>' + (Number(data.tracks_count) || 0) + ' 首</dd>' +
      '</dl>' +
      '<div class="row" style="gap:10px;margin-top:12px">' +
      '<button class="btn ghost" id="data-refresh">' + icon('refresh', 16) + '重新统计</button></div>') +
    card('选择要清除的数据', '取消勾选即可只清部分内容，例如只清日志而保留曲库。',
      '<div class="row wrap" style="gap:8px;margin-bottom:10px">' +
        '<button class="chip-btn" id="data-select-all">全选</button>' +
        '<button class="chip-btn" id="data-select-none">全不选</button></div>' +
      '<div class="data-list">' + groups.map((group) =>
        '<label class="data-row"><input type="checkbox" data-data-target="' + esc(group.id) + '" checked />' +
        '<span class="data-info"><strong>' + esc(group.name) + '</strong><em>' + esc(group.description) + '</em></span>' +
        '<span class="data-size">' + (Number(group.files) ? Number(group.files) + ' 项 · ' + fmtBytes(group.bytes) : '空') + '</span>' +
        '</label>').join('') + '</div>') +
    card('危险操作', '<strong class="danger-text">删除后无法恢复</strong>，请确认曲目文件不需要保留。',
      '<p class="note" style="margin-bottom:12px">已选 <strong id="data-selected-count">0</strong> 类 · 约 ' +
      '<strong id="data-selected-size">0 B</strong></p>' +
      '<div class="row wrap" style="gap:10px">' +
      '<button class="btn danger" id="data-purge">' + icon('trash', 16) + '清除所选数据</button>' +
      '<button class="btn ghost" id="data-purge-all">清除全部数据</button></div>');

  const boxes = () => $$('[data-data-target]', host);
  const picked = () => groups.filter((group) => {
    const box = boxes().find((entry) => entry.dataset.dataTarget === group.id);
    return box && box.checked;
  });
  const updateSummary = () => {
    const selected = picked();
    const bytes = selected.reduce((sum, group) => sum + (Number(group.bytes) || 0), 0);
    $('#data-selected-count', host).textContent = String(selected.length);
    $('#data-selected-size', host).textContent = fmtBytes(bytes);
    $('#data-purge', host).disabled = selected.length === 0;
  };

  const runPurge = async (targets, labels) => {
    if (!targets.length) { toast('请至少选择一类数据', 'error'); return; }
    const phrase = '确认清除';
    const typed = await promptDialog({
      title: '清除数据（1/2）',
      label: '将永久删除：' + labels.join('、') + '。此操作无法恢复，请输入「' + phrase + '」继续。',
      placeholder: phrase,
      okLabel: '继续'
    });
    if (typed === null) return;
    if (String(typed).trim() !== phrase) { toast('确认短语不匹配，已取消', 'error'); return; }
    const confirmed = await confirmDialog({
      title: '清除数据（2/2）',
      message: '最后确认：' + labels.join('、') + ' 将被永久删除，且无法恢复。',
      okLabel: '确认清除',
      danger: true
    });
    if (!confirmed) return;

    const response = await api('POST', '/music/data/purge', {targets, confirm: 'PURGE'});
    if (response.code !== 0) { toast(errText(response), 'error'); return; }
    const removed = Number(response.data && response.data.removed_files) || 0;
    const bytes = Number(response.data && response.data.removed_bytes) || 0;
    const failed = Number(response.data && response.data.failed) || 0;
    toast('已清除 ' + removed + ' 个文件（' + fmtBytes(bytes) + '）' +
      (failed ? '，' + failed + ' 项被占用未能删除' : ''), failed ? 'error' : 'ok');

    // 播放器与内存中的列表都指向已删除的文件，统一复位
    player.queue = [];
    player.index = -1;
    player.current = null;
    player.base = null;
    player.serverLoaded = false;
    player.variants = [];
    player.lyric = [];
    player.lyricActive = -1;
    stopPlayback();
    if (targets.includes('settings')) await loadSettings();
    await loadLibrary(true);
    if (activeTab === 'data') await renderData(host);
  };

  boxes().forEach((box) => box.addEventListener('change', updateSummary));
  $('#data-select-all', host).addEventListener('click', () => {
    boxes().forEach((box) => { box.checked = true; });
    updateSummary();
  });
  $('#data-select-none', host).addEventListener('click', () => {
    boxes().forEach((box) => { box.checked = false; });
    updateSummary();
  });
  $('#data-refresh', host).addEventListener('click', () => renderData(host));
  $('#data-purge', host).addEventListener('click', () => {
    const selected = picked();
    runPurge(selected.map((group) => group.id), selected.map((group) => group.name));
  });
  $('#data-purge-all', host).addEventListener('click', () => {
    runPurge(groups.map((group) => group.id), ['全部数据（' + groups.length + ' 类）']);
  });

  // ---- 存储位置 / 迁移数据 ----
  if (migration.state && migration.state !== 'idle') renderMigrate(host, migration);
  if (migration.state === 'running') startMigratePoll(host);
  $('#migrate-start', host).addEventListener('click', async () => {
    const target = ($('#migrate-target', host).value || '').trim();
    if (!target) { toast('请输入新的数据目录', 'error'); return; }
    const ok = await confirmDialog({
      title: '迁移数据并切换位置',
      message: '将把整个数据目录（曲目 + 缓存 + 设置，共约 ' + fmtBytes(total) + '）复制到「' + target +
        '」，完成后应用会自动重启切换到新位置；旧数据会保留。迁移期间请勿操作。',
      okLabel: '开始迁移',
      danger: true
    });
    if (!ok) return;
    const response = await api('POST', '/music/data/migrate', {new_root: target, confirm: 'MIGRATE'});
    if (response.code !== 0) { toast(errText(response), 'error'); return; }
    toast('迁移已开始', 'ok');
    renderMigrate(host, response.data || {});
    startMigratePoll(host);
  });

  updateSummary();
}

const MIGRATE_STATE_TEXT = {idle: '空闲', running: '进行中', done: '完成', failed: '失败', canceled: '已取消'};
function renderMigrate(host, m) {
  const box = $('#migrate-status', host);
  if (!box) return;
  const s = m.state || 'idle';
  if (s === 'idle') { box.innerHTML = ''; return; }
  const done = Number(m.bytes_done) || 0;
  const all = Number(m.bytes_total) || 0;
  const pct = all ? Math.floor((done / all) * 100) : 0;
  let html = '<div>状态：' + esc(MIGRATE_STATE_TEXT[s] || s) + ' · 已复制 ' + (Number(m.files_done) || 0) +
    ' / ' + (Number(m.files_total) || 0) + ' 个文件 · ' + fmtBytes(done) + ' / ' + fmtBytes(all);
  if (m.current) html += ' · 当前：' + esc(m.current);
  if (Number(m.failed)) html += ' · 跳过 ' + m.failed + ' 个（被占用等）';
  html += '</div>' +
    '<div style="height:6px;background:rgba(128,128,128,.25);border-radius:3px;margin-top:6px;overflow:hidden">' +
    '<div style="height:100%;width:' + pct + '%;background:var(--accent,#4c8dff);transition:width .2s"></div></div>';
  box.innerHTML = html;
}
function startMigratePoll(host) {
  stopMigratePoll();
  let fails = 0;
  const tick = async () => {
    const r = await api('GET', '/music/data/migrate');
    if (r.code !== 0) {
      // 迁移完成后服务会用新数据根重启，期间连接可能瞬断；短时间内重试，服务回来即视为完成
      if (++fails >= 20) {
        stopMigratePoll();
        toast('服务已重启但无法确认迁移状态，请手动刷新页面查看', 'error');
      }
      return;
    }
    fails = 0;
    const m = r.data || {};
    renderMigrate(host, m);
    if (m.state === 'failed') {
      stopMigratePoll();
      toast('迁移失败：' + (m.error || '未知错误') + '（已保留原数据，未切换）', 'error');
    } else if (m.state === 'canceled') {
      stopMigratePoll();
      toast('迁移已取消', 'error');
    } else if (m.state === 'done' || m.state === 'idle') {
      // done = 旧实例收尾；idle = 服务已用新数据根重启（全新迁移器）——都表示迁移完成、已切换
      stopMigratePoll();
      const box = $('#migrate-status', host);
      if (box) box.innerHTML = '<div>✅ 迁移完成，已切换到新数据目录。若页面数据未刷新，请稍等几秒后刷新。</div>';
      toast('迁移完成，已切换到新数据目录', 'ok');
    }
  };
  tick();
  migrateTimer = setInterval(tick, 800);
}
function stopMigratePoll() {
  if (migrateTimer) { clearInterval(migrateTimer); migrateTimer = null; }
}

// ---------------------------------------------------------------- 关于 / API
async function renderAbout(host) {
  const [pluginsResult, qualityResult, cacheResult] = await Promise.all([
    api('GET', '/music/plugins'),
    api('GET', '/music/quality'),
    api('GET', '/music/cache')
  ]);
  const pluginCount = (pluginsResult.code === 0 && pluginsResult.data && pluginsResult.data.plugins || []).length;
  const sourceCount = (qualityResult.code === 0 && qualityResult.data && qualityResult.data.sources || []).length;
  const cacheSize = (cacheResult.code === 0 && cacheResult.data && cacheResult.data.usage_bytes) || 0;
  host.innerHTML =
    card('Rad Music', '纯前端 + C++17 零依赖服务端的音乐库应用。',
      '<dl class="kv">' +
        '<dt>连接地址</dt><dd>' + esc(location.origin) + '</dd>' +
        '<dt>连接状态</dt><dd id="about-conn">' + (state.conn === 'ok' ? '已连接' : '未知') + '</dd>' +
        '<dt>插件数量</dt><dd>' + pluginCount + '</dd>' +
        '<dt>音源数量</dt><dd>' + sourceCount + '</dd>' +
        '<dt>缓存占用</dt><dd>' + fmtBytes(cacheSize) + '</dd>' +
        '<dt>曲库数量</dt><dd>' + state.library.length + ' 首</dd>' +
      '</dl>' +
      '<div class="row" style="gap:10px;margin-top:12px">' +
      '<button class="btn" id="about-ping">' + icon('refresh', 16) + '重新检测连接</button>' +
      '<button class="btn ghost" id="about-reload-lib">' + icon('library', 16) + '刷新曲库</button></div>') +
    card('服务控制', '重启宿主进程：会断开当前连接，浏览器会重连到新进程。',
      '<p class="note">如果新进程能正常监听，新页面加载完成后会自动重新建立连接。</p>' +
      '<div class="row" style="gap:10px;margin-top:12px">' +
      '<button class="btn" id="about-restart">' + icon('refresh', 16) + '重启宿主服务</button>' +
      '<span id="about-restart-state" class="note"></span></div>') +
    card('API 工具', '直接调用 API 做诊断。',
      '<div class="field-grid">' +
        '<label>verify sha<input type="text" id="tool-sha" placeholder="64 位 sha" /></label>' +
        '<label>password<input type="text" id="tool-password" placeholder="可选" /></label>' +
      '</div>' +
      '<div class="row" style="gap:10px;margin-top:10px"><button class="btn" id="tool-verify">校验曲目</button></div>' +
      '<div class="divider"></div>' +
      '<div class="field-grid">' +
        '<label>source<input type="text" id="tool-source" placeholder="例如 demo" /></label>' +
        '<label>op<input type="text" id="tool-op" placeholder="例如 get_lyric" /></label>' +
      '</div>' +
      '<label class="col" style="gap:6px;margin-top:10px"><span class="note">params（JSON 对象，可空）</span>' +
      '<textarea id="tool-params" style="min-height:80px" placeholder=\'{"id":"track-1"}\'></textarea></label>' +
      '<div class="row" style="gap:10px;margin-top:10px"><button class="btn" id="tool-request">发送 request</button></div>' +
      '<pre class="note mono" id="tool-output" style="white-space:pre-wrap;font-size:11.5px;margin-top:10px"></pre>') +
    card('接口覆盖情况', '前端已接入的 API（详细字段见 api/Readme.md）。',
      '<div class="note" style="line-height:1.9">' +
      ['load', 'start / pause / resume / stop / seek', 'status', 'info', 'verify', 'search', 'request', 'lyric',
        'quality', 'quality/{platform}/{id}', 'cache 全族', 'download 全族', 'import / scan / status / report',
        'library', 'eq / presets', 'dsp / bypass', 'playlist 全族', 'album 全族', 'mv 全族 + search',
        'logs / level', 'settings（新增）', 'plugins（新增）', 'data / purge（新增）'].map((item) =>
        '<span class="tag" style="margin:0 6px 6px 0">' + esc(item) + '</span>').join('') +
      '</div>');

  $('#about-ping', host).addEventListener('click', async () => {
    const status = await refreshConnection();
    const conn = $('#about-conn', host);
    if (conn) conn.textContent = status === 'ok' ? '已连接' : '连接失败';
    toast(status === 'ok' ? '服务器在线' : '无法连接服务器', status === 'ok' ? 'ok' : 'error');
  });
  $('#about-reload-lib', host).addEventListener('click', async () => {
    await loadLibrary(true);
    toast('曲库已刷新（' + state.library.length + ' 首）', 'ok');
  });
  $('#tool-verify', host).addEventListener('click', async () => {
    const sha = String($('#tool-sha', host).value || '').trim();
    if (!sha) { toast('请输入 sha'); return; }
    const response = await api('POST', '/music/verify', {sha, password: String($('#tool-password', host).value || '')});
    $('#tool-output', host).textContent = JSON.stringify(response, null, 2);
  });
  $('#tool-request', host).addEventListener('click', async () => {
    const source = String($('#tool-source', host).value || '').trim();
    const op = String($('#tool-op', host).value || '').trim();
    if (!source || !op) { toast('请填写 source 与 op'); return; }
    let params = {};
    const text = String($('#tool-params', host).value || '').trim();
    if (text) {
      try { params = JSON.parse(text); } catch (error) { toast('params 不是合法 JSON', 'error'); return; }
    }
    const response = await api('POST', '/music/request', {source, op, params});
    $('#tool-output', host).textContent = JSON.stringify(response, null, 2);
  });
  // 重启宿主（POST /music/restart）
  const restartButton = $('#about-restart', host);
  if (restartButton) {
    restartButton.addEventListener('click', async () => {
      if (!(await confirmDialog({
        title: '重启宿主服务',
        message: '会让当前 radmusic_server 退出并重新拉起。浏览器与播放队列会短暂中断，确认继续吗？',
        okLabel: '重启',
        danger: true
      }))) return;
      restartButton.disabled = true;
      const state = $('#about-restart-state', host);
      if (state) state.textContent = '正在重启…';
      try {
        const response = await fetch(location.origin + '/music/restart', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({confirm: 'RESTART'})
        });
        const json = await response.json().catch(() => ({code: -1, message: '响应不是 JSON'}));
        if (json.code === 0) {
          if (state) state.textContent = '已发起重启，正在等待新进程…';
          // 给宿主管道时间：800ms 之后开始 ping 状态，掉线时持续重试直到恢复
          const deadline = Date.now() + 15000;
          const tryStatus = async () => {
            try {
              const resp = await fetch(location.origin + '/music/status', {cache: 'no-store'});
              if (resp.ok) {
                const text = await resp.text();
                if (text && text.length) {
                  if (state) state.textContent = '新进程已就绪，连接已恢复';
                  restartButton.disabled = false;
                  if (typeof refreshConnection === 'function') refreshConnection();
                  return true;
                }
              }
            } catch (e) { /* keep polling */ }
            if (Date.now() > deadline) {
              if (state) state.textContent = '新进程未在 15 秒内就绪，请检查服务端日志';
              restartButton.disabled = false;
              return true;
            }
            return false;
          };
          const tick = async () => {
            if (await tryStatus()) return;
            setTimeout(tick, 500);
          };
          setTimeout(tick, 800);
        } else {
          if (state) state.textContent = '重启失败：' + (json.message || ('错误 ' + json.code));
          restartButton.disabled = false;
          toast('重启失败：' + (json.message || ''), 'error');
        }
      } catch (error) {
        if (state) state.textContent = '重启请求失败：' + (error && error.message ? error.message : String(error));
        restartButton.disabled = false;
      }
    });
  }
}

// ---------------------------------------------------------------- 插件设置面板
async function openPluginSettings(pluginId, parentHost) {
  const response = await api('GET', '/music/plugins/' + encodeURIComponent(pluginId) + '/settings');
  if (response.code !== 0) { toast(errText(response), 'error'); return; }
  const data = response.data || {};
  const schema = (data.settings_schema && data.settings_schema.schema) || [];
  const endpoint = data.endpoint || (data.settings_schema && data.settings_schema.endpoint) || '';
  const loginPage = data.login_page || '';
  const values = data.values || {};
  const login = data.login || {};
  const platform = (data.platforms && data.platforms[0]) || pluginId;
  const canLogin = login.logged_in != null;  // 插件上报了登录态 → 支持登录

  const rows = schema.map((field) => {
    const key = field.key;
    const value = values[key] != null ? values[key] : (field.default != null ? field.default : '');
    const min = field.min != null ? ' min="' + esc(field.min) + '"' : '';
    const max = field.max != null ? ' max="' + esc(field.max) + '"' : '';
    const step = field.type === 'int' || field.type === 'number' ? ' step="1"' : '';
    return '<div class="field">' +
      '<div class="field-info"><strong>' + esc(field.label || key) + '</strong>' +
      (field.hint ? '<small>' + esc(field.hint) + '</small>' : '') + '</div>' +
      '<div class="field-control">' +
        '<input id="ps-' + esc(key) + '" type="number"' + min + max + step + ' value="' + esc(value) + '" />' +
      '</div>' +
      '</div>';
  }).join('');

  const loginStateBlock = canLogin ? (
    '<div class="row"><span>账号</span><b id="ps-who">' + esc(login.nickname || (login.logged_in ? '已登录' : '未登录')) +
      (login.user_id ? '（uid ' + esc(login.user_id) + '）' : '') + '</b></div>' +
    '<div class="row"><span>会员</span><b id="ps-vip">' +
      (login.vip ? '已开通' + (login.vip_label ? '·' + esc(login.vip_label) : '') : '未开通') + '</b></div>' +
    '<div class="row"><span>Cookie</span><code id="ps-ck">' +
      (login.cookie_present ? esc(login.cookie_masked || '已配置') : '未配置') + '</code></div>' +
    '<p class="hint" style="margin:12px 0 6px">粘贴该音源的登录 Cookie 后点「验证并登录」：浏览器打开对应网站并登录，' +
      'F12 → Application / 应用 → Cookies，复制 <b>SESSDATA</b>（或整段 Cookie）粘贴到下面。</p>' +
    '<textarea id="ps-cookie" class="ps-cookie" placeholder="SESSDATA=xxxx%2Cxxxx%2Cxxxx%2C*xxxx"></textarea>' +
    '<div class="row" style="gap:8px;margin-top:10px">' +
      '<button type="button" class="btn primary" id="ps-login">验证并登录</button>' +
      '<button type="button" class="btn ghost" id="ps-logout">退出登录</button>' +
    '</div>' +
    '<div class="state" id="ps-login-msg"></div>'
  ) : '';

  const versionBlock = '<div class="row"><span>插件版本</span><b>v' + esc(data.version || '?') + '</b></div>' +
    '<div class="row"><span>Endpoint</span><code>' + esc(data.endpoint || endpoint || '') + '</code></div>';

  const warningBlock = data.warning ? '<div class="note" style="color:var(--warn)">' + esc(data.warning) + '</div>' : '';

  const body =
    warningBlock +
    (canLogin ? card('账号登录', '登录后可解锁会员音质 / 收藏夹等；凭据写入插件目录的 cookie.txt，修改后可能需要重启插件。', loginStateBlock) : '') +
    (schema.length ? card('插件行为',
      '修改后保存到宿主 settings.json，下次启动插件时生效。',
      '<form id="plugin-settings-form" autocomplete="off">' + rows +
      '<div class="row" style="gap:8px;margin-top:14px"><button type="submit" class="btn primary">保存到宿主设置</button>' +
      (endpoint ? '<button type="button" class="btn ghost" id="ps-open-external">在浏览器打开</button>' : '') + '</div></form>'
    ) : '') +
    card('版本与诊断', '', versionBlock);

  openModal({title: '插件设置 · ' + (data.name || pluginId), body});

  const form = $('#plugin-settings-form');
  if (form) {
    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      const next = {};
      for (const field of schema) {
        const input = $('#ps-' + field.key);
        if (!input) continue;
        let value = Number(input.value);
        if (Number.isNaN(value)) value = field.default != null ? field.default : 0;
        if (field.min != null && value < field.min) value = field.min;
        if (field.max != null && value > field.max) value = field.max;
        next[field.key] = value;
      }
      const resp = await api('POST', '/music/plugins/' + encodeURIComponent(pluginId) + '/settings', {values: next});
      if (resp.code === 0) {
        toast('已保存到 settings.json，下次启动插件生效', 'ok');
      } else {
        toast(errText(resp), 'error');
      }
    });
  }
  const external = $('#ps-open-external');
  if (external) external.addEventListener('click', () => {
    if (!endpoint) { toast('插件未声明 endpoint', 'warn'); return; }
    // 优先打开登录页（如 /login），否则打开 /settings
    window.open(endpoint + (loginPage || '/settings'), '_blank', 'noopener');
  });
  const sayLogin = (text, kind) => {
    const el = $('#ps-login-msg');
    if (!el) return;
    el.textContent = text || '';
    el.className = 'state' + (kind ? ' ' + kind : '');
  };
  const refreshLoginView = (st) => {
    const who = $('#ps-who'), vip = $('#ps-vip'), ck = $('#ps-ck');
    if (who) who.textContent = (st && st.uname) ? st.uname : (st && st.logged_in ? '已登录' : '未登录');
    if (vip) vip.textContent = (st && st.vip) ? ('已开通' + (st.vip_label ? '·' + st.vip_label : '')) : '未开通';
    if (ck) ck.textContent = (st && st.cookie_present) ? (st.cookie_masked || '已配置') : '未配置';
  };
  const loginBtn = $('#ps-login');
  if (loginBtn) loginBtn.addEventListener('click', async () => {
    const cookie = String($('#ps-cookie').value || '').trim();
    if (!cookie) { sayLogin('请先粘贴 Cookie 或 SESSDATA', 'warn'); return; }
    loginBtn.disabled = true;
    sayLogin('验证中…');
    const resp = await api('POST', '/music/request', {source: platform, op: 'login_cookie', params: {cookie}});
    loginBtn.disabled = false;
    if (resp.code === 0) {
      const st = (resp.data && resp.data.result) || {};
      refreshLoginView(st);
      $('#ps-cookie').value = '';
      sayLogin('登录成功：' + (st.uname || '已登录'), 'ok');
    } else {
      sayLogin(errText(resp), 'err');
    }
  });
  const logoutBtn = $('#ps-logout');
  if (logoutBtn) logoutBtn.addEventListener('click', async () => {
    logoutBtn.disabled = true;
    const resp = await api('POST', '/music/request', {source: platform, op: 'logout', params: {}});
    logoutBtn.disabled = false;
    if (resp.code === 0) {
      refreshLoginView({logged_in: false, cookie_present: false});
      sayLogin('已退出登录', 'ok');
    } else {
      sayLogin(errText(resp), 'err');
    }
  });
}


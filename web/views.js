// Rad Music - 各功能页面视图
import { api, esc, errText, fmtTime, fmtDuration, fmtBytes, toast, formatDate,
  qualityBadge, qualityLabel, qualityDetail, sortQualities, qualityRank,
  lyricLinesFromData, coverUrl } from './api.js';
import { icon } from './icons.js';
import {
  state, player, $, $$, openModal, closeModal, confirmDialog, promptDialog,
  contextMenu, closeContextMenu, navigate, setTopbarActions, loadLibrary,
  playQueue, enqueue, enqueueAndPlay, addToPlaylistFlow, itemFromTrack, itemFromResult,
  platformLabel, variantsFor, saveSettings, renderPlayer, openMvStage, stop
} from './app.js';

// ---------------------------------------------------------------- 小工具
// 封面可能是在线 URL、data: URL，或曲库相对路径 covers/<hash>.<ext>；
// 相对路径必须经 coverUrl() 走宿主 /music/cover/ 路由，否则浏览器直接 404。
function coverMarkup(pic, size = 20) {
  if (pic) return '<img src="' + esc(coverUrl(pic)) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />';
  return icon('music', size);
}

function qualityLine(quality) {
  const detail = qualityDetail(quality);
  return esc(qualityLabel(quality)) + (detail ? ' · ' + esc(detail) : '');
}

function emptyState(title, detail, iconName = 'music') {
  return '<div class="empty">' + icon(iconName, 42) + '<strong>' + esc(title) + '</strong>' +
    (detail ? '<span>' + esc(detail) + '</span>' : '') + '</div>';
}

function spinnerRow(text) {
  return '<div class="empty"><strong>' + esc(text || '加载中…') + '</strong></div>';
}

function safeArray(value) { return Array.isArray(value) ? value : []; }

async function libraryItems() {
  const tracks = await loadLibrary();
  return tracks;
}

// 把本地曲目按 song_key 聚合成「同一首歌的不同音质」
function groupLibrary(tracks) {
  const groups = new Map();
  for (const track of tracks) {
    const key = track.song_key || track.group_key || ('sha:' + track.sha);
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(track);
  }
  const list = [];
  for (const [key, variantsRaw] of groups) {
    const variants = variantsRaw.slice().sort((a, b) =>
      qualityRank(b.quality && b.quality.level) - qualityRank(a.quality && a.quality.level));
    const best = variants[0];
    list.push({
      key,
      best,
      variants,
      // 最高音质那条未必有封面（常见于本地文件），回退到任一带封面的版本
      pic: best.pic || (variants.find((track) => track.pic) || {}).pic || '',
      title: best.name || '(未知标题)',
      artist: best.author || '未知艺术家',
      album: best.album || '',
      duration: Number(best.duration) || 0,
      // 自定义合并：这些音质版本是用户手动合到一起的（可取消合并）
      merged: !!best.merged,
      mergeCount: Number(best.merge_count) || 0
    });
  }
  list.sort((a, b) => String(a.title).localeCompare(String(b.title), 'zh-Hans-CN'));
  return list;
}

function bindTrackActions(root, {onPlay, onAdd, extraMenu} = {}) {
  $$('[data-play-key]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      if (onPlay) onPlay(button.dataset.playKey, button.dataset.playSha || '');
    });
  });
  $$('[data-add-key]', root).forEach((button) => {
    button.addEventListener('click', (event) => { event.stopPropagation(); if (onAdd) onAdd(button.dataset.addKey); });
  });
  $$('[data-more-key]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const rect = button.getBoundingClientRect();
      const key = button.dataset.moreKey;
      const items = [];
      if (extraMenu) items.push(...extraMenu(key));
      contextMenu(rect.left - 160, rect.bottom + 6, items);
    });
  });
}

function trackRowMarkup({index, title, artist, pic, duration, qualityHtml, actionsHtml, expandable, expandCount, key, sha, playing, dataAttrs = '', lead = null}) {
  return '<div class="track-row' + (playing ? ' playing' : '') + '" ' + dataAttrs + ' data-track="' + esc(key || '') + '">' +
    '<div class="track-index">' + (lead != null ? lead : (playing ? icon('play', 12) : (index != null ? index : ''))) + '</div>' +
    '<div class="track-cover">' + coverMarkup(pic) + '</div>' +
    '<div class="track-main">' +
      '<div class="track-title-line"><span class="track-name">' + esc(title) + '</span>' +
      (expandable ? '<button class="chip-btn" style="height:22px;padding:0 9px;font-size:11px" data-expand="' + esc(key) + '">' +
        icon('chevronDown', 13, 'caret') + (expandCount || '') + '</button>' : '') +
      '</div>' +
      '<div class="track-sub"><span>' + esc(artist) + '</span>' +
        (qualityHtml ? '<span class="sep">·</span>' + qualityHtml : '') + '</div>' +
    '</div>' +
    (actionsHtml || '') +
    '<div class="track-duration">' + (duration ? fmtDuration(duration) : '--:--') + '</div>' +
  '</div>';
}

// ---------------------------------------------------------------- 曲库
export async function renderLibrary(root) {
  const mergeMode = libraryMergeMode();
  setTopbarActions(
    '<button class="btn" id="lib-import">' + icon('upload', 16) + '导入</button>' +
    '<button class="btn' + (mergeMode ? ' primary' : '') + '" id="lib-merge" title="把识别错误拆开的多条曲目合并为同一首歌">' +
      icon('link', 16) + (mergeMode ? '退出合并' : '合并') + '</button>' +
    '<button class="icon-btn" id="lib-refresh" title="刷新曲库">' + icon('refresh', 19) + '</button>' +
    '<button class="icon-btn" id="lib-playall" title="播放全部">' + icon('play', 19) + '</button>',
    (host) => {
      $('#lib-import', host).addEventListener('click', () => navigate('import'));
      $('#lib-merge', host).addEventListener('click', () => {
        state.viewState.libraryMergeMode = !libraryMergeMode();
        librarySelected().length = 0;
        renderLibrary(root);
      });
      $('#lib-refresh', host).addEventListener('click', async () => {
        await loadLibrary(true);
        renderLibrary(root);
        toast('曲库已刷新', 'ok');
      });
      $('#lib-playall', host).addEventListener('click', async () => {
        const groups = groupLibrary(await libraryItems());
        if (!groups.length) { toast('曲库为空'); return; }
        playQueue(groups.map((group) => itemFromTrack(group.best)), 0);
      });
    });

  root.innerHTML = spinnerRow('读取曲库…');
  await Promise.all([loadLibrary(), ensureSearchPlatforms().catch(() => {})]);
  renderLibraryBody(root);
}

function libraryFilter() {
  state.viewState = state.viewState || {};
  state.viewState.libraryFilter = Object.assign(
    {platform: '', qualityMin: '', sort: 'title'}, state.viewState.libraryFilter || {});
  return state.viewState.libraryFilter;
}

// 自定义合并：多选模式与已选 song_key（页面级临时状态，不持久化）
function libraryMergeMode() {
  state.viewState = state.viewState || {};
  return !!state.viewState.libraryMergeMode;
}

function librarySelected() {
  state.viewState = state.viewState || {};
  if (!Array.isArray(state.viewState.librarySelected)) state.viewState.librarySelected = [];
  return state.viewState.librarySelected;
}

function renderLibraryBody(root) {
  const keyword = String((state.viewState && state.viewState.libraryKeyword) || '').trim().toLowerCase();
  state.viewState = state.viewState || {};
  const filter = libraryFilter();
  const qualityNeed = filter.qualityMin ? qualityRank(filter.qualityMin) : 0;
  const platforms = Array.from(new Set(state.library.map((track) => track.platform || 'local'))).sort();
  const groups = groupLibrary(state.library).filter((group) => {
    if (keyword && !(group.title + ' ' + group.artist + ' ' + group.album).toLowerCase().includes(keyword)) return false;
    if (filter.platform && !group.variants.some((track) => (track.platform || 'local') === filter.platform)) return false;
    if (qualityNeed && !group.variants.some((track) => qualityRank(track.quality && track.quality.level) >= qualityNeed)) return false;
    return true;
  });
  if (filter.sort === 'artist') {
    groups.sort((a, b) => String(a.artist).localeCompare(String(b.artist), 'zh-Hans-CN'));
  } else if (filter.sort === 'duration') {
    groups.sort((a, b) => Number(a.duration) - Number(b.duration));
  } else if (filter.sort === 'quality') {
    groups.sort((a, b) => qualityRank(b.best.quality && b.best.quality.level) - qualityRank(a.best.quality && a.best.quality.level));
  } else if (filter.sort === 'album') {
    groups.sort((a, b) => String(a.album || '').localeCompare(String(b.album || ''), 'zh-Hans-CN'));
  }
  state.viewGroups = groups;
  const totalTracks = state.library.length;
  const filterActive = !!(filter.platform || filter.qualityMin || filter.sort !== 'title');
  const mergeMode = libraryMergeMode();
  const selected = librarySelected().filter((key) => groups.some((group) => group.key === key));
  const toolbar =
    '<div class="toolbar">' +
      '<div class="search-field">' + icon('search', 18) +
        '<input type="search" id="lib-filter" placeholder="筛选曲目（歌名 / 歌手 / 专辑）" value="' + esc(keyword) + '" />' +
      '</div>' +
      '<select class="filter-select" id="lib-platform" title="按音源筛选">' +
        '<option value="">全部音源</option>' +
        platforms.map((platform) => '<option value="' + esc(platform) + '"' +
          (filter.platform === platform ? ' selected' : '') + '>' +
          esc(platform === 'local' ? '本地文件' : platformName(platform)) + '</option>').join('') +
      '</select>' +
      '<select class="filter-select" id="lib-quality" title="按音质筛选">' +
        SEARCH_QUALITIES.map((option) => '<option value="' + esc(option.id) + '"' +
          (filter.qualityMin === option.id ? ' selected' : '') + '>' +
          (option.id ? option.label : '全部音质') + '</option>').join('') +
      '</select>' +
      '<select class="filter-select" id="lib-sort" title="排序方式">' +
        '<option value="title"' + (filter.sort === 'title' ? ' selected' : '') + '>按标题</option>' +
        '<option value="artist"' + (filter.sort === 'artist' ? ' selected' : '') + '>按歌手</option>' +
        '<option value="album"' + (filter.sort === 'album' ? ' selected' : '') + '>按专辑</option>' +
        '<option value="quality"' + (filter.sort === 'quality' ? ' selected' : '') + '>按音质</option>' +
        '<option value="duration"' + (filter.sort === 'duration' ? ' selected' : '') + '>按时长</option>' +
      '</select>' +
      (filterActive ? '<button class="chip-btn" id="lib-reset">' + icon('refresh', 14) + '重置</button>' : '') +
      '<span class="tag">' + groups.length + ' 首 / ' + totalTracks + ' 个版本</span>' +
      (groups.some((group) => group.variants.length > 1) ? '<span class="tag accent">含多音质堆叠</span>' : '') +
    '</div>';
  // 合并模式：勾选后合并（用于识别错误把同一首歌拆成多条的情况）
  const mergeBar = mergeMode
    ? '<div class="toolbar">' +
        '<span class="tag accent">合并模式</span>' +
        '<span class="note">勾选两条以上「其实是同一首歌」的曲目，合并后它们会成为同一首歌的不同音质。</span>' +
        '<span class="spacer"></span>' +
        '<span class="tag" id="lib-merge-count">已选 ' + selected.length + '</span>' +
        '<button class="btn primary" id="lib-merge-go"' + (selected.length >= 2 ? '' : ' disabled') + '>' +
          icon('link', 16) + '合并所选</button>' +
        '<button class="chip-btn" id="lib-merge-exit">退出</button>' +
      '</div>'
    : '';
  const head = toolbar + mergeBar;
  if (!groups.length) {
    root.innerHTML = head + emptyState(totalTracks ? '没有匹配的曲目' : '曲库还是空的',
      totalTracks ? '换个关键词，或放宽音源 / 音质筛选。' : '到「导入」页上传音频文件，或扫描本地目录。', 'library');
    bindLibrarySearch(root);
    bindLibraryFilters(root);
    return;
  }
  root.innerHTML = head + '<div class="track-list">' +
    groups.map((group, index) => {
      const best = group.best;
      const playing = player.current && !!best.song_key && player.current.songKey === best.song_key;
      const actions =
        '<div class="track-actions">' +
          '<button class="icon-btn" data-play-key="' + esc(group.key) + '" title="播放">' + icon('play', 17) + '</button>' +
          '<button class="icon-btn" data-add-key="' + esc(group.key) + '" title="加入歌单">' + icon('plus', 17) + '</button>' +
          '<button class="icon-btn" data-more-key="' + esc(group.key) + '" title="更多">' + icon('more', 17) + '</button>' +
        '</div>';
      const sourcePlatform = best.platform || 'local';
      // 在线曲目额外标出音源，避免和本地文件混淆
      const sourceTag = sourcePlatform === 'local' ? ''
        : '<span class="mini-tag">' + esc(platformName(sourcePlatform)) + '</span>';
      const mergeTag = group.merged ? '<span class="mini-tag accent">已合并 ' + group.mergeCount + ' 条</span>' : '';
      const qualityHtml = mergeTag + sourceTag + (best.quality ? qualityBadge(best.quality) : '');
      const lead = mergeMode
        ? '<input type="checkbox" class="track-check" data-check="' + esc(group.key) + '"' +
          (selected.indexOf(group.key) >= 0 ? ' checked' : '') + ' title="勾选要合并的曲目" />'
        : null;
      const row = trackRowMarkup({
        index: index + 1, title: group.title, artist: group.artist, pic: group.pic || best.pic,
        duration: group.duration, qualityHtml, actionsHtml: actions, lead,
        expandable: group.variants.length > 1, expandCount: group.variants.length + ' 个音质',
        key: group.key, playing
      });
      const stack = group.variants.length > 1
        ? '<div class="stack-list" data-stack="' + esc(group.key) + '" hidden="true">' +
            group.variants.map((track) =>
              '<div class="stack-item' + (player.current && player.current.sha === track.sha ? ' playing' : '') + '">' +
                icon('music', 14) +
                '<span class="q-meta">' + qualityLine(track.quality) + ' · ' + esc(track.quality_key || '') +
                  (track.orig_name && track.orig_name !== group.title ? ' · 原名：' + esc(track.orig_name) : '') +
                '</span>' +
                '<button class="chip-btn stack-play" data-stack-play="' + esc(track.sha) + '" data-stack-key="' + esc(group.key) + '">' +
                  icon('play', 13) + '播放</button>' +
                '<button class="chip-btn" data-stack-add="' + esc(track.sha) + '" title="加入歌单">' + icon('plus', 13) + '</button>' +
                '<button class="chip-btn" data-stack-remove="' + esc(track.sha) + '" title="从曲库移除">' + icon('trash', 13) + '</button>' +
              '</div>').join('') +
          '</div>'
        : '';
      return row + stack;
    }).join('') + '</div>';

  bindLibrarySearch(root);
  bindLibraryFilters(root);
  if (mergeMode) bindLibraryMerge(root, groups);
  $$('[data-expand]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const key = button.dataset.expand;
      const stack = $('[data-stack="' + CSS.escape(key) + '"]', root);
      if (!stack) return;
      const open = stack.hidden;
      stack.hidden = !open;
      const caret = $('.caret', button);
      if (caret) caret.classList.toggle('open', open);
    });
  });
  $$('[data-play-key]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const group = groups.find((entry) => entry.key === button.dataset.playKey);
      if (!group) return;
      // 单曲播放：追加到队列并播放，不清空原列表
      enqueueAndPlay([itemFromTrack(group.best)]);
    });
  });
  $$('[data-add-key]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const group = groups.find((entry) => entry.key === button.dataset.addKey);
      if (group) addToPlaylistFlow(group.variants.map((track) => itemFromTrack(track)));
    });
  });
  $$('[data-more-key]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const group = groups.find((entry) => entry.key === button.dataset.moreKey);
      if (!group) return;
      const rect = button.getBoundingClientRect();
      const rerender = () => renderLibraryBody(root);
      const menuItems = [
        {label: '播放整首（最高音质）', icon: 'play', onClick: () => enqueueAndPlay([itemFromTrack(group.best)])},
        {label: '加入播放队列', icon: 'queue', onClick: () => enqueue(group.variants.map((track) => itemFromTrack(track)))},
        {label: '加入歌单', icon: 'playlist', onClick: () => addToPlaylistFlow([itemFromTrack(group.best)])},
        {label: '加入专辑', icon: 'album', onClick: () => addToAlbumFlow([itemFromTrack(group.best)])},
        {divider: true},
        {label: '下载当前音质', icon: 'download', onClick: () => downloadTrack(group.best)},
        {label: '打开所在文件夹', icon: 'folder', onClick: () => openTrackFolder(group.best)},
        {label: '查看文件信息', icon: 'info', onClick: () => showTrackInfo(group)},
        {divider: true},
        {label: '从曲库移除（当前音质）', icon: 'trash', danger: true,
          onClick: () => removeTrackFromLibrary(group.best, {rerender})}
      ];
      // 手动合并过的曲目：可以拆回合并前的多条
      if (group.merged) {
        menuItems.push({
          label: '取消合并（拆回 ' + group.mergeCount + ' 条）', icon: 'link',
          onClick: () => unmergeGroup(group, root)
        });
      }
      // 多音质堆叠：整首歌（所有音质版本）也可以一次移除
      if (group.variants.length > 1) {
        menuItems.push({
          label: '移除整首歌（' + group.variants.length + ' 个音质）', icon: 'trash', danger: true,
          onClick: () => removeTrackFromLibrary(group.best, {all: true, rerender})
        });
      }
      contextMenu(rect.left - 170, rect.bottom + 6, menuItems);
    });
  });
  $$('[data-stack-play]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const group = groups.find((entry) => entry.key === button.dataset.stackKey);
      const track = group && group.variants.find((entry) => entry.sha === button.dataset.stackPlay);
      if (!track) return;
      // 单曲播放（指定音质版本）：追加到队列并播放，不清空原列表
      enqueueAndPlay([itemFromTrack(track)]);
    });
  });
  $$('[data-stack-add]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const track = state.library.find((entry) => entry.sha === button.dataset.stackAdd);
      if (track) addToPlaylistFlow([itemFromTrack(track)]);
    });
  });
  $$('[data-stack-remove]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const track = state.library.find((entry) => entry.sha === button.dataset.stackRemove);
      removeTrackFromLibrary(track, {rerender: () => renderLibraryBody(root)});
    });
  });
  $$('.track-row', root).forEach((row) => {
    row.addEventListener('dblclick', () => {
      const group = groups.find((entry) => entry.key === row.dataset.track);
      if (group) enqueueAndPlay([itemFromTrack(group.best)]);
    });
  });
}

// ---------------------------------------------------------------- 自定义合并
// 识别错误常把同一首歌拆成多条（歌名多了后缀、歌手写法不同、时长差几秒），
// 这里让用户手动把它们归一到同一首歌：合并后按音质堆叠，原始标签保留以支持取消合并。
function bindLibraryMerge(root, groups) {
  const sync = () => {
    const count = $('#lib-merge-count', root);
    if (count) count.textContent = '已选 ' + librarySelected().length;
    const go = $('#lib-merge-go', root);
    if (go) go.disabled = librarySelected().length < 2;
  };
  $$('[data-check]', root).forEach((box) => {
    box.addEventListener('change', () => {
      const key = box.dataset.check;
      const list = librarySelected();
      const at = list.indexOf(key);
      if (box.checked && at < 0) list.push(key);
      else if (!box.checked && at >= 0) list.splice(at, 1);
      sync();
    });
  });
  const go = $('#lib-merge-go', root);
  if (go) {
    go.addEventListener('click', () => {
      const chosen = groups.filter((group) => librarySelected().indexOf(group.key) >= 0);
      if (chosen.length < 2) { toast('请至少勾选两条曲目'); return; }
      openMergeDialog(chosen, root);
    });
  }
  const exit = $('#lib-merge-exit', root);
  if (exit) exit.addEventListener('click', () => exitMergeMode(root));
}

function exitMergeMode(root) {
  state.viewState.libraryMergeMode = false;
  librarySelected().length = 0;
  renderLibrary(root);
}

function openMergeDialog(groups, root) {
  const base = groups[0];
  openModal({
    title: '合并曲目',
    wide: true,
    body:
      '<p class="note" style="margin:0 0 10px">合并后这些曲目会成为同一首歌的不同音质版本，' +
      '以「基准」的歌名 / 歌手 / 时长为准。原始标签会保留，随时可以取消合并。</p>' +
      '<div class="pick-list">' +
        groups.map((group, index) =>
          '<label class="pick-row">' +
            '<input type="radio" name="merge-base" value="' + esc(group.key) + '"' +
              (index === 0 ? ' checked' : '') + ' />' +
            '<span class="pick-main">' +
              '<strong>' + esc(group.title) + '</strong>' +
              '<span class="note">' + esc(group.artist) + ' · ' + fmtDuration(group.duration) + ' · ' +
                group.variants.length + ' 个音质' + (group.merged ? ' · 已合并' : '') + '</span>' +
            '</span>' +
          '</label>').join('') +
      '</div>' +
      '<div class="merge-fields">' +
        '<label class="col"><span class="note">合并后的歌名</span>' +
          '<input type="text" id="merge-title" value="' + esc(base.title) + '" /></label>' +
        '<label class="col"><span class="note">歌手</span>' +
          '<input type="text" id="merge-artist" value="' + esc(base.artist) + '" /></label>' +
      '</div>',
    footer: '<button class="btn ghost" data-modal-close>取消</button>' +
            '<button class="btn primary" id="merge-confirm">合并</button>',
    onMount: (box, close) => {
      const titleInput = $('#merge-title', box);
      const artistInput = $('#merge-artist', box);
      $$('input[name="merge-base"]', box).forEach((radio) => {
        radio.addEventListener('change', () => {
          const picked = groups.find((group) => group.key === radio.value);
          if (!picked) return;
          titleInput.value = picked.title;
          artistInput.value = picked.artist;
        });
      });
      $('#merge-confirm', box).addEventListener('click', async () => {
        const checked = $$('input[name="merge-base"]', box).filter((radio) => radio.checked)[0];
        const picked = (checked && groups.find((group) => group.key === checked.value)) || groups[0];
        const payload = {
          target: picked.key,
          sources: groups.filter((group) => group.key !== picked.key).map((group) => group.key),
          title: (titleInput.value || '').trim() || picked.title,
          artist: (artistInput.value || '').trim() || picked.artist
        };
        close();
        const result = await api('POST', '/music/library/merge', payload);
        if (result.code !== 0) { toast(errText(result), 'error'); return; }
        toast('已合并为同一首歌（' + (result.data.moved || 0) + ' 个音质版本）', 'ok');
        await loadLibrary(true);
        exitMergeMode(root);
      });
    }
  });
}

async function unmergeGroup(group, root) {
  const ok = await confirmDialog({
    title: '取消合并',
    message: '把「' + group.title + '」拆回合并前的 ' + group.mergeCount + ' 条独立曲目？' +
      '各音质版本会恢复它们原来的歌名 / 歌手。',
    okLabel: '取消合并'
  });
  if (!ok) return;
  const result = await api('POST', '/music/library/unmerge', {song_key: group.key});
  if (result.code !== 0) { toast(errText(result), 'error'); return; }
  toast('已拆回 ' + (result.data.restored || 0) + ' 条曲目', 'ok');
  await loadLibrary(true);
  renderLibraryBody(root);
}

function bindLibrarySearch(root) {
  const input = $('#lib-filter', root);
  if (!input) return;
  let timer = null;
  input.addEventListener('input', () => {
    state.viewState = state.viewState || {};
    state.viewState.libraryKeyword = input.value;
    clearTimeout(timer);
    timer = setTimeout(() => {
      const focused = document.activeElement === input;
      renderLibraryBody(root);
      const next = $('#lib-filter', root);
      if (next && focused) { next.focus(); next.setSelectionRange(next.value.length, next.value.length); }
    }, 160);
  });
}

function bindLibraryFilters(root) {
  const filter = libraryFilter();
  const update = (patch) => {
    Object.assign(filter, patch);
    renderLibraryBody(root);
  };
  const platform = $('#lib-platform', root);
  if (platform) platform.addEventListener('change', () => update({platform: platform.value}));
  const quality = $('#lib-quality', root);
  if (quality) quality.addEventListener('change', () => update({qualityMin: quality.value}));
  const sort = $('#lib-sort', root);
  if (sort) sort.addEventListener('change', () => update({sort: sort.value}));
  const reset = $('#lib-reset', root);
  if (reset) reset.addEventListener('click', () => update({platform: '', qualityMin: '', sort: 'title'}));
}

// 根据 POST /music/download 的返回决定提示：已存在的文件不再谎报「已创建任务」
function downloadResultToast(result) {
  if (result.code !== 0) { toast(errText(result), 'error'); return; }
  const t = (result.data && result.data.tasks && result.data.tasks[0]) || null;
  if (t && t.state === 'done' && (t.message === 'already exists' || t.message === '已存在')) {
    toast('文件已存在，无需重复下载', 'ok');
  } else {
    toast('已创建下载任务', 'ok');
  }
}

// 在系统文件管理器里定位曲目的本地文件（需要已经落盘，在线占位记录会报错）
async function openTrackFolder(track) {
  if (!track) return;
  const response = await api('POST', '/music/open-folder', {sha: track.sha});
  toast(response.code === 0 ? '已打开文件所在位置' : errText(response), response.code === 0 ? 'ok' : 'error');
}

// 打开下载落盘根目录（tracks/）
async function openDownloadFolder() {
  const response = await api('POST', '/music/open-folder', {});
  toast(response.code === 0 ? '已打开下载目录' : errText(response), response.code === 0 ? 'ok' : 'error');
}

async function downloadLocal(track) {
  const result = await api('POST', '/music/download', {
    source: 'local', item_id: track.sha, quality: 'auto', filename: '{artist} - {title} [{quality}]'
  });
  downloadResultToast(result);
}

// 曲库条目下载：本地曲目按 sha，在线曲目按 platform + item_id 交给插件
async function downloadTrack(track) {
  if (!track) return;
  const platform = track.platform || 'local';
  if (platform === 'local') return downloadLocal(track);
  const itemId = track.item_id || track.sha;
  if (!itemId) { toast('该曲目缺少在线 id，无法下载', 'error'); return; }
  const quality = (track.quality && (track.quality.quality_id || track.quality.level)) || 'auto';
  const result = await api('POST', '/music/download', {
    source: platform, item_id: String(itemId), quality,
    filename: '{artist} - {title} [{quality}]'
  });
  downloadResultToast(result);
}

// ---------------------------------------------------------------- 从曲库移除
// 曲库记录 = cache/meta 里的 base.json + 库索引；删文件 = tracks/<sha>/ 下的音频与其附属资源。
// 在线曲目若没有落盘（location 是 http 占位），只能移除记录，不存在可删的文件。
function askRemoveConfirm(track, {all = false} = {}) {
  const location = String((track && track.location) || '');
  const hasFile = !!location && !/^https?:\/\//i.test(location);
  const scope = all ? '这首歌的全部音质版本' : '这个音质版本';
  const body =
    '<p class="note" style="font-size:13.5px;line-height:1.7">' +
      '确定要把 <strong>' + esc((track && track.name) || '该曲目') + '</strong> 的' + esc(scope) +
      '移出曲库吗？' +
    '</p>' +
    (hasFile
      ? '<label class="row" style="gap:8px;align-items:center;margin-top:12px">' +
          '<input type="checkbox" data-del checked />' +
          '<span style="font-size:13px">同时删除本地音频文件（不可恢复）</span>' +
        '</label>'
      : '<p class="note" style="margin-top:10px;font-size:12.5px">该曲目没有本地文件，只会移除曲库记录。</p>') +
    '<p class="note" style="margin-top:8px;font-size:12.5px">歌单 / 专辑中的对应条目也会一并清理。</p>';
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (settled) return; settled = true; resolve(value); closeModal(); };
    openModal({
      title: '从曲库移除',
      body,
      footer: '<button class="btn ghost" data-cancel>取消</button>' +
              '<button class="btn danger" data-ok>' + icon('trash', 16) + '移除</button>',
      onMount: (box) => {
        const del = $('[data-del]', box);
        $('[data-ok]', box).addEventListener('click', () => finish({deleteFile: !!(del && del.checked)}));
        $('[data-cancel]', box).addEventListener('click', () => finish(null));
      },
      onClose: () => finish(null)
    });
  });
}

// 移除并刷新；返回是否真的移除了
async function removeTrackFromLibrary(track, {all = false, rerender = null} = {}) {
  if (!track || !track.sha) { toast('无法定位该曲目', 'error'); return false; }
  const choice = await askRemoveConfirm(track, {all});
  if (!choice) return false;
  const query = '?delete_file=' + (choice.deleteFile ? 'true' : 'false') + (all ? '&all=true' : '');
  const result = await api('DELETE', '/music/library/' + encodeURIComponent(track.sha) + query);
  if (result.code !== 0) { toast(errText(result), 'error'); return false; }
  const data = result.data || {};
  const removed = Array.isArray(data.shas) && data.shas.length ? data.shas : [track.sha];
  // 正在播放的被删掉了：复位播放器 UI（后端已停掉播放）
  if (player.current && removed.indexOf(player.current.sha) >= 0) {
    player.current = null;
    stop();
  }
  await loadLibrary(true);
  if (rerender) rerender();
  const parts = ['已移除 ' + (Number(data.removed) || 1) + ' 条曲库记录'];
  if (data.deleted_files || data.deleted_dirs) parts.push('删除 ' + fmtBytes(Number(data.deleted_bytes) || 0));
  if (data.playlist_refs) parts.push('清理歌单 ' + data.playlist_refs + ' 处');
  if (data.album_refs) parts.push('清理专辑 ' + data.album_refs + ' 处');
  if (Array.isArray(data.skipped) && data.skipped.length) parts.push(data.skipped.length + ' 个曲库外文件已保留');
  toast(parts.join(' · '), 'ok');
  return true;
}

function showTrackInfo(group) {
  const best = group.best;
  const rows = [
    ['标题', best.name || ''],
    ['艺术家', best.author || ''],
    ['专辑', best.album || ''],
    ['时长', fmtDuration(best.duration)],
    ['SHA', best.sha || ''],
    ['song_key', best.song_key || ''],
    ['quality_key', best.quality_key || ''],
    ['位置', best.location || ''],
    ['音质', best.quality ? qualityLabel(best.quality) + '（' + qualityDetail(best.quality) + '）' : '—'],
    ['音质版本', group.variants.map((track) => qualityLabel(track.quality)).join(' / ')]
  ];
  openModal({
    title: '曲目信息',
    body: '<dl class="kv">' + rows.map(([key, value]) =>
      '<dt>' + esc(key) + '</dt><dd>' + esc(value) + '</dd>').join('') + '</dl>',
    footer: '<button class="btn primary" data-modal-close>关闭</button>'
  });
}

// ---------------------------------------------------------------- 搜索
// 插件清单（含 capabilities）：搜索引擎与 MV 都靠它判断谁能提供什么
async function ensurePlugins() {
  if (state.pluginsLoaded) return state.plugins;
  const result = await api('GET', '/music/plugins');
  if (result.code === 0 && result.data) {
    state.plugins = safeArray(result.data.plugins);
    // 顺手补齐平台显示名（下载任务、来源标签都用 platformName 显示）
    const meta = Object.assign({}, state.platformMeta);
    for (const plugin of state.plugins) {
      for (const platform of safeArray(plugin.platforms)) {
        if (!meta[platform]) meta[platform] = {name: plugin.name || platform};
      }
    }
    state.platformMeta = meta;
    state.pluginsLoaded = true;
  }
  return state.plugins;
}

function pluginPlatforms(plugins, capability) {
  const list = [];
  const seen = new Set();
  for (const plugin of safeArray(plugins)) {
    if (plugin.enabled === false) continue;
    if (plugin.has_implementation === false) continue;
    if (capability && !safeArray(plugin.capabilities).includes(capability)) continue;
    for (const platform of safeArray(plugin.platforms)) {
      if (!platform || seen.has(platform)) continue;
      seen.add(platform);
      list.push({platform, name: plugin.name || platform, pluginId: plugin.id || ''});
    }
  }
  return list;
}

// 支持 MV 的视频源（与设置页「音源」判断保持一致）
function mvPlatformList(plugins) {
  const list = [];
  const seen = new Set();
  for (const plugin of safeArray(plugins)) {
    if (plugin.enabled === false) continue;
    if (plugin.has_implementation === false) continue;
    const caps = safeArray(plugin.capabilities);
    if (!caps.includes('search_mv') && !caps.includes('get_mv')) continue;
    for (const platform of safeArray(plugin.platforms)) {
      if (!platform || seen.has(platform)) continue;
      seen.add(platform);
      list.push({platform, name: plugin.name || platform, pluginId: plugin.id || ''});
    }
  }
  return list;
}

// 歌单 / 专辑搜索专用：找声明了对应 op 的插件平台
function collectionPlatformList(plugins, op) {
  const list = [];
  const seen = new Set();
  for (const plugin of safeArray(plugins)) {
    if (plugin.enabled === false) continue;
    if (plugin.has_implementation === false) continue;
    if (!safeArray(plugin.capabilities).includes(op)) continue;
    for (const platform of safeArray(plugin.platforms)) {
      if (!platform || seen.has(platform)) continue;
      seen.add(platform);
      list.push({platform, name: plugin.name || platform, pluginId: plugin.id || ''});
    }
  }
  return list;
}
const SEARCH_TYPES = [
  {id: 'song', label: '歌曲'},
  {id: 'album', label: '专辑'},
  {id: 'artist', label: '歌手'},
  {id: 'playlist', label: '歌单'},
  {id: 'mv', label: 'MV'}
];

const SEARCH_QUALITIES = [
  {id: '', label: '不限'},
  {id: 'medium', label: '标准及以上'},
  {id: 'high', label: '高音质及以上'},
  {id: 'lossless', label: '无损及以上'},
  {id: 'hires', label: '仅 Hi-Res'}
];

const SEARCH_DURATIONS = [
  {id: '', label: '不限'},
  {id: 'short', label: '2 分钟内', min: 1, max: 119},
  {id: 'normal', label: '2 - 5 分钟', min: 120, max: 300},
  {id: 'long', label: '5 分钟以上', min: 301, max: 0}
];

const SEARCH_SORTS = [
  {id: 'relevance', label: '相关度'},
  {id: 'title', label: '标题'},
  {id: 'artist', label: '歌手'},
  {id: 'duration', label: '时长'},
  {id: 'quality', label: '音质'}
];

const SEARCH_FILTER_DEFAULTS = {
  sources: [],            // 空 = 全部音源（含本地曲库）
  type: 'song',
  qualityMin: '',
  duration: '',
  availableOnly: false,
  sort: 'relevance'
};

function searchFilters() {
  state.search.filters = Object.assign({}, SEARCH_FILTER_DEFAULTS, state.search.filters || {});
  state.search.filters.sources = safeArray(state.search.filters.sources);
  return state.search.filters;
}

function searchFilterCount() {
  const filters = searchFilters();
  let count = 0;
  if (filters.sources.length) count += 1;
  if (filters.type !== 'song') count += 1;
  // 音质 / 仅可用 / 排序只对歌曲搜索有意义，MV 模式下这三项不参与
  if (filters.type !== 'mv') {
    if (filters.qualityMin) count += 1;
    if (filters.availableOnly) count += 1;
    if (filters.sort !== 'relevance') count += 1;
  }
  if (filters.duration) count += 1;
  return count;
}

function platformName(platform) {
  return platformLabel(platform);
}

// 平台清单（音源筛选 chips）：本地 + 所有已启用插件的平台
async function ensureSearchPlatforms() {
  if (state.platforms && state.platforms.length) return state.platforms;
  const list = [{platform: 'local', name: '本地'}];
  const meta = {};
  const seen = new Set(['local']);
  const [plugins, quality] = await Promise.all([ensurePlugins(), api('GET', '/music/quality')]);
  for (const plugin of safeArray(plugins)) {
    if (plugin.enabled === false) continue;
    for (const platform of safeArray(plugin.platforms)) {
      if (seen.has(platform)) continue;
      seen.add(platform);
      const name = plugin.name || platform;
      meta[platform] = {name};
      list.push({platform, name});
    }
  }
  if (quality.code === 0 && quality.data) {
    for (const source of safeArray(quality.data.sources)) {
      if (!source.platform || seen.has(source.platform)) continue;
      seen.add(source.platform);
      list.push({platform: source.platform, name: source.platform});
    }
  }
  state.platformMeta = Object.assign({}, state.platformMeta, meta);
  state.platforms = list;
  return list;
}

// MV 模式下的视频源清单：只保留声明了 search_mv / get_mv 的插件平台
async function ensureMvPlatforms() {
  if (state.mvPlatforms && state.mvPlatforms.length) return state.mvPlatforms;
  const plugins = await ensurePlugins();
  const list = mvPlatformList(plugins);
  const meta = Object.assign({}, state.platformMeta);
  for (const entry of list) meta[entry.platform] = {name: entry.name};
  state.platformMeta = meta;
  state.mvPlatforms = list;
  return list;
}

// 歌单搜索：声明了 search_playlist 的平台
async function ensurePlaylistPlatforms() {
  if (state.playlistPlatforms && state.playlistPlatforms.length) return state.playlistPlatforms;
  const plugins = await ensurePlugins();
  const list = collectionPlatformList(plugins, 'search_playlist');
  const meta = Object.assign({}, state.platformMeta);
  for (const entry of list) meta[entry.platform] = {name: entry.name};
  state.platformMeta = meta;
  state.playlistPlatforms = list;
  return list;
}

// 专辑搜索：声明了 search_album 的平台
async function ensureAlbumPlatforms() {
  if (state.albumPlatforms && state.albumPlatforms.length) return state.albumPlatforms;
  const plugins = await ensurePlugins();
  const list = collectionPlatformList(plugins, 'search_album');
  const meta = Object.assign({}, state.platformMeta);
  for (const entry of list) meta[entry.platform] = {name: entry.name};
  state.platformMeta = meta;
  state.albumPlatforms = list;
  return list;
}

function sourceChipsMarkup(platforms, mvMode = false) {
  const filters = searchFilters();
  const chips = ['<button class="chip-btn' + (filters.sources.length ? '' : ' active') + '" data-source-chip="">' +
    (mvMode ? '全部视频源' : '全部音源') + '</button>'];
  for (const platform of platforms) {
    const active = filters.sources.includes(platform.platform);
    chips.push('<button class="chip-btn' + (active ? ' active' : '') + '" data-source-chip="' + esc(platform.platform) + '">' +
      esc(platform.name) + '</button>');
  }
  return chips.join('');
}

function typeChipsMarkup() {
  const filters = searchFilters();
  return SEARCH_TYPES.map((type) =>
    '<button class="chip-btn' + (filters.type === type.id ? ' active' : '') + '" data-type-chip="' + esc(type.id) + '">' +
    esc(type.label) + '</button>').join('');
}

function durationSelectMarkup(filters) {
  return '<select id="filter-duration" class="filter-select">' +
    SEARCH_DURATIONS.map((option) => '<option value="' + esc(option.id) + '"' +
      (filters.duration === option.id ? ' selected' : '') + '>' + esc(option.label) + '</option>').join('') +
    '</select>';
}

function filterPanelMarkup(platforms) {
  const filters = searchFilters();
  const mvMode = filters.type === 'mv';
  const typeRow = '<div class="filter-row">' +
      '<span class="filter-label">类型</span>' +
      '<div class="chip-row">' + typeChipsMarkup() + '</div>' +
    '</div>';
  const sourceRow = '<div class="filter-row">' +
      '<span class="filter-label">' + (mvMode ? '视频源' : '音源') + '</span>' +
      '<div class="chip-row" id="filter-sources">' + sourceChipsMarkup(safeArray(platforms), mvMode) + '</div>' +
      (mvMode ? '<span class="note">MV 搜索一次只用一个视频源</span>' : '') +
    '</div>';
  if (mvMode) {
    return '<div class="filter-panel" id="search-filters"' + (state.search.filtersOpen === false ? ' hidden' : '') + '>' +
      sourceRow + typeRow +
      '<div class="filter-row">' +
        '<span class="filter-label">时长</span>' + durationSelectMarkup(filters) +
        '<span class="spacer"></span>' +
        '<span class="note">结果来自视频源，点卡片即可播放；未入库的会先自动入库</span>' +
      '</div>' +
    '</div>';
  }
  return '<div class="filter-panel" id="search-filters"' + (state.search.filtersOpen === false ? ' hidden' : '') + '>' +
    sourceRow + typeRow +
    '<div class="filter-row">' +
      '<span class="filter-label">音质</span>' +
      '<select id="filter-quality" class="filter-select">' +
        SEARCH_QUALITIES.map((option) => '<option value="' + esc(option.id) + '"' +
          (filters.qualityMin === option.id ? ' selected' : '') + '>' + esc(option.label) + '</option>').join('') +
      '</select>' +
      '<span class="filter-label">时长</span>' + durationSelectMarkup(filters) +
      '<span class="filter-label">排序</span>' +
      '<select id="filter-sort" class="filter-select">' +
        SEARCH_SORTS.map((option) => '<option value="' + esc(option.id) + '"' +
          (filters.sort === option.id ? ' selected' : '') + '>' + esc(option.label) + '</option>').join('') +
      '</select>' +
    '</div>' +
    '<div class="filter-row">' +
      '<label class="check-inline"><input type="checkbox" id="filter-available"' + (filters.availableOnly ? ' checked' : '') + ' />' +
        '<span>仅显示可用音源</span></label>' +
      '<label class="check-inline"><input type="checkbox" id="filter-fold"' +
        (state.settings.fold_results !== false ? ' checked' : '') + ' /><span>折叠同一首歌</span></label>' +
      '<span class="spacer"></span>' +
      '<span class="note">本地曲库始终参与搜索，音源可多选</span>' +
    '</div>' +
  '</div>';
}

export async function renderSearch(root) {
  const searchState = state.search;
  // 支持 #search/<关键词> 深链（可分享、可刷新）
  const deepLink = state.routeParams && state.routeParams.id ? String(state.routeParams.id) : '';
  let autoSearch = false;
  if (deepLink && searchState.deepLinkApplied !== deepLink) {
    searchState.deepLinkApplied = deepLink;
    searchState.keyword = deepLink;
    searchState.all = [];
    searchState.data = null;
    searchState.page = 1;
    searchState.total = 0;
    searchState.tab = 'all';
    autoSearch = true;
  }
  const keyword = searchState.keyword || '';
  const filtersType = searchFilters().type;
  const mvMode = filtersType === 'mv';
  const playlistMode = filtersType === 'playlist';
  const albumMode = filtersType === 'album';
  const artistMode = filtersType === 'artist';
  // 不同类型的「可用平台」分别取：MV 用视频源；歌单/专辑用声明了对应 op 的音源；歌手用声明了 search_artist 的音源；其它走通用音源
  let platforms = [];
  if (mvMode) platforms = await ensureMvPlatforms();
  else if (playlistMode) platforms = await ensurePlaylistPlatforms();
  else if (albumMode) platforms = await ensureAlbumPlatforms();
  else if (artistMode) platforms = await ensureArtistPlatforms();
  else platforms = await ensureSearchPlatforms();
  if (!mvMode && (playlistMode || albumMode || artistMode)) {
    // 歌单 / 专辑搜索一次只用一个源
    const allowed = new Set(platforms.map((entry) => entry.platform));
    const kept = searchFilters().sources.filter((platform) => allowed.has(platform)).slice(0, 1);
    if (kept.length !== searchFilters().sources.length) searchFilters().sources = kept;
  }
  if (mvMode) {
    // 视频源只接受一个平台，切到 MV 时清掉不支持的音源
    const allowed = new Set(platforms.map((entry) => entry.platform));
    const kept = searchFilters().sources.filter((platform) => allowed.has(platform)).slice(0, 1);
    if (kept.length !== searchFilters().sources.length) searchFilters().sources = kept;
  }
  const activeCount = searchFilterCount();

  root.innerHTML =
    '<div class="toolbar search-toolbar">' +
      '<div class="search-field">' + icon('search', 18) +
        '<input type="search" id="search-keyword" placeholder="' +
          (mvMode ? '搜索 MV（视频源里的标题 / UP 主）'
            : playlistMode ? '搜索歌单（歌单名 / 创建者）'
            : albumMode ? '搜索专辑（专辑名 / 歌手）'
            : artistMode ? '搜索歌手（歌手名）'
            : '搜索歌曲、歌手、专辑（本地 + 在线音源）') +
          '" value="' + esc(keyword) + '" />' +
      '</div>' +
      '<button class="btn primary" id="search-go">' + icon('search', 16) +
        (mvMode ? '搜索 MV' : playlistMode ? '搜索歌单' : albumMode ? '搜索专辑' : artistMode ? '搜索歌手' : '搜索') + '</button>' +
      '<button class="chip-btn' + (activeCount ? ' active' : '') + '" id="search-filter-toggle" title="展开筛选">' +
        icon('equalizer', 15) + '筛选' + (activeCount ? '<span class="chip-count">' + activeCount + '</span>' : '') + '</button>' +
      (activeCount ? '<button class="chip-btn" id="search-reset">' + icon('refresh', 14) + '重置</button>' : '') +
    '</div>' +
    filterPanelMarkup(platforms) +
    '<div id="search-body"></div>';

  const go = () => {
    const input = $('#search-keyword', root);
    const value = String((input && input.value) || '').trim();
    if (!value) { toast('请输入搜索关键词'); return; }
    searchState.keyword = value;
    searchState.deepLinkApplied = value;
    state.routeParams = Object.assign({}, state.routeParams, {id: value});
    try { history.replaceState(null, '', '#search/' + encodeURIComponent(value)); } catch { /* noop */ }
    runSearch(root);
  };

  // 改了查询条件（音源 / 类型）：先重建页面，再按新条件重新搜索
  // 只要有关键词就重搜——切到 MV 类型时上一条 Mv 缓存是空的，不能拿它当"搜过"的依据
  const refreshAndSearch = () => {
    Promise.resolve(renderSearch(root)).then(() => {
      if (searchState.keyword) runSearch(root);
    });
  };

  $('#search-go', root).addEventListener('click', go);
  const input = $('#search-keyword', root);
  input.addEventListener('keydown', (event) => { if (event.key === 'Enter') go(); });

  $('#search-filter-toggle', root).addEventListener('click', () => {
    searchState.filtersOpen = searchState.filtersOpen === false;
    const panel = $('#search-filters', root);
    if (panel) panel.hidden = !searchState.filtersOpen;
    $('#search-filter-toggle', root).classList.toggle('active', !!searchFilterCount());
  });

  const reset = $('#search-reset', root);
  if (reset) reset.addEventListener('click', () => {
    searchState.filters = Object.assign({}, SEARCH_FILTER_DEFAULTS);
    refreshAndSearch();
  });

  // 音源 chips：歌曲模式多选，MV 模式单选（后端 mv/search 只接受一个 source）
  const chipHost = $('#filter-sources', root);
  if (chipHost) {
    $$('[data-source-chip]', chipHost).forEach((button) => {
      button.addEventListener('click', () => {
        const platform = button.dataset.sourceChip;
        const current = searchFilters().sources.slice();
        let next;
        if (searchFilters().type === 'mv') {
          next = platform ? [platform] : [];
        } else if (!platform) {
          next = [];
        } else if (current.includes(platform)) {
          next = current.filter((entry) => entry !== platform);
        } else {
          next = current.concat(platform);
        }
        searchFilters().sources = next;
        searchState.filtersOpen = true;
        refreshAndSearch();
      });
    });
  }

  $$('[data-type-chip]', root).forEach((button) => {
    button.addEventListener('click', () => {
      const nextType = button.dataset.typeChip || 'song';
      if (nextType === searchFilters().type) return;
      searchFilters().type = nextType;
      // 歌曲音源与视频源不是同一批平台，切换类型时清空已选音源
      searchFilters().sources = [];
      searchState.filtersOpen = true;
      refreshAndSearch();
    });
  });

  const quality = $('#filter-quality', root);
  if (quality) quality.addEventListener('change', () => {
    searchFilters().qualityMin = quality.value;
    renderSearchResults(root);
    renderFilterSummary(root);
  });
  const duration = $('#filter-duration', root);
  if (duration) duration.addEventListener('change', () => {
    searchFilters().duration = duration.value;
    if (searchFilters().type === 'mv') renderMvSearchResults(root);
    else renderSearchResults(root);
    renderFilterSummary(root);
  });
  const sort = $('#filter-sort', root);
  if (sort) sort.addEventListener('change', () => {
    searchFilters().sort = sort.value;
    renderSearchResults(root);
    renderFilterSummary(root);
  });
  const available = $('#filter-available', root);
  if (available) available.addEventListener('change', () => {
    searchFilters().availableOnly = available.checked;
    renderSearchResults(root);
    renderFilterSummary(root);
  });
  const foldInput = $('#filter-fold', root);
  if (foldInput) foldInput.addEventListener('change', async () => {
    await saveSettings({fold_results: foldInput.checked});
    if (searchState.keyword) runSearch(root);
  });

  if (autoSearch) runSearch(root);
  else if (mvMode && searchState.mv && safeArray(searchState.mv.list).length) renderMvSearchResults(root);
  else if (playlistMode && searchState.playlist && safeArray(searchState.playlist.list).length) renderPlaylistSearchResults(root);
  else if (albumMode && searchState.album && safeArray(searchState.album.list).length) renderAlbumSearchResults(root);
  else if (!mvMode && searchState.all && searchState.all.length) renderSearchResults(root);
  else if (artistMode && searchState.artist && safeArray(searchState.artist.list).length) renderArtistSearchResults(root);
  else if (searchState.keyword && searchState.loading) $('#search-body', root).innerHTML = spinnerRow('正在搜索…');
  else if (mvMode) {
    $('#search-body', root).innerHTML = platforms.length
      ? emptyState('搜索 MV', '输入关键词后按 Enter，结果来自视频源，点卡片即可播放。', 'video')
      : emptyState('没有可用的视频源', '没有插件声明 search_mv / get_mv 能力，请先在「设置 → 音源」检查插件。', 'video');
  } else if (playlistMode) {
    $('#search-body', root).innerHTML = platforms.length
      ? emptyState('搜索歌单', '输入关键词后按 Enter，结果来自音源，点卡片即可把曲目同步到本地库。', 'playlist')
      : emptyState('没有可用的歌单搜索源', '没有插件声明 search_playlist 能力，请先在「设置 → 音源」检查插件。', 'playlist');
  } else if (albumMode) {
    $('#search-body', root).innerHTML = platforms.length
      ? emptyState('搜索专辑', '输入关键词后按 Enter，结果来自音源，点卡片即可把曲目同步到本地专辑。', 'album')
      : emptyState('没有可用的专辑搜索源', '没有插件声明 search_album 能力，请先在「设置 → 音源」检查插件。', 'album');
  } else if (artistMode) {
    $('#search-body', root).innerHTML = platforms.length
      ? emptyState('搜索歌手', '输入关键词后按 Enter，点卡片即可进入歌手主页。', 'star')
      : emptyState('没有可用的歌手搜索源', '没有插件声明 search_artist 能力，请先在「设置 → 音源」检查插件。', 'star');
  } else {
    $('#search-body', root).innerHTML = emptyState('搜索本地与在线音乐',
      '输入关键词后按 Enter；可以在筛选里选择单个音源，或勾选本地曲库之外的平台。', 'search');
  }
}

// 筛选项变化后同步顶部按钮上的计数
function renderFilterSummary(root) {
  const button = $('#search-filter-toggle', root);
  if (!button) return;
  const count = searchFilterCount();
  button.classList.toggle('active', count > 0);
  const badge = $('.chip-count', button);
  if (count > 0) {
    if (badge) badge.textContent = count;
    else button.insertAdjacentHTML('beforeend', '<span class="chip-count">' + count + '</span>');
  } else if (badge) {
    badge.remove();
  }
}

async function runSearch(root, {append = false} = {}) {
  const filters = searchFilters();
  const keyword = String(state.search.keyword || '').trim();
  if (!keyword) { toast('请输入搜索关键词'); return; }
  // MV 走视频源接口，和歌曲搜索是两条链路
  if (filters.type === 'mv') { await runMvSearch(root); return; }
  // 歌单 / 专辑走专属搜索接口（结果不是曲目）
  if (filters.type === 'playlist') { await runPlaylistSearch(root); return; }
  if (filters.type === 'album') { await runAlbumSearch(root); return; }
  // 歌手走 request(op=search_artist)，结果是一组歌手实体（不是曲目）
  if (filters.type === 'artist') { await runArtistSearch(root); return; }
  const host = $('#search-body', root);
  const page = append ? (Number(state.search.page) || 1) + 1 : 1;
  const scope = filters.sources.length ? filters.sources.map(platformName).join(' / ') : '全部音源';
  state.search.loading = true;
  if (!append && host) host.innerHTML = spinnerRow('正在搜索「' + keyword + '」· ' + scope);

  const payload = {
    keyword,
    page,
    page_size: 60,
    strategy: 'fanout',
    type: filters.type,
    fold: state.settings.fold_results !== false
  };
  if (filters.sources.length) payload.sources = filters.sources;

  const result = await api('POST', '/music/search', payload);
  state.search.loading = false;
  if (result.code !== 0) {
    if (append) { toast(errText(result), 'error'); return; }
    state.search.all = [];
    state.search.data = null;
    state.search.statuses = [];
    if (host) host.innerHTML = emptyState('搜索失败', errText(result), 'search');
    return;
  }

  const data = result.data || {};
  const incoming = safeArray(data.results);
  state.search.data = data;
  state.search.page = Number(data.page) || page;
  state.search.total = Number(data.total) || incoming.length;
  state.search.statuses = safeArray(data.sources_status);
  if (append) {
    const seen = new Set(state.search.all.map((item) => String(item.group_key || '') + '|' + String(item.title || '')));
    for (const item of incoming) {
      const key = String(item.group_key || '') + '|' + String(item.title || '');
      if (seen.has(key)) continue;
      seen.add(key);
      state.search.all.push(item);
    }
  } else {
    state.search.all = incoming;
    state.search.tab = 'all';
  }
  renderSearchResults(root);
}

// ---------------------------------------------------------------- MV 搜索（在搜索页的 MV 类型下）
function mvPayloadFromResult(mv, source) {
  const data = mv || {};
  return {
    sha: data.sha || '',
    platform: data.platform || source || '',
    item_id: String(data.id || data.item_id || ''),
    version: data.version || '',
    title: data.title || data.name || '',
    artist: data.artist || data.author || '',
    album: data.album || '',
    location: data.url || data.location || '',
    pic: data.pic || data.cover || '',
    duration_seconds: Number(data.duration || data.duration_seconds || 0) || 0,
    width: Number(data.width) || 0,
    height: Number(data.height) || 0
  };
}

function upsertMvState(mv) {
  if (!mv || !mv.sha) return null;
  const list = safeArray(state.mvs);
  const index = list.findIndex((entry) => entry.sha === mv.sha);
  if (index >= 0) list[index] = Object.assign({}, list[index], mv);
  else list.push(mv);
  state.mvs = list;
  return index >= 0 ? list[index] : mv;
}

async function saveMvResult(mv, source) {
  const payload = mvPayloadFromResult(mv, source);
  if (!payload.sha) { toast('该结果没有 sha，无法入库'); return false; }
  if (!payload.item_id) payload.item_id = payload.sha;
  const response = await api('POST', '/music/mv', payload);
  if (response.code !== 0) { toast(errText(response), 'error'); return false; }
  upsertMvState(response.data || payload);
  toast('已加入 MV 库', 'ok');
  return true;
}

async function playMvResult(mv, source, root) {
  const payload = mvPayloadFromResult(mv, source);
  if (!payload.sha) { toast('该结果没有 sha，无法播放'); return; }
  const existing = safeArray(state.mvs).find((entry) => entry.sha === payload.sha);
  if (!existing) {
    // 视频源的搜索结果通常只有 ID，先入库拿到稳定记录，再走 open 解析播放地址
    const response = await api('POST', '/music/mv', payload);
    if (response.code !== 0) { toast(errText(response), 'error'); return; }
    upsertMvState(response.data || payload);
    if (root) renderMvSearchResults(root);
  }
  playMv(payload.sha, true);
}

async function runMvSearch(root) {
  const filters = searchFilters();
  const keyword = String(state.search.keyword || '').trim();
  if (!keyword) { toast('请输入 MV 关键词'); return; }
  const platform = filters.sources[0] || '';
  const host = $('#search-body', root);
  state.search.loading = true;
  if (host) {
    host.innerHTML = spinnerRow('正在搜索 MV「' + keyword + '」· ' +
      (platform ? platformName(platform) : '自动选择视频源'));
  }
  const payload = {keyword, page: 1, page_size: 40};
  if (platform) payload.source = platform;
  const result = await api('POST', '/music/mv/search', payload);
  state.search.loading = false;
  if (result.code !== 0) {
    state.search.mv = null;
    if (host) host.innerHTML = emptyState('MV 搜索失败', errText(result), 'video');
    return;
  }
  const value = result.data && result.data.result;
  let list = [];
  if (Array.isArray(value)) list = value;
  else if (value && Array.isArray(value.mvs)) list = value.mvs;
  else if (value && Array.isArray(value.results)) list = value.results;
  else if (value && Array.isArray(value.list)) list = value.list;
  else if (value && typeof value === 'object') list = [value];
  state.search.mv = {
    list,
    source: (result.data && result.data.source) || platform,
    op: (result.data && result.data.op) || '',
    keyword
  };
  renderMvSearchResults(root);
}

// 歌单搜索：与 MV 类似的形态（结果不是曲目，是「歌单条目」）
async function runPlaylistSearch(root) {
  const filters = searchFilters();
  const keyword = String(state.search.keyword || '').trim();
  if (!keyword) { toast('请输入歌单关键词'); return; }
  const source = filters.sources[0] || '';
  const host = $('#search-body', root);
  state.search.loading = true;
  if (host) {
    host.innerHTML = spinnerRow('正在搜索歌单「' + keyword + '」· ' +
      (source ? platformName(source) : '自动选择'));
  }
  const payload = {keyword, page: 1, page_size: 30};
  if (source) payload.source = source;
  const result = await api('POST', '/music/playlist/search', payload);
  state.search.loading = false;
  if (result.code !== 0) {
    state.search.playlist = null;
    if (host) host.innerHTML = emptyState('歌单搜索失败', errText(result), 'playlist');
    return;
  }
  const value = result.data && result.data.result;
  let list = [];
  if (Array.isArray(value)) list = value;
  else if (value && Array.isArray(value.playlists)) list = value.playlists;
  else if (value && Array.isArray(value.results)) list = value.results;
  else if (value && Array.isArray(value.list)) list = value.list;
  else if (value && Array.isArray(value.items)) list = value.items;
  state.search.playlist = {
    list,
    source: (result.data && result.data.source) || source,
    keyword
  };
  renderPlaylistSearchResults(root);
}

async function runAlbumSearch(root) {
  const filters = searchFilters();
  const keyword = String(state.search.keyword || '').trim();
  if (!keyword) { toast('请输入专辑关键词'); return; }
  const source = filters.sources[0] || '';
  const host = $('#search-body', root);
  state.search.loading = true;
  if (host) {
    host.innerHTML = spinnerRow('正在搜索专辑「' + keyword + '」· ' +
      (source ? platformName(source) : '自动选择'));
  }
  const payload = {keyword, page: 1, page_size: 30};
  if (source) payload.source = source;
  const result = await api('POST', '/music/album/search', payload);
  state.search.loading = false;
  if (result.code !== 0) {
    state.search.album = null;
    if (host) host.innerHTML = emptyState('专辑搜索失败', errText(result), 'album');
    return;
  }
  const value = result.data && result.data.result;
  let list = [];
  if (Array.isArray(value)) list = value;
  else if (value && Array.isArray(value.albums)) list = value.albums;
  else if (value && Array.isArray(value.results)) list = value.results;
  else if (value && Array.isArray(value.list)) list = value.list;
  else if (value && Array.isArray(value.items)) list = value.items;
  state.search.album = {
    list,
    source: (result.data && result.data.source) || source,
    keyword
  };
  renderAlbumSearchResults(root);
}

function renderPlaylistSearchResults(root) {
  const host = $('#search-body', root);
  if (!host) return;
  const data = state.search.playlist;
  if (!data) {
    host.innerHTML = emptyState('搜索歌单',
      '输入关键词后按 Enter，结果来自音源；点卡片会把歌单曲目同步到本地库。', 'playlist');
    return;
  }
  const list = safeArray(data.list);
  const source = String(data.source || '');
  const keyword = String(data.keyword || '');
  if (!list.length) {
    host.innerHTML = emptyState('没有找到歌单',
      '换一个关键词，或在筛选里换一个音源。', 'playlist');
    return;
  }
  host.innerHTML =
    '<div class="section-head"><h2>歌单结果</h2><span class="sub">' +
      esc(platformName(source) || '音源') + ' · ' + list.length + ' 个' +
      (keyword ? ' · 「' + esc(keyword) + '」' : '') + '</span></div>' +
    '<div class="card-grid">' + list.map((pl, index) => {
      const id = pl.id || pl.item_id || '';
      const title = pl.name || ('歌单 ' + (index + 1));
      const creator = pl.creator || pl.owner || '';
      const tracks = Number(pl.track_count || pl.size || 0);
      const pic = pl.pic || pl.cover || '';
      return '<div class="media-card collection-card" data-pl-result="' + index + '">' +
        '<div class="media-art wide">' + coverMarkup(pic, 34) +
          (tracks ? '<span class="collection-count">' + tracks + ' 首</span>' : '') +
        '</div>' +
        '<div class="media-title">' + esc(title) + '</div>' +
        '<div class="media-sub">' + esc(creator || platformName(pl.platform || source)) + '</div>' +
        '<div class="media-actions">' +
          '<button class="icon-btn" data-pl-result-import="' + index + '" title="导入到本地库">' + icon('plus', 17) + '</button>' +
        '</div>' +
      '</div>';
    }).join('') + '</div>';

  const pick = (value) => list[Number(value)] || null;
  $$('[data-pl-result]', host).forEach((card) =>
    card.addEventListener('click', () => importPlaylistResult(pick(card.dataset.plResult), source, root)));
  $$('[data-pl-result-import]', host).forEach((button) =>
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      importPlaylistResult(pick(button.dataset.plResultImport), source, root);
    }));
}

function renderAlbumSearchResults(root) {
  const host = $('#search-body', root);
  if (!host) return;
  const data = state.search.album;
  if (!data) {
    host.innerHTML = emptyState('搜索专辑',
      '输入关键词后按 Enter，结果来自音源；点卡片会把专辑曲目同步到本地库。', 'album');
    return;
  }
  const list = safeArray(data.list);
  const source = String(data.source || '');
  const keyword = String(data.keyword || '');
  if (!list.length) {
    host.innerHTML = emptyState('没有找到专辑',
      '换一个关键词，或在筛选里换一个音源。', 'album');
    return;
  }
  host.innerHTML =
    '<div class="section-head"><h2>专辑结果</h2><span class="sub">' +
      esc(platformName(source) || '音源') + ' · ' + list.length + ' 张' +
      (keyword ? ' · 「' + esc(keyword) + '」' : '') + '</span></div>' +
    '<div class="card-grid">' + list.map((album, index) => {
      const id = album.id || album.item_id || '';
      const title = album.name || ('专辑 ' + (index + 1));
      const artist = album.artist || '';
      const tracks = Number(album.size || album.track_count || 0);
      const pic = album.pic || album.cover || '';
      const year = Number(album.year || 0);
      return '<div class="media-card collection-card" data-al-result="' + index + '">' +
        '<div class="media-art wide">' + coverMarkup(pic, 34) +
          (year ? '<span class="collection-count">' + year + '</span>' : '') +
        '</div>' +
        '<div class="media-title">' + esc(title) + '</div>' +
        '<div class="media-sub">' + esc(artist || platformName(album.platform || source)) + '</div>' +
        '<div class="media-actions">' +
          '<button class="icon-btn" data-al-result-import="' + index + '" title="导入到本地库">' + icon('plus', 17) + '</button>' +
        '</div>' +
      '</div>';
    }).join('') + '</div>';

  const pick = (value) => list[Number(value)] || null;
  $$('[data-al-result]', host).forEach((card) =>
    card.addEventListener('click', () => importAlbumResult(pick(card.dataset.alResult), source, root)));
  $$('[data-al-result-import]', host).forEach((button) =>
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      importAlbumResult(pick(button.dataset.alResultImport), source, root);
    }));
}

// 从插件结果导入歌单：POST /music/playlist 注册后用 sync 拉曲目
async function importPlaylistResult(item, source, root) {
  const id = String((item && (item.id || item.item_id)) || '').trim();
  if (!id) { toast('该结果缺少 id', 'error'); return; }
  toast('正在导入「' + ((item && item.name) || id) + '」…');
  const created = await api('POST', '/music/playlist', {
    name: (item && item.name) || ('歌单 ' + id),
    description: (item && item.description) || '',
    platform: source || (item && item.platform) || 'netease',
    source_id: id,
    pic: (item && item.pic) || ''
  });
  if (created.code !== 0) { toast(errText(created), 'error'); return; }
  const playlistId = (created.data && created.data.id) || '';
  if (!playlistId) { toast('导入失败：未返回歌单 id', 'error'); return; }
  const sync = await api('POST', '/music/playlist/' + encodeURIComponent(playlistId) + '/sync', {});
  if (sync.code !== 0) { toast('导入失败：' + errText(sync), 'error'); return; }
  const count = (sync.data && sync.data.tracks && sync.data.tracks.length) || 0;
  toast('已导入「' + ((item && item.name) || id) + '」共 ' + count + ' 首', 'ok');
  if (root) renderPlaylistSearchResults(root);
}

async function importAlbumResult(item, source, root) {
  const id = String((item && (item.id || item.item_id)) || '').trim();
  if (!id) { toast('该结果缺少 id', 'error'); return; }
  toast('正在导入「' + ((item && item.name) || id) + '」…');
  const created = await api('POST', '/music/album', {
    name: (item && item.name) || ('专辑 ' + id),
    artist: (item && item.artist) || '',
    platform: source || (item && item.platform) || 'netease',
    source_id: id,
    pic: (item && item.pic) || '',
    year: (item && (item.year || 0)) || 0
  });
  if (created.code !== 0) { toast(errText(created), 'error'); return; }
  const albumId = (created.data && created.data.id) || '';
  if (!albumId) { toast('导入失败：未返回专辑 id', 'error'); return; }
  const sync = await api('POST', '/music/album/' + encodeURIComponent(albumId) + '/sync', {});
  if (sync.code !== 0) { toast('导入失败：' + errText(sync), 'error'); return; }
  const count = (sync.data && sync.data.tracks && sync.data.tracks.length) || 0;
  toast('已导入「' + ((item && item.name) || id) + '」共 ' + count + ' 首', 'ok');
  if (root) renderAlbumSearchResults(root);
}

function renderMvSearchResults(root) {
  const host = $('#search-body', root);
  if (!host) return;
  const data = state.search.mv;
  if (!data) {
    host.innerHTML = emptyState('搜索 MV', '输入关键词后按 Enter，结果来自视频源，点卡片即可播放。', 'video');
    return;
  }
  const source = String(data.source || '');
  const keyword = String(data.keyword || '');
  const filters = searchFilters();
  const durationOption = SEARCH_DURATIONS.find((option) => option.id === filters.duration) || null;
  const list = safeArray(data.list).filter((mv) => {
    if (!durationOption || !durationOption.id) return true;
    const duration = Number(mv.duration || mv.duration_seconds || 0) || 0;
    if (!duration) return true;
    if (durationOption.min && duration < durationOption.min) return false;
    if (durationOption.max && duration > durationOption.max) return false;
    return true;
  });
  if (!list.length) {
    host.innerHTML = emptyState('没有找到 MV', '换个关键词，或在「视频源」里换一个平台再试。', 'video');
    return;
  }
  const savedShas = new Set(safeArray(state.mvs).map((entry) => entry.sha).filter(Boolean));
  host.innerHTML =
    '<div class="section-head"><h2>视频源结果</h2><span class="sub">' +
      esc(platformName(source) || '视频源') + ' · ' + list.length + ' 个' +
      (keyword ? ' · 「' + esc(keyword) + '」' : '') + '</span></div>' +
    '<div class="card-grid mv-grid">' + list.map((mv, index) => {
      const title = mv.title || mv.name || ('结果 ' + (index + 1));
      const artist = mv.artist || mv.author || '';
      const pic = mv.pic || mv.cover || '';
      const duration = Number(mv.duration || mv.duration_seconds || 0) || 0;
      const saved = !!mv.sha && savedShas.has(mv.sha);
      return '<div class="media-card mv-card" data-mv-result="' + index + '">' +
        '<div class="media-art wide">' + coverMarkup(pic, 34) +
          (duration ? '<span class="mv-duration">' + esc(fmtDuration(duration)) + '</span>' : '') +
        '</div>' +
        '<div class="media-title">' + esc(title) + '</div>' +
        '<div class="media-sub">' + esc(artist || platformName(mv.platform || source)) + '</div>' +
        '<div class="media-actions">' +
          '<button class="icon-btn" data-mv-result-play="' + index + '" title="播放">' + icon('play', 17) + '</button>' +
          '<button class="icon-btn' + (saved ? ' active' : '') + '" data-mv-result-save="' + index +
            '" title="' + (saved ? '已在 MV 库' : '加入 MV 库') + '">' + icon(saved ? 'check' : 'plus', 17) + '</button>' +
        '</div>' +
      '</div>';
    }).join('') + '</div>';

  const pick = (value) => list[Number(value)] || null;
  $$('[data-mv-result]', host).forEach((card) =>
    card.addEventListener('click', () => playMvResult(pick(card.dataset.mvResult), source, root)));
  $$('[data-mv-result-play]', host).forEach((button) =>
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      playMvResult(pick(button.dataset.mvResultPlay), source, root);
    }));
  $$('[data-mv-result-save]', host).forEach((button) =>
    button.addEventListener('click', async (event) => {
      event.stopPropagation();
      if (await saveMvResult(pick(button.dataset.mvResultSave), source)) renderMvSearchResults(root);
    }));
}

function sourceQualityList(source) {
  const list = safeArray(source && source.qualities).slice();
  if (source && source.quality) {
    const id = source.quality.quality_id || source.quality.level;
    if (!list.some((quality) => (quality.quality_id || quality.level) === id)) list.unshift(source.quality);
  }
  return sortQualities(list);
}

function bestQualityOf(sources) {
  const list = [];
  for (const source of safeArray(sources)) {
    if (source.available === false) continue;
    list.push(...sourceQualityList(source));
  }
  return sortQualities(list)[0] || null;
}

function maxQualityRank(sources) {
  let best = 0;
  for (const source of safeArray(sources)) {
    if (source.available === false) continue;
    for (const quality of sourceQualityList(source)) best = Math.max(best, qualityRank(quality && quality.level));
  }
  return best;
}

// 把服务端结果按当前筛选条件过一遍（不含 tab），返回 [{item, sources}]
function filteredResultEntries() {
  const filters = searchFilters();
  const durationOption = SEARCH_DURATIONS.find((option) => option.id === filters.duration) || null;
  const qualityNeed = filters.qualityMin ? qualityRank(filters.qualityMin) : 0;
  const entries = [];
  for (const item of state.search.all || []) {
    const sources = safeArray(item.sources).filter((source) => {
      if (filters.availableOnly && source.available === false) return false;
      if (filters.sources.length && !filters.sources.includes(source.platform)) return false;
      return true;
    });
    if (!sources.length) continue;
    if (qualityNeed && maxQualityRank(sources) < qualityNeed) continue;
    if (durationOption && durationOption.id) {
      const duration = Number(item.duration) || 0;
      if (duration > 0) {
        if (durationOption.min && duration < durationOption.min) continue;
        if (durationOption.max && duration > durationOption.max) continue;
      }
    }
    entries.push({item, sources});
  }

  const sort = filters.sort;
  if (sort === 'quality') {
    entries.sort((a, b) => maxQualityRank(b.sources) - maxQualityRank(a.sources));
  } else if (sort === 'duration') {
    entries.sort((a, b) => (Number(a.item.duration) || 0) - (Number(b.item.duration) || 0));
  } else if (sort === 'artist') {
    entries.sort((a, b) => String((a.item.artists || [])[0] || '').localeCompare(String((b.item.artists || [])[0] || ''), 'zh-Hans-CN'));
  } else if (sort === 'title') {
    entries.sort((a, b) => String(a.item.title || '').localeCompare(String(b.item.title || ''), 'zh-Hans-CN'));
  }
  return entries;
}

function statusMarkup(statuses) {
  if (!statuses.length) return '';
  return '<div class="source-status">' + statuses.map((status) => {
    const ok = status.code === 0;
    return '<span class="source-status-item' + (ok ? '' : ' bad') + '">' +
      esc(platformName(status.platform)) +
      (ok
        ? '<span class="muted">' + (status.result_count || 0) + ' 条 · ' + (status.elapsed_ms || 0) + 'ms</span>'
        : '<span class="muted">' + esc(errText(status).replace(/^.*?：/, '')) + '</span>') +
      '</span>';
  }).join('') + '</div>';
}

function renderSearchResults(root) {
  const host = $('#search-body', root);
  if (!host) return;
  const entriesAll = filteredResultEntries();
  let tab = state.search.tab || 'all';

  // 各音源（按当前筛选后的结果计数）
  const counts = new Map();
  for (const entry of entriesAll) {
    const platforms = new Set(safeArray(entry.sources).map((source) => source.platform));
    for (const platform of platforms) counts.set(platform, (counts.get(platform) || 0) + 1);
  }
  // 当前 tab 已不在结果里（例如切换了音源）→ 回到全部
  if (tab !== 'all' && !counts.has(tab)) {
    tab = 'all';
    state.search.tab = 'all';
  }
  const entries = tab === 'all' ? entriesAll : entriesAll.filter((entry) =>
    safeArray(entry.sources).some((source) => source.platform === tab));

  const tabs = ['<button class="source-tab' + (tab === 'all' ? ' active' : '') + '" data-tab="all">全部<span>' + entriesAll.length + '</span></button>'];
  for (const [platform, count] of counts) {
    tabs.push('<button class="source-tab' + (tab === platform ? ' active' : '') + '" data-tab="' + esc(platform) + '">' +
      esc(platformName(platform)) + '<span>' + count + '</span></button>');
  }

  if (!entriesAll.length) {
    host.innerHTML =
      '<div class="section-head"><h2>搜索结果</h2><span class="sub">0 首</span></div>' +
      emptyState(state.search.all && state.search.all.length ? '没有符合筛选条件的结果' : '没有找到结果',
        state.search.all && state.search.all.length ? '试试放宽音质 / 时长条件，或切换音源。' : '换一个关键词，或在筛选里选择其它音源。', 'search') +
      statusMarkup(safeArray(state.search.statuses));
    bindSearchTabs(root, host);
    return;
  }

  const cards = entries.map((entry, ri) => {
    const item = entry.item;
    const sources = entry.sources;
    const available = sources.filter((source) => source.available !== false);
    const best = available[0] || sources[0];
    const bestQuality = bestQualityOf(sources);
    const platforms = sources.map((source) =>
      '<span class="mini-tag' + (source.platform === 'local' ? ' local' : '') + (source.available === false ? ' off' : '') + '">' +
      esc(platformName(source.platform)) + '</span>').join('');
    return '<div class="result-card" data-card="' + ri + '">' +
      '<div class="result-head" data-result="' + ri + '">' +
        '<div class="track-cover">' + coverMarkup(item.pic || (best && best.base && best.base.pic)) + '</div>' +
        '<div class="track-main">' +
          '<div class="track-title-line"><span class="track-name">' + esc(item.title || '(未知标题)') + '</span>' +
            (bestQuality ? qualityBadge(bestQuality) : '') + '</div>' +
          '<div class="track-sub"><span>' + esc((item.artists || []).join(' / ') || '未知艺术家') + '</span>' +
            (item.album ? '<span class="sep">·</span><span>' + esc(item.album) + '</span>' : '') +
            '<span class="sep">·</span><span>' + fmtDuration(item.duration) + '</span></div>' +
          '<div class="mini-tags">' + platforms + '</div>' +
        '</div>' +
        '<div class="search-actions">' +
          '<button class="icon-btn" data-result-play="' + ri + '" title="播放最佳音源">' + icon('play', 18) + '</button>' +
          '<button class="icon-btn" data-add-playlist="' + ri + '" title="加入歌单">' + icon('plus', 18) + '</button>' +
          '<button class="icon-btn" data-add-album="' + ri + '" title="加入专辑">' + icon('album', 18) + '</button>' +
          '<button class="icon-btn" data-more="' + ri + '" title="更多">' + icon('more', 18) + '</button>' +
        '</div>' +
        '<span class="caret">' + icon('chevronDown', 18) + '</span>' +
      '</div>' +
      '<div class="result-sources" data-sources="' + ri + '" hidden="true">' +
        sources.map((source, si) => {
          const qualities = sourceQualityList(source);
          const current = source.quality || qualities[0] || null;
          const options = (source.platform === 'local' ? [] : qualities).map((quality) => {
            const id = quality.quality_id || quality.level || '';
            return '<option value="' + esc(id) + '"' +
              (current && (current.quality_id === id || current.level === id) ? ' selected' : '') + '>' +
              esc(qualityLabel(quality)) + (qualityDetail(quality) ? ' · ' + esc(qualityDetail(quality)) : '') + '</option>';
          }).join('');
          return '<div class="source-row' + (source.available === false ? ' unavailable' : '') + '">' +
            '<span class="source-platform' + (source.platform === 'local' ? ' local' : '') + '">' +
              esc(platformName(source.platform)) + '</span>' +
            '<div class="source-q">' +
              (source.available === false
                ? '<span class="source-reason">' + esc(source.reason || '不可用') + '</span>'
                : (options
                  ? '<select class="quality-pick" data-quality-select="' + ri + ':' + si + '">' + options + '</select>'
                  : '<span class="q-badge q-' + esc((current && current.level) || 'unknown') + '">' + esc(qualityLabel(current)) + '</span>')) +
              (source.platform === 'local' && source.base && source.base.location
                ? '<span class="tag">本地文件</span>'
                : (source.location ? '<span class="tag">含播放地址</span>' : '')) +
            '</div>' +
            '<div class="source-actions">' +
              (source.available === false ? '' :
                '<button class="icon-btn" data-source-play="' + ri + ':' + si + '" title="播放此音源">' + icon('play', 17) + '</button>' +
                '<button class="icon-btn" data-source-lyric="' + ri + ':' + si + '" title="查看歌词">' + icon('lyric', 17) + '</button>' +
                '<button class="icon-btn" data-source-download="' + ri + ':' + si + '" title="下载">' + icon('download', 17) + '</button>') +
            '</div>' +
          '</div>';
        }).join('') +
      '</div></div>';
  }).join('');

  const shown = state.search.all.length;
  const total = Number(state.search.total) || shown;
  host.innerHTML =
    '<div class="source-tabs" id="search-tabs">' + tabs.join('') + '</div>' +
    statusMarkup(safeArray(state.search.statuses)) +
    '<div class="section-head" style="margin-top:12px"><h2>搜索结果</h2><span class="sub">' +
      (tab === 'all' ? '' : esc(platformName(tab)) + ' · ') + entries.length + ' 首 · 已加载 ' + shown + ' / 共 ' + total + '</span></div>' +
    cards +
    (shown < total
      ? '<div class="row" style="justify-content:center;margin-top:18px"><button class="btn" id="search-more">' +
        icon('chevronDown', 16) + '加载更多结果</button></div>'
      : '');

  bindSearchTabs(root, host);

  const playSource = async (ri, si, qualityId) => {
    const source = entries[ri] && entries[ri].sources[si];
    const item = entries[ri] && entries[ri].item;
    if (!source || !item) return;
    let quality = source.quality;
    if (qualityId) {
      quality = sourceQualityList(source).find((entry) => (entry.quality_id || entry.level) === qualityId) || quality;
    }
    const playable = itemFromResult(item, source, quality);
    // 单曲播放：把该曲目追加进播放队列并立即播放，不清空原有队列
    await enqueueAndPlay([playable], {quality: qualityId || 'auto'});
    renderSearchResults(root);
  };

  const itemsOf = (ri) => {
    const entry = entries[ri];
    if (!entry) return [];
    return entry.sources
      .filter((source) => source.available !== false)
      .map((source) => itemFromResult(entry.item, source, source.quality));
  };

  $$('[data-result]', host).forEach((head) => {
    head.addEventListener('click', () => {
      const box = $('[data-sources="' + head.dataset.result + '"]', host);
      if (!box) return;
      box.hidden = !box.hidden;
      const caret = $('.caret', head);
      if (caret) caret.classList.toggle('open', !box.hidden);
    });
  });

  $$('[data-result-play]', host).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const ri = Number(button.dataset.resultPlay);
      const sources = (entries[ri] && entries[ri].sources) || [];
      const index = sources.findIndex((source) => source.available !== false);
      if (index >= 0) playSource(ri, index);
      else toast('该歌曲没有可用音源', 'error');
    });
  });

  $$('[data-source-play]', host).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const [ri, si] = button.dataset.sourcePlay.split(':').map(Number);
      const select = $('[data-quality-select="' + ri + ':' + si + '"]', host);
      playSource(ri, si, select ? select.value : '');
    });
  });

  $$('[data-source-lyric]', host).forEach((button) => {
    button.addEventListener('click', async (event) => {
      event.stopPropagation();
      const [ri, si] = button.dataset.sourceLyric.split(':').map(Number);
      const entry = entries[ri];
      const source = entry && entry.sources[si];
      if (!source) return;
      const sha = (source.base && source.base.sha) || (source.platform === 'local' ? source.id : '');
      const url = source.platform === 'local'
        ? '/music/lyric?sha=' + encodeURIComponent(sha)
        : '/music/lyric?source=' + encodeURIComponent(source.platform) + '&id=' + encodeURIComponent(source.id || sha);
      const response = await api('GET', url);
      const lines = lyricLinesFromData(response.code === 0 && response.data ? response.data : null);
      openModal({
        title: (entry.item.title || '') + ' · 歌词',
        body: '<div class="lyric-panel">' + (lines.length
          ? lines.map((line) => '<div class="lyric-line">' + esc(line.text || '') + '</div>').join('')
          : '<div class="lyric-placeholder">' + esc(response.code === 0 ? '暂无歌词' : errText(response)) + '</div>') + '</div>'
      });
    });
  });

  $$('[data-source-download]', host).forEach((button) => {
    button.addEventListener('click', async (event) => {
      event.stopPropagation();
      const [ri, si] = button.dataset.sourceDownload.split(':').map(Number);
      const entry = entries[ri];
      const source = entry && entry.sources[si];
      if (!source) return;
      const select = $('[data-quality-select="' + ri + ':' + si + '"]', host);
      const quality = select && select.value ? select.value : 'auto';
      if (source.platform === 'local') {
        await downloadLocal({sha: source.base && source.base.sha ? source.base.sha : source.id});
        return;
      }
      const response = await api('POST', '/music/download', {
        source: source.platform, item_id: source.id, quality,
        filename: '{artist} - {title} [{quality}]'
      });
      toast(response.code === 0 ? '已创建下载任务' : errText(response), response.code === 0 ? 'ok' : 'error');
    });
  });

  $$('[data-add-playlist]', host).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      addToPlaylistFlow(itemsOf(Number(button.dataset.addPlaylist)));
    });
  });

  $$('[data-add-album]', host).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      addToAlbumFlow(itemsOf(Number(button.dataset.addAlbum)));
    });
  });

  $$('[data-more]', host).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const ri = Number(button.dataset.more);
      const entry = entries[ri];
      if (!entry) return;
      const rect = button.getBoundingClientRect();
      contextMenu(rect.left - 170, rect.bottom + 6, [
        {label: '播放最佳音源', icon: 'play', onClick: () => {
          const index = entry.sources.findIndex((source) => source.available !== false);
          if (index >= 0) playSource(ri, index);
        }},
        {label: '加入播放队列', icon: 'queue', onClick: () => enqueue(itemsOf(ri))},
        {label: '加入歌单', icon: 'playlist', onClick: () => addToPlaylistFlow(itemsOf(ri))},
        {label: '加入专辑', icon: 'album', onClick: () => addToAlbumFlow(itemsOf(ri))},
        {divider: true},
        {label: '只搜这首歌', icon: 'search', onClick: () => {
          state.search.keyword = entry.item.title || '';
          renderSearch(root);
          runSearch(root);
        }}
      ]);
    });
  });

  $$('[data-card]', host).forEach((card) => {
    card.addEventListener('dblclick', (event) => {
      if (event.target.closest('button, select, input')) return;
      const ri = Number(card.dataset.card);
      const entry = entries[ri];
      if (!entry) return;
      const index = entry.sources.findIndex((source) => source.available !== false);
      if (index >= 0) playSource(ri, index);
    });
  });

  const more = $('#search-more', host);
  if (more) more.addEventListener('click', () => runSearch(root, {append: true}));
}

function bindSearchTabs(root, host) {
  $$('[data-tab]', host).forEach((button) => {
    button.addEventListener('click', () => {
      state.search.tab = button.dataset.tab || 'all';
      renderSearchResults(root);
    });
  });
}

// ---------------------------------------------------------------- 歌单
// 导入外部音源歌单：粘贴分享链接 / ID → POST /music/playlist/import
async function openPlaylistImportDialog(root) {
  const plugins = await ensurePlugins();
  const platforms = pluginPlatforms(plugins, 'get_playlist');
  if (!platforms.length) {
    toast('没有可导入的歌单源：请先在「设置 → 音源」启用网易云音乐等插件', 'error');
    return;
  }
  const options = platforms.map((entry) =>
    '<option value="' + esc(entry.platform) + '">' + esc(entry.name || entry.platform) + '</option>').join('');
  const {box, close} = openModal({
    title: '导入歌单',
    body:
      '<label class="col" style="gap:8px"><span class="note">音源</span>' +
      '<select data-import-source>' + options + '</select></label>' +
      '<label class="col" style="gap:8px;margin-top:14px"><span class="note">歌单链接或 ID</span>' +
      '<input type="text" data-import-input placeholder="https://music.163.com/#/playlist?id=…" /></label>' +
      '<p class="note" style="margin-top:12px;font-size:12.5px;line-height:1.7">' +
      '支持网易云歌单分享链接（music.163.com）或直接输入歌单 ID；' +
      '导入后曲目保存在本地歌单中，之后可随时「同步」更新。同一歌单重复导入会刷新曲目，不会重复建库。</p>',
    footer: '<button class="btn ghost" data-modal-close>取消</button>' +
            '<button class="btn primary" data-import-ok>导入</button>'
  });
  const input = $('[data-import-input]', box);
  const source = $('[data-import-source]', box);
  if (input) { input.focus(); }
  const run = async () => {
    const raw = (input && input.value || '').trim();
    if (!raw) { toast('请输入歌单链接或 ID', 'error'); if (input) input.focus(); return; }
    const okBtn = $('[data-import-ok]', box);
    if (okBtn) { okBtn.disabled = true; okBtn.textContent = '导入中…'; }
    const result = await api('POST', '/music/playlist/import', {
      platform: (source && source.value) || 'netease',
      source_id: raw
    });
    if (result.code === 0) {
      const data = result.data || {};
      const count = Number(data.track_count || (data.tracks && data.tracks.length) || 0) || 0;
      toast('已导入「' + (data.name || '歌单') + '」共 ' + count + ' 首' + (data.existed ? '（刷新了已有歌单）' : ''), 'ok');
      close();
      if (data.id) navigate('playlists', {id: data.id});
      else if (root) renderPlaylists(root);
      return;
    }
    if (okBtn) { okBtn.disabled = false; okBtn.textContent = '导入'; }
    toast(errText(result), 'error');
  };
  $('[data-import-ok]', box).addEventListener('click', run);
  if (input) input.addEventListener('keydown', (event) => { if (event.key === 'Enter') run(); });
}

export async function renderPlaylists(root, params = {}) {
  if (params && params.id) return renderPlaylistDetail(root, params.id);
  setTopbarActions(
    '<button class="btn primary" id="pl-new">' + icon('plus', 16) + '新建歌单</button>' +
    '<button class="btn" id="pl-import">' + icon('download', 16) + '导入歌单</button>' +
    '<button class="icon-btn" id="pl-refresh" title="刷新">' + icon('refresh', 19) + '</button>',
    (host) => {
      $('#pl-new', host).addEventListener('click', async () => {
        const name = await promptDialog({title: '新建歌单', label: '歌单名称', placeholder: '例如：通勤路上'});
        if (!name) return;
        const created = await api('POST', '/music/playlist', {name});
        if (created.code === 0) { toast('歌单已创建', 'ok'); renderPlaylists(root); }
        else toast(errText(created), 'error');
      });
      $('#pl-import', host).addEventListener('click', () => openPlaylistImportDialog(root));
      $('#pl-refresh', host).addEventListener('click', () => renderPlaylists(root));
    });
  root.innerHTML = spinnerRow('读取歌单…');
  const result = await api('GET', '/music/playlist');
  if (result.code !== 0) { root.innerHTML = emptyState('读取歌单失败', errText(result), 'playlist'); return; }
  const playlists = safeArray(result.data && result.data.playlists);
  state.playlists = playlists;
  if (!playlists.length) {
    root.innerHTML = emptyState('还没有歌单', '点击右上角新建歌单，或在曲库里把歌曲加入歌单。', 'playlist');
    return;
  }
  root.innerHTML = '<div class="card-grid">' + playlists.map((list) =>
    '<div class="media-card" data-playlist-card="' + esc(list.id) + '">' +
      '<div class="media-art">' + coverMarkup(list.pic, 38) + '</div>' +
      '<div class="media-title">' + esc(list.name || '未命名歌单') + '</div>' +
      '<div class="media-sub">' + (list.track_count || 0) + ' 首 · ' + fmtDuration(list.total_duration) + '</div>' +
      '<div class="media-actions">' +
        '<button class="icon-btn" data-playlist-play="' + esc(list.id) + '" title="播放">' + icon('play', 17) + '</button>' +
        '<button class="icon-btn" data-playlist-more="' + esc(list.id) + '" title="更多">' + icon('more', 17) + '</button>' +
      '</div>' +
    '</div>').join('') + '</div>';
  $$('[data-playlist-card]', root).forEach((card) => {
    card.addEventListener('click', () => navigate('playlists', {id: card.dataset.playlistCard}));
  });
  $$('[data-playlist-play]', root).forEach((button) => {
    button.addEventListener('click', async (event) => {
      event.stopPropagation();
      await playPlaylist(button.dataset.playlistPlay);
    });
  });
  $$('[data-playlist-more]', root).forEach((button) => {
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const playlist = playlists.find((entry) => entry.id === button.dataset.playlistMore);
      if (!playlist) return;
      const rect = button.getBoundingClientRect();
      contextMenu(rect.left - 150, rect.bottom + 6, [
        {label: '打开', icon: 'chevronRight', onClick: () => navigate('playlists', {id: playlist.id})},
        {label: '播放全部', icon: 'play', onClick: () => playPlaylist(playlist.id)},
        {label: '重命名', icon: 'edit', onClick: async () => {
          const name = await promptDialog({title: '重命名歌单', label: '新的名称', value: playlist.name || ''});
          if (!name) return;
          const updated = await api('POST', '/music/playlist/' + encodeURIComponent(playlist.id), {name});
          if (updated.code === 0) { toast('已重命名', 'ok'); renderPlaylists(root); }
          else toast(errText(updated), 'error');
        }},
        {label: '同步（来自音源）', icon: 'refresh', onClick: async () => {
          const synced = await api('POST', '/music/playlist/' + encodeURIComponent(playlist.id) + '/sync', {});
          toast(synced.code === 0 ? '同步完成' : errText(synced), synced.code === 0 ? 'ok' : 'error');
        }},
        {divider: true},
        {label: '删除歌单', icon: 'trash', danger: true, onClick: async () => {
          if (!(await confirmDialog({title: '删除歌单', message: '确定删除「' + (playlist.name || '') + '」？此操作不可撤销。', okLabel: '删除', danger: true}))) return;
          const removed = await api('DELETE', '/music/playlist/' + encodeURIComponent(playlist.id));
          if (removed.code === 0) { toast('已删除', 'ok'); renderPlaylists(root); }
          else toast(errText(removed), 'error');
        }}
      ]);
    });
  });
}

async function playPlaylist(id) {
  const result = await api('GET', '/music/playlist/' + encodeURIComponent(id));
  if (result.code !== 0) { toast(errText(result), 'error'); return; }
  const playlist = result.data && (result.data.playlist || result.data);
  const tracks = safeArray(playlist && playlist.tracks);
  if (!tracks.length) { toast('歌单是空的'); return; }
  playQueue(tracks.map(playlistTrackToItem), 0);
}

function playlistTrackToItem(track) {
  return {
    key: (track.platform || 'local') + ':' + (track.sha || track.item_id),
    platform: track.platform || 'local',
    itemId: track.item_id || track.sha,
    sha: track.sha || '',
    title: track.name || '(未知标题)',
    artist: track.author || '未知艺术家',
    album: '',
    duration: Number(track.duration) || 0,
    pic: '',
    quality: null,
    qualities: [],
    groupKey: track.group_key || '',
    songKey: '',
    sourceLabel: track.platform === 'local' ? '本地' : track.platform
  };
}

async function renderPlaylistDetail(root, id, opts = {}) {
  setTopbarActions('<button class="btn ghost" id="pl-back">' + icon('chevronLeft', 16) + '返回歌单</button>',
    (host) => $('#pl-back', host).addEventListener('click', () => navigate('playlists')));
  root.innerHTML = spinnerRow('读取歌单详情…');
  const result = await api('GET', '/music/playlist/' + encodeURIComponent(id));
  if (result.code !== 0) { root.innerHTML = emptyState('歌单不存在', errText(result), 'playlist'); return; }
  const playlist = result.data && (result.data.playlist || result.data);
  const tracks = safeArray(playlist && playlist.tracks);
  // 搜索关键词（移除曲目后重渲染时沿用）
  let keyword = String((opts && opts.keyword) || '');
  root.innerHTML =
    '<div class="panel" style="display:flex;gap:18px;align-items:center;margin-bottom:20px">' +
      '<div class="media-art" style="width:96px;height:96px;flex:none">' + coverMarkup(playlist.pic, 34) + '</div>' +
      '<div class="col" style="gap:6px;min-width:0">' +
        '<h2 style="font-size:22px;font-weight:750">' + esc(playlist.name || '未命名歌单') + '</h2>' +
        '<p class="note">' + esc(playlist.description || '没有描述') + '</p>' +
        '<p class="note" id="pl-meta">' + tracks.length + ' 首 · ' + fmtDuration(playlist.total_duration) + ' · 更新于 ' + esc(formatDate(playlist.updated_at) || '—') + '</p>' +
      '</div>' +
      '<span class="spacer"></span>' +
      '<button class="btn primary" id="pl-detail-play">' + icon('play', 16) + '播放全部</button>' +
      '<button class="btn" id="pl-detail-add">' + icon('plus', 16) + '从曲库添加</button>' +
      '<button class="btn" id="pl-detail-sync">' + icon('refresh', 16) + '同步</button>' +
      '<button class="btn danger" id="pl-detail-delete">' + icon('trash', 16) + '删除</button>' +
    '</div>' +
    (tracks.length
      ? '<div class="toolbar pl-toolbar">' +
          '<div class="search-field">' + icon('search', 17) +
            '<input type="search" id="pl-filter" placeholder="搜索歌单内的歌曲（歌名 / 歌手 / 音源）" value="' + esc(keyword) + '" />' +
          '</div>' +
          '<span class="tag" id="pl-filter-count">共 ' + tracks.length + ' 首</span>' +
          '<button class="chip-btn" id="pl-filter-clear"' + (keyword.trim() ? '' : ' hidden') + '>' + icon('refresh', 14) + '清空搜索</button>' +
        '</div>' +
        '<div class="track-list" id="pl-tracks"></div>'
      : emptyState('歌单为空', '在曲库或搜索结果里把歌曲加入这个歌单。', 'playlist'));

  // ---------------------------------------------------------------- 曲目列表（搜索 + 双击播放）
  const tracksHost = $('#pl-tracks', root);
  const filterInput = $('#pl-filter', root);
  const clearBtn = $('#pl-filter-clear', root);
  let visible = [];

  const matchedTracks = () => {
    const needle = keyword.trim().toLowerCase();
    if (!needle) return tracks.slice();
    return tracks.filter((track) => {
      const hay = [track.name, track.author, track.album, track.platform].filter(Boolean).join(' ').toLowerCase();
      return hay.includes(needle);
    });
  };

  const paintMeta = () => {
    const meta = $('#pl-meta', root);
    if (meta) {
      const total = tracks.reduce((sum, track) => sum + (Number(track.duration) || 0), 0);
      meta.textContent = tracks.length + ' 首 · ' + fmtDuration(total) + ' · 更新于 ' + (formatDate(playlist.updated_at) || '—');
    }
    const count = $('#pl-filter-count', root);
    if (count) count.textContent = keyword.trim() ? '匹配 ' + visible.length + ' / ' + tracks.length + ' 首' : '共 ' + tracks.length + ' 首';
    if (clearBtn) clearBtn.hidden = !keyword.trim();
  };

  const paintRows = () => {
    visible = matchedTracks();
    paintMeta();
    if (!tracksHost) return;
    if (!visible.length) {
      tracksHost.innerHTML = '<div class="empty"><strong>没有匹配的歌曲</strong><span>换个关键词试试，或点「清空搜索」查看全部曲目。</span></div>';
      return;
    }
    tracksHost.innerHTML = visible.map((track, index) =>
      '<div class="track-row" data-pl-row="' + index + '" title="双击播放">' +
        '<div class="track-index">' + (index + 1) + '</div>' +
        '<div class="track-cover">' + icon('music', 20) + '</div>' +
        '<div class="track-main"><div class="track-title-line"><span class="track-name">' + esc(track.name || '') + '</span></div>' +
        '<div class="track-sub"><span>' + esc(track.author || '') + '</span><span class="sep">·</span><span>' + esc(track.platform || 'local') + '</span></div></div>' +
        '<div class="track-actions">' +
          '<button class="icon-btn" data-pl-track-play="' + index + '" title="播放">' + icon('play', 17) + '</button>' +
          '<button class="icon-btn" data-pl-track-remove="' + index + '" title="移出歌单">' + icon('minus', 17) + '</button>' +
        '</div>' +
        '<div class="track-duration">' + fmtDuration(track.duration) + '</div>' +
      '</div>').join('');
  };

  if (tracksHost) {
    const playVisibleFrom = (index) => {
      if (!visible.length) return;
      playQueue(visible.map(playlistTrackToItem), index);
    };
    tracksHost.addEventListener('click', async (event) => {
      const playBtn = event.target.closest('[data-pl-track-play]');
      if (playBtn) { playVisibleFrom(Number(playBtn.dataset.plTrackPlay)); return; }
      const removeBtn = event.target.closest('[data-pl-track-remove]');
      if (!removeBtn) return;
      const track = visible[Number(removeBtn.dataset.plTrackRemove)];
      if (!track) return;
      const removed = await api('POST', '/music/playlist/' + encodeURIComponent(id) + '/remove',
        {platform: track.platform || 'local', item_id: track.item_id || track.sha});
      if (removed.code !== 0) { toast(errText(removed), 'error'); return; }
      toast('已移除', 'ok');
      const at = tracks.indexOf(track);
      if (at >= 0) tracks.splice(at, 1);
      if (!tracks.length) { renderPlaylistDetail(root, id, {keyword}); return; }
      paintRows();
    });
    tracksHost.addEventListener('dblclick', (event) => {
      if (event.target.closest('button')) return;
      const row = event.target.closest('[data-pl-row]');
      if (!row) return;
      playVisibleFrom(Number(row.dataset.plRow));
    });
    paintRows();
  }

  if (filterInput) {
    // 大歌单（上千首）输入时做轻量防抖，避免每次按键都重建整张列表
    let filterTimer = 0;
    filterInput.addEventListener('input', () => {
      keyword = filterInput.value;
      window.clearTimeout(filterTimer);
      filterTimer = window.setTimeout(paintRows, 80);
      if (clearBtn) clearBtn.hidden = !keyword.trim();
    });
    filterInput.addEventListener('keydown', (event) => {
      if (event.key !== 'Escape') return;
      window.clearTimeout(filterTimer);
      filterInput.value = '';
      keyword = '';
      paintRows();
    });
  }
  if (clearBtn) {
    clearBtn.addEventListener('click', () => {
      keyword = '';
      if (filterInput) { filterInput.value = ''; filterInput.focus(); }
      paintRows();
    });
  }

  $('#pl-detail-play').addEventListener('click', () => {
    // 搜索状态下「播放全部」只播当前匹配的结果，符合所见即所得的预期
    const queue = keyword.trim() ? visible : tracks;
    if (!queue.length) { toast(keyword.trim() ? '没有匹配的歌曲' : '歌单是空的'); return; }
    playQueue(queue.map(playlistTrackToItem), 0);
  });
  $('#pl-detail-add').addEventListener('click', async () => {
    const picked = await pickLibraryTracks({title: '从曲库添加到「' + (playlist.name || '') + '」'});
    if (!picked || !picked.length) return;
    const existing = new Set(tracks.map((track) => (track.platform || 'local') + ':' + (track.item_id || track.sha)));
    const items = picked.map((track) => itemFromTrack(track))
      .filter((item) => !existing.has(item.platform + ':' + item.itemId));
    if (!items.length) { toast('所选曲目都已在歌单中'); return; }
    const added = await api('POST', '/music/playlist/' + encodeURIComponent(id) + '/add_many',
      {items: items.map(itemToAlbumPayload)});
    if (added.code === 0) { toast('已加入歌单', 'ok'); renderPlaylistDetail(root, id, {keyword}); }
    else toast(errText(added), 'error');
  });
  $('#pl-detail-sync').addEventListener('click', async () => {
    const synced = await api('POST', '/music/playlist/' + encodeURIComponent(id) + '/sync', {});
    toast(synced.code === 0 ? '同步完成' : errText(synced), synced.code === 0 ? 'ok' : 'error');
    if (synced.code === 0) renderPlaylistDetail(root, id, {keyword});
  });
  $('#pl-detail-delete').addEventListener('click', async () => {
    if (!(await confirmDialog({title: '删除歌单', message: '确定删除这个歌单？', okLabel: '删除', danger: true}))) return;
    const removed = await api('DELETE', '/music/playlist/' + encodeURIComponent(id));
    if (removed.code === 0) { toast('已删除', 'ok'); navigate('playlists'); }
    else toast(errText(removed), 'error');
  });
}

// ---------------------------------------------------------------- 专辑
function albumTrackKey(track) {
  return (track.platform || 'local') + ':' + (track.item_id || track.sha || '');
}

function itemToAlbumPayload(item) {
  return {
    platform: item.platform,
    item_id: item.itemId,
    sha: item.sha || '',
    name: item.title,
    author: item.artist,
    duration: item.duration || 0,
    group_key: item.groupKey || ''
  };
}

async function albumAddItems(albumId, items) {
  const payload = {items: items.map(itemToAlbumPayload)};
  const response = await api('POST', '/music/album/' + encodeURIComponent(albumId) + '/add_many', payload);
  if (response.code === 0) {
    const data = response.data || {};
    toast('已加入专辑（共 ' + (data.track_count || 0) + ' 首）', 'ok');
    return true;
  }
  toast(errText(response), 'error');
  return false;
}

// 从曲库多选（专辑 / 歌单的「从曲库选取」）
async function pickLibraryTracks({title = '从曲库选取曲目'} = {}) {
  await loadLibrary();
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (settled) return; settled = true; resolve(value); };
    const groups = groupLibrary(state.library || []);
    const selected = new Map();
    let keyword = '';

    const visibleGroups = () => {
      const needle = keyword.trim().toLowerCase();
      if (!needle) return groups;
      return groups.filter((group) => (group.title + ' ' + group.artist + ' ' + group.album).toLowerCase().includes(needle));
    };

    openModal({
      title,
      wide: true,
      body:
        '<div class="toolbar pick-toolbar">' +
          '<div class="search-field">' + icon('search', 17) +
            '<input type="search" id="pick-filter" placeholder="筛选歌名 / 歌手 / 专辑" /></div>' +
          '<button class="chip-btn" id="pick-all">' + icon('check', 14) + '全选当前</button>' +
          '<button class="chip-btn" id="pick-none">清空</button>' +
          '<span class="tag accent" id="pick-count">已选 0 首</span>' +
        '</div>' +
        '<div class="pick-list" id="pick-list"></div>',
      footer: '<button class="btn ghost" data-cancel>取消</button>' +
        '<button class="btn primary" data-ok>' + icon('check', 16) + '加入</button>',
      onMount: (box) => {
        const countEl = () => $('#pick-count', box);
        const paint = () => {
          const host = $('#pick-list', box);
          if (!host) return;
          const list = visibleGroups();
          if (!list.length) {
            host.innerHTML = '<div class="empty"><strong>' + (groups.length ? '没有匹配的曲目' : '曲库还是空的') + '</strong>' +
              '<span>' + (groups.length ? '换个关键词试试。' : '先到「导入」页导入本地音乐。') + '</span></div>';
            return;
          }
          host.innerHTML = list.map((group) => {
            const best = group.best;
            const checked = selected.has(group.key);
            return '<label class="pick-row">' +
              '<input type="checkbox" data-pick-key="' + esc(group.key) + '"' + (checked ? ' checked' : '') + ' />' +
              '<div class="track-cover">' + coverMarkup(best.pic, 18) + '</div>' +
              '<div class="pick-main"><strong>' + esc(group.title) + '</strong>' +
                '<span>' + esc(group.artist) + (group.album ? ' · ' + esc(group.album) : '') +
                (group.variants.length > 1 ? ' · ' + group.variants.length + ' 个音质' : '') + '</span></div>' +
              (best.quality ? qualityBadge(best.quality) : '') +
              '<span class="pick-duration">' + fmtDuration(group.duration) + '</span>' +
            '</label>';
          }).join('');
          if (countEl()) countEl().textContent = '已选 ' + selected.size + ' 首';
          $$('[data-pick-key]', host).forEach((input) => {
            input.addEventListener('change', () => {
              const group = groups.find((entry) => entry.key === input.dataset.pickKey);
              if (!group) return;
              if (input.checked) selected.set(group.key, group.best);
              else selected.delete(group.key);
              if (countEl()) countEl().textContent = '已选 ' + selected.size + ' 首';
            });
          });
        };

        paint();

        const filter = $('#pick-filter', box);
        if (filter) {
          filter.addEventListener('input', () => { keyword = filter.value; paint(); });
          filter.focus();
        }
        $('#pick-all', box).addEventListener('click', () => {
          for (const group of visibleGroups()) selected.set(group.key, group.best);
          paint();
        });
        $('#pick-none', box).addEventListener('click', () => { selected.clear(); paint(); });
        $('[data-ok]', box).addEventListener('click', () => {
          if (!selected.size) { toast('还没有选择曲目'); return; }
          finish(Array.from(selected.values()));
          closeModal();
        });
        $('[data-cancel]', box).addEventListener('click', () => { finish(null); closeModal(); });
      },
      onClose: () => finish(null)
    });
  });
}

async function addToAlbumFlow(items) {
  const list = items || [];
  if (!list.length) return;
  const response = await api('GET', '/music/album');
  const albums = (response.code === 0 && response.data && response.data.albums) || [];
  const rows = albums.length
    ? albums.map((album) => '<button class="ctx-item" data-album="' + esc(album.id) + '">' +
        icon('album', 17) + '<span>' + esc(album.name || '未命名专辑') + '</span>' +
        '<span class="spacer"></span><span class="muted">' + (album.track_count || 0) + ' 首</span></button>').join('')
    : '<div class="qm-empty">还没有专辑，先创建一个</div>';

  openModal({
    title: '加入专辑（' + list.length + ' 首）',
    body: '<div class="col" style="gap:2px">' + rows + '</div>' +
      '<div class="divider"></div><button class="btn primary" data-new-album style="width:100%">' +
      icon('plus', 16) + '新建专辑并加入</button>',
    onMount: (box) => {
      $$('[data-album]', box).forEach((button) => {
        button.addEventListener('click', async () => {
          const ok = await albumAddItems(button.dataset.album, list);
          if (ok) closeModal();
        });
      });
      const create = $('[data-new-album]', box);
      if (create) create.addEventListener('click', async () => {
        const name = await promptDialog({title: '新建专辑', label: '专辑名称', placeholder: '例如：丑奴儿'});
        if (!name) return;
        const created = await api('POST', '/music/album', {name, artist: list[0].artist || ''});
        if (created.code !== 0) { toast(errText(created), 'error'); return; }
        const album = created.data && (created.data.album || created.data);
        closeModal();
        if (album && album.id) await albumAddItems(album.id, list);
      });
    }
  });
}

// 从曲库选取并加入指定专辑（列表页 / 详情页共用）
async function addLibraryTracksToAlbum(albumId, existingTracks) {
  const picked = await pickLibraryTracks({title: '从曲库选取曲目'});
  if (!picked || !picked.length) return false;
  const existing = new Set(safeArray(existingTracks).map(albumTrackKey));
  const items = picked.map((track) => itemFromTrack(track))
    .filter((item) => !existing.has(item.platform + ':' + item.itemId));
  if (!items.length) { toast('所选曲目都已在专辑中'); return false; }
  return albumAddItems(albumId, items);
}

export async function renderAlbums(root, params = {}) {
  if (params && params.id) return renderAlbumDetail(root, params.id);
  setTopbarActions(
    '<button class="btn primary" id="al-new">' + icon('plus', 16) + '新建专辑</button>' +
    '<button class="icon-btn" id="al-refresh" title="刷新">' + icon('refresh', 19) + '</button>',
    (host) => {
      $('#al-new', host).addEventListener('click', () => createAlbumFlow(root));
      $('#al-refresh', host).addEventListener('click', () => renderAlbums(root));
    });
  root.innerHTML = spinnerRow('读取专辑…');
  const result = await api('GET', '/music/album');
  if (result.code !== 0) { root.innerHTML = emptyState('读取专辑失败', errText(result), 'album'); return; }
  const albums = safeArray(result.data && result.data.albums);
  state.albums = albums;
  if (!albums.length) {
    root.innerHTML = emptyState('还没有专辑', '点击右上角新建专辑，再从曲库中选取曲目；也可以从音源同步一张专辑。', 'album');
    return;
  }
  root.innerHTML =
    '<div class="section-head"><h2>专辑</h2><span class="sub">' + albums.length + ' 张 · 曲目可从本地曲库自由挑选</span></div>' +
    '<div class="card-grid">' + albums.map((album) =>
      '<div class="media-card" data-album-card="' + esc(album.id) + '">' +
        '<div class="media-art">' + coverMarkup(album.pic, 38) + '</div>' +
        '<div class="media-title">' + esc(album.name || '未命名专辑') + '</div>' +
        '<div class="media-sub">' + esc(album.artist || '未知艺术家') +
          (album.year ? ' · ' + album.year : '') + ' · ' + (album.track_count || 0) + ' 首</div>' +
        '<div class="media-actions">' +
          '<button class="icon-btn" data-album-play="' + esc(album.id) + '" title="播放">' + icon('play', 17) + '</button>' +
          '<button class="icon-btn" data-album-add="' + esc(album.id) + '" title="从曲库添加">' + icon('plus', 17) + '</button>' +
          '<button class="icon-btn" data-album-more="' + esc(album.id) + '" title="更多">' + icon('more', 17) + '</button>' +
        '</div></div>').join('') + '</div>';

  const findAlbum = (id) => albums.find((entry) => entry.id === id);

  $$('[data-album-card]', root).forEach((card) =>
    card.addEventListener('click', () => navigate('albums', {id: card.dataset.albumCard})));
  $$('[data-album-play]', root).forEach((button) =>
    button.addEventListener('click', async (event) => {
      event.stopPropagation();
      await playAlbum(button.dataset.albumPlay);
    }));
  $$('[data-album-add]', root).forEach((button) =>
    button.addEventListener('click', async (event) => {
      event.stopPropagation();
      const album = findAlbum(button.dataset.albumAdd);
      if (!album) return;
      const detail = await api('GET', '/music/album/' + encodeURIComponent(album.id));
      const tracks = safeArray(detail.code === 0 && detail.data && detail.data.tracks);
      if (await addLibraryTracksToAlbum(album.id, tracks)) renderAlbums(root);
    }));
  $$('[data-album-more]', root).forEach((button) =>
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const album = findAlbum(button.dataset.albumMore);
      if (!album) return;
      const rect = button.getBoundingClientRect();
      contextMenu(rect.left - 170, rect.bottom + 6, [
        {label: '打开', icon: 'chevronRight', onClick: () => navigate('albums', {id: album.id})},
        {label: '播放全部', icon: 'play', onClick: () => playAlbum(album.id)},
        {label: '从曲库添加曲目', icon: 'plus', onClick: () => {
          const card = button.closest('.media-card');
          const add = card ? $('[data-album-add]', card) : null;
          if (add) add.click();
          else addLibraryTracksToAlbum(album.id, safeArray(album.tracks)).then((ok) => { if (ok) renderAlbums(root); });
        }},
        {label: '编辑信息', icon: 'edit', onClick: () => editAlbumFlow(album, () => renderAlbums(root))},
        {label: '同步（来自音源）', icon: 'refresh', onClick: async () => {
          const synced = await api('POST', '/music/album/' + encodeURIComponent(album.id) + '/sync', {});
          toast(synced.code === 0 ? '同步完成' : errText(synced), synced.code === 0 ? 'ok' : 'error');
          if (synced.code === 0) renderAlbums(root);
        }},
        {divider: true},
        {label: '删除专辑', icon: 'trash', danger: true, onClick: async () => {
          if (!(await confirmDialog({title: '删除专辑', message: '确定删除「' + (album.name || '') + '」？', okLabel: '删除', danger: true}))) return;
          const removed = await api('DELETE', '/music/album/' + encodeURIComponent(album.id));
          if (removed.code === 0) { toast('已删除', 'ok'); renderAlbums(root); }
          else toast(errText(removed), 'error');
        }}
      ]);
    }));
}

// formDialog 的输入值都是字符串，后端 get_int 只接受数字，这里统一转换
function normalizeAlbumValues(values) {
  const year = Number(values.year);
  values.year = Number.isFinite(year) ? Math.trunc(year) : 0;
  for (const key of ['name', 'artist', 'description', 'pic', 'platform', 'source_id']) {
    if (values[key] == null) values[key] = '';
  }
}

async function createAlbumFlow(root) {
  const values = await formDialog({    title: '新建专辑',
    fields: [
      {key: 'name', label: '专辑名称', placeholder: '必填'},
      {key: 'artist', label: '艺术家', placeholder: '可选'},
      {key: 'year', label: '年份', placeholder: '可选', type: 'number'},
      {key: 'description', label: '简介', placeholder: '可选'},
      {key: 'pic', label: '封面地址', placeholder: '可选（图片 URL 或本地路径）'},
      {key: 'platform', label: '来源平台', placeholder: '默认 local', value: 'local'},
      {key: 'source_id', label: '来源专辑 ID', placeholder: '可选（用于从音源同步）'}
    ]
  });
  if (!values || !values.name) return;
  normalizeAlbumValues(values);
  const created = await api('POST', '/music/album', values);
  if (created.code !== 0) { toast(errText(created), 'error'); return; }
  const album = created.data && (created.data.album || created.data);
  toast('专辑已创建，可以从曲库选取曲目了', 'ok');
  if (album && album.id) navigate('albums', {id: album.id});
  else renderAlbums(root);
}

async function editAlbumFlow(album, after) {
  const values = await formDialog({
    title: '编辑专辑',
    fields: [
      {key: 'name', label: '专辑名称', value: album.name || ''},
      {key: 'artist', label: '艺术家', value: album.artist || ''},
      {key: 'year', label: '年份', value: album.year ? String(album.year) : '', type: 'number'},
      {key: 'description', label: '简介', value: album.description || ''},
      {key: 'pic', label: '封面地址', value: album.pic || ''}
    ]
  });
  if (!values) return;
  normalizeAlbumValues(values);
  const updated = await api('POST', '/music/album/' + encodeURIComponent(album.id), values);
  if (updated.code === 0) {
    toast('专辑已更新', 'ok');
    if (after) after();
  } else {
    toast(errText(updated), 'error');
  }
}

async function playAlbum(id) {
  const result = await api('GET', '/music/album/' + encodeURIComponent(id));
  if (result.code !== 0) { toast(errText(result), 'error'); return; }
  const album = result.data && (result.data.album || result.data);
  const tracks = safeArray(album && album.tracks);
  if (!tracks.length) { toast('专辑里还没有曲目'); return; }
  playQueue(tracks.map(playlistTrackToItem), 0);
}

async function renderAlbumDetail(root, id) {
  setTopbarActions('<button class="btn ghost" id="al-back">' + icon('chevronLeft', 16) + '返回专辑</button>',
    (host) => $('#al-back', host).addEventListener('click', () => navigate('albums')));
  root.innerHTML = spinnerRow('读取专辑详情…');
  const result = await api('GET', '/music/album/' + encodeURIComponent(id));
  if (result.code !== 0) { root.innerHTML = emptyState('专辑不存在', errText(result), 'album'); return; }
  const album = result.data && (result.data.album || result.data);
  const tracks = safeArray(album && album.tracks);

  root.innerHTML =
    '<div class="album-hero">' +
      '<div class="album-art">' + coverMarkup(album.pic, 42) + '</div>' +
      '<div class="album-info">' +
        '<span class="eyebrow">' + esc(album.platform || 'local') + (album.year ? ' · ' + album.year : '') + '</span>' +
        '<h2>' + esc(album.name || '未命名专辑') + '</h2>' +
        '<p class="note">' + esc(album.artist || '未知艺术家') + '</p>' +
        (album.description ? '<p class="note">' + esc(album.description) + '</p>' : '') +
        '<p class="note">' + tracks.length + ' 首 · ' + fmtDuration(album.total_duration) + '</p>' +
      '</div>' +
      '<div class="album-actions">' +
        '<button class="btn primary" id="al-detail-play">' + icon('play', 16) + '播放全部</button>' +
        '<button class="btn" id="al-detail-add">' + icon('plus', 16) + '从曲库添加</button>' +
        '<button class="btn" id="al-detail-edit">' + icon('edit', 16) + '编辑</button>' +
        '<button class="btn" id="al-detail-sync">' + icon('refresh', 16) + '同步</button>' +
        '<button class="btn danger" id="al-detail-delete">' + icon('trash', 16) + '删除</button>' +
      '</div>' +
    '</div>' +
    (tracks.length
      ? '<div class="section-head"><h2>曲目</h2><span class="sub">' + tracks.length + ' 首</span></div>' +
        '<div class="track-list">' + tracks.map((track, index) =>
          '<div class="track-row" data-al-row="' + index + '">' +
            '<div class="track-index">' + (index + 1) + '</div>' +
            '<div class="track-cover">' + icon('music', 20) + '</div>' +
            '<div class="track-main"><div class="track-title-line"><span class="track-name">' + esc(track.name || '') + '</span></div>' +
            '<div class="track-sub"><span>' + esc(track.author || '') + '</span>' +
              '<span class="sep">·</span><span class="mini-tag' + (track.platform === 'local' ? ' local' : '') + '">' +
              esc(track.platform || 'local') + '</span></div></div>' +
            '<div class="track-actions">' +
              '<button class="icon-btn" data-al-track-play="' + index + '" title="播放">' + icon('play', 17) + '</button>' +
              '<button class="icon-btn" data-al-track-remove="' + index + '" title="移出专辑">' + icon('minus', 17) + '</button>' +
            '</div>' +
            '<div class="track-duration">' + fmtDuration(track.duration) + '</div>' +
          '</div>').join('') + '</div>'
      : emptyState('专辑里还没有曲目', '点击「从曲库添加」，勾选本地曲目即可组成专辑。', 'album'));

  $('#al-detail-play').addEventListener('click', () => {
    if (!tracks.length) { toast('专辑里还没有曲目'); return; }
    playQueue(tracks.map(playlistTrackToItem), 0);
  });
  $('#al-detail-add').addEventListener('click', async () => {
    if (await addLibraryTracksToAlbum(album.id, tracks)) renderAlbumDetail(root, id);
  });
  $('#al-detail-edit').addEventListener('click', () =>
    editAlbumFlow(album, () => renderAlbumDetail(root, id)));
  $('#al-detail-sync').addEventListener('click', async () => {
    const synced = await api('POST', '/music/album/' + encodeURIComponent(id) + '/sync', {});
    toast(synced.code === 0 ? '同步完成' : errText(synced), synced.code === 0 ? 'ok' : 'error');
    if (synced.code === 0) renderAlbumDetail(root, id);
  });
  $('#al-detail-delete').addEventListener('click', async () => {
    if (!(await confirmDialog({title: '删除专辑', message: '确定删除「' + (album.name || '') + '」？', okLabel: '删除', danger: true}))) return;
    const removed = await api('DELETE', '/music/album/' + encodeURIComponent(id));
    if (removed.code === 0) { toast('已删除', 'ok'); navigate('albums'); }
    else toast(errText(removed), 'error');
  });
  $$('[data-al-track-play]', root).forEach((button) =>
    button.addEventListener('click', () => playQueue(tracks.map(playlistTrackToItem), Number(button.dataset.alTrackPlay))));
  $$('[data-al-track-remove]', root).forEach((button) =>
    button.addEventListener('click', async () => {
      const track = tracks[Number(button.dataset.alTrackRemove)];
      const removed = await api('POST', '/music/album/' + encodeURIComponent(id) + '/remove',
        {platform: track.platform || 'local', item_id: track.item_id || track.sha});
      if (removed.code === 0) { toast('已移出专辑', 'ok'); renderAlbumDetail(root, id); }
      else toast(errText(removed), 'error');
    }));
  $$('[data-al-row]', root).forEach((row) =>
    row.addEventListener('dblclick', () => playQueue(tracks.map(playlistTrackToItem), Number(row.dataset.alRow))));
}

// ---------------------------------------------------------------- MV
// MV 搜索统一放在「搜索」页（类型选 MV），这里只做入口跳转
function openMvSearch(keyword) {
  const value = String(keyword || '').trim();
  state.search.filters = Object.assign({}, SEARCH_FILTER_DEFAULTS, {type: 'mv'});
  state.search.keyword = value;
  state.search.mv = null;
  state.search.all = [];
  state.search.data = null;
  state.search.page = 1;
  state.search.total = 0;
  state.search.tab = 'all';
  state.search.filtersOpen = true;
  state.search.loading = false;
  // 清掉去重标记，让搜索页的深链逻辑对这次跳转执行一次自动搜索
  state.search.deepLinkApplied = '';
  navigate('search', value ? {id: value} : {});
}

export async function renderMv(root) {
  const platforms = await ensureMvPlatforms();
  const current = String(state.settings.mv_source || '');
  const gotoSearch = () => {
    const input = $('#mv-keyword');
    openMvSearch(input ? input.value : '');
  };
  setTopbarActions(
    '<div class="search-field" style="width:220px">' + icon('search', 17) +
      '<input type="search" id="mv-keyword" placeholder="搜索 MV…" /></div>' +
    '<button class="btn" id="mv-search">' + icon('search', 16) + '搜索 MV</button>' +
    '<button class="btn primary" id="mv-add">' + icon('plus', 16) + '添加 MV</button>' +
    '<button class="icon-btn" id="mv-refresh" title="刷新">' + icon('refresh', 19) + '</button>',
    (host) => {
      $('#mv-search', host).addEventListener('click', gotoSearch);
      $('#mv-keyword', host).addEventListener('keydown', (event) => { if (event.key === 'Enter') gotoSearch(); });
      $('#mv-add', host).addEventListener('click', () => addMvFlow(root));
      $('#mv-refresh', host).addEventListener('click', () => renderMv(root));
    });
  root.innerHTML = spinnerRow('读取 MV…');
  const result = await api('GET', '/music/mv');
  if (result.code !== 0) { root.innerHTML = emptyState('读取 MV 失败', errText(result), 'video'); return; }
  const mvs = safeArray(result.data && result.data.mvs);
  state.mvs = mvs;
  const sourceText = platforms.length
    ? '可用视频源：' + platforms.map((entry) => entry.name).join(' / ') +
      (current ? '，默认 ' + platformName(current) : '')
    : '当前没有声明 search_mv / get_mv 的插件，请到「设置 → 音源」检查。';
  root.innerHTML =
    '<div class="mv-hint">' + icon('info', 16) +
      '<span>MV 搜索已整合到「搜索」页：在类型里选 <strong>MV</strong> 即可搜索和切换视频源。' + esc(sourceText) + '</span>' +
    '</div>' +
    (mvs.length ? '<div class="section-head"><h2>MV 库</h2><span class="sub">' + mvs.length + ' 个视频</span></div>' +
      '<div class="card-grid">' + mvs.map((mv) =>
        '<div class="media-card" data-mv-card="' + esc(mv.sha) + '">' +
          '<div class="media-art wide">' + coverMarkup(mv.pic, 34) + '</div>' +
          '<div class="media-title">' + esc(mv.title || '未命名 MV') + '</div>' +
          '<div class="media-sub">' + esc(mv.artist || '') + ' · ' + esc(platformName(mv.platform) || mv.platform || '') + '</div>' +
          '<div class="media-actions">' +
            '<button class="icon-btn" data-mv-open="' + esc(mv.sha) + '" title="解析并播放">' + icon('play', 17) + '</button>' +
            '<button class="icon-btn" data-mv-more="' + esc(mv.sha) + '" title="更多">' + icon('more', 17) + '</button>' +
          '</div></div>').join('') + '</div>'
      : emptyState('还没有 MV', '点右上角「搜索 MV」到搜索页找视频，或手动添加一个 MV 条目。', 'video'));
  bindMvActions(root);
}

function bindMvActions(root) {
  $$('[data-mv-card]', root).forEach((card) =>
    card.addEventListener('click', () => playMv(card.dataset.mvCard)));
  $$('[data-mv-open]', root).forEach((button) =>
    button.addEventListener('click', (event) => { event.stopPropagation(); playMv(button.dataset.mvOpen, true); }));
  $$('[data-mv-more]', root).forEach((button) =>
    button.addEventListener('click', (event) => {
      event.stopPropagation();
      const mv = state.mvs.find((entry) => entry.sha === button.dataset.mvMore);
      if (!mv) return;
      const rect = button.getBoundingClientRect();
      contextMenu(rect.left - 150, rect.bottom + 6, [
        {label: '播放', icon: 'play', onClick: () => playMv(mv.sha, true)},
        {label: '复制视频地址', icon: 'link', onClick: async () => {
          if (!mv.location) { toast('没有视频地址'); return; }
          try { await navigator.clipboard.writeText(mv.location); toast('已复制', 'ok'); }
          catch { toast('复制失败'); }
        }},
        {divider: true},
        {label: '删除 MV', icon: 'trash', danger: true, onClick: async () => {
          if (!(await confirmDialog({title: '删除 MV', message: '确定从 MV 库删除「' + (mv.title || '') + '」？', okLabel: '删除', danger: true}))) return;
          const removed = await api('DELETE', '/music/mv/' + encodeURIComponent(mv.sha));
          if (removed.code === 0) { toast('已删除', 'ok'); renderMv(root); }
          else toast(errText(removed), 'error');
        }}
      ]);
    }));
}

async function playMv(sha, resolveFirst = false) {
  let mv = state.mvs.find((entry) => entry.sha === sha);
  if (!mv) return;
  if (resolveFirst || !mv.location) {
    const opened = await api('POST', '/music/mv/' + encodeURIComponent(sha) + '/open', {});
    if (opened.code === 0) {
      mv = opened.data && (opened.data.mv || opened.data);
      toast('已从视频源解析地址', 'ok');
    } else {
      toast(errText(opened), 'error');
      return;
    }
  }
  // 全窗口播放：打开时自动暂停音乐，关闭时停掉视频并恢复音乐
  openMvStage(mv);
}

async function addMvFlow(root) {
  const values = await formDialog({
    title: '添加 MV',
    fields: [
      {key: 'sha', label: 'SHA（必填）', placeholder: '64 位 sha，用于唯一标识'},
      {key: 'platform', label: '平台', value: 'local'},
      {key: 'item_id', label: '来源 ID'},
      {key: 'title', label: '标题'},
      {key: 'artist', label: '艺术家'},
      {key: 'location', label: '视频地址', placeholder: 'http(s) 直链或本地相对路径'},
      {key: 'pic', label: '封面地址'},
      {key: 'duration_seconds', label: '时长（秒）', type: 'number'}
    ]
  });
  if (!values || !values.sha) return;
  const saved = await api('POST', '/music/mv', values);
  if (saved.code === 0) { toast('已添加 MV', 'ok'); renderMv(root); }
  else toast(errText(saved), 'error');
}

// ---------------------------------------------------------------- 下载
// 任务卡片的标题：优先用后端落盘的真实文件名，老数据从 path 兜底
function downloadFileName(task) {
  const explicit = String((task && task.filename) || '').trim();
  if (explicit) return explicit;
  const path = String((task && task.path) || '');
  const base = path.split(/[\\/]/).pop() || '';
  if (base && base.indexOf('{') === -1) return base;
  return String((task && task.item_id) || '');
}

export async function renderDownloads(root) {
  await ensurePlugins();   // 让来源标签显示中文名
  setTopbarActions(
    '<button class="btn primary" id="dl-new">' + icon('plus', 16) + '新建下载</button>' +
    '<button class="icon-btn" id="dl-open-dir" title="打开下载目录">' + icon('folder', 19) + '</button>' +
    '<button class="icon-btn" id="dl-refresh" title="刷新">' + icon('refresh', 19) + '</button>',
    (host) => {
      $('#dl-new', host).addEventListener('click', () => newDownloadFlow(root));
      $('#dl-open-dir', host).addEventListener('click', () => openDownloadFolder());
      $('#dl-refresh', host).addEventListener('click', () => renderDownloads(root));
    });
  root.innerHTML = '<div id="dl-body">' + spinnerRow('读取下载任务…') + '</div>';
  await refreshDownloads(root);
  if (state._downloadTimer) clearInterval(state._downloadTimer);
  state._downloadTimer = setInterval(() => {
    if (!document.body.contains(root)) { clearInterval(state._downloadTimer); return; }
    refreshDownloads(root, true);
  }, 2000);
}

// 轮询时发现有任务刚变成 done 就提示一句，顺带告诉用户可以打开所在文件夹。
// 首次渲染（quiet=false）只记录已有完成项，不打扰用户。
function announceFinished(tasks, quiet) {
  const doneIds = state._dlDoneIds || (state._dlDoneIds = new Set());
  const fresh = [];
  tasks.forEach((task) => {
    if (task.state !== 'done' || doneIds.has(task.id)) return;
    doneIds.add(task.id);
    if (quiet) fresh.push(task);
  });
  if (!fresh.length) return;
  if (fresh.length > 1) toast('已完成 ' + fresh.length + ' 个下载，可点文件夹图标打开所在位置', 'ok');
  else toast('下载完成：' + downloadFileName(fresh[0]) + '（点文件夹图标可打开所在位置）', 'ok');
}

async function refreshDownloads(root, quiet = false) {
  const host = $('#dl-body', root);
  if (!host) return;
  const result = await api('GET', '/music/download');
  if (result.code !== 0) { if (!quiet) host.innerHTML = emptyState('读取下载任务失败', errText(result), 'download'); return; }
  const tasks = safeArray(result.data && result.data.tasks);
  state.downloads = tasks;
  announceFinished(tasks, quiet);
  if (!tasks.length) {
    host.innerHTML = emptyState('没有下载任务', '新建下载，把在线歌曲按指定音质保存到曲库。', 'download');
    return;
  }
  host.innerHTML = '<div class="track-list">' + tasks.map((task) => {
    const total = Number(task.total) || 0;
    const received = Number(task.received) || 0;
    const percent = total > 0 ? Math.min(100, Math.round(received * 100 / total)) : (task.state === 'done' ? 100 : 0);
    const stateText = {queued: '排队中', running: '下载中', paused: '已暂停', done: '已完成', failed: '失败', canceled: '已取消'}[task.state] || task.state;
    const fileName = downloadFileName(task);
    const failed = task.state === 'failed';
    let errorText = failed ? String(task.message || task.error || '') : '';
    if (errorText === 'ok') errorText = '';
    const qualityValue = task.quality_resolved || task.quality;
    const qualityText = qualityValue && typeof qualityValue === 'object' ? qualityLabel(qualityValue) : (qualityValue || 'auto');
    return '<div class="panel" style="margin-bottom:8px;padding:12px 14px">' +
      '<div class="row" style="gap:10px">' +
        '<div class="plugin-icon">' + icon(task.state === 'done' ? 'check' : 'download', 20) + '</div>' +
        '<div class="track-main">' +
          '<div class="track-title-line"><span class="track-name" title="' + esc(fileName) + '">' + esc(fileName) + '</span>' +
            '<span class="tag">' + esc(platformName(task.source) || task.source || '') + '</span>' +
            '<span class="tag">' + esc(qualityText) + '</span>' +
            '<span class="tag ' + (failed ? 'accent' : '') + '">' + esc(stateText) + '</span></div>' +
          '<div class="track-sub"><span>' + fmtBytes(received) + ' / ' + fmtBytes(total) + '</span>' +
            (errorText ? '<span class="sep">·</span><span style="color:var(--danger)">' + esc(errorText) + '</span>' : '') + '</div>' +
        '</div>' +
        '<div class="field-control" style="gap:4px">' +
          (task.state === 'running' ? '<button class="icon-btn" data-dl-pause="' + esc(task.id) + '" title="暂停">' + icon('pause', 17) + '</button>' : '') +
          (task.state === 'paused' ? '<button class="icon-btn" data-dl-resume="' + esc(task.id) + '" title="继续">' + icon('play', 17) + '</button>' : '') +
          (task.state !== 'canceled' && task.state !== 'done' ? '<button class="icon-btn" data-dl-cancel="' + esc(task.id) + '" title="取消">' + icon('close', 17) + '</button>' : '') +
          (task.path ? '<button class="icon-btn" data-dl-folder="' + esc(task.id) + '" title="打开所在文件夹">' + icon('folder', 17) + '</button>' : '') +
          '<button class="icon-btn" data-dl-remove="' + esc(task.id) + '" title="删除任务">' + icon('trash', 17) + '</button>' +
        '</div>' +
      '</div>' +
      '<div class="layer-row" style="margin-top:8px"><span class="bar"><i style="width:' + percent + '%"></i></span>' +
      '<span class="num">' + percent + '%</span></div>' +
    '</div>';
  }).join('') + '</div>';
  const action = async (id, kind, body) => {
    const response = await api('POST', '/music/download/' + encodeURIComponent(id) + '/' + kind, body || {});
    toast(response.code === 0 ? '操作成功' : errText(response), response.code === 0 ? 'ok' : 'error');
    refreshDownloads(root, true);
  };
  $$('[data-dl-pause]', host).forEach((button) => button.addEventListener('click', () => action(button.dataset.dlPause, 'pause')));
  $$('[data-dl-resume]', host).forEach((button) => button.addEventListener('click', () => action(button.dataset.dlResume, 'resume')));
  $$('[data-dl-cancel]', host).forEach((button) => button.addEventListener('click', () => action(button.dataset.dlCancel, 'cancel')));
  $$('[data-dl-folder]', host).forEach((button) => button.addEventListener('click', async () => {
    const response = await api('POST', '/music/open-folder', {task_id: button.dataset.dlFolder});
    toast(response.code === 0 ? '已打开文件所在位置' : errText(response), response.code === 0 ? 'ok' : 'error');
  }));
  $$('[data-dl-remove]', host).forEach((button) => button.addEventListener('click', async () => {
    const response = await api('DELETE', '/music/download/' + encodeURIComponent(button.dataset.dlRemove));
    toast(response.code === 0 ? '已删除任务' : errText(response), response.code === 0 ? 'ok' : 'error');
    refreshDownloads(root, true);
  }));
}

async function newDownloadFlow(root) {
  // 音源平台从已加载插件取，避免手输平台名拼错导致 1001
  const platforms = ['local'];
  const info = await api('GET', '/music/plugins');
  if (info.code === 0) {
    safeArray(info.data && info.data.plugins).forEach((plugin) => {
      safeArray(plugin.platforms).forEach((platform) => {
        if (platform && !platforms.includes(platform)) platforms.push(platform);
      });
    });
  }
  const values = await formDialog({
    title: '新建下载',
    fields: [
      {key: 'source', label: '音源平台', type: 'select', value: 'local',
        options: platforms.map((platform) => ({value: platform, label: platform === 'local' ? 'local（本地曲库）' : platform}))},
      {key: 'item_id', label: '曲目 ID / sha', placeholder: '必填；在线曲目建议在搜索结果里直接点下载'},
      {key: 'quality', label: '音质', placeholder: 'auto 或 quality_id', value: 'auto'},
      {key: 'filename', label: '文件名模板', value: '{artist} - {title} [{quality}]'}
    ]
  });
  if (!values || !values.item_id) return;
  const response = await api('POST', '/music/download', values);
  if (response.code === 0) { toast('已创建下载任务', 'ok'); refreshDownloads(root, true); }
  else toast(errText(response), 'error');
}

// ---------------------------------------------------------------- 导入
export async function renderImport(root) {
  const api2 = api;
  setTopbarActions('<button class="icon-btn" id="im-refresh" title="刷新">' + icon('refresh', 19) + '</button>',
    (host) => $('#im-refresh', host).addEventListener('click', () => renderImport(root)));
  root.innerHTML =
    '<div class="settings-layout" style="grid-template-columns:minmax(0,1fr) minmax(0,1fr)">' +
      '<div class="settings-panel">' +
        '<div class="set-card">' +
          '<h4>上传音频文件</h4>' +
          '<p class="hint">选择本地音频（mp3 / flac / m4a / wav / ogg 等），上传后会自动解析标签并导入曲库。</p>' +
          '<div class="row" style="gap:10px"><button class="btn primary" id="im-pick">' + icon('upload', 16) + '选择文件</button>' +
          '<span class="note" id="im-upload-state">未选择文件</span></div>' +
          '<input type="file" id="im-file" multiple accept="audio/*,.mp3,.flac,.m4a,.wav,.ogg,.aac,.opus" hidden />' +
        '</div>' +
        '<div class="set-card">' +
          '<h4>扫描 / 导入目录</h4>' +
          '<p class="hint">输入服务器可访问的目录路径（每行一个），先扫描再导入。模式支持 copy / move / link。</p>' +
          '<label class="col" style="gap:6px;margin-bottom:10px"><span class="note">目录路径</span>' +
            '<textarea id="im-paths" placeholder="D:\\\\Music\\\\专辑&#10;D:\\\\Music\\\\单曲"></textarea></label>' +
          '<div class="field-grid">' +
            '<label>导入模式<select id="im-mode"><option value="copy">copy（复制）</option><option value="move">move（移动）</option><option value="link">link（链接）</option></select></label>' +
            '<label>冲突处理<select id="im-conflict"><option value="skip">skip（跳过）</option><option value="overwrite">overwrite（覆盖）</option><option value="merge">merge（合并）</option></select></label>' +
            '<label>标签回退<select id="im-tag"><option value="filename">filename（用文件名）</option><option value="none">none（不解析）</option></select></label>' +
            '<label>递归<select id="im-recursive"><option value="1">是</option><option value="0">否</option></select></label>' +
          '</div>' +
          '<div class="row" style="gap:10px;margin-top:12px">' +
            '<button class="btn" id="im-scan">' + icon('search', 16) + '扫描</button>' +
            '<button class="btn primary" id="im-run">' + icon('download', 16) + '开始导入</button>' +
          '</div>' +
        '</div>' +
      '</div>' +
      '<div class="settings-panel">' +
        '<div class="set-card"><h4>导入报告</h4><p class="hint">最近一次扫描或导入的结果。</p><div id="im-report">' +
        '<div class="note">还没有执行过任务。</div></div></div>' +
      '</div>' +
    '</div>';

  const fileInput = $('#im-file', root);
  $('#im-pick', root).addEventListener('click', () => fileInput.click());
  fileInput.addEventListener('change', async () => {
    const files = Array.from(fileInput.files || []);
    if (!files.length) return;
    const stateText = $('#im-upload-state', root);
    let imported = 0, skipped = 0, failed = 0;
    for (let index = 0; index < files.length; index += 1) {
      const file = files[index];
      if (stateText) stateText.textContent = '上传中（' + (index + 1) + '/' + files.length + '）：' + file.name;
      const result = await api2('POST', '/music/import/upload?filename=' + encodeURIComponent(file.name), file);
      if (result.code === 0) { imported += 1; } else { failed += 1; }
    }
    if (stateText) stateText.textContent = '完成：成功 ' + imported + '，失败 ' + failed;
    toast('上传导入完成：成功 ' + imported + ' / 失败 ' + failed, failed ? 'error' : 'ok');
    await loadLibrary(true);
    fileInput.value = '';
  });

  const pathsValue = () => String($('#im-paths', root).value || '').split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  $('#im-scan', root).addEventListener('click', async () => {
    const paths = pathsValue();
    if (!paths.length) { toast('请输入至少一个目录'); return; }
    const result = await api2('POST', '/music/import/scan', {paths});
    renderImportReport(root, result, '扫描结果');
  });
  $('#im-run', root).addEventListener('click', async () => {
    const paths = pathsValue();
    if (!paths.length) { toast('请输入至少一个目录'); return; }
    const body = {
      paths,
      mode: $('#im-mode', root).value,
      conflict: $('#im-conflict', root).value,
      tag_fallback: $('#im-tag', root).value,
      recursive: $('#im-recursive', root).value === '1'
    };
    const result = await api2('POST', '/music/import', body);
    renderImportReport(root, result, '导入报告');
    if (result.code === 0) await loadLibrary(true);
  });
}

function renderImportReport(root, result, title) {
  const host = $('#im-report', root);
  if (!host) return;
  if (result.code !== 0) {
    host.innerHTML = '<div class="note" style="color:var(--danger)">' + esc(errText(result)) + '</div>';
    return;
  }
  const data = result.data || {};
  const rows = [];
  if (data.import_id) rows.push(['任务 ID', data.import_id]);
  if (data.scanned != null) rows.push(['扫描曲目', data.scanned]);
  if (data.imported != null) rows.push(['成功导入', data.imported]);
  if (data.skipped != null) rows.push(['跳过', data.skipped]);
  if (data.failed != null) rows.push(['失败', data.failed]);
  if (data.total != null) rows.push(['总数', data.total]);
  const items = safeArray(data.items).slice(0, 40);
  host.innerHTML = '<h4 style="margin-bottom:8px">' + esc(title) + '</h4>' +
    (rows.length ? '<dl class="kv">' + rows.map(([key, value]) => '<dt>' + esc(key) + '</dt><dd>' + esc(value) + '</dd>').join('') + '</dl>' : '') +
    (items.length ? '<div class="divider"></div><div class="log-list">' + items.map((item) =>
      '<div class="log-row"><span class="lvl ' + esc(item.status === 'failed' ? 'error' : item.status === 'skipped' ? 'warn' : 'info') + '">' +
      esc(item.status || '') + '</span><span class="msg">' + esc(item.path || item.source || '') +
      (item.message ? ' — ' + esc(item.message) : '') + '</span></div>').join('') + '</div>' : '');
}

// ---------------------------------------------------------------- 通用表单弹窗
export function formDialog({title, fields, okLabel = '保存'}) {
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (settled) return; settled = true; resolve(value); };
    const body = '<div class="col" style="gap:10px">' + fields.map((field) => {
      const type = field.type || 'text';
      const id = 'field-' + field.key;
      const control = field.options
        ? '<select id="' + id + '" data-field="' + esc(field.key) + '">' +
          field.options.map((option) => {
            const optionValue = option && option.value != null ? option.value : option;
            const optionLabel = option && option.label != null ? option.label : optionValue;
            const selected = String(optionValue) === String(field.value == null ? '' : field.value) ? ' selected' : '';
            return '<option value="' + esc(optionValue) + '"' + selected + '>' + esc(optionLabel) + '</option>';
          }).join('') + '</select>'
        : '<input id="' + id + '" type="' + type + '" data-field="' + esc(field.key) + '" placeholder="' + esc(field.placeholder || '') +
          '" value="' + esc(field.value != null ? field.value : '') + '" />';
      return '<label class="col" style="gap:5px"><span class="note">' + esc(field.label || field.key) + '</span>' +
        control + '</label>';
    }).join('') + '</div>';
    openModal({
      title,
      body,
      footer: '<button class="btn ghost" data-cancel>取消</button><button class="btn primary" data-ok>' + esc(okLabel) + '</button>',
      onMount: (box) => {
        $('[data-ok]', box).addEventListener('click', () => {
          const values = {};
          $$('[data-field]', box).forEach((input) => { values[input.dataset.field] = input.value; });
          finish(values);
          closeModal();
        });
        $('[data-cancel]', box).addEventListener('click', () => { finish(null); closeModal(); });
        const first = $('[data-field]', box);
        if (first) first.focus();
      },
      onClose: () => finish(null)
    });
  });
}

// ---------------------------------------------------------------- 歌手搜索 / 歌手页 / 收藏
async function ensureArtistPlatforms() {
  if (state.artistPlatforms && state.artistPlatforms.length) return state.artistPlatforms;
  const plugins = await ensurePlugins();
  const list = pluginPlatforms(plugins, 'search_artist');
  const meta = Object.assign({}, state.platformMeta);
  for (const entry of list) meta[entry.platform] = {name: entry.name};
  state.platformMeta = meta;
  state.artistPlatforms = list;
  return list;
}

async function runArtistSearch(root) {
  const filters = searchFilters();
  const keyword = String(state.search.keyword || '').trim();
  if (!keyword) { toast('请输入歌手名'); return; }
  const platforms = await ensureArtistPlatforms();
  const platform = filters.sources[0] || (platforms[0] && platforms[0].platform) || '';
  const host = $('#search-body', root);
  state.search.loading = true;
  if (!platform) {
    state.search.loading = false;
    if (host) host.innerHTML = emptyState('没有可用的歌手搜索源', '没有插件声明 search_artist 能力，请先在「设置 → 音源」检查插件。', 'star');
    return;
  }
  if (host) host.innerHTML = spinnerRow('正在搜索歌手「' + keyword + '」· ' + platformName(platform));
  const result = await api('POST', '/music/request', {
    source: platform, op: 'search_artist', params: {keyword, page: 1, page_size: 30}
  });
  state.search.loading = false;
  if (result.code !== 0) {
    state.search.artist = null;
    if (host) host.innerHTML = emptyState('歌手搜索失败', errText(result), 'star');
    return;
  }
  const value = (result.data && result.data.result) || result.data || {};
  let list = [];
  if (Array.isArray(value.artists)) list = value.artists;
  else if (Array.isArray(value)) list = value;
  state.search.artist = {list, source: platform, keyword};
  renderArtistSearchResults(root);
}

function renderArtistSearchResults(root) {
  const host = $('#search-body', root);
  if (!host) return;
  const data = state.search.artist || {list: [], source: ''};
  const list = safeArray(data.list);
  if (!list.length) {
    host.innerHTML = emptyState('没有找到歌手', '换个关键词，或在筛选里选择其它音源。', 'star');
    return;
  }
  const source = data.source;
  const cards = list.map((artist, i) => {
    const pic = artist.avatar || artist.pic || '';
    const alias = safeArray(artist.alias).filter(Boolean).slice(0, 2).join(' / ');
    const songs = Number(artist.music_count) || 0;
    const albums = Number(artist.album_count) || 0;
    const sub = alias || (albums ? albums + ' 张专辑' : '') || platformName(source);
    return '<div class="media-card collection-card" data-artist="' + i + '" tabindex="0" role="button">' +
      '<div class="media-art">' + (pic
        ? '<img src="' + esc(pic) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />'
        : icon('music', 40)) +
        (songs ? '<span class="collection-count">' + songs + ' 首</span>' : '') +
      '</div>' +
      '<div class="media-title">' + esc(artist.name || '(未知)') + '</div>' +
      '<div class="media-sub">' + esc(sub) + '</div>' +
    '</div>';
  }).join('');
  host.innerHTML =
    '<div class="section-head"><h2>歌手</h2><span class="sub">' + list.length + ' 位</span></div>' +
    '<div class="card-grid">' + cards + '</div>';
  $$('[data-artist]', host).forEach((card) => {
    card.addEventListener('click', () => {
      const artist = list[Number(card.dataset.artist)];
      if (!artist) return;
      const plat = artist.platform || source;
      navigate('artist', {id: plat + ':' + artist.id, title: artist.name});
    });
  });
}

function artistNames(author) {
  if (Array.isArray(author)) return author;
  return String(author || '').split('/').map((part) => part.trim()).filter(Boolean);
}

function artistSongToItem(song) {
  const item = {
    title: song.name || '(未知标题)',
    artists: artistNames(song.author),
    album: song.album || '',
    duration: Number(song.duration_seconds) || 0,
    pic: song.pic || ''
  };
  const source = {
    platform: song.platform || 'netease',
    id: String(song.item_id != null ? song.item_id : song.id),
    base: {
      name: song.name, author: song.author, pic: song.pic, sha: song.sha,
      duration: Number(song.duration_seconds) || 0, album: song.album,
      quality: song.quality, qualities: song.qualities,
      available: song.available, reason: song.reason
    },
    quality: song.quality, qualities: song.qualities, available: song.available
  };
  return itemFromResult(item, source, song.quality);
}

function favoriteArtists() { return safeArray(state.settings.favorite_artists); }

function isFavoriteArtist(platform, id) {
  return favoriteArtists().some((entry) => entry.platform === platform && String(entry.id) === String(id));
}

async function toggleFavoriteArtist(artist) {
  const list = favoriteArtists().slice();
  const index = list.findIndex((entry) => entry.platform === artist.platform && String(entry.id) === String(artist.id));
  let nowFav;
  if (index >= 0) { list.splice(index, 1); nowFav = false; }
  else {
    list.push({
      platform: artist.platform, id: String(artist.id),
      name: artist.name || '', pic: artist.pic || '', alias: artist.alias || []
    });
    nowFav = true;
  }
  const result = await saveSettings({favorite_artists: list});
  if (result.code !== 0) { toast(errText(result), 'error'); return nowFav; }
  toast(nowFav ? '已收藏歌手' : '已取消收藏', 'ok');
  return nowFav;
}

export async function renderArtists(root) {
  setTopbarActions(
    '<button class="btn primary" id="artists-search">' + icon('search', 16) + '搜索歌手</button>',
    (host) => {
      $('#artists-search', host).addEventListener('click', () => {
        state.search.filters = Object.assign({}, searchFilters(), {type: 'artist'});
        navigate('search');
      });
    });
  const favs = favoriteArtists();
  if (!favs.length) {
    root.innerHTML = emptyState('还没有收藏歌手', '在搜索页切到「歌手」类型找到喜欢的歌手，进入主页即可收藏。', 'star') +
      '<div class="row" style="justify-content:center"><button class="btn primary" id="go-search">' +
        icon('search', 16) + '去搜索歌手</button></div>';
    const button = $('#go-search', root);
    if (button) button.addEventListener('click', () => {
      state.search.filters = Object.assign({}, searchFilters(), {type: 'artist'});
      navigate('search');
    });
    return;
  }
  const cards = favs.map((entry, i) => {
    const pic = entry.pic || '';
    return '<div class="media-card collection-card" data-fav="' + i + '" tabindex="0" role="button">' +
      '<div class="media-art">' + (pic
        ? '<img src="' + esc(pic) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />'
        : icon('music', 40)) + '</div>' +
      '<div class="media-title">' + esc(entry.name || '(未知)') + '</div>' +
      '<div class="media-sub">' + esc(platformName(entry.platform)) + '</div>' +
    '</div>';
  }).join('');
  root.innerHTML =
    '<div class="section-head"><h2>收藏的歌手</h2><span class="sub">' + favs.length + ' 位</span></div>' +
    '<div class="card-grid">' + cards + '</div>';
  $$('[data-fav]', root).forEach((card) => {
    card.addEventListener('click', () => {
      const entry = favs[Number(card.dataset.fav)];
      if (entry) navigate('artist', {id: entry.platform + ':' + entry.id, title: entry.name});
    });
  });
}

export async function renderArtist(root, params) {
  const idArg = String((params && params.id) || '').trim();
  const separator = idArg.indexOf(':');
  const platform = separator > 0 ? idArg.slice(0, separator) : ((state.search.artist && state.search.artist.source) || 'netease');
  const artistId = separator > 0 ? idArg.slice(separator + 1) : idArg;
  setTopbarActions('');
  root.innerHTML = spinnerRow('加载歌手信息…');
  const result = await api('POST', '/music/request', {
    source: platform, op: 'get_artist', params: {id: artistId, item_id: artistId}
  });
  if (result.code !== 0) {
    root.innerHTML = emptyState('歌手信息加载失败', errText(result), 'star');
    return;
  }
  const data = (result.data && result.data.result) || result.data || {};
  renderArtistDetail(root, data, platform, artistId);
}

function renderArtistDetail(root, data, platform, artistId) {
  const id = data.source_id || data.id || artistId;
  const name = data.name || '未知歌手';
  const alias = safeArray(data.alias).filter(Boolean);
  const pic = data.avatar || data.pic || '';
  const fav = isFavoriteArtist(platform, id);
  const stats = [];
  if (data.album_count) stats.push('专辑 ' + data.album_count);
  if (data.music_count) stats.push('单曲 ' + data.music_count);
  if (data.mv_count) stats.push('MV ' + data.mv_count);
  const desc = data.brief_desc || '';
  const songs = safeArray(data.hot_songs).map((song) => artistSongToItem(song));

  root.innerHTML =
    '<div class="artist-hero">' +
      '<div class="artist-avatar">' + (pic
        ? '<img src="' + esc(pic) + '" alt="" referrerpolicy="no-referrer" onerror="this.remove()" />'
        : icon('music', 52)) + '</div>' +
      '<div class="artist-hero-text">' +
        '<h2 class="artist-name">' + esc(name) + '</h2>' +
        (alias.length ? '<div class="artist-alias">' + alias.map((part) => '<span class="tag">' + esc(part) + '</span>').join('') + '</div>' : '') +
        (stats.length ? '<div class="artist-stats">' + stats.map((part) => '<span>' + esc(part) + '</span>').join('<span class="sep">·</span>') + '</div>' : '') +
        '<div class="artist-actions">' +
          '<button class="btn primary" id="artist-playall">' + icon('play', 16) + '播放热门</button>' +
          '<button class="btn' + (fav ? ' fav-on' : '') + '" id="artist-fav">' + icon(fav ? 'heartFill' : 'heart', 16) + (fav ? '已收藏' : '收藏') + '</button>' +
        '</div>' +
      '</div>' +
    '</div>' +
    (desc ? '<p class="artist-desc">' + esc(desc) + '</p>' : '') +
    '<div class="artist-search">' +
      '<div class="search-field">' + icon('search', 18) +
        '<input type="search" id="artist-song-search" placeholder="搜索 TA 的歌曲（歌名 / 关键词）" /></div>' +
      '<button class="btn" id="artist-song-go">搜索</button>' +
      '<button class="chip-btn" id="artist-song-reset" hidden="true">返回热门</button>' +
    '</div>' +
    '<div id="artist-songs"></div>';

  $('#artist-playall', root).addEventListener('click', () => {
    if (songs.length) playQueue(songs, 0);
    else toast('没有可播放的歌曲');
  });
  const favButton = $('#artist-fav', root);
  favButton.addEventListener('click', async () => {
    const now = await toggleFavoriteArtist({platform, id, name, pic, alias});
    favButton.className = 'btn' + (now ? ' fav-on' : '');
    favButton.innerHTML = icon(now ? 'heartFill' : 'heart', 16) + (now ? '已收藏' : '收藏');
  });

  const songHost = $('#artist-songs', root);
  renderArtistSongs(songHost, songs, '热门歌曲');

  const searchInput = $('#artist-song-search', root);
  const doSearch = () => {
    const query = String(searchInput.value || '').trim();
    if (!query) return;
    searchArtistSongs(songHost, data, platform, query);
    $('#artist-song-reset', root).hidden = false;
  };
  $('#artist-song-go', root).addEventListener('click', doSearch);
  searchInput.addEventListener('keydown', (event) => { if (event.key === 'Enter') doSearch(); });
  $('#artist-song-reset', root).addEventListener('click', () => {
    searchInput.value = '';
    $('#artist-song-reset', root).hidden = true;
    renderArtistSongs(songHost, songs, '热门歌曲');
  });
}

async function searchArtistSongs(host, data, platform, query) {
  if (!host) return;
  host.innerHTML = spinnerRow('正在搜索「' + query + '」的歌曲…');
  const result = await api('POST', '/music/search', {
    keyword: query, type: 'song', sources: [platform], page: 1, page_size: 60,
    fold: state.settings.fold_results !== false
  });
  if (result.code !== 0) { host.innerHTML = emptyState('搜索失败', errText(result), 'search'); return; }
  const target = String(data.name || '').toLowerCase();
  const matched = [];
  for (const entry of safeArray(result.data && result.data.results)) {
    const names = safeArray(entry.artists).map((part) => String(part).toLowerCase());
    if (!names.length && entry.author) names.push(String(entry.author).toLowerCase());
    if (names.some((part) => part.includes(target) || target.includes(part))) {
      const source = safeArray(entry.sources).find((item) => item.platform === platform)
        || safeArray(entry.sources).find((item) => item.available !== false)
        || entry.sources[0];
      if (source) matched.push(itemFromResult(entry, source, source.quality));
    }
  }
  if (!matched.length) {
    host.innerHTML = emptyState('未找到 TA 的歌曲', '换个关键词试试，或检查是否选对音源。', 'music');
    return;
  }
  renderArtistSongs(host, matched, '搜索结果');
}

function renderArtistSongs(host, items, title) {
  if (!host) return;
  if (!items.length) {
    host.innerHTML = emptyState('没有歌曲', '这位歌手暂时没有可展示的歌曲。', 'music');
    return;
  }
  host.innerHTML =
    '<div class="section-head" style="margin-top:18px"><h2>' + esc(title) + '</h2>' +
      '<span class="sub">' + items.length + ' 首</span></div>' +
    '<div class="track-list">' + items.map((item, i) => songRow(item, i)).join('') + '</div>';
  bindSongRows(host, items);
}

function songRow(item, index) {
  const playing = player.current && player.current.key === item.key;
  return '<div class="track-row' + (playing ? ' playing' : '') + '" data-song="' + index + '">' +
    '<div class="track-index">' + (playing ? icon('play', 12) : (index + 1)) + '</div>' +
    '<div class="track-cover">' + coverMarkup(item.pic) + '</div>' +
    '<div class="track-main">' +
      '<div class="track-title-line"><span class="track-name">' + esc(item.title) + '</span></div>' +
      '<div class="track-sub"><span>' + esc(item.artist) + '</span>' +
        (item.album ? '<span class="sep">·</span><span>' + esc(item.album) + '</span>' : '') + '</div>' +
    '</div>' +
    '<div class="track-actions">' +
      '<button class="icon-btn" data-play="' + index + '" title="播放">' + icon('play', 18) + '</button>' +
      '<button class="icon-btn" data-add="' + index + '" title="加入歌单">' + icon('plus', 18) + '</button>' +
      '<button class="icon-btn" data-more="' + index + '" title="更多">' + icon('more', 18) + '</button>' +
    '</div>' +
    '<div class="track-duration">' + fmtDuration(item.duration) + '</div>' +
  '</div>';
}

function bindSongRows(host, items) {
  $$('[data-song]', host).forEach((row) => {
    const index = Number(row.dataset.song);
    const item = items[index];
    if (!item) return;
    const play = row.querySelector('[data-play]');
    const add = row.querySelector('[data-add]');
    const more = row.querySelector('[data-more]');
    // 单曲播放：插入到当前曲目之后并播放，不清空播放列表
    if (play) play.addEventListener('click', (event) => { event.stopPropagation(); enqueueAndPlay([item]); });
    if (add) add.addEventListener('click', (event) => { event.stopPropagation(); addToPlaylistFlow([item]); });
    if (more) more.addEventListener('click', (event) => {
      event.stopPropagation();
      const rect = more.getBoundingClientRect();
      contextMenu(rect.left - 160, rect.bottom + 6, [
        {label: '加入播放队列', icon: 'queue', onClick: () => enqueue([item])},
        {label: '加入歌单', icon: 'playlist', onClick: () => addToPlaylistFlow([item])}
      ]);
    });
    row.addEventListener('dblclick', () => enqueueAndPlay([item]));
  });
}

// ---------------------------------------------------------------- 发现 / 智能推荐
// 把推荐插件返回的扁平 item 转成播放引擎需要的规范 item
function recommendItemToPlayItem(it) {
  const qualities = (Array.isArray(it.qualities) && it.qualities.length) ? it.qualities
    : (it.quality ? [it.quality] : []);
  return {
    key: 'online:' + it.platform + ':' + it.id,
    platform: it.platform,
    itemId: String(it.id),
    sha: it.sha || '',
    title: it.title || '(未知标题)',
    artist: it.artist || (Array.isArray(it.artists) ? it.artists.join(' / ') : '') || '未知艺术家',
    album: it.album || '',
    duration: Number(it.duration || it.duration_seconds || 0) || 0,
    pic: it.pic || '',
    quality: it.quality || null,
    qualities: qualities,
    groupKey: '',
    songKey: '',
    sourceLabel: it.source_name || platformLabel(it.platform),
    hidden: !it.available
  };
}

// 宿主会把插件响应再包一层：顶端 code 来自宿主转发，真实插件结果在 data.result（= 插件原始的 data 字段）。
// 这里统一拆出来，省得每个调用点都手动剥两层。
function unwrapPlugin(res) {
  const top = res || {};
  const wrap = top.data || {};
  const result = (wrap && wrap.result !== undefined) ? wrap.result : wrap;
  const hasResult = result && typeof result === 'object' && !Array.isArray(result);
  let message = top.message || '';
  if (!hasResult && top.code === 0) message = '推荐插件未返回结果（可能无可用音源或登录失效）';
  return {ok: top.code === 0 && hasResult, code: top.code, message: message,
          data: hasResult ? result : {}};
}

export async function renderDiscover(root) {
  const PLUGIN = 'recommend';
  const ui = {
    mode: 'recommend_daily',
    keyword: '', artist: '', mood: '', seed: '', sources: '',
    loading: false, items: [], prefs: null,
    modes: [], moods: [], platforms: [], error: '', meta: null,
    feedbackGiven: {}
  };
  const modeDef = (id) => ui.modes.find((m) => m.id === id) || {};
  const needsInput = (m) => !!(m && m.needs_input && m.needs_input !== 'none');

  root.innerHTML = spinnerRow('连接推荐插件…');
  let metaRes;
  try {
    metaRes = await api('POST', '/music/request', {source: PLUGIN, op: 'modes'});
  } catch (error) {
    root.innerHTML = emptyState('推荐插件未连接',
      '请确保 radmusic_server 已加载 recommend 插件（plugins/recommend/main.py）并正在运行。',
      'plugin');
    return;
  }
  const m = unwrapPlugin(metaRes);
  if (!m.ok) {
    root.innerHTML = emptyState('推荐插件返回异常', m.message || '无法获取模式列表', 'warning');
    return;
  }
  ui.modes = m.data.modes || [];
  ui.moods = m.data.moods || [];
  ui.platforms = m.data.platforms || [];
  if (!ui.modes.length) {
    ui.modes = [
      {id: 'recommend_daily', label: '每日推荐', icon: 'calendar', desc: '根据你的收藏歌手与曲风，叠加当天轮换的探索词。', needs_input: 'none'},
      {id: 'roam', label: '漫游', icon: 'globe', desc: '以新颖度优先的随机游走，越听越陌生又不全是噪音。', needs_input: 'none'},
      {id: 'explore', label: '探索', icon: 'compass', desc: '输入关键词 / 曲风 / 心情，跨音源聚合排序。', needs_input: 'keyword'},
      {id: 'similar', label: '相似推荐', icon: 'link', desc: '输入歌手或歌曲，找风格相近的曲目。', needs_input: 'artist'},
      {id: 'mood', label: '心情电台', icon: 'heart', desc: '选一种心情，自动编排契合情绪的音乐。', needs_input: 'mood'}
    ];
  }

  try {
    const pg = await api('POST', '/music/request', {source: PLUGIN, op: 'preferences_get'});
    const pu = unwrapPlugin(pg);
    if (pu.ok) ui.prefs = pu.data;
  } catch (e) { /* 偏好可选 */ }

  paint();
  if (!needsInput(modeDef(ui.mode))) run();

  function paint() {
    const tabs = ui.modes.map((m) =>
      '<button class="chip-btn' + (m.id === ui.mode ? ' active' : '') + '" data-mode="' + esc(m.id) + '">' +
      icon(m.icon || 'music', 15) + esc(m.label) + '</button>').join('');
    const md = modeDef(ui.mode);
    const controls = needsInput(md) ? controlPanel() :
      '<div class="discover-hint">' + icon('sparkles', 16) +
      '<span>' + esc(md.desc || '点击「生成推荐」开始') + '</span></div>';
    root.innerHTML =
      '<div class="discover">' +
        '<div class="discover-modes">' + tabs + '</div>' +
        '<div class="discover-controls">' + controls +
          '<div class="discover-actions">' +
            '<button class="btn primary" id="disc-run">' + icon('refresh', 16) +
              (ui.mode === 'recommend_daily' ? '生成今日推荐' : '生成推荐') + '</button>' +
            '<button class="btn ghost" id="disc-prefs">' + icon('sliders', 16) + '偏好</button>' +
          '</div>' +
        '</div>' +
        '<div class="discover-meta" id="disc-meta"></div>' +
        '<div class="track-list" id="disc-results"></div>' +
      '</div>';

    $$('.chip-btn[data-mode]', root).forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.mode === ui.mode) return;
      ui.mode = b.dataset.mode;
      ui.items = []; ui.meta = null; ui.error = '';
      paint();
      if (!needsInput(modeDef(ui.mode))) run();
    }));
    $$('.chip-btn[data-mood]', root).forEach((b) => b.addEventListener('click', () => {
      ui.mood = (ui.mood === b.dataset.mood) ? '' : b.dataset.mood;
      paint();
      if (ui.mode === 'mood' && ui.mood) run();
    }));
    $$('.chip-btn[data-src]', root).forEach((b) => b.addEventListener('click', () => {
      ui.sources = b.dataset.src;
      paint();
      if (ui.items.length) run();
    }));
    const runBtn = $('#disc-run', root);
    if (runBtn) runBtn.addEventListener('click', run);
    const prefsBtn = $('#disc-prefs', root);
    if (prefsBtn) prefsBtn.addEventListener('click', openPrefs);
    const kw = $('#disc-keyword', root);
    if (kw) {
      kw.addEventListener('input', () => { ui.keyword = kw.value; });
      kw.addEventListener('keydown', (e) => { if (e.key === 'Enter') run(); });
    }
    const ar = $('#disc-artist', root);
    if (ar) {
      ar.addEventListener('input', () => { ui.artist = ar.value; });
      ar.addEventListener('keydown', (e) => { if (e.key === 'Enter') run(); });
    }
    const sd = $('#disc-seed', root);
    if (sd) {
      sd.addEventListener('input', () => { ui.seed = sd.value; });
      sd.addEventListener('keydown', (e) => { if (e.key === 'Enter') run(); });
    }
    paintResults();
  }

  function controlPanel() {
    let inner = '';
    if (ui.mode === 'explore') {
      inner = '<input class="text-input" id="disc-keyword" placeholder="关键词 / 曲风 / 歌手，如：周杰伦、电子、City Pop" value="' + esc(ui.keyword) + '" />' +
        (ui.moods.length ? moodRow() : '');
    } else if (ui.mode === 'similar') {
      inner = '<input class="text-input" id="disc-artist" placeholder="歌手名或歌曲名，如：周杰伦" value="' + esc(ui.artist) + '" />';
    } else if (ui.mode === 'mood') {
      inner = moodRow() + (ui.mood ? '' : '<span class="muted">选择一种心情后自动生成</span>');
    } else if (ui.mode === 'roam') {
      inner = '<input class="text-input" id="disc-seed" placeholder="可选：自定义随机种子（留空每 10 分钟自动轮换）" value="' + esc(ui.seed) + '" />';
    }
    const srcRow = '<div class="discover-sources"><span class="muted">音源</span>' +
      '<button class="chip-btn' + (ui.sources === '' ? ' active' : '') + '" data-src="">全部</button>' +
      ui.platforms.map((p) => '<button class="chip-btn' + (ui.sources === p ? ' active' : '') + '" data-src="' + esc(p) + '">' + esc(platformLabel(p)) + '</button>').join('') + '</div>';
    return '<div class="discover-control-panel">' + inner + srcRow + '</div>';
  }

  function moodRow() {
    return '<div class="discover-moodrow"><span class="muted">心情</span>' +
      ui.moods.map((mo) => '<button class="chip-btn' + (ui.mood === mo.id ? ' active' : '') + '" data-mood="' + esc(mo.id) + '">' + esc(mo.label) + '</button>').join('') + '</div>';
  }

  async function run() {
    if (ui.loading) return;
    // 兜底：直接从输入框同步一次，避免 input 事件未触发时拿到空值
    const kwEl = $('#disc-keyword', root);
    if (kwEl) ui.keyword = kwEl.value;
    const arEl = $('#disc-artist', root);
    if (arEl) ui.artist = arEl.value;
    const sdEl = $('#disc-seed', root);
    if (sdEl) ui.seed = sdEl.value;
    ui.loading = true; ui.error = '';
    paintResults();
    const op = ui.mode;
    const params = {};
    if (ui.sources) params.sources = ui.sources.split(',').map((s) => s.trim()).filter(Boolean);
    if (op === 'explore') { params.keyword = ui.keyword.trim(); if (ui.mood) params.mood = ui.mood; }
    else if (op === 'similar') { params.artist = ui.artist.trim(); }
    else if (op === 'mood') { params.mood = ui.mood; }
    else if (op === 'roam') { if (ui.seed.trim()) params.seed = ui.seed.trim(); }
    if (op === 'explore' && !params.keyword && !params.mood) {
      ui.loading = false;
      ui.error = '请输入关键词（曲风 / 歌手 / 歌曲），或选择一种心情';
      paintResults();
      toast(ui.error, 'error');
      return;
    }
    if (op === 'similar' && !params.artist) {
      ui.loading = false;
      ui.error = '请输入歌手名或歌曲名';
      paintResults();
      toast(ui.error, 'error');
      return;
    }
    if (op === 'mood' && !params.mood) {
      ui.loading = false;
      ui.error = '请先选择一种心情';
      paintResults();
      toast(ui.error, 'error');
      return;
    }
    let res;
    try {
      res = await api('POST', '/music/request', {source: PLUGIN, op: op, params: params});
    } catch (error) {
      ui.loading = false;
      ui.error = '请求推荐插件失败：' + (error && error.message ? error.message : error);
      paintResults();
      return;
    }
    ui.loading = false;
    const r = unwrapPlugin(res);
    if (!r.ok) {
      ui.error = r.message || errText(res) || '推荐失败';
      paintResults();
      return;
    }
    ui.items = safeArray(r.data && r.data.items);
    ui.meta = r.data || null;
    paintResults();
  }

  function paintResults() {
    const box = $('#disc-results', root);
    if (!box) return;
    if (ui.loading) { box.innerHTML = spinnerRow('正在跨音源聚合候选曲目…'); return; }
    if (ui.error) { box.innerHTML = emptyState('出错了', ui.error, 'warning'); return; }
    if (!ui.items.length) {
      box.innerHTML = emptyState('还没有推荐', '选好模式后点「生成推荐」。推荐会跨 ' +
        (ui.platforms.length || '多个') + ' 个音源聚合，并按你的偏好排序。', 'compass');
      return;
    }
    box.innerHTML = ui.items.map((it, idx) => rowMarkup(it, idx)).join('');
    wireRows(box);
    const metaEl = $('#disc-meta', root);
    if (metaEl) {
      const seeds = (ui.meta && ui.meta.seeds) || [];
      const note = (ui.meta && ui.meta.mode === 'mood' && ui.meta.meta && ui.meta.meta.mood_label) ? ('心情：' + ui.meta.meta.mood_label) : '';
      metaEl.innerHTML = seeds.length
        ? '<span class="muted">本次种子</span> ' + seeds.slice(0, 8).map((s) => '<span class="tag">' + esc(s) + '</span>').join('') + (note ? ' · ' + esc(note) : '')
        : '';
    }
  }

  function rowMarkup(it, idx) {
    const fb = ui.feedbackGiven[it.id];
    const actions =
      '<div class="track-actions">' +
        '<button class="icon-btn" data-play="' + idx + '" title="播放">' + icon('play', 17) + '</button>' +
        '<button class="icon-btn' + (fb === 'like' ? ' fav-on' : '') + '" data-like="' + idx + '" title="喜欢">' + icon('heart', 17) + '</button>' +
        '<button class="icon-btn' + (fb === 'dislike' ? ' fav-on' : '') + '" data-dislike="' + idx + '" title="不喜欢">' + icon('close', 17) + '</button>' +
        '<button class="icon-btn" data-add="' + idx + '" title="加入队列">' + icon('queue', 17) + '</button>' +
      '</div>';
    const dur = Number(it.duration || it.duration_seconds || 0) || 0;
    const qHtml = it.quality ? qualityBadge(it.quality) : '';
    return '<div class="track-row" data-idx="' + idx + '">' +
      '<div class="track-index">' + (idx + 1) + '</div>' +
      '<div class="track-cover">' + coverMarkup(it.pic) + '</div>' +
      '<div class="track-main">' +
        '<div class="track-title-line"><span class="track-name">' + esc(it.title || '(未知标题)') + '</span>' +
          (it.reason ? '<span class="track-reason">' + esc(it.reason) + '</span>' : '') + '</div>' +
        '<div class="track-sub"><span>' + esc(it.artist || '未知艺术家') + '</span>' +
          '<span class="sep">·</span><span>' + esc(it.source_name || platformLabel(it.platform)) + '</span>' +
          (qHtml ? '<span class="sep">·</span>' + qHtml : '') +
        '</div>' +
      '</div>' + actions +
      '<div class="track-duration">' + (dur ? fmtDuration(dur) : '--:--') + '</div>' +
    '</div>';
  }

  function wireRows(box) {
    $$('[data-play]', box).forEach((b) => b.addEventListener('click', (e) => {
      e.stopPropagation();
      const it = ui.items[Number(b.dataset.play)];
      if (it) { enqueueAndPlay([recommendItemToPlayItem(it)]); sendFeedback('play', it); }
    }));
    $$('[data-add]', box).forEach((b) => b.addEventListener('click', (e) => {
      e.stopPropagation();
      const it = ui.items[Number(b.dataset.add)];
      if (it) { enqueue([recommendItemToPlayItem(it)]); toast('已加入播放队列', 'ok'); }
    }));
    $$('[data-like]', box).forEach((b) => b.addEventListener('click', async (e) => {
      e.stopPropagation();
      const it = ui.items[Number(b.dataset.like)];
      if (!it) return;
      ui.feedbackGiven[it.id] = 'like';
      paintResults();
      await sendFeedback('like', it);
      toast('已标记喜欢，将更常推荐', 'ok');
    }));
    $$('[data-dislike]', box).forEach((b) => b.addEventListener('click', async (e) => {
      e.stopPropagation();
      const it = ui.items[Number(b.dataset.dislike)];
      if (!it) return;
      ui.feedbackGiven[it.id] = 'dislike';
      paintResults();
      await sendFeedback('dislike', it);
      toast('已标记不喜欢，会减少推荐', 'ok');
    }));
    $$('.track-row', box).forEach((row) => row.addEventListener('dblclick', () => {
      const it = ui.items[Number(row.dataset.idx)];
      if (it) { enqueueAndPlay([recommendItemToPlayItem(it)]); sendFeedback('play', it); }
    }));
  }

  async function sendFeedback(action, it) {
    try {
      await api('POST', '/music/request', {source: PLUGIN, op: 'feedback', params: {
        action: action,
        track: {platform: it.platform, item_id: it.id, title: it.title, artist: it.artist, artists: it.artists, sha: it.sha}
      }});
    } catch (e) { /* 反馈失败不影响播放 */ }
  }

  function openPrefs() {
    const p = ui.prefs || {};
    const fav = (p.favorite_artists || []).join(', ');
    const genres = (p.genres || []).join(', ');
    const excluded = new Set(p.excluded_platforms || []);
    const cfg = p.config || {};
    const srcChecks = ui.platforms.map((pl) =>
      '<label class="check"><input type="checkbox" data-excl="' + esc(pl) + '"' + (excluded.has(pl) ? ' checked' : '') + '/> ' + esc(platformLabel(pl)) + '</label>').join('');
    const body =
      '<div class="field"><div class="field-info"><strong>收藏歌手</strong><span>逗号分隔，用于每日推荐与相似推荐的种子</span></div>' +
        '<div class="field-control wide"><input class="text-input" id="pref-fav" value="' + esc(fav) + '" placeholder="周杰伦, Taylor Swift" /></div></div>' +
      '<div class="field"><div class="field-info"><strong>偏好曲风</strong><span>逗号分隔，影响每日推荐的探索方向</span></div>' +
        '<div class="field-control wide"><input class="text-input" id="pref-genres" value="' + esc(genres) + '" placeholder="流行, 摇滚, 民谣" /></div></div>' +
      '<div class="field"><div class="field-info"><strong>排除音源</strong><span>勾选的音源不会出现在推荐里</span></div>' +
        '<div class="field-control wide"><div class="field-grid">' + (srcChecks || '<span class="muted">暂无可用音源</span>') + '</div></div></div>' +
      '<div class="field"><div class="field-info"><strong>每日推荐条数</strong><span>10 - 60</span></div>' +
        '<div class="field-control"><input class="text-input" id="pref-limit" type="number" min="10" max="60" value="' + esc(String(cfg.daily_limit || 30)) + '" style="width:90px" /></div></div>' +
      '<div class="field"><div class="field-info"><strong>漫游新颖度</strong><span>0 最熟悉 ~ 1 最新颖</span></div>' +
        '<div class="field-control"><input class="text-input" id="pref-disco" type="number" min="0" max="1" step="0.05" value="' + esc(String(cfg.discovery_ratio != null ? cfg.discovery_ratio : 0.5)) + '" style="width:90px" /></div></div>';
    const footer = '<button class="btn ghost" data-modal-close>取消</button><button class="btn primary" id="pref-save">保存</button>';
    openModal({title: '推荐偏好', body: body, footer: footer, wide: true, onMount: (box, close) => {
      $('#pref-save', box).addEventListener('click', async () => {
        const patch = {
          favorite_artists: $('#pref-fav', box).value.split(',').map((s) => s.trim()).filter(Boolean),
          genres: $('#pref-genres', box).value.split(',').map((s) => s.trim()).filter(Boolean),
          excluded_platforms: $$('[data-excl]', box).filter((c) => c.checked).map((c) => c.dataset.excl),
          config: {
            daily_limit: Math.max(10, Math.min(60, Number($('#pref-limit', box).value) || 30)),
            discovery_ratio: Math.max(0, Math.min(1, Number($('#pref-disco', box).value) || 0.5))
          }
        };
        const r = await api('POST', '/music/request', {source: PLUGIN, op: 'preferences_set', params: patch});
        const ru = unwrapPlugin(r);
        if (ru.ok) { ui.prefs = ru.data; toast('偏好已保存', 'ok'); }
        else toast(ru.message || errText(r) || '保存失败', 'error');
        close();
      });
    }});
  }
}

// 视图注册表（新增页面在此登记）
export const VIEWS = {
  library: renderLibrary,
  search: renderSearch,
  artists: renderArtists,
  artist: renderArtist,
  playlists: renderPlaylists,
  albums: renderAlbums,
  mv: renderMv,
  downloads: renderDownloads,
  import: renderImport,
  discover: renderDiscover
};

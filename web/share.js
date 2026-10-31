// Rad Music - 歌单分享 / 曲库导出导入（前端侧）
//
// 两种分享文件：
//   .rdl 歌单（JSON）：服务端给内容，前端用 Blob 直接落盘（几十 KB）
//   .rdm 音频包（zip）：音频可能有几百 MB，服务端写到 <曲库>/exports/ 再由这里给下载链接
// 导入统一走 POST /music/share/import?filename=（原始字节），服务端按扩展名分流。
import { api, esc, errText, fmtBytes, toast, API_BASE } from './api.js';
import { icon } from './icons.js';
import { $, openModal, closeModal, contextMenu } from './app.js';

export const SHARE_ACCEPT = '.rdl,.rdm,audio/*,.mp3,.flac,.m4a,.wav,.ogg,.aac,.opus,.wma,.aiff,.ape';

// ---------------------------------------------------------------- 下载

// 文本内容 → 浏览器下载（.rdl）
export function downloadText(filename, text, mime = 'application/json;charset=utf-8') {
  const blob = new Blob([text], { type: mime });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename || 'radmusic.rdl';
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 5000);
}

// 已落盘的导出产物 → 浏览器下载（.rdm）
export function downloadExportFile(name) {
  if (!name) return;
  const anchor = document.createElement('a');
  anchor.href = API_BASE + '/music/share/download?name=' + encodeURIComponent(name);
  anchor.download = name;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
}

export async function openFolder(path) {
  if (!path) { toast('没有可打开的目录', 'error'); return; }
  const result = await api('POST', '/music/open-folder', { path });
  toast(result.code === 0 ? '已打开所在文件夹' : errText(result), result.code === 0 ? 'ok' : 'error');
}

// ---------------------------------------------------------------- 分享歌单（.rdl）

export async function sharePlaylistFile(id) {
  const result = await api('GET', '/music/share/playlist?id=' + encodeURIComponent(id));
  if (result.code !== 0) { toast(errText(result), 'error'); return; }
  const data = result.data || {};
  if (data.empty) { toast('歌单是空的，没有可分享的内容'); return; }
  downloadText(data.filename || 'playlist.rdl', JSON.stringify(data.playlist, null, 2));
  const count = Number((data.playlist && data.playlist.track_count) || 0);
  toast('已导出歌单文件（' + count + ' 首），可以发给别人用 Rad Music 导入', 'ok');
}

// ---------------------------------------------------------------- 导出音频

// 统一的导出结果弹窗：位置 + 下载 / 打开目录
function showExportResult(data, { packageMode }) {
  const count = Number(data.count) || 0;
  const bytes = Number(data.bytes) || 0;
  const skipped = Array.isArray(data.skipped) ? data.skipped : [];
  const dir = data.dir || '';
  const body =
    '<p class="note" style="margin:0 0 12px">' +
      '已导出 <strong>' + count + '</strong> 首' + (bytes ? ' · ' + fmtBytes(bytes) : '') +
      (packageMode ? '（音频包 .rdm）' : '（常规音频文件）') +
    '</p>' +
    '<div class="field-grid" style="grid-template-columns:1fr">' +
      '<label><span class="note">保存位置</span>' +
        '<input type="text" readonly value="' + esc(packageMode ? (data.file || dir) : dir) + '" /></label>' +
    '</div>' +
    (skipped.length
      ? '<p class="note" style="margin-top:10px;color:var(--text-dim)">跳过 ' + skipped.length + ' 首：' +
          esc(skipped.slice(0, 4).join('；')) + (skipped.length > 4 ? ' …' : '') + '</p>'
      : '') +
    '<p class="note" style="margin-top:10px">' +
      (packageMode
        ? '.rdm 是 zip 封装（base.json + 音频 + 封面），可在任意一台装了 Rad Music 的机器上「导入」还原。'
        : '这些都是普通音频文件，可以直接拖进任何播放器；也可以再用 Rad Music 导入回来。') +
    '</p>';
  const footer =
    (packageMode && data.download_name
      ? '<button class="btn primary" data-download>' + icon('download', 16) + '下载 .rdm</button>'
      : '') +
    '<button class="btn' + (packageMode && data.download_name ? '' : ' primary') + '" data-open-folder>' +
      icon('folder', 16) + '打开所在文件夹</button>' +
    '<button class="btn ghost" data-modal-close>关闭</button>';
  openModal({
    title: '导出完成',
    body,
    footer,
    onMount: (box) => {
      const download = $('[data-download]', box);
      if (download) download.addEventListener('click', () => downloadExportFile(data.download_name));
      const open = $('[data-open-folder]', box);
      if (open) open.addEventListener('click', () => openFolder(packageMode ? (data.file || dir) : dir));
    }
  });
}

// 导出指定 sha 列表；packageMode=true → .rdm，否则 → exports/<名字>/ 下的常规文件
export async function exportTracks(shas, { name, packageMode, playlistId, description } = {}) {
  const unique = Array.from(new Set((shas || []).filter(Boolean)));
  // 歌单导出时曲目由服务端按 playlist_id 解析，此处 shas 可以为空
  if (!unique.length && !playlistId) { toast('没有可导出的曲目'); return null; }
  const body = {
    mode: packageMode ? 'package' : 'folder',
    shas: unique,
    name: name || ''
  };
  if (playlistId) body.playlist_id = playlistId;
  if (description) body.description = description;
  toast(packageMode ? '正在打包音频…' : '正在复制音频…');
  const result = await api('POST', '/music/share/export', body);
  if (result.code !== 0) { toast(errText(result), 'error'); return null; }
  showExportResult(result.data || {}, { packageMode });
  return result.data;
}

// 歌单 → .rdm（按歌单曲目导出；在线未下载的会被跳过）
export async function exportPlaylistPackage(playlist) {
  if (!playlist || !playlist.id) return null;
  return exportTracks([], {
    name: playlist.name || '歌单导出',
    packageMode: true,
    playlistId: playlist.id,
    description: playlist.description || ''
  });
}

// 歌单入口菜单：分享 .rdl / 导出 .rdm
export function playlistShareMenu(x, y, playlist) {
  if (!playlist) return;
  contextMenu(x, y, [
    { label: '分享歌单文件（.rdl）', icon: 'download', onClick: () => sharePlaylistFile(playlist.id) },
    { label: '导出歌单音频包（.rdm）', icon: 'album', onClick: () => exportPlaylistPackage(playlist) }
  ]);
}

// ---------------------------------------------------------------- 曲库导出弹窗

export function openLibraryExportDialog({ tracks = [], filtered = false } = {}) {
  const shas = tracks.map((track) => track && track.sha).filter(Boolean);
  const localCount = tracks.filter((track) => {
    const location = String((track && track.location) || '');
    return location && !/^https?:\/\//i.test(location);
  }).length;
  if (!shas.length) { toast('当前列表里没有可导出的曲目（在线曲目请先下载）'); return; }

  const stamp = new Date().toISOString().slice(0, 10);
  openModal({
    title: '导出曲目',
    body:
      '<p class="note" style="margin:0 0 12px">当前列表 ' + tracks.length + ' 个音频版本（其中 ' + localCount +
        ' 个已有本地文件）' + (filtered ? '，已应用筛选' : '') + '。在线曲目若没有下载到曲库，导出时会自动跳过。</p>' +
      '<div class="col" style="gap:10px">' +
        '<label class="col" style="gap:5px"><span class="note">导出名称</span>' +
          '<input type="text" id="exp-name" value="' + esc('曲库导出-' + stamp) + '" /></label>' +
        '<label class="col" style="gap:5px"><span class="note">导出形式</span>' +
          '<select id="exp-mode">' +
            '<option value="folder">常规音频文件（导出为一个文件夹，文件名带序号/歌名/歌手）</option>' +
            '<option value="package">音频包 .rdm（zip：base.json + 音频 + 封面，便于整体迁移）</option>' +
          '</select></label>' +
      '</div>',
    footer:
      '<button class="btn ghost" data-cancel>取消</button>' +
      '<button class="btn primary" data-ok>' + icon('download', 16) + '开始导出</button>',
    onMount: (box) => {
      const ok = $('[data-ok]', box);
      $('[data-cancel]', box).addEventListener('click', () => closeModal());
      ok.addEventListener('click', async () => {
        const name = String(($('#exp-name', box) || {}).value || '').trim();
        const mode = ($('#exp-mode', box) || {}).value || 'folder';
        ok.disabled = true;
        closeModal();
        await exportTracks(shas, { name, packageMode: mode === 'package' });
      });
    }
  });
}

// ---------------------------------------------------------------- 导入

// 把用户选中的文件逐个上传导入；返回汇总结果
export async function importShareFiles(files, onProgress) {
  const list = Array.from(files || []);
  const summary = { playlist: 0, package: 0, audio: 0, failed: 0, tracks: 0, playlists: [] };
  for (let index = 0; index < list.length; index += 1) {
    const file = list[index];
    if (onProgress) onProgress(file, index, list.length);
    const name = String(file.name || '').toLowerCase();
    const isShare = name.endsWith('.rdl') || name.endsWith('.rdm');
    const result = await api('POST',
      '/music/share/import?filename=' + encodeURIComponent(file.name), file);
    if (result.code !== 0) {
      summary.failed += 1;
      toast('导入「' + file.name + '」失败：' + errText(result), 'error');
      continue;
    }
    const data = result.data || {};
    if (isShare) {
      if (data.kind === 'playlist') {
        summary.playlist += 1;
        summary.tracks += Number(data.track_count) || 0;
        if (data.playlist_id) summary.playlists.push({ id: data.playlist_id, name: data.name || file.name });
      } else {
        summary.package += 1;
        summary.tracks += Number(data.track_count) || Number(data.imported) || 0;
        if (data.playlist_id) summary.playlists.push({ id: data.playlist_id, name: data.playlist_name || data.name || file.name });
      }
    } else {
      summary.audio += 1;
    }
  }
  return summary;
}

// 打开系统文件选择框并导入（歌单 / 音频包 / 普通音频都收）
export function pickAndImportShare({ onDone } = {}) {
  const input = document.createElement('input');
  input.type = 'file';
  input.multiple = true;
  input.accept = SHARE_ACCEPT;
  input.style.display = 'none';
  document.body.appendChild(input);
  input.addEventListener('change', async () => {
    const files = Array.from(input.files || []);
    input.remove();
    if (!files.length) return;
    const summary = await importShareFiles(files);
    const parts = [];
    if (summary.playlist) parts.push('歌单 ' + summary.playlist + ' 个');
    if (summary.package) parts.push('音频包 ' + summary.package + ' 个');
    if (summary.audio) parts.push('音频 ' + summary.audio + ' 个');
    if (summary.failed) parts.push('失败 ' + summary.failed + ' 个');
    toast(parts.length ? '导入完成：' + parts.join('，') : '没有导入任何内容', summary.failed ? 'error' : 'ok');
    if (onDone) onDone(summary);
  });
  input.click();
}

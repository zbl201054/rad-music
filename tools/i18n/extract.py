"""从 web/ 前端源码里抽取「会出现在界面上的中文文案」，用于生成 .rlan 语言包。

用法:
    python tools/i18n/extract.py            # 打印统计
    python tools/i18n/extract.py --json out.json

抽取规则（目的是尽量贴近运行时 DOM 里的真实文本节点）：
  * 去掉 JS 注释（否则注释里的中文会污染词表）
  * 只取字符串字面量（"..." / '...' / `...`）
  * 模板字符串按 ${...} 切开，HTML 标签剥掉——剩下的才是文本节点
  * HTML 里的 title / placeholder / aria-label / alt 属性单独收集（DOM 层会翻译这些属性）
"""

import glob
import json
import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CJK = re.compile(r'[\u4e00-\u9fff]')
ATTR = re.compile(r'(?:title|placeholder|aria-label|alt|data-label)="([^"<>]*)"')
TAG = re.compile(r'<[^>]*>')
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'|`((?:[^`\\]|\\.)*)`')
VAR = re.compile(r'\$\{[^}]*\}')


def strip_comments(src: str) -> str:
    """去掉 // 与 /* */ 注释，但保留字符串里的内容。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c in '"\'':
            q = c
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == q:
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        elif c == '`':
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == '`':
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


CONCAT_DQ = re.compile(r'"([^"\\\n]*)"\s*\+\s*"([^"\\\n]*)"')
CONCAT_SQ = re.compile(r"'([^'\\\n]*)'\s*\+\s*'([^'\\\n]*)'")


def join_concats(src: str) -> str:
    """把 '上半句' + '下半句' 拼成一个字面量。

    代码里为了排版经常把一句话拆成两段字面量再相加，运行时是**一个**文本节点；
    不合并的话词表里只有两半，永远匹配不上整句。
    """
    text = src
    while True:
        joined = CONCAT_DQ.sub(r'"\1\2"', text)
        joined = CONCAT_SQ.sub(r"'\1\2'", joined)
        if joined == text:
            return joined
        text = joined


def collect() -> dict:
    counts = {}

    def add(text: str):
        # 去掉从 HTML 残片里带出来的包裹符号（> 按标题 、"播放" 之类），留下真正的文字
        text = text.strip().strip('<>"\'=,;:/\\·>').strip()
        if not text or len(text) > 420:
            return
        if not CJK.search(text):
            return
        # 纯标点/符号单元没有翻译价值
        if not re.search(r'[\u4e00-\u9fffA-Za-z0-9]', text):
            return
        # HTML 残片（属性名、引号、尖括号）不会作为文本节点出现，翻它纯属浪费
        if re.search(r'[<>"]|="|=\'|&lt;|srcset|class |style ', text):
            return
        counts[text] = counts.get(text, 0) + 1

    files = sorted(glob.glob(os.path.join(ROOT, 'web', '*.js')))
    files.append(os.path.join(ROOT, 'web', 'index.html'))
    for path in files:
        raw = strip_comments(open(path, encoding='utf-8').read())
        # 两个变体都要扫：合并后的整句覆盖「没有内联标签」的情况，
        # 原始字面量覆盖「句子里夹了 <b>/<code> 导致 DOM 被拆成多段」的情况。
        variants = [raw]
        merged = join_concats(raw)
        if merged != raw:
            variants.append(merged)
        for src in variants:
          for m in LITERAL.finditer(src):
              text = m.group(1) or m.group(2) or m.group(3) or ''
              if not CJK.search(text):
                  continue
              for attr in ATTR.findall(text):
                  for part in VAR.split(attr):
                      add(part)
              for chunk in TAG.split(text):
                  for part in VAR.split(chunk):
                      add(part)
        # index.html 的文案直接写在标签之间，不是字符串字面量，要单独取一次
        if path.endswith('.html'):
            for attr in ATTR.findall(src):
                add(attr)
            for chunk in TAG.split(src):
                if '--' in chunk or '/*' in chunk:
                    continue    # 注释块
                add(chunk)
    return counts


if __name__ == '__main__':
    counts = collect()
    if '--json' in sys.argv:
        target = sys.argv[sys.argv.index('--json') + 1]
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'w', encoding='utf-8') as handle:
            json.dump({'total': sum(counts.values()), 'strings': list(counts.keys())},
                      handle, ensure_ascii=False, indent=1)
        print('wrote', target, len(counts))
    else:
        print('unique:', len(counts), 'chars:', sum(len(k) for k in counts))
        for key, value in sorted(counts.items(), key=lambda kv: -kv[1])[:40]:
            print(value, key)

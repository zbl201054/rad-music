"""把「界面中文原文 + 译文表」编译成 .rlan 语言包。

词表来源（都在 tools/i18n/ 下，随仓库一起维护）：
    strings.json     extract.py 自动抽出的界面文案（只读，重新跑 extract.py 会重排）
    en.json          { 序号: 英文译文 }，序号对应 strings.json 里的下标
    en_gb.json       { 中文原文: 英式写法 }，只放 en-US 需要改拼写的那几条
    patterns.json    { 带占位符的中文模板: 译文 }，处理「共 12 首」这类运行时拼接
    moe_rules.json   [[匹配, 替换], ...]，萌猫语的整句改造规则

用法: python tools/i18n/build_packs.py
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, 'tools', 'i18n')
OUT = os.path.join(ROOT, 'web', 'langs')


def load(name, default):
    path = os.path.join(HERE, name)
    if not os.path.exists(path):
        return default
    with open(path, encoding='utf-8') as handle:
        return json.load(handle)


def load_en():
    """英文词表：en.json 是 { 中文原文: 英文 }，en.*.json 是可选的分片补充。
    以原文为键（不用序号），这样重新跑 extract.py 抽到新文案也不会打乱已有译文。"""
    merged = {}
    for name in sorted(os.listdir(HERE)):
        if not (name == 'en.json' or (name.startswith('en.') and name.endswith('.json'))):
            continue
        data = load(name, {})
        if isinstance(data, dict):
            merged.update({str(k): v for k, v in data.items() if isinstance(v, str) and v.strip()})
    return merged


# 英式 / 美式拼写对照。只覆盖本项目词表里真实出现的差异，不做通用转换
# （通用规则会把 size / prize / analysis 这类词改坏）。
SPELLING = [
    ('colour', 'color'), ('behaviour', 'behavior'), ('centre', 'center'),
    ('favourite', 'favorite'), ('grey', 'gray'), ('licence', 'license'),
    ('customis', 'customiz'), ('organis', 'organiz'), ('recognis', 'recogniz'),
    ('personalis', 'personaliz'), ('initialis', 'initializ'), ('normalis', 'normaliz'),
    ('synchronis', 'synchroniz'), ('optimis', 'optimiz'), ('utilis', 'utiliz'),
    ('prioritis', 'prioritiz'), ('authoris', 'authoriz'), ('finalis', 'finaliz'),
    ('summaris', 'summariz'), ('categoris', 'categoriz'), ('visualis', 'visualiz'),
    ('equalis', 'equaliz'), ('specialis', 'specializ'),
    ('analyse', 'analyze'), ('analysed', 'analyzed'), ('analysing', 'analyzing'),
]


def _apply_spelling(text, pairs):
    out = text
    for source, target in pairs:
        for src, dst in ((source, target), (source.capitalize(), target.capitalize()),
                         (source.upper(), target.upper())):
            if src in out:
                out = out.replace(src, dst)
    return out


# 词表以「英式写法」为基准维护（读起来更统一），再按目标语言统一转换：
# en-US 全部转美式，en-GB 原样保留（只额外套 en_gb.json 的手工条目）。
def to_american(text):
    return _apply_spelling(text, SPELLING)


def to_british(text):
    return _apply_spelling(text, [(a, b) for a, b in SPELLING])


def dump_pack(pack):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, pack['id'] + '.rlan')
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(pack, handle, ensure_ascii=False, indent=1)
        handle.write('\n')
    size = os.path.getsize(path)
    print('  %-8s %6d strings %4d patterns %4d rules  %6.1f KB' % (
        pack['id'], len(pack.get('strings', {})), len(pack.get('patterns', [])),
        len(pack.get('rules', [])), size / 1024))
    return path


def base_pack(pid, name, native, locale, description, **extra):
    pack = {
        'format': 'rlan',
        'format_version': 1,
        'id': pid,
        'name': name,
        'native_name': native,
        'locale': locale,
        'author': 'Rad Music',
        'version': '1.0.0',
        'description': description,
        'direction': 'ltr',
        'strings': {},
        'patterns': []
    }
    pack.update(extra)
    return pack


def main():
    strings = load('strings.json', {'strings': []})['strings']
    en = load_en()
    en_gb_patch = load('en_gb.json', {})
    patterns = load('patterns.json', {})
    moe_rules = load('moe_rules.json', [])

    # ---------- 简体中文：基准语言，界面本身就是中文，包只提供元信息 ----------
    zh_cn = base_pack('zh-CN', '中文（简体）', '简体中文', 'zh-CN',
                      '界面默认语言。其它语言包未覆盖的词条会回落成这里的原文。')
    dump_pack(zh_cn)

    # ---------- 英文（美国）：主词表 ----------
    en_us = base_pack('en-US', '英语（美国）', 'English (United States)', 'en-US',
                      '美式英语。en-GB 继承本包，只覆盖拼写差异。')
    en_raw = {source: value for source, value in en.items() if value != source}
    en_us['strings'] = {source: to_american(value) for source, value in en_raw.items()}
    missing = [source for source in strings if source not in en_raw]
    en_us['patterns'] = [[k, v] for k, v in patterns.items()]
    dump_pack(en_us)
    if missing:
        print('  en-US 缺少译文 %d 条（会回落成中文）' % len(missing))
        for source in missing[:30]:
            print('    ' + source)

    # ---------- 英文（英国）：继承 en-US，自动英式化 + 手工覆盖 ----------
    en_gb = base_pack('en-GB', '英语（英国）', 'English (United Kingdom)', 'en-GB',
                      '英式英语：继承美式词表，只覆盖 colour / customise 这类拼写差异。')
    en_gb['inherit'] = 'en-US'
    # 词表本身是按英式写法维护的，所以这里只挑出「美式化后有变化」的条目写进 en-GB，
    # 剩下没写的就是与 en-US 完全相同的部分（靠继承）。
    for source, value in en_raw.items():
        if to_american(value) != value:
            en_gb['strings'][source] = value
    en_gb['strings'].update({k: v for k, v in en_gb_patch.items() if k and v})
    dump_pack(en_gb)

    # ---------- 繁体中文：简体原文逐条转繁体（opencc s2twp，含台湾用词） ----------
    try:
        from opencc import OpenCC
        converter = OpenCC('s2twp')
    except Exception as error:      # noqa: BLE001 - 缺 opencc 时不能静默产出半成品
        print('跳过 zh-TW：未安装 opencc（pip install opencc-python-reimplemented）', file=sys.stderr)
        print(error, file=sys.stderr)
        converter = None
    if converter is not None:
        zh_tw = base_pack('zh-TW', '中文（繁體）', '繁體中文', 'zh-TW',
                          '繁體中文（台灣用語）。由簡體原文自動轉換，專有名詞可自行修訂。')
        for source in strings:
            converted = converter.convert(source)
            if converted != source:
                zh_tw['strings'][source] = converted
        # 占位符模板也要转：词条是「简体原文 → 繁体」，模板则是「简体模板 → 繁体模板」
        for key in patterns:
            converted = converter.convert(key)
            if converted != key:
                zh_tw['patterns'].append([key, converted])
        dump_pack(zh_tw)

    # ---------- 中文（萌猫语）：靠替换规则整体改造，不逐条写词表 ----------
    # 没有词条、没有占位符模式：所有文案都走 rules，这样连以后新加的界面文字也会被猫化。
    moe = base_pack('zh-Moe', '中文（萌猫语）', '喵呜～', 'zh-Moe',
                    '把界面变成猫娘语气：按替换规则逐句改造，效果覆盖全部文案。')
    moe['rules'] = moe_rules
    moe['rules_fallback'] = True
    dump_pack(moe)

    print('输出目录:', OUT)


if __name__ == '__main__':
    main()

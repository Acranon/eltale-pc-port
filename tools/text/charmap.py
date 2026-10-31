"""Eltale's text encoding: the font's 30 pages of 30 glyphs (font at ROM 0xD3A930, 0x7B8 bytes a page,
304x13 4bpp; glyph n of a page is 10 pixels wide at x = 10n). Read off the glyphs by eye.

A string is bytes up to 0xFF:
- 0x00..0x7E: glyph n of the current page; 0x7F: a space;
- 0x80 | p (p < 0x20): select page p;
- 0xA0 | n: palette n (colour);
- 0xC0 | n: font n (n < 16) or extra glyph spacing (n >= 16);
- 0xE0 | n: control: 0xE0 new line, 0xF0 new page (wait for a button), others below;
- 0xFF: end.

The kanji pages 9..26 are sorted by reading; 27..29 are later additions in no order.
"""

PAGES = [
    "0123456789!?「」『』.,…・:―♪♡～。、ー々ヴ",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ&",
    "abcdefghijklmnopqrstuvwxyz'“”",
    "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほ",
    "まみむめもやゆよらりるれろわをんぁぃぅぇぉゃゅょっ",
    "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホ",
    "マミムメモヤユヨラリルレロワヲンァィゥェォャュョッ",
    "がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽガギグゲゴ",
    "ザジズゼゾダヂヅデドバビブベボパピプペポ",
    "愛悪安暗案以意偉位囲異井遺移一印飲院因引雨運雲栄英永炎遠押王",
    "奥黄応屋踊恩音下何家果歌火花化会解回海開界階壊外街革各確活感",
    "間岸顔願危喜帰気記輝騎貴姫祈妃義議客逆休泣久究去許巨御供強教",
    "橋恐玉金近銀苦紅君形兄恵景芸決結月見険剣研元減言原限庫湖娘古",
    "呼後向光効口更航好皇荒広行香高考坑合国黒獄今困砂左座最祭再妻",
    "災作殺三散山残私仕使士子思詩師止紙姿歯死事持時自耳治式色質",
    "失室実捨車者謝借若弱主守手酒取首種呪受修終集習舟住獣重宿",
    "祝出術瞬準初書所女除助商小少晶消焼賞勝傷丈城場杖状上乗情",
    "食信寝心森神親身震真新深振進人陣図水数性星整晴正清精聖声",
    "生成静青西姓石赤昔跡説接絶先戦船千前全然組僧早草相争層送憎蔵",
    "足息属族賊続孫多他体態替待太退代対貸大脱誰探弾男壇段知地遅",
    "着中昼朝町長鳥帳直沈追通突低定底弟敵的笛天店伝殿都途渡戸土度",
    "怒堵島塔倒闘当逃盗堂道動働同導特毒届扉内南二似荷日入値年燃破",
    "葉馬拝配廃買売倍泊白爆漠麦発髪範晩番彼疲秘飛妃美備微必百氷病",
    "品夫布父浮巫負不怖吹部封風復福物分粉文聞兵平閉碧別変歩帆母宝",
    "方法砲豊放防暴忘望北牧本魔妹毎未味民眠密無夢霧名命明迷滅面",
    "木目黙戻門問夜谷野役薬厄闇由湧遊夕勇右世予様用羊要陽翼来",
    "落楽乱理離利裏立竜流旅侶料良力緑類令礼霊練牢録和話",
    "有関係欲存在角攻撃視点砦仲友非鬼鉱穏調透噴降臨波建崖絵",
    "保選了読込抜空容量規可能電源切円模型産邪東学契約",
    "条件十達吸収連違箱ヶ歴史←→",
]

CONTROLS = {0xE0: "\n", 0xF0: "\n▼\n"}


def decode(data, start=0, page=0, markup=True):
    """Decode one string from `data` at `start`; returns (text, end offset after 0xFF)."""
    out = []
    i = start
    while i < len(data):
        b = data[i]
        i += 1
        if b == 0xFF:
            break
        if b == 0x7F:
            out.append("　")
        elif b < 0x80:
            row = PAGES[page] if page < len(PAGES) else ""
            out.append(row[b] if b < len(row) else "{%d.%d}" % (page, b))
        elif b < 0xA0:
            page = b & 0x1F
        elif b < 0xC0:
            if markup and b != 0xA0:
                out.append("{c%d}" % (b & 0x1F))
        elif b < 0xE0:
            if markup and b != 0xC0:
                out.append("{f%d}" % (b & 0x1F))
        elif b in CONTROLS:
            out.append(CONTROLS[b])
        elif markup:
            out.append("{%02X}" % b)
    return "".join(out), i

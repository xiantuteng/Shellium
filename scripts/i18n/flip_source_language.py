#!/usr/bin/env python3
"""Rewrite Chinese string literals inside tr()/translate()/QT_TRANSLATE_NOOP()/QT_TR_NOOP()
to their English equivalents from tmap_all.json. Source language becomes English."""
import json, re, sys, pathlib

ROOT = pathlib.Path("/Users/john/CLionProjects/Shellium")
TMAP = json.load(open(pathlib.Path(__file__).parent / "tmap_all.json"))

CALL_RE = re.compile(r'(?<![A-Za-z0-9_])(QT_TRANSLATE_NOOP|QT_TR_NOOP|translate|tr)\s*\(')

def decode_cpp_string(s):
    """s includes surrounding quotes. Return decoded content."""
    assert s[0] == '"' and s[-1] == '"'
    body = s[1:-1]
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == '\\':
            n = body[i+1]
            mp = {'n':'\n','t':'\t','r':'\r','"':'"','\\':'\\',"'":"'",'0':'\0','a':'\a','b':'\b','f':'\f','v':'\v'}
            if n in mp:
                out.append(mp[n]); i += 2; continue
            if n == 'u':
                out.append(chr(int(body[i+2:i+6],16))); i += 6; continue
            if n == 'x':
                j = i+2
                while j < len(body) and body[j] in '0123456789abcdefABCDEF': j += 1
                out.append(chr(int(body[i+2:j],16))); i = j; continue
            out.append(n); i += 2; continue
        out.append(c); i += 1
    return ''.join(out)

def encode_cpp_string(text):
    out = ['"']
    for c in text:
        if c == '\\': out.append('\\\\')
        elif c == '"': out.append('\\"')
        elif c == '\n': out.append('\\n')
        elif c == '\t': out.append('\\t')
        elif c == '\r': out.append('\\r')
        else: out.append(c)
    out.append('"')
    return ''.join(out)

def scan_string_literal(src, i):
    """src[i] == '"'. Return (raw_with_quotes, end_index_exclusive)."""
    assert src[i] == '"'
    j = i + 1
    while j < len(src):
        if src[j] == '\\': j += 2; continue
        if src[j] == '"': return src[i:j+1], j+1
        j += 1
    raise ValueError("unterminated string")

def parse_args(src, start):
    """start = index just after '('. Return (list_of_arg_spans, close_paren_index).
    Each arg span = (arg_start, arg_end) into src (raw text, trimmed later)."""
    depth = 1
    i = start
    args = []
    cur_start = start
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            _, i = scan_string_literal(src, i); continue
        if c == "'":
            j = i+1
            while j < n:
                if src[j] == '\\': j += 2; continue
                if src[j] == "'": j += 1; break
                j += 1
            i = j; continue
        if c == '/' and i+1 < n and src[i+1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '/' and i+1 < n and src[i+1] == '*':
            j = src.find('*/', i)
            i = n if j < 0 else j+2
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append((cur_start, i))
                return args, i
        elif c == ',' and depth == 1:
            args.append((cur_start, i))
            cur_start = i + 1
        i += 1
    raise ValueError("unbalanced parens")

def literal_run(src, a, b):
    """If src[a:b] (trimmed) is purely one or more adjacent string literals (+ optional
    trailing '_s' / 'sv' UDL suffix is NOT expected here), return decoded concatenation,
    plus (run_start, run_end). Otherwise return None."""
    i = a
    n = b
    # skip leading ws
    while i < n and src[i] in ' \t\r\n': i += 1
    run_start = i
    parts = []
    saw = False
    while i < n:
        while i < n and src[i] in ' \t\r\n': i += 1
        if i < n and src[i] == '/' and i+1 < n and src[i+1] == '/':
            j = src.find('\n', i); i = n if j<0 else j; continue
        if i < n and src[i] == '/' and i+1 < n and src[i+1] == '*':
            j = src.find('*/', i); i = n if j<0 else j+2; continue
        if i < n and src[i] == '"':
            raw, i = scan_string_literal(src, i)
            parts.append(decode_cpp_string(raw)); saw = True; continue
        break
    # trailing ws
    run_end = i
    while i < n and src[i] in ' \t\r\n': i += 1
    if not saw or i != n:
        return None
    return ''.join(parts), run_start, run_end

def process(src, path, stats):
    out = []
    pos = 0
    for m in CALL_RE.finditer(src):
        name = m.group(1)
        open_paren = m.end() - 1
        try:
            args, close = parse_args(src, open_paren + 1)
        except ValueError:
            continue
        if not args:
            continue
        text_idx = 1 if name in ('translate', 'QT_TRANSLATE_NOOP') else 0
        if text_idx >= len(args):
            continue
        a, b = args[text_idx]
        lr = literal_run(src, a, b)
        if lr is None:
            continue
        decoded, rs, re_ = lr
        if decoded not in TMAP:
            if re.search(r'[一-鿿]', decoded):
                stats['unmapped'].append((str(path), decoded))
            continue
        eng = TMAP[decoded]
        if eng == decoded:
            continue
        out.append(src[pos:rs])
        out.append(encode_cpp_string(eng))
        pos = re_
        stats['count'] += 1
    out.append(src[pos:])
    return ''.join(out)

def main():
    stats = {'count': 0, 'unmapped': [], 'files': 0}
    exts = ('.cpp', '.h', '.mm')
    for base in ('src', 'tests'):
        for p in sorted((ROOT / base).rglob('*')):
            if p.suffix not in exts or not p.is_file():
                continue
            txt = p.read_text(encoding='utf-8')
            new = process(txt, p, stats)
            if new != txt:
                p.write_text(new, encoding='utf-8')
                stats['files'] += 1
    print(f"replacements: {stats['count']}  files changed: {stats['files']}")
    if stats['unmapped']:
        print(f"UNMAPPED Chinese text args ({len(stats['unmapped'])}):")
        for f, d in stats['unmapped'][:60]:
            print(" ", f, json.dumps(d, ensure_ascii=False)[:120])

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The signatures of the host functions the guest calls, for the x86-64 build.

On ARM64 the guest calls a host function directly: both sides follow AAPCS64. On x86-64 the guest's code runs
translated, its registers live in a CPU structure, and a host function is reached through a stub that moves the
arguments from the guest's registers into Windows x64's. That move needs to know, for every argument, whether it
came in a general register (x0-x7) or a floating-point one (v0-v7), and how wide it is. This script reads it from
clang's AST of the tables (TL_WRAP entries and the like) and writes one line per host function:

    { "pthread_mutex_lock", "il" },

The signature is the return type then the arguments, one letter each:
    v void   c int8   C uint8/bool   s int16   S uint16   i int32   I uint32   l 64-bit integer or any pointer
    f float  d double q 128-bit vector (the guest's long double)
    F{...}   a guest function the host may call, with its own signature inside the braces
    V        a variadic entry (TL_VA_STUB): the stub builds an AAPCS64 frame from the guest's registers
    *        a data symbol (no stub: the guest reads it in place)
A function the generic move cannot serve (a structure by value, a function-pointer argument the host would call)
is listed with the reason, and needs a handler of its own (husk-tl-x64.c); the script fails if one is not known.

Usage: gen-sigs.py <out.inc> <clang> <clang flags...> -- <file:array[,array...]>...
"""
import json, os, re, subprocess, sys, tempfile

INT = {
    'char': 'c', 'signed char': 'c', 'unsigned char': 'C', '_Bool': 'C', 'bool': 'C',
    'short': 's', 'short int': 's', 'unsigned short': 'S', 'unsigned short int': 'S',
    'int': 'i', 'signed int': 'i', 'unsigned int': 'I', 'unsigned': 'I',
    'long': 'i', 'unsigned long': 'I',                 # Windows: 32 bits (the guest's long is int64_t in our code)
    'long long': 'l', 'long long int': 'l', 'unsigned long long': 'l', 'unsigned long long int': 'l',
    '__int128': '!', 'unsigned __int128': '!',
    'float': 'f', 'double': 'd', 'long double': '!',   # mingw's long double is x87: never used across the boundary
    'void': 'v',
    '__size_t': 'l', '__ptrdiff_t': 'l', '__intptr_t': 'l', '__uintptr_t': 'l', 'size_t': 'l', 'ssize_t': 'l',
    'ptrdiff_t': 'l', 'intptr_t': 'l', 'uintptr_t': 'l',
    'int8_t': 'c', 'uint8_t': 'C', 'int16_t': 's', 'uint16_t': 'S', 'int32_t': 'i', 'uint32_t': 'I',
    'int64_t': 'l', 'uint64_t': 'l',
}

def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([': depth += 1
        if ch in ')]': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def parse_fn(qt):
    """'int (void *, const int64_t *)' -> ('int', ['void *', 'const int64_t *'], variadic)"""
    qt = re.sub(r'__attribute__\(\((cdecl|stdcall|ms_abi|sysv_abi)\)\)', '', qt).strip()
    # the return type may itself be a pointer: find the parameter list's opening paren at depth 0 from the end
    depth = 0
    for i in range(len(qt) - 1, -1, -1):
        if qt[i] == ')': depth += 1
        elif qt[i] == '(':
            depth -= 1
            if depth == 0:
                ret, params = qt[:i].strip(), qt[i + 1:-1]
                break
    else:
        raise ValueError('not a function type: ' + qt)
    ps = split_params(params)
    variadic = bool(ps) and ps[-1] == '...'
    if variadic: ps = ps[:-1]
    if ps == ['void']: ps = []
    return ret, ps, variadic

def strip_quals(t):
    t = re.sub(r'\b(const|volatile|restrict|__restrict|_Nonnull|_Nullable|struct|union|enum)\b', lambda m: m.group(1) if m.group(1) in ('struct', 'union', 'enum') else '', t)
    return ' '.join(t.split())

def classify(t, typedefs, depth=0):
    """one letter for a C type, '!' when the generic move can't carry it, 'F' for a function pointer"""
    t = strip_quals(t)
    if '(*)' in t or re.search(r'\(\s*\*\s*\w*\s*\)\s*\(', t):
        # a function pointer: the host gets a host function that runs the guest's (F{its signature})
        if depth: return '!'
        inner = re.sub(r'\(\s*\*\s*\w*\s*\)', '', t, count=1)
        try:
            ret, ps, variadic = parse_fn(inner)
        except ValueError:
            return '!'
        if variadic: return '!'
        letters = [classify(x, typedefs, depth + 1) for x in [ret] + ps]
        if any(len(l) != 1 or l in '!F?V*' for l in letters): return '!'
        return 'F{' + ''.join(letters) + '}'
    if 'vector_size' in t or t.startswith('__m128'): return 'q'
    if '*' in t or '[' in t: return 'l'
    if t in INT: return INT[t]
    if t.startswith('enum '): return 'i'
    if t.startswith('struct ') or t.startswith('union '): return '!'
    if 'vector_size' in t or '__attribute__((__vector_size__' in t or t.startswith('__m128'): return 'q'
    if t in typedefs: return classify(typedefs[t], typedefs, depth)
    return '?' + t

def decode_all(txt):
    dec = json.JSONDecoder(); i = 0; out = []
    while i < len(txt):
        while i < len(txt) and txt[i].isspace(): i += 1
        if i >= len(txt): break
        o, i = dec.raw_decode(txt, i); out.append(o)
    return out

def ast(clang, flags, src, flt):
    r = subprocess.run([clang] + flags + ['-fsyntax-only', '-Xclang', '-ast-dump=json', '-Xclang', '-ast-dump-filter=' + flt, src],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('clang failed on %s:\n%s' % (src, r.stderr[-3000:]))
    return decode_all(r.stdout)

def walk(n, f):
    if isinstance(n, dict):
        f(n)
        for v in n.values(): walk(v, f)
    elif isinstance(n, list):
        for v in n: walk(v, f)

def entries_of(var):
    """(name, referenced decl) for each { "name", fn } initializer of a table array"""
    out = []
    def visit(n):
        if n.get('kind') != 'InitListExpr': return
        inner = n.get('inner') or []
        if len(inner) < 2: return
        lits, refs = [], []
        walk(inner[0], lambda m: lits.append(m['value']) if m.get('kind') == 'StringLiteral' else None)
        walk(inner[1], lambda m: refs.append(m['referencedDecl']) if m.get('kind') == 'DeclRefExpr' else None)
        if len(lits) == 1 and refs:
            out.append((json.loads(lits[0]), refs[0]))
    walk(var, visit)
    return out

def slot_assignments(fn, target, prefix):
    """array[n] = (cast)fn  ->  (prefix#n, fn's decl)"""
    out = []
    def visit(n):
        if n.get('kind') != 'BinaryOperator' or n.get('opcode') != '=': return
        lhs, rhs = n['inner'][0], n['inner'][1]
        if lhs.get('kind') != 'ArraySubscriptExpr': return
        names = []
        walk(lhs['inner'][0], lambda m: names.append(m['referencedDecl'].get('name')) if m.get('kind') == 'DeclRefExpr' else None)
        idx = [const_int(lhs['inner'][1])]
        if names[:1] != [target] or idx[0] is None: return
        refs = []
        walk(rhs, lambda m: refs.append(m['referencedDecl']) if m.get('kind') == 'DeclRefExpr' else None)
        if refs: out.append(('%s#%d' % (prefix, idx[0]), refs[0]))
    walk(fn, visit)
    return out

def const_int(n):
    """the value of a small constant integer expression (a literal, a sum, casts and parentheses), or None"""
    k = n.get('kind')
    if k == 'IntegerLiteral': return int(n['value'])
    if k in ('ImplicitCastExpr', 'ParenExpr', 'CStyleCastExpr', 'ConstantExpr'): return const_int(n['inner'][0])
    if k == 'BinaryOperator' and n.get('opcode') in ('+', '-', '*'):
        a, b = const_int(n['inner'][0]), const_int(n['inner'][1])
        if a is None or b is None: return None
        return a + b if n['opcode'] == '+' else a - b if n['opcode'] == '-' else a * b
    return None

def positional(var, prefix):
    out = []
    for n in var.get('inner', []):
        if n.get('kind') != 'InitListExpr': continue
        for i, item in enumerate(n.get('inner', [])):
            refs = []
            walk(item, lambda m: refs.append(m['referencedDecl']) if m.get('kind') == 'DeclRefExpr' else None)
            if refs: out.append(('%s#%d' % (prefix, i), refs[0]))
    return out

def resolve_typedefs(clang, flags, src, names):
    """the canonical type of each typedef name, from a probe compiled in the same file's context"""
    names = {n for n in names if re.match(r'^[A-Za-z_]\w*$', n)}
    if not names: return {}
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False, dir=os.path.dirname(os.path.abspath(src))) as f:
        f.write('#include "%s"\n' % os.path.basename(src))
        for i, n in enumerate(sorted(names)): f.write('%s __tlprobe_%d;\n' % (n, i))
        probe = f.name
    try:
        objs = ast(clang, flags, probe, '__tlprobe_')
    finally:
        os.unlink(probe)
    found = {}
    order = sorted(names)
    for o in objs:
        m = re.match(r'__tlprobe_(\d+)$', o.get('name', ''))
        if m:
            t = o.get('type', {})
            found[order[int(m.group(1))]] = t.get('desugaredQualType', t.get('qualType'))
    return found

def main():
    out_path, clang = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    sep = rest.index('--')
    flags, specs = rest[:sep], rest[sep + 1:]
    sigs, problems, va_names = {}, [], set()
    for spec in specs:
        src, arrays = spec.split(':')
        pending = []
        for arr in arrays.split(','):
            if arr.startswith('%'):
                # %gl+egl: every function declared with one of these prefixes (the GLES and EGL headers)
                for prefix in arr[1:].split('+'):
                    for o in ast(clang, flags, src, prefix):
                        nm = o.get('name', '')
                        if o.get('kind') == 'FunctionDecl' and nm.startswith(prefix) and len(nm) > len(prefix) and nm[len(prefix)].isupper():
                            pending.append((nm, {'kind': 'FunctionDecl', 'name': nm, 'type': o['type']}))
                continue
            if arr.startswith('@'):
                # @array=prefix@fn1+fn2: the assignments array[n] = fn in those functions, named prefix#n
                target, rest = arr[1:].split('=')
                prefix, fns = rest.split('@')
                for fname in fns.split('+'):
                    objs = [o for o in ast(clang, flags, src, fname) if o.get('name') == fname and o.get('kind') == 'FunctionDecl']
                    if not objs: sys.exit('%s: no function %s' % (src, fname))
                    pending += slot_assignments(objs[-1], target, prefix)
                continue
            if arr.startswith('#'):
                # #array=prefix: an initialised array of functions, positional, named prefix#i
                target, prefix = arr[1:].split('=')
                objs = [o for o in ast(clang, flags, src, target) if o.get('name') == target and o.get('kind') == 'VarDecl']
                if not objs: sys.exit('%s: no array %s' % (src, target))
                pending += positional(objs[-1], prefix)
                continue
            objs = [o for o in ast(clang, flags, src, arr) if o.get('name') == arr]
            if not objs: sys.exit('%s: no array %s' % (src, arr))
            pending += entries_of(objs[0])
        unknown = set()
        for name, ref in pending:
            if ref.get('kind') == 'FunctionDecl':
                ret, ps, _ = parse_fn(ref['type']['qualType'])
                for t in [ret] + ps:
                    c = classify(t, {})
                    if c.startswith('?'): unknown.add(c[1:])
        typedefs = resolve_typedefs(clang, flags, src, unknown)
        for name, ref in pending:
            kind = ref.get('kind')
            if kind == 'VarDecl':
                sigs[name] = '*'; continue
            if kind != 'FunctionDecl':
                problems.append('%s: refers to a %s' % (name, kind)); continue
            fn = ref.get('name', '')
            ret, ps, variadic = parse_fn(ref['type']['qualType'])
            if fn.startswith('tl_va_') and ret == 'void' and not ps:
                sigs[name] = 'V'; va_names.add(name); continue
            letters = [classify(t, typedefs) for t in [ret] + ps]
            bad = [(t, c) for t, c in zip([ret] + ps, letters) if c == '!' or c.startswith('?') or (c.startswith('F') and t is ret)]
            if variadic: bad.append(('...', '!'))
            if bad:
                problems.append('%s (%s): %s' % (name, fn, ', '.join('%s=%s' % (t, c) for t, c in bad)))
                sigs[name] = '!' + ''.join(c if len(c) == 1 or c.startswith('F') else '?' for c in letters)
                continue
            if len(ps) > 16: problems.append('%s: %d arguments' % (name, len(ps)))
            sigs[name] = ''.join(letters)
    with open(out_path, 'w') as f:
        f.write('/* Generated by gen-sigs.py: the host functions the guest calls, and how their arguments move. */\n')
        for name in sorted(sigs):
            f.write('{ "%s", "%s" },\n' % (name, sigs[name]))
    with open(out_path + '.problems', 'w') as f:
        f.write('\n'.join(problems) + '\n')
    print('%d signatures, %d variadic, %d need a handler of their own (%s.problems)' % (len(sigs), len(va_names), len(problems), out_path))

if __name__ == '__main__':
    main()

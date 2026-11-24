#!/usr/bin/env python3
"""
Generate big-endian converters for RM control parameter structures.

Input : a preprocessed translation unit (gcc -E -P) that includes the control
        headers, and the original control headers (for the command ids).
Output: a C header with one static converter per parameter structure and a
        dispatcher _kgspCtrlSwapParams(cmd, pParams, paramsSize).

The generated code never states a size or an offset.  It reverses
sizeof(member) bytes of each scalar member through the real member
expression, so the compiler supplies the layout.  The parser only decides the
KIND of each member: scalar, byte (nothing to do), array, nested struct, or
unsupported.  A structure with an unsupported member (union, bit-field,
pointer, flexible array) is refused, and so is every command that uses it;
the refusals are listed in the report.
"""
import re
import sys

KW_SKIP = {'__extension__', '__restrict', '__restrict__', 'volatile',
           'const', 'extern'}
BASE_WORDS = {'char', 'short', 'int', 'long', 'unsigned', 'signed',
              'float', 'double', '_Bool', 'void'}
BYTE_NAMES = {'char', 'unsigned char', 'signed char', '_Bool'}
OPAQUE_SCALARS = {'NvP64', 'NvP32', 'NvUPtr'}


def strip_attributes(s):
    out = []
    i = 0
    n = len(s)
    while i < n:
        j = s.find('__attribute__', i)
        if j < 0:
            out.append(s[i:])
            break
        out.append(s[i:j])
        k = j + len('__attribute__')
        while k < n and s[k] in ' \t\n':
            k += 1
        if k < n and s[k] == '(':
            depth = 0
            while k < n:
                if s[k] == '(':
                    depth += 1
                elif s[k] == ')':
                    depth -= 1
                    if depth == 0:
                        k += 1
                        break
                k += 1
        i = k
    return ''.join(out)


TOK = re.compile(
    r'[A-Za-z_][A-Za-z0-9_]*|0[xX][0-9a-fA-F]+[uUlL]*|[0-9]+[uUlL]*'
    r'|\.\.\.|[{}\[\]();,*:=<>+\-/%&|^~!?.]'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def tokenize(text):
    return TOK.findall(text)


class Struct:
    def __init__(self, kind, tag):
        self.kind = kind          # 'struct' or 'union'
        self.tag = tag
        self.members = []         # list of (name, type)
        self.complete = False

class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0
        self.typedefs = {}        # name -> type
        self.tags = {}            # (kind, tag) -> Struct

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else None

    def next(self):
        v = self.t[self.i]
        self.i += 1
        return v

    def expect(self, v):
        x = self.next()
        if x != v:
            raise ValueError('expected %r got %r at token %d' % (v, x, self.i))

    def skip_balanced(self, open_, close):
        depth = 0
        while True:
            x = self.next()
            if x == open_:
                depth += 1
            elif x == close:
                depth -= 1
                if depth == 0:
                    return

    # Types: ('base', words) ('named', n) ('struct', Struct) ('enum',)
    #        ('ptr', t) ('array', t, dimtokens) ('func',) ('bitfield',)
    def parse_type_spec(self):
        words = []
        while True:
            x = self.peek()
            if x is None:
                break
            if x in KW_SKIP:
                self.next()
                continue
            if x in ('struct', 'union'):
                self.next()
                tag = None
                if self.peek() != '{':
                    tag = self.next()
                key = (x, tag)
                if tag is not None and key in self.tags:
                    st = self.tags[key]
                else:
                    st = Struct(x, tag)
                    if tag is not None:
                        self.tags[key] = st
                if self.peek() == '{':
                    self.next()
                    st.members = self.parse_members()
                    st.complete = True
                return ('struct', st)
            if x == 'enum':
                self.next()
                if self.peek() != '{':
                    self.next()
                if self.peek() == '{':
                    self.skip_balanced('{', '}')
                return ('enum',)
            if x in BASE_WORDS:
                words.append(self.next())
                continue
            if re.match(r'[A-Za-z_]', x) and not words:
                self.next()
                return ('named', x)
            break
        if not words:
            raise ValueError('no type at token %d (%r)' % (self.i, self.peek()))
        return ('base', ' '.join(words))

    def parse_declarator(self, base):
        """Returns (name, type); name is None for anonymous members."""
        t = base
        while self.peek() == '*':
            self.next()
            while self.peek() in KW_SKIP:
                self.next()
            t = ('ptr', t)
        if self.peek() == '(':
            # function pointer or parenthesised declarator: unsupported
            self.skip_balanced('(', ')')
            if self.peek() == '(':
                self.skip_balanced('(', ')')
            return (None, ('func',))
        name = None
        x = self.peek()
        if x is not None and re.match(r'[A-Za-z_]', x):
            name = self.next()
        dims = []
        while self.peek() == '[':
            self.next()
            d = []
            depth = 1
            while True:
                x = self.next()
                if x == '[':
                    depth += 1
                elif x == ']':
                    depth -= 1
                    if depth == 0:
                        break
                d.append(x)
            dims.append(d)
        for d in reversed(dims):
            t = ('array', t, d)
        if self.peek() == ':':
            self.next()
            while self.peek() not in (',', ';'):
                self.next()
            t = ('bitfield',)
        return (name, t)

    def parse_members(self):
        members = []
        while self.peek() != '}':
            base = self.parse_type_spec()
            while True:
                name, t = self.parse_declarator(base)
                members.append((name, t))
                if self.peek() == ',':
                    self.next()
                    continue
                break
            self.expect(';')
        self.expect('}')
        return members

    def parse_typedef(self):
        self.expect('typedef')
        base = self.parse_type_spec()
        while True:
            name, t = self.parse_declarator(base)
            if name:
                self.typedefs[name] = t
            if self.peek() == ',':
                self.next()
                continue
            break
        self.expect(';')

    def run(self):
        depth = 0
        n = len(self.t)
        while self.i < n:
            x = self.t[self.i]
            if x == 'typedef' and depth == 0:
                save = self.i
                try:
                    self.parse_typedef()
                except (ValueError, IndexError):
                    # resynchronise at the next top-level ';'
                    self.i = save + 1
                    d = 0
                    while self.i < n:
                        y = self.t[self.i]
                        if y == '{':
                            d += 1
                        elif y == '}':
                            d -= 1
                        elif y == ';' and d <= 0:
                            self.i += 1
                            break
                        self.i += 1
                continue
            if x == '{':
                depth += 1
            elif x == '}':
                depth -= 1
            self.i += 1

class Gen:
    def __init__(self, parser):
        self.p = parser
        self.func_for = {}      # typedef name -> function name, or None
        self.reasons = {}       # typedef name -> why it was refused
        self.out_funcs = []     # emitted C, dependencies first

    def resolve(self, t):
        for _ in range(50):
            if t[0] != 'named':
                return t
            nm = t[1]
            if nm in OPAQUE_SCALARS:
                return ('ptr', t)
            if nm not in self.p.typedefs:
                return ('unknown', nm)
            t = self.p.typedefs[nm]
        return ('unknown', 'typedef loop')

    def kind(self, t):
        r = self.resolve(t)
        k = r[0]
        if k == 'base':
            words = r[1].split()
            if r[1] in BYTE_NAMES:
                return 'byte'
            if 'long' in words and words.count('long') < 2:
                return 'unsupported:long has no fixed width'
            if r[1] == 'void':
                return 'unsupported:void'
            return 'scalar'
        if k == 'enum':
            return 'scalar'
        if k == 'struct':
            return 'struct' if r[1].kind == 'struct' else 'unsupported:union'
        if k == 'array':
            return 'array'
        if k == 'ptr':
            return 'unsupported:pointer'
        if k == 'bitfield':
            return 'unsupported:bit-field'
        if k == 'func':
            return 'unsupported:function pointer'
        return 'unsupported:unknown type %s' % (r[1] if len(r) > 1 else k)

    @staticmethod
    def ident(s):
        return re.sub(r'[^A-Za-z0-9_]', '_', s)

    def emit_value(self, t, expr, depth, lines, indent):
        """Append C that converts the object `expr` of type t.
        Returns None on success or a reason string."""
        pad = ' ' * indent
        k = self.kind(t)
        r = self.resolve(t)
        if k == 'byte':
            return None
        if k == 'scalar':
            lines.append('%sKGSP_RPC_REVERSE(%s);' % (pad, expr))
            return None
        if k == 'array':
            if not r[2]:
                return 'flexible array'
            if self.kind(r[1]) == 'byte':
                return None
            iv = 'i%d' % depth
            lines.append('%sfor (%s = 0; %s < NV_ARRAY_ELEMENTS(%s); %s++)'
                         % (pad, iv, iv, expr, iv))
            lines.append('%s{' % pad)
            why = self.emit_value(r[1], '%s[%s]' % (expr, iv),
                                  depth + 1, lines, indent + 4)
            lines.append('%s}' % pad)
            return why
        if k == 'struct':
            st = r[1]
            if t[0] == 'named':
                fn, why = self.struct_func(t)
                if why:
                    return why
                lines.append('%s%s(&%s);' % (pad, fn, expr))
                return None
            if not st.complete:
                return 'incomplete struct %s' % st.tag
            return self.emit_members(st, expr, depth, lines, indent)
        return k.split(':', 1)[1]

    def emit_members(self, st, expr, depth, lines, indent):
        for (mn, mt) in st.members:
            if mn is None:
                # anonymous struct member: its fields belong to the parent
                if self.kind(mt) != 'struct':
                    return 'anonymous non-struct member'
                sub = self.resolve(mt)[1]
                why = self.emit_members(sub, expr, depth, lines, indent)
            else:
                why = self.emit_value(mt, '%s.%s' % (expr, mn),
                                      depth, lines, indent)
                if why:
                    why = '%s: %s' % (mn, why)
            if why:
                return why
        return None

    def struct_func(self, t):
        """Returns (function name, None) or (None, reason).  The function is
        generated on first use."""
        key = t[1]
        if key in self.func_for:
            return self.func_for[key], self.reasons.get(key)
        st = self.resolve(t)[1]
        if not st.complete:
            self.func_for[key] = None
            self.reasons[key] = 'incomplete struct'
            return None, 'incomplete struct'
        fn = '_kgspCtrlSwap_%s' % self.ident(key)
        self.func_for[key] = fn
        body = []
        why = self.emit_members(st, '(*p)', 0, body, 4)
        if why:
            self.func_for[key] = None
            self.reasons[key] = why
            return None, why
        text = '\n'.join(body)
        names = []
        d = 0
        while re.search(r'\bi%d\b' % d, text):
            names.append('i%d' % d)
            d += 1
        decl = '    NvU32 %s;\n\n' % ', '.join(names) if names else ''
        if not text:
            text = '    (void)p;'
        src = 'static void\n%s(%s *p)\n{\n%s%s\n}\n' % (fn, key, decl, text)
        self.out_funcs.append((fn, src))
        return fn, None

    def reachable(self, roots):
        """Generated functions needed by the root functions, dependencies
        first, in generation order."""
        src = dict(self.out_funcs)
        need = set()
        todo = list(roots)
        while todo:
            f = todo.pop()
            if f in need:
                continue
            need.add(f)
            todo.extend(re.findall(r'_kgspCtrlSwap_\w+', src[f]))
            todo = [x for x in todo if x != f]
        return [(f, s) for (f, s) in self.out_funcs if f in need]

CMD_RE = re.compile(
    r'#define\s+(NV[0-9A-Fa-f]+_CTRL_CMD_\w+)\s+\(?\s*(0x[0-9a-fA-F]+)[uU]?\s*\)?'
    r'\s*/\*\s*finn:\s*Evaluated from\s*"[^"]*\|\s*(\w+?)_MESSAGE_ID"')


def read_commands(paths):
    cmds = []
    for p in paths:
        with open(p, 'r', errors='replace') as f:
            txt = f.read()
        for m in CMD_RE.finditer(txt):
            cmds.append((m.group(1), int(m.group(2), 16), m.group(3)))
    return cmds


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--pp', required=True,
                    help='preprocessed translation unit (gcc -E -P)')
    ap.add_argument('--hdr', nargs='+', required=True,
                    help='control headers that define the commands')
    ap.add_argument('--out', required=True)
    ap.add_argument('--report', default=None)
    ap.add_argument('--only', nargs='*', default=None,
                    help='restrict to these command names')
    a = ap.parse_args()

    text = strip_attributes(open(a.pp, 'r', errors='replace').read())
    pr = Parser(tokenize(text))
    pr.run()
    g = Gen(pr)

    handled, refused, seen = [], [], set()
    for (name, val, sname) in read_commands(a.hdr):
        if (a.only and name not in a.only) or val in seen:
            continue
        seen.add(val)
        t = ('named', sname)
        r = g.resolve(t)
        if sname not in pr.typedefs:
            refused.append((name, val, sname, 'parameter typedef not found'))
        elif r[0] != 'struct' or r[1].kind != 'struct':
            refused.append((name, val, sname, 'parameter type is not a struct'))
        else:
            fn, why = g.struct_func(t)
            if why:
                refused.append((name, val, sname, why))
            else:
                handled.append((name, val, sname, fn))

    funcs = g.reachable([h[3] for h in handled])
    o = []
    o.append('/*\n * GENERATED by kernel-illumos/gen-ctrl-endian.py. DO NOT EDIT.\n *\n'
             ' * Big-endian converters for RM control parameter structures that cross\n'
             ' * the GSP message queue as raw (not FINN-serialized) bytes.  Each converter\n'
             ' * reverses sizeof(member) bytes of every multi-byte member and leaves byte\n'
             ' * members alone.  Reversal is its own inverse, so one converter serves both\n'
             ' * directions.  Structures containing a union, bit-field, pointer or\n'
             ' * flexible array are not generated; their commands are reported as\n'
             ' * "NOT converted" by the caller.\n */\n\n')
    o.append('#if NVCPU_IS_BIG_ENDIAN\n\n')
    for (_, s) in funcs:
        o.append(s + '\n')
    o.append('/* NV_TRUE: command known and buffer large enough; params converted. */\n'
             'static NvBool\n_kgspCtrlSwapParams(NvU32 cmd, NvU8 *pParams, NvU32 paramsSize)\n'
             '{\n    switch (cmd)\n    {\n')
    for (name, val, sname, fn) in sorted(handled, key=lambda x: x[1]):
        o.append('        case 0x%08x: /* %s */\n'
                 '            if (paramsSize < sizeof(%s))\n'
                 '                return NV_FALSE;\n'
                 '            %s((%s *)pParams);\n'
                 '            return NV_TRUE;\n' % (val, name, sname, fn, sname))
    o.append('        default:\n            break;\n    }\n\n'
             '    return NV_FALSE;\n}\n\n#endif /* NVCPU_IS_BIG_ENDIAN */\n')
    open(a.out, 'w').write(''.join(o))

    if a.report:
        with open(a.report, 'w') as f:
            f.write('handled commands: %d\nrefused commands: %d\n'
                    'converter functions: %d\n\n'
                    % (len(handled), len(refused), len(funcs)))
            for (name, val, sname, why) in sorted(refused, key=lambda x: x[1]):
                f.write('REFUSED 0x%08x %s (%s): %s\n' % (val, name, sname, why))
    print('handled %d, refused %d, functions %d'
          % (len(handled), len(refused), len(funcs)))


if __name__ == '__main__':
    main()

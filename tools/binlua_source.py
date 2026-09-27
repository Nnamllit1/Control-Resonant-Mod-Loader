"""Conservative per-prototype source reconstruction; never executes bytecode."""
import json


class Unsupported(ValueError):
    pass


def quote(raw):
    # Lua decimal escapes preserve arbitrary byte strings, including invalid UTF-8.
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92) else f'\\{b:03d}' for b in raw) + '"'


def reconstruct(doc, pid):
    """Return an analysis fragment, or explicit reasons with no partial source.

    Registers remain explicit to preserve evaluation order. The caller supplies
    the original environment, mutable upvalue cells and resolved import values.
    This is not an automatic replacement script or a VM security verifier.
    """
    p = doc['prototypes'][pid]
    code = p['instructions']
    by_pc = {i['pc']: i for i in code}
    previous = {i['pc'] + i['size']: i for i in code}
    unsupported = []
    arithmetic = {'ADD': '+', 'SUB': '-', 'MUL': '*', 'DIV': '/', 'MOD': '%', 'POW': '^',
                  'AND': 'and', 'OR': 'or', 'IDIV': '//'}
    simple = {'NOP', 'LOADNIL', 'LOADN', 'LOADK', 'LOADKX', 'LOADB', 'MOVE', 'GETGLOBAL', 'SETGLOBAL',
              'GETUPVAL', 'SETUPVAL', 'GETIMPORT', 'GETTABLE', 'SETTABLE', 'GETTABLEKS', 'SETTABLEKS',
              'GETTABLEN', 'SETTABLEN', 'NEWTABLE', 'NOT', 'MINUS', 'LENGTH', 'CONCAT', 'CALL', 'RETURN',
              'SUBRK', 'DIVRK', 'PREPVARARGS', 'JUMP', 'JUMPBACK', 'JUMPX'}
    comparisons = {'JUMPIFEQ': '==', 'JUMPIFLE': '<=', 'JUMPIFLT': '<',
                   'JUMPIFNOTEQ': '==', 'JUMPIFNOTLE': '<=', 'JUMPIFNOTLT': '<'}
    conditional = set(comparisons) | {'JUMPIF', 'JUMPIFNOT', 'JUMPXEQKNIL', 'JUMPXEQKB', 'JUMPXEQKN', 'JUMPXEQKS'}
    for i in code:
        op = i['op']
        reason = None
        if op not in simple | conditional and op not in arithmetic and not (op.endswith('K') and op[:-1] in arithmetic) and not op.startswith('FASTCALL'):
            reason = 'unsupported opcode ' + op
        elif op == 'CALL' and (not i['b'] or not i['c']) or op == 'RETURN' and not i['b']:
            reason = 'dynamic multiple-result flow'
        elif op == 'LOADB' and i['c']:
            reason = 'boolean load with jump'
        elif 'target' in i and i['target'] <= i['pc']:
            reason = 'backward control flow'
        if reason:
            unsupported.append({'pc': i['pc'], 'reason': reason})
    if unsupported:
        return {'prototype': pid, 'status': 'unsupported', 'reasons': unsupported, 'source': None}

    def r(n):
        return f'r[{n}]'

    def text(s):
        return quote(bytes.fromhex(doc['strings'][s]['hex']))

    def k(index):
        c = p['constants'][index]
        kind = c['kind']
        if kind == 'nil':
            return 'nil'
        if kind == 'boolean':
            return 'true' if c['value'] else 'false'
        if kind == 'number':
            if isinstance(c['value'], dict):
                return {'nan': '(0/0)', 'inf': '(1/0)', '-inf': '(-1/0)'}[c['value']['nonfinite']]
            return repr(c['value'])
        if kind == 'string':
            return text(c['string'])
        # Four-component vector construction is engine-specific, not ordinary Lua.
        raise Unsupported('constant kind ' + kind)

    def condition(i):
        op, a, aux = i['op'], i['a'], i.get('aux')
        if op == 'JUMPIF':
            return r(a)
        if op == 'JUMPIFNOT':
            return f'not {r(a)}'
        if op in comparisons:
            expr = f'{r(a)} {comparisons[op]} {r(aux & 255)}'
            return f'not ({expr})' if op.startswith('JUMPIFNOT') else expr
        value = 'nil' if op == 'JUMPXEQKNIL' else ('true' if aux & 1 else 'false') if op == 'JUMPXEQKB' else k(aux & 0xffffff)
        expr = f'{r(a)} == {value}'
        return f'not ({expr})' if aux & 0x80000000 else expr

    def statement(i):
        op, a, b, c, d, aux = i['op'], i['a'], i['b'], i['c'], i['d'], i.get('aux')
        value = None
        if op == 'LOADNIL': value = 'nil'
        elif op == 'LOADB': value = 'true' if b else 'false'
        elif op == 'LOADN': value = str(d)
        elif op in ('LOADK', 'LOADKX'): value = k(d if op == 'LOADK' else aux)
        elif op == 'MOVE': value = r(b)
        elif op == 'GETGLOBAL': value = f'env[{k(aux)}]'
        elif op == 'SETGLOBAL': return f'env[{k(aux)}] = {r(a)}'
        elif op == 'GETUPVAL': value = f'uv[{b}].value'
        elif op == 'SETUPVAL': return f'uv[{b}].value = {r(a)}'
        elif op == 'GETIMPORT':
            path = [doc['strings'][s]['text'] for s in p['constants'][d]['path_strings']]
            return f'{r(a)} = imports[{d}] -- import {json.dumps(path, ensure_ascii=True)}'
        elif op == 'GETTABLE': value = f'{r(b)}[{r(c)}]'
        elif op == 'SETTABLE': return f'{r(b)}[{r(c)}] = {r(a)}'
        elif op == 'GETTABLEKS': value = f'{r(b)}[{k(aux)}]'
        elif op == 'SETTABLEKS': return f'{r(b)}[{k(aux)}] = {r(a)}'
        elif op == 'GETTABLEN': value = f'{r(b)}[{c + 1}]'
        elif op == 'SETTABLEN': return f'{r(b)}[{c + 1}] = {r(a)}'
        elif op == 'NEWTABLE': value = '{}'
        elif op in arithmetic: value = f'{r(b)} {arithmetic[op]} {r(c)}'
        elif op.endswith('K') and op[:-1] in arithmetic: value = f'{r(b)} {arithmetic[op[:-1]]} {k(c)}'
        elif op in ('SUBRK', 'DIVRK'): value = f'{k(b)} {"-" if op == "SUBRK" else "/"} {r(c)}'
        elif op in ('NOT', 'MINUS', 'LENGTH'): value = {'NOT': 'not ', 'MINUS': '-', 'LENGTH': '#'}[op] + r(b)
        elif op == 'CONCAT': value = ' .. '.join(r(n) for n in range(b, c + 1))
        elif op == 'CALL':
            call = f'{r(a)}(' + ', '.join(r(n) for n in range(a + 1, a + b)) + ')'
            return (', '.join(r(n) for n in range(a, a + c - 1)) + ' = ' if c > 1 else '') + call
        elif op == 'RETURN':
            return 'do return' + (' ' + ', '.join(r(n) for n in range(a, a + b - 1)) if b > 1 else '') + ' end'
        elif op in ('NOP', 'PREPVARARGS') or op.startswith('FASTCALL'):
            return None  # FASTCALL's ordinary fallback remains in the output.
        if value is not None:
            return f'{r(a)} = {value}'
        raise Unsupported('unstructured ' + op)

    def region(start, end, indent, depth=0):
        if depth > 32:
            raise Unsupported('structured branch nesting exceeds limit')
        lines, pc = [], start
        while pc < end:
            i = by_pc[pc]
            next_pc = pc + i['size']
            if i['op'] in conditional:
                target = i['target']
                if not next_pc <= target <= end:
                    raise Unsupported(f'crossing branch at pc {pc}')
                # A forward jump immediately before the branch target denotes
                # a conventional if/else join. Other forward shapes are refused.
                prior = previous.get(target)
                join = prior['target'] if prior and prior['op'] == 'JUMP' and prior['pc'] >= next_pc and prior['target'] > target else None
                lines.append(indent + f'if not ({condition(i)}) then -- pc {pc}')
                if join is not None:
                    if join > end:
                        raise Unsupported(f'crossing else join at pc {pc}')
                    lines.extend(region(next_pc, prior['pc'], indent + '  ', depth + 1))
                    lines.append(indent + 'else')
                    lines.extend(region(target, join, indent + '  ', depth + 1))
                else:
                    lines.extend(region(next_pc, target, indent + '  ', depth + 1))
                lines.append(indent + 'end')
                pc = join if join is not None else target
                continue
            if i['op'] in ('JUMP', 'JUMPBACK', 'JUMPX'):
                if i['target'] != next_pc:
                    raise Unsupported(f'unstructured jump at pc {pc}')
            else:
                line = statement(i)
                if line:
                    lines.append(indent + line)
            pc = next_pc
        return lines

    try:
        args = [f'arg{n}' for n in range(p['parameters'])] + (['...'] if p['vararg'] else [])
        lines = ['-- Analysis fragment: original comments/local names are not reconstructed.',
                 '-- Supply original env, mutable uv[index].value cells and imports[index].',
                 f'-- Prototype {pid}, definition line {p["defined_line"]}; register-oriented Luau.',
                 'return function(env, uv, imports)', '  return function(' + ', '.join(args) + ')', '    local r = {}']
        lines += [f'    r[{n}] = arg{n}' for n in range(p['parameters'])]
        lines += region(0, len(p['code']), '    ')
        lines += ['  end', 'end']
        return {'prototype': pid, 'status': 'reconstructed_fragment', 'reasons': [], 'source': '\n'.join(lines) + '\n'}
    except Unsupported as error:
        return {'prototype': pid, 'status': 'unsupported', 'reasons': [{'reason': str(error)}], 'source': None}

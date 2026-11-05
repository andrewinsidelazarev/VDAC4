"""Получить регистровое ядро SID из cRSID, сохранив порядок всех операций.

6502 исполняет короткие операции над локальными регистрами; медленный путь
шины вынесен из каждой команды. Эталон cRSID при этом остаётся неизменным.
Заодно разделяется init на подготовку, прерываемый цикл и завершение.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'modules/sid/main/cpu_generated.inc'


def uncomment(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


def generate():
    source = uncomment((ROOT / 'components/crsid/C64/CPU.c').read_text(encoding='utf-8'))
    helpers = source[source.index('static INLINE void addrModeImmediate'):source.index('unsigned char cRSID_emulateCPU')]
    body = source[source.index('unsigned char cRSID_emulateCPU'):]
    # Встроенные помощники получают локальные регистры, шина их не видит.
    names = re.findall(r'static INLINE (?:unsigned char|void) (\w+)\s*\(', helpers)
    code = helpers + body
    for name in names:
        code = re.sub(r'\b' + name + r'\s*\(\s*(?:void\s*)?\)', 'fast_' + name + '(r)', code)
        code = re.sub(r'\b' + name + r'\s*\(', 'fast_' + name + '(r, ', code)
    code = re.sub(r'(static INLINE (?:unsigned char|void) fast_\w+\()r', r'\1cRSID_CPUinstance* r', code)
    code = code.replace('cRSID_C64.CPU.', 'r->')
    code = re.sub(r'\brd\s*\(', 'fast_read(', code)
    code = re.sub(r'\bwr2\s*\(', 'fast_write_store(', code)
    code = re.sub(r'\bwr\s*\(', 'fast_write_rmw(', code)
    code = code.replace('unsigned char cRSID_emulateCPU (void) {',
                        'static unsigned char fast_cpu(cRSID_CPUinstance* state) {\n cRSID_CPUinstance regs = *state;\n cRSID_CPUinstance* r = &regs;')
    code = code.replace('static INLINE char cRSID_handleCPUinterrupts ()',
                        'static INLINE char fast_interrupts(cRSID_CPUinstance* r)')
    # Записать регистры при каждом из четырёх ранних выходов тоже.
    start = code.index('static unsigned char fast_cpu')
    stop = code.index('static INLINE char fast_interrupts')
    code = code[:start] + re.sub(r'\breturn ([^;]+);', r'{ *state = regs; return \1; }', code[start:stop]) + code[stop:]
    # A small inlined, predecoded front-end; the general interpreter receives
    # the already fetched opcode so CIA register instruction fetches occur once.
    code=code.replace('static unsigned char fast_cpu(cRSID_CPUinstance* state)',
                      'static unsigned char fast_cpu_general(cRSID_CPUinstance* state, unsigned char opcode)')
    code=code.replace('IR = fast_read(r->PC);','IR = opcode;',1)
    quick=(Path(__file__).with_name('sid_cpu_quick.inc')).read_text(encoding='utf-8')
    at=code.index('static INLINE char fast_interrupts')
    code=code[:at]+quick+'\n'+code[at:]
    at=code.index('static INLINE char fast_interrupts(')
    interrupts=code[at:].replace('fast_interrupts(cRSID_CPUinstance* r)',
                                'fast_interrupts_state(cRSID_CPUinstance* r,unsigned irq,unsigned nmi)')
    interrupts=interrupts.replace('cRSID_C64.NMI','nmi').replace('cRSID_C64.IRQ','irq')
    code=code[:at]+interrupts+'\nstatic INLINE char fast_interrupts(cRSID_CPUinstance* r) {\n return fast_interrupts_state(r,cRSID_C64.IRQ,cRSID_C64.NMI);\n}\n'
    lib = uncomment((ROOT / 'components/crsid/libcRSID.c').read_text(encoding='utf-8'))
    init = lib[lib.index('void cRSID_initSIDtune'):lib.index('char* cRSID_setSongLengthData')]
    before, rest = init.split(' if (!cRSID.RealSIDmode) {', 1)
    after = rest[rest.index('\n }') + 3:]
    before = before.replace('void cRSID_initSIDtune', 'static void fast_init_begin')
    before = re.sub(r' unsigned int InitTimeout=10000000;[^\n]*\n', '', before)
    before = re.sub(r' static const unsigned char PowersOf2\[\].*?;', '', before, flags=re.S)
    end = ('static void fast_init_end(cRSID_SIDheader* SIDheader, char subtune) {\n'
           ' cRSID_C64instance* C64 = &cRSID_C64;\n'
           ' static const unsigned char PowersOf2[] = {1,2,4,8,16,32,64,128};\n' + after)
    result = ('/* Производный код cRSID 1.58, Hermit (Mihaly Horvath), лицензия WTF.\n'
              ' * Создан tools/make_sid_cpu.py; править генератор, не этот файл.\n'
              ' * Порядок чтений, записей, флаги и особенности cRSID сохранены. */\n'
              + code + '\n' + before + '\n}\n\n' + end)
    result = re.sub(r'\n[ \t]*\n(?:[ \t]*\n)+', '\n\n', result)
    result = '\n'.join(line.rstrip() for line in result.splitlines()).rstrip() + '\n'
    return result.encode('utf-8')


if __name__ == '__main__':
    data = generate()
    if '--check' in sys.argv:
        raise SystemExit(0 if DEST.read_bytes() == data else 1)
    DEST.write_bytes(data)

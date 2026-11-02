"""Карточки, реальные ELF, тесты парсера/рендера, ASan, Xtensa, PNG/APNG."""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import sys
sys.dont_write_bytecode = True
from run_stage2 import ROOT, BUILD, CLANG, XTENSA, COMMON, ENV, run, host, xtensa, previews
sys.path.insert(0, str(ROOT/'tools'))
from add_card import sections

NAMES = ('NES', 'NEXT', 'AMIGA')
# ELF ядер без карточки (NES 0.9, NEXT 0.10, AMIGA с карты 01.11): в SD/CORES они с 2026-11-02 уже с карточкой
REAL = ROOT/'tests/fixtures'
FIRMWARE = ROOT.parents[2]/'Firmware'
SOUND = ROOT.parents[2]/'Sound'


def fixtures():
    print('Карточки и копии настоящих ELF', flush=True)
    (BUILD/'elf').mkdir(exist_ok=True)
    report = {}
    for name in NAMES:
        folder = ROOT/'cards'/name
        run([sys.executable, '-B', ROOT/'tools/make_card.py', folder/'card.json',
             folder/'icon.png', folder/'image.png', folder/'card.bin'], f'card_{name}.log')
        original = REAL/f'{name}.ELF'
        target = BUILD/'elf'/f'{name}.ELF'
        before = original.read_bytes()
        (BUILD/'elf'/f'{name}_original.ELF').write_bytes(before)
        run([sys.executable, '-B', ROOT/'tools/add_card.py', original, folder/'card.bin', target], f'add_{name}.log')
        # Replacement with a different key proves the existing section is replaced.
        replacement = bytearray((folder/'card.bin').read_bytes())
        string_bytes = sum(struct.unpack_from('<8H', replacement, 40))
        replacement[64+string_bytes-1] = ord('9')
        import zlib
        replacement[16:20] = bytes(4)
        struct.pack_into('<I', replacement, 16, zlib.crc32(replacement))
        replacement_file = BUILD/f'{name}_replacement.bin'
        replacement_file.write_bytes(replacement)
        replaced = BUILD/'elf'/f'{name}_replaced.ELF'
        run([sys.executable, '-B', ROOT/'tools/add_card.py', target, replacement_file, replaced], f'replace_{name}.log')
        a, _, _, old_names = sections(before)
        for path, expected in ((target, (folder/'card.bin').read_bytes()), (replaced, bytes(replacement))):
            after = path.read_bytes()
            b, _, _, new_names = sections(after)
            assert new_names.count(b'.vdac4.card') == 1
            card = b[new_names.index(b'.vdac4.card')]
            assert card[1:4] == [1, 0, 0]
            assert after[card[4]:card[4]+card[5]] == expected
            assert before[24:32] == after[24:32] # entry and phoff
            ph, entsize, count = struct.unpack_from('<I', before, 28)[0], *struct.unpack_from('<HH', before, 42)
            assert before[ph:ph+entsize*count] == after[ph:ph+entsize*count]
            for i, section in enumerate(a):
                # ELF ядра в SD/CORES может уже нести карточку (2026-11-02): её раздел заменяется
                if old_names[i] in (b'.shstrtab', b'.vdac4.card'):
                    continue
                assert section == b[i] # symbols, relocations, flags, addresses, offsets unchanged
                if section[1] != 8:
                    offset, size = section[4:6]
                    assert before[offset:offset+size] == after[offset:offset+size]
        assert original.read_bytes() == before
        report[name] = {'original_bytes':len(before), 'card_bytes':(folder/'card.bin').stat().st_size,
                        'elf_bytes':target.stat().st_size, 'original_sha256':hashlib.sha256(before).hexdigest()}
    run([sys.executable, '-B', ROOT/'tools/generate_art.py'], 'generic_art.log')
    (BUILD/'card_sizes.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    from card_tools_test import test_tools
    test_tools()


def loader():
    print('Штатные проверки ELF из Firmware и Sound', flush=True)
    paths = [BUILD/'elf'/f'{name}{suffix}.ELF' for name in NAMES for suffix in ('', '_replaced')]
    exe = BUILD/'loader_check.exe'
    run([CLANG, *COMMON, '-D_CRT_SECURE_NO_WARNINGS', '-I', FIRMWARE/'include',
         FIRMWARE/'src/elf_image.cpp', ROOT/'tests/loader_check.cpp', '-o', exe], 'build_loader.log')
    print(run([exe, *paths], 'loader_check.log').strip(), flush=True)
    print(run([sys.executable, '-B', SOUND/'tools/check_module_symbols.py', *paths], 'module_symbols.log').strip(), flush=True)
    run([XTENSA/'xtensa-esp32s3-elf-readelf.exe', '-SW', paths[0]], 'card_section_readelf.log')


def parser_tests(asan=False):
    name = 'asan' if asan else 'host'
    flags = COMMON + ['-D_CRT_SECURE_NO_WARNINGS', '-I', str(ROOT)]
    if asan:
        flags += ['-fsanitize=address', '-g', '-fno-omit-frame-pointer']
    exe = BUILD/f'cui_card_{name}.exe'
    run([CLANG, *flags, ROOT/'cui_card.cpp', ROOT/'cores_ui.cpp', ROOT/'fonts/pui_font.cpp',
         ROOT/'assets/cui_art.cpp', ROOT/'tests/cui_card_test.cpp', '-o', exe], f'build_card_{name}.log')
    print(run([exe], f'card_{name}.log').strip(), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=('fixtures','loader','parser','host','asan','xtensa','previews'))
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    actions = {'fixtures':fixtures, 'loader':loader, 'parser':lambda:parser_tests(False),
               'host':lambda:host(False), 'asan':lambda:(parser_tests(True),host(True)),
               'xtensa':xtensa, 'previews':previews}
    for name, action in actions.items():
        if not args.only or args.only == name:
            action()
    if not args.only:
        report = {'status':'passed', 'checks':list(actions)}
        for file in ('card_sizes','xtensa_sizes','stage2_host_metrics','stage2_asan_metrics','approved_comparison','artifact_check'):
            report[file] = json.loads((BUILD/f'{file}.json').read_text(encoding='utf-8'))
        (BUILD/'stage3_result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        sources = [p for directory in ('cards','tests','tools','fonts','assets','preview')
                   for p in (ROOT/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
        sources += [ROOT/p for p in ('cores_ui.h','cores_ui.cpp','cui_card.h','cui_card.cpp','cui_preview.py','test_stage3.cmd')]
        (BUILD/'stage3_inputs.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                                                        for p in sorted(sources)},indent=2)+'\n',encoding='utf-8')
    print('PASS: этап 3; результаты в build-pc', flush=True)


if __name__ == '__main__':
    main()

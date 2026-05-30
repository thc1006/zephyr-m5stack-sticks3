#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

REQUIRED = [
    'README.md',
    'CONTRIBUTING.md',
    'docs/00_RESEARCH_SNAPSHOT.md',
    'docs/02_SDD.md',
    'docs/03_TDD.md',
    'docs/04_AGILE_BACKLOG.md',
    'docs/05_VALIDATION_MATRIX.md',
    'boards/m5stack/m5stack_sticks3/board.yml',
    'boards/m5stack/m5stack_sticks3/m5stack_sticks3_procpu.dts',
    'app/CMakeLists.txt',
    'app/src/main.c',
]

FORBIDDEN = [
    'Co-' + 'Authored-By:',
    'Generated' + ' by Claude',
    '🤖 ' + 'Generated',
]

PIN_TOKENS = [
    'G39 MOSI', 'G40 SCK', 'G45 RS', 'G41 CS', 'G21 RST', 'G38 BL',
    'BMI270', '0x68', 'M5PM1', '0x6e', 'ES8311', '0x18',
    'KEY1', 'G11', 'KEY2', 'G12', 'IR_TX', 'G46', 'IR_RX', 'G42',
]


def main() -> int:
    missing = [p for p in REQUIRED if not (ROOT / p).exists()]
    if missing:
        print('Missing required files:')
        for p in missing:
            print(f'  - {p}')
        return 1

    errors: list[str] = []
    for path in ROOT.rglob('*'):
        if path.is_dir() or '.git' in path.parts:
            continue
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        for token in FORBIDDEN:
            if token in text:
                errors.append(f'{path.relative_to(ROOT)} contains forbidden authorship/signature token')
        if '\t' in text and path.suffix == '.md':
            # Markdown tabs are not fatal, but they are usually accidental.
            pass

    snapshot = (ROOT / 'docs/00_RESEARCH_SNAPSHOT.md').read_text(encoding='utf-8')
    for token in PIN_TOKENS:
        if token not in snapshot:
            errors.append(f'docs/00_RESEARCH_SNAPSHOT.md missing pin token: {token}')

    if errors:
        print('Integrity check failed:')
        for e in errors:
            print(f'  - {e}')
        return 1

    print('check_file_integrity.py: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())

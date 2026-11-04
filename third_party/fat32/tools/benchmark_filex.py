"""Повторяемый замер READ_AT: такты ядра Z80 без времени физического read512."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from filex_support import resource, measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    h, chain = resource(build=args.build)
    base = 86 * 1024 * 1024 + 7
    cases = [('первый', base)]
    cases += [(f'последовательный {i}', base + i * 1016) for i in range(1, 5)]
    cases += [('повтор внутри сектора', base + 4 * 1016 + 1000, 8),
              ('назад 40 МиБ', 40 * 1024 * 1024 + 333),
              ('вперёд 85 МиБ', 85 * 1024 * 1024 + 997),
              ('назад 12 МиБ', 12 * 1024 * 1024 + 511),
              ('вперёд 86 МиБ', base),
              ('граница кластера', 86 * 1024 * 1024 + 32760),
              ('назад к кластеру 1001', 1001 * 32768 + 333),
              ('вперёд к кластеру 2601', 2601 * 32768 + 511),
              ('назад к кластеру 127', 127 * 32768 + 511)]
    result = [dict(case=c[0], **measure(h, chain, *c[1:])) for c in cases]
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

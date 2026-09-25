"""Search a local engine research report without reading or running the game."""
import argparse
import json
from pathlib import Path


def matches(report, category, term):
    needle = term.casefold()
    if category == 'asset':
        for pack in report['packs']:
            for record in pack['files']:
                if needle in record['path'].casefold():
                    yield {'pack': pack['path'], **record}
    else:
        key, field = {'binding': ('binding_candidates', 'name'),
                      'system': ('ecs_systems', 'signature'),
                      'anchor': ('anchors', 'text')}[category]
        for record in report['executable'][key]:
            if needle in record[field].casefold():
                yield record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('category', choices=('asset', 'binding', 'system', 'anchor'))
    parser.add_argument('term', help='Case-insensitive substring')
    parser.add_argument('--limit', type=int, default=20)
    args = parser.parse_args()
    if not 1 <= args.limit <= 1000:
        parser.error('--limit must be between 1 and 1000')
    try:
        report = json.loads(args.report.read_text(encoding='utf-8'))
        if report.get('schema') != 1:
            raise ValueError('Unsupported report schema')
        count = 0
        for record in matches(report, args.category, args.term):
            if count < args.limit:
                print(json.dumps(record, ensure_ascii=True))
            count += 1
        print(f'{count} matches; displayed {min(count, args.limit)}. RVAs are decimal in JSON.')
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Query failed: {error}\n')


if __name__ == '__main__':
    main()

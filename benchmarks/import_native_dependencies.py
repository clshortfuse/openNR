"""Import local SDK headers and MinHook sources; never copy runtime binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout', type=Path, help='Local RenoDX checkout containing the SDK sources')
    args = parser.parse_args()
    destination = Path(__file__).resolve().parents[1] / 'local/native-dependencies'
    if destination.exists():
        raise ValueError('Dependency destination already exists; retained unchanged')
    roots = {
        'reshade/include': args.checkout / 'external/reshade/include',
        'streamline/include': args.checkout / 'external/Streamline/include',
        'ngx/include': args.checkout / 'external/Streamline/external/ngx-sdk/include',
        'minhook/include': args.checkout / 'external/reshade/deps/minhook/include',
        'minhook/src': args.checkout / 'external/reshade/deps/minhook/src',
    }
    files = [(file, destination / name / file.relative_to(root))
             for name, root in roots.items() for file in root.rglob('*')
             if file.is_file() and file.suffix.lower() in ('.h', '.hpp', '.c', '.inl')]
    for root in roots.values():
        if not root.is_dir():
            raise ValueError(f'Missing SDK directory: {root}')
    for name, root in [('reshade', args.checkout / 'external/reshade'),
                       ('streamline', args.checkout / 'external/Streamline'),
                       ('ngx', args.checkout / 'external/Streamline/external/ngx-sdk'),
                       ('minhook', args.checkout / 'external/reshade/deps/minhook')]:
        files.extend((p, destination / name / p.name) for p in root.iterdir()
                     if p.is_file() and p.name.lower().startswith(('license', 'copying', 'notice')))
    records = []
    for source, target in files:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        digest = hashlib.sha256(source.read_bytes()).hexdigest().upper()
        if hashlib.sha256(target.read_bytes()).hexdigest().upper() != digest:
            raise ValueError(f'Copy mismatch: {source}')
        records.append({'source': str(source.resolve()), 'path': str(target.relative_to(destination)), 'sha256': digest})
    (destination / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    print(f'Imported {len(records)} source/license files to {destination}')
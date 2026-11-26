#!/usr/bin/env python3
"""Prepare a private Studio model profile from VTS settings, without changing source assets."""
import argparse
import copy
import json
import math
from pathlib import Path
import shutil
import sys
import tempfile
from check_model import validate


def read_json(path):
    data = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(data, dict):
        raise ValueError(f'{path.name}: expected object')
    return data


def number(v, lo, hi):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not lo <= v <= hi:
        raise ValueError(f'number outside {lo}..{hi}: {v!r}')
    return float(v)


def asset(root, name):
    if not isinstance(name, str) or not name or '\\' in name:
        raise ValueError(f'invalid asset reference: {name!r}')
    p = Path(name)
    if p.is_absolute() or '..' in p.parts:
        raise ValueError(f'unsafe asset reference: {name}')
    source = (root / p).resolve(strict=True)
    if not source.is_relative_to(root) or not source.is_file():
        raise ValueError(f'asset outside source model or not a file: {name}')
    return p, source


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def physics_profile(original, settings, custom, fps=None):
    """Scale physics output amplitudes, not solver time or input weights."""
    result = copy.deepcopy(original)
    if result.get('Version') != 3:
        raise ValueError('expected physics3 Version 3')
    strength = number(settings.get('PhysicsStrength', 50), 0, 100) / 50
    multipliers = {}
    for item in custom.get('PhysicsMultipliersPerPhysicsGroup', []):
        key = item['ID']
        if not isinstance(key, str) or key in multipliers:
            raise ValueError('invalid or duplicate physics group ID')
        multipliers[key] = number(item['Value'], 0, 2)
    used = set(); applied = {}; unsupported = set()
    for group in result['PhysicsSettings']:
        key = group['Id']
        if key in used:
            raise ValueError(f'duplicate physics group: {key}')
        used.add(key)
        factor = strength * multipliers.get(key, 1)
        for output in group['Output']:
            if output.get('Type') == 'Angle':
                output['Scale'] = number(output['Scale'], -1e9, 1e9) * factor
            elif factor != 1:
                unsupported.add(key)
        applied[key] = factor
    if fps is not None:
        result['Meta']['Fps'] = fps
    return result, applied, sorted(set(multipliers)-used), sorted(unsupported)


def prepare(model, output, config=None, fps=None):
    model = Path(model).resolve(strict=True)
    root = model.parent
    if not model.name.endswith('.model3.json'):
        raise ValueError('expected .model3.json')
    config = Path(config).resolve(strict=True) if config else model.with_name(model.name.removesuffix('.model3.json')+'.vtube.json')
    data = read_json(config)
    if data.get('Version') != 1 or data.get('FileReferences', {}).get('Model') != model.name:
        raise ValueError('VTS config does not identify this model (Version 1)')
    output = Path(output).resolve()
    if output.is_relative_to(root):
        raise ValueError('output must be outside original model folder')
    if output.exists():
        raise FileExistsError(f'{output} already exists; use a new profile directory')
    manifest = read_json(model)
    validate(model)
    refs = manifest['FileReferences']
    settings = data.get('PhysicsSettings', {})
    custom = data.get('PhysicsCustomizationSettings', {})
    warnings = []
    applied = {}; physics = None; physics_name = refs.get('Physics')
    enabled = settings.get('Use', True)
    if not isinstance(enabled, bool): raise ValueError('PhysicsSettings.Use must be boolean')
    if physics_name and not enabled:
        refs.pop('Physics'); physics_name = None
    elif physics_name:
        _, source = asset(root, physics_name)
        physics, applied, missing, unsupported = physics_profile(read_json(source), settings, custom, fps)
        if missing: warnings.append('Unknown physics groups: '+', '.join(missing))
        if unsupported: warnings.append('Non-angle physics outputs unchanged in: '+', '.join(unsupported))
    elif enabled:
        warnings.append('Model has no physics file')
    idle = data['FileReferences'].get('IdleAnimation', '')
    if idle:
        if not isinstance(idle, str) or not idle.endswith('.motion3.json'):
            raise ValueError('invalid IdleAnimation reference')
        _, source = asset(root, idle)
        motion = read_json(source)
        if motion.get('Version') != 3: raise ValueError('expected idle motion3 Version 3')
        motions = refs.setdefault('Motions', {})
        entry = next((copy.deepcopy(m) for group in motions.values() for m in group if m.get('File') == idle), {'File': idle})
        motions['Idle'] = [entry]
    if settings.get('WindStrength', 0): warnings.append('Wind simulation not imported')
    if settings.get('UseLegacyPhysics'): warnings.append('Legacy physics not implemented')
    if data['FileReferences'].get('IdleAnimationWhenTrackingLost'): warnings.append('Tracking-lost idle not imported')
    report = dict(version=1, sourceModel=str(model), sourceConfig=str(config), physicsEnabled=bool(physics_name), physicsOutputMultipliers=applied, physicsFpsOverride=fps, idle=idle, warnings=warnings,
                  preservedNotApplied={k:data[k] for k in ('SavedModelPosition','ModelPositionMovement','PhysicsSettings','PhysicsCustomizationSettings','GeneralSettings') if k in data})
    # Only create links for explicitly referenced files, never mirror the entire source tree.
    names = [refs['Moc'], *refs['Textures']]
    names += [refs[k] for k in ('Physics','Pose','UserData','DisplayInfo') if refs.get(k)]
    names += [e['File'] for e in refs.get('Expressions', [])]
    for group in refs.get('Motions', {}).values():
        for motion in group:
            names += [motion['File']]
            if motion.get('Sound'): names += [motion['Sound']]
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.studio-model-', dir=output.parent))
    try:
        reserved = {Path(model.name), Path('studio-profile.json')}
        for name in sorted(set(names)):
            rel, source = asset(root, name)
            if rel in reserved: raise ValueError(f'asset conflicts with profile metadata: {name}')
            dest = stage / rel; dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.exists() or dest.is_symlink():
                raise ValueError(f"conflicting normalized asset path: {name}")
            if name == physics_name and physics is not None: write_json(dest, physics)
            else: dest.symlink_to(source)
        write_json(stage / model.name, manifest)
        write_json(stage / 'studio-profile.json', report)
        validate(stage / model.name)
        if output.exists(): raise FileExistsError(output)
        stage.rename(output)
    finally:
        if stage.exists(): shutil.rmtree(stage)
    return output / model.name, report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('model', type=Path); p.add_argument('--config', type=Path)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--physics-fps', type=int, choices=(30, 60, 120), help='explicit solver FPS; otherwise retain exported model setting')
    a = p.parse_args()
    try:
        path, report = prepare(a.model, a.output_dir, a.config, a.physics_fps)
        print(f'[profile] {path}')
        print(f'[profile] idle: {report["idle"] or "original manifest"}; physics: {report["physicsEnabled"]}')
        for key, value in report['physicsOutputMultipliers'].items():
            if value != 1: print(f'[profile] {key}: angular output scale x{value:.6g}')
        for warning in report['warnings']: print('[profile] warning:', warning)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        p.exit(1, f'[profile] {e}\n')

if __name__ == '__main__': main()

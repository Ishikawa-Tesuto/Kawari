"""Real supervisor startup diagnostics, using a relocated checkout and fake assets."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / 'api-rust/target/release'

@unittest.skipUnless((RELEASE/'studio').is_file() and (RELEASE/'studio-vts-api').is_file(),
                     'Build Rust Core first: ./scripts/build-core.sh')
class StartupPaths(unittest.TestCase):
    def test_relocated_startup_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix='kawari-startup-') as tmp:
            base = Path(tmp)
            root = base/'relocated checkout'
            release = root/'api-rust/target/release'
            release.mkdir(parents=True)
            for name in ('studio', 'studio-vts-api'):
                shutil.copy2(RELEASE/name, release/name)
            (release/'kawari').symlink_to('studio')
            (root/'scripts').mkdir()
            source_id = root/'scripts/source-id.sh'
            source_id.write_text('#!/bin/bash\necho test-build\n')
            original = base/'model'; original.mkdir()
            (original/'a.moc3').touch(); (original/'a.png').touch()
            model = original/'Test.model3.json'
            model.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'a.moc3','Textures':['a.png']}}))
            env = dict(os.environ, XDG_DATA_HOME=str(base/'data'), TMPDIR=tmp)
            env.pop('STUDIO_VIEWER_BIN', None)
            def run():
                result = subprocess.run([str(release/'kawari'),'--allow-plugin','Test',str(model)],
                                        env=env,cwd=base,capture_output=True,text=True,timeout=10)
                self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn(f'[studio] runtime: {root}',result.stdout)
                return result.stderr
            build = root/'build'; build.mkdir()
            self.assertIn(str(build/'studio-build-id'),run())
            (build/'studio-build-id').write_text('test-build\n')
            self.assertIn(str(build/'studio-broadcast-enabled'),run())
            (build/'studio-broadcast-enabled').write_text('1\n')
            viewer = build/'native/bin/Demo/Demo'
            self.assertIn(f'Renderer not found at {viewer}',run())
            viewer.parent.mkdir(parents=True)
            viewer.write_text('#!/nonexistent-kawari-interpreter\n')
            viewer.chmod(0o600)
            self.assertIn(f'Renderer is not an executable file: {viewer}',run())
            viewer.chmod(0o700)
            self.assertIn(f'Cannot start viewer: {viewer}',run())
            source_id.write_text('#!/bin/bash\necho missing-source >&2\nexit 1\n')
            self.assertIn('missing-source',run())
            source_id.write_text('#!/bin/bash\necho different-build\n')
            self.assertIn('Native sources and build do not match',run())

if __name__ == '__main__':
    unittest.main()

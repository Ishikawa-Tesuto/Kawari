"""Exercise core build guards using a fake Cargo; no Rust compiler required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class CoreBuild(unittest.TestCase):
    def run_build(self, source_current=True, binary_current=True, api_current=True, broadcast_current=True):
        with tempfile.TemporaryDirectory(prefix='core-build-') as td:
            root = Path(td)
            scripts = root / 'scripts'
            scripts.mkdir()
            for name in ('build-core.sh', 'build-api.sh'):
                shutil.copy2(ROOT / 'scripts' / name, scripts / name)
            package = root / 'api-rust'
            source = package / 'src/bin/studio.rs'
            source.parent.mkdir(parents=True)
            source.write_text('Kawari expression UI protocol 3 broadcast settings protocol 1 plugin manager protocol 1' if source_current else 'old source')
            (package / 'src/main.rs').write_text('Kawari expression UI protocol 3 plugin manager protocol 1')
            (package / 'Cargo.lock').touch()
            log = root / 'cargo.log'
            cargo = root / 'cargo'
            cargo.write_text('''#!/bin/bash
set -eu
printf '%s\\n' "$*" >> "$MOCK_LOG"
if [[ "$1" == build ]]; then
  mkdir -p "$CARGO_TARGET_DIR/release"
  printf '#!/bin/sh\\nprintf "%%s\\\\n" "%s"\\n' "$MOCK_HELP" > "$CARGO_TARGET_DIR/release/studio"
  printf '#!/bin/sh\\nprintf "%%s\\n" "%s"\\n' "$MOCK_API_HELP" > "$CARGO_TARGET_DIR/release/studio-vts-api"
  chmod +x "$CARGO_TARGET_DIR/release/studio" "$CARGO_TARGET_DIR/release/studio-vts-api"
fi
''')
            cargo.chmod(0o755)
            env = dict(os.environ, PATH=f'{root}:{os.environ["PATH"]}', MOCK_LOG=str(log), MOCK_HELP=('Kawari expression UI protocol 3 plugin manager protocol 1'+(' broadcast settings protocol 1' if broadcast_current else '')) if binary_current else 'old binary', MOCK_API_HELP='Kawari expression UI protocol 3 plugin manager protocol 1' if api_current else 'old API')
            result = subprocess.run(['bash', str(scripts / 'build-core.sh')], env=env, capture_output=True, text=True, timeout=10)
            return result, log.read_text().splitlines() if log.exists() else []

    def test_stale_source_rejected_before_build(self):
        result, commands = self.run_build(source_current=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('source is outdated', result.stderr)
        self.assertEqual(commands, [])

    def test_stale_binary_rejected_after_build(self):
        result, commands = self.run_build(binary_current=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Built binary does not support', result.stderr)
        self.assertIn('build --locked --release --bins', commands)

    def test_stale_broadcast_binary_rejected(self):
        result, _ = self.run_build(broadcast_current=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("broadcast settings protocol 1", result.stderr)

    def test_stale_api_rejected_after_build(self):
        result, _ = self.run_build(api_current=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Built API binary does not support', result.stderr)

    def test_forced_build_and_verified_binary(self):
        result, commands = self.run_build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(commands, ['clean --release -p studio-vts-api', 'test --locked', 'build --locked --release --bins'])
        self.assertIn('Kawari ready (expression UI protocol 3)', result.stdout)

if __name__ == '__main__':
    unittest.main()

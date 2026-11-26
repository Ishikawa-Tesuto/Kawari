"""Opt-in local persistence for untimed expression toggles, separate from VTS assets."""
import json
import os
from pathlib import Path
import tempfile


class ModelState:
    def __init__(self, path, model_id, model):
        self.path = Path(path).resolve()
        self.model_id = model_id
        if model and self.path.is_relative_to(model.parent.resolve()):
            raise ValueError('Studio state must be outside original model directory')
        self.saved = None
        if self.path.exists():
            data = json.loads(self.path.read_text(encoding='utf-8'))
            if not isinstance(data, dict) or data.get('schema') != 'studio-model-state-1' or data.get('modelID') != model_id:
                raise ValueError('State file belongs to another model or has incompatible format')
            names = data.get('activeExpressions')
            if not isinstance(names, list) or len(names) > 64 or not all(isinstance(n, str) for n in names):
                raise ValueError('Invalid saved expression list')
            self.saved = list(dict.fromkeys(names))

    def save(self, names):
        names = list(dict.fromkeys(names))
        if names == self.saved: return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix='.studio-state-', dir=self.path.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                json.dump(dict(schema='studio-model-state-1', modelID=self.model_id, activeExpressions=names), f, ensure_ascii=False, indent=2)
                f.write('\n'); f.flush(); os.fsync(f.fileno())
            os.replace(tmp, self.path)
        finally:
            if os.path.exists(tmp): os.unlink(tmp)
        self.saved = names

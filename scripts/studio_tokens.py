"""Private opt-in persistence of VTS plugin tokens (no automatic authorization)."""
import json
import os
from pathlib import Path
import re
import stat
import tempfile


class Tokens:
    def __init__(self, path, allowed):
        self.path = Path(path).absolute()
        self.allowed = set(allowed)
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.values = {}
        try:
            fd = os.open(self.path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            return
        with os.fdopen(fd, 'r', encoding='utf-8') as f:
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077 or st.st_size > 65536:
                raise ValueError('Token file must be a private user-owned regular file (0600)')
            data = json.load(f)
        if not isinstance(data, dict) or data.get('schema') != 'studio-api-tokens-1' or not isinstance(data.get('tokens'), list) or len(data['tokens']) > 64:
            raise ValueError('Invalid token file')
        for item in data['tokens']:
            if not isinstance(item, dict): raise ValueError("Invalid token record")
            name, dev, token = (item.get(k) for k in ('name','developer','token'))
            if not all(isinstance(x, str) and 3 <= len(x) <= 32 for x in (name,dev)) or not isinstance(token,str) or not re.fullmatch('[0-9a-f]{64}',token):
                raise ValueError('Invalid stored plugin token')
            if name.casefold() in self.allowed:
                self.values[(name,dev)] = token

    def save(self, values):
        rows = [dict(name=n,developer=d,token=t) for (n,d),t in values.items() if n.casefold() in self.allowed]
        fd,tmp = tempfile.mkstemp(prefix='.studio-api-', dir=self.path.parent)
        try:
            with os.fdopen(fd,'w',encoding='utf-8') as f:
                json.dump(dict(schema='studio-api-tokens-1',tokens=rows),f)
                f.write('\n'); f.flush(); os.fsync(f.fileno())
            os.replace(tmp,self.path)
        finally:
            if os.path.exists(tmp): os.unlink(tmp)

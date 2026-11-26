import importlib.util,json,os,tempfile,unittest
from unittest.mock import patch
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('bridge',ROOT/'scripts/twitch-bridge.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
class Twitch(unittest.TestCase):
    def event(self, eid='one', channel='123', reward='reward'):
        return {'metadata':{'message_type':'notification'},'payload':{'subscription':{'type':b.EVENT},'event':{'id':eid,'broadcaster_user_id':channel,'reward':{'id':reward}}}}
    def test_dedup_and_channel(self):
        d=b.Redemptions();self.assertIsNone(d.take(self.event(channel='other'),'123'));self.assertEqual(d.take(self.event(),'123'),'reward');self.assertIsNone(d.take(self.event(),'123'));self.assertEqual(d.take(self.event(eid='two'),'123'),'reward')
    def test_model_scope(self):
        class API:
            def __init__(self):self.calls=[];self.model='A';self.loaded=True
            def call(self,k,data=None):
                self.calls.append((k,data));return {'modelLoaded':self.loaded,'modelID':self.model} if k=='CurrentModelRequest' else {}
        a=API();rows={'A':{'reward':{'expressionFile':'Halo.exp3.json','mode':'timed','duration':5}}};self.assertTrue(b.dispatch(a,rows,'reward'));self.assertEqual(a.calls[-1],('KawariRewardExpressionRequest',{'modelID':'A','expressionFile':'Halo.exp3.json','mode':'timed','duration':5}));a.model='B';self.assertFalse(b.dispatch(a,rows,'reward'));a.model='A';a.loaded=False;self.assertFalse(b.dispatch(a,rows,'reward'))
    def test_default_binding_is_toggle(self):
        class API:
            def __init__(self,*args):pass
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def call(self,k,data=None):
                return {'modelLoaded':True,'modelID':'A','modelName':'Test'} if k=='CurrentModelRequest' else {}
        with tempfile.TemporaryDirectory() as d, patch.object(b,'directory',return_value=Path(d)), patch.object(b,'LocalApi',API):
            with patch('sys.argv',['twitch-bridge.py','bind','reward','Halo.exp3.json']):b.main()
            self.assertEqual(b.private_read(Path(d)/'bindings.json')['A']['reward'],{'expressionFile':'Halo.exp3.json','mode':'toggle','duration':0})
            with patch('sys.argv',['twitch-bridge.py','bind','other','Tail.exp3.json','--mode','timed','--duration','60']):b.main()
            rows=b.private_read(Path(d)/'bindings.json')['A']
            self.assertEqual(rows['other']['duration'],60)
            self.assertEqual(rows['reward']['mode'],'toggle')
    def test_private_storage(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'private.json';b.private_write(p,{'secret':'test'});self.assertEqual(p.stat().st_mode&0o777,0o600);self.assertEqual(b.private_read(p),{'secret':'test'});p.chmod(0o644);self.assertRaises(RuntimeError,b.private_read,p)
if __name__=='__main__':unittest.main()

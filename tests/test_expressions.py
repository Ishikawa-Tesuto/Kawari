import asyncio
import json
from pathlib import Path
import unittest
import test_vts_api as base
vts=base.vts

# Do not inherit ApiTest (which would run all its tests a second time).
class ExpressionApiTest(unittest.IsolatedAsyncioTestCase):
    asyncSetUp=base.ApiTest.asyncSetUp
    auth=base.ApiTest.auth
    request=base.ApiTest.request
    def prepare(self):
        root=Path(self.temp.name); model=root/'Test.model3.json'
        model.write_text(json.dumps({'FileReferences':{}}))
        asset='Face - Toggle.exp3.json'
        (root/asset).write_text(json.dumps({'Type':'Live2D Expression','Parameters':[{'Id':'ParamAngleX','Value':10,'Blend':'Add'}]}))
        (root/'Test.vtube.json').write_text(json.dumps({'Hotkeys':[{'Action':'ToggleExpression','Name':'Toggle','HotkeyID':'myid','File':asset,'IsActive':True,'FadeSecondsAmount':.4,'DeactivateAfterSeconds':True,'DeactivateAfterSecondsAmount':2},{'Action':'MoveModel','Name':'Move','HotkeyID':'move','File':''}], 'SavedActiveExpressions':[asset]}))
        self.api.expressions=vts.Expressions(model,{})
        return asset
    async def recv(self):
        return (await asyncio.wait_for(asyncio.get_running_loop().sock_recv(self.rx,16384),1)).decode()
    async def test_hotkey_and_state(self):
        asset=self.prepare(); await self.auth()
        r=await self.request('HotkeysInCurrentModelRequest'); self.assertEqual(len(r['data']['availableHotkeys']),1)
        self.assertEqual(r['data']['availableHotkeys'][0]['hotkeyID'],'myid')
        r=await self.request('HotkeyTriggerRequest',{'hotkeyID':'toggle'}); self.assertEqual(r['messageType'],'HotkeyTriggerResponse')
        self.assertTrue((await self.recv()).endswith('400 2000 ParamAngleX A 10'))
        r=await self.request('ExpressionStateRequest',{'details':True}); self.assertTrue(r['data']['expressions'][0]['active']); self.assertTrue(r['data']['expressions'][0]['autoDeactivateAfterSeconds'])
        await self.request('HotkeyTriggerRequest',{'hotkeyID':'myid'}); self.assertTrue((await self.recv()).startswith('expr off '))
        r=await self.request('ExpressionStateRequest',{'expressionFile':asset}); self.assertFalse(r['data']['expressions'][0]['active'])
    async def test_activation_and_missing_rig(self):
        asset=self.prepare(); await self.auth()
        r=await self.request('ExpressionActivationRequest',{'expressionFile':asset,'active':True,'fadeTime':3});self.assertEqual(r['messageType'],'ExpressionActivationResponse');self.assertTrue((await self.recv()).endswith('2000 0 ParamAngleX A 10'))
        self.api.snapshot.path.write_text('P\tOther\t0\t1\t0\t0\n')
        r=await self.request('ExpressionActivationRequest',{'expressionFile':asset,'active':True});self.assertEqual(r['messageType'],'APIError')
        with self.assertRaises(BlockingIOError): self.rx.recv(16384)
    async def test_persistent_expression_state(self):
        asset=self.prepare(); await self.auth()
        from studio_model_state import ModelState
        statepath=Path(self.temp.name)/'studio-state.json'
        self.api.state=ModelState(statepath,self.api.modelid,None)
        await self.request('ExpressionActivationRequest',{'expressionFile':asset,'active':True}); await self.recv()
        self.assertEqual(ModelState(statepath,self.api.modelid,None).saved,[asset])
        await self.request('ExpressionActivationRequest',{'expressionFile':asset,'active':False}); await self.recv()
        self.assertEqual(ModelState(statepath,self.api.modelid,None).saved,[])
        await self.request('HotkeyTriggerRequest',{'hotkeyID':'myid'}); await self.recv()
        self.assertEqual(ModelState(statepath,self.api.modelid,None).saved,[])
    async def test_path_traversal(self):
        root=Path(self.temp.name); model=root/'Test.model3.json'
        exp=vts.Expressions(model,{'Expressions':[{'File':'../escape.exp3.json'}]});self.assertEqual(exp.assets,{});self.assertEqual(len(exp.warnings),1)

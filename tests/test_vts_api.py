import asyncio
import importlib.util
import json
from pathlib import Path
import socket
import tempfile
import unittest
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

spec=importlib.util.spec_from_file_location('bridge',Path(__file__).resolve().parents[1]/'scripts/vts-api.py')
vts=importlib.util.module_from_spec(spec); spec.loader.exec_module(vts)

class ApiTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        base=Path(self.temp.name); self.sock=base/'input.sock'
        self.rx=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); self.rx.bind(str(self.sock)); self.rx.setblocking(False); self.addCleanup(self.rx.close)
        (base/'parameters.tsv').write_text('P\tParamAngleX\t-30\t30\t0\t12\nP\tParamMouthPucker\t0\t2\t0\t0.5\nM\tParamAngleX\tParamAngleX\t-30\t30\t-30\t30\nM\tVBCustom\tParamMouthPucker\t0\t1\t0\t2\n')
        self.api=vts.Bridge(self.sock,None,['VBridger']); self.addCleanup(self.api.sender.close)
        self.srv=await serve(self.api.handle,'127.0.0.1',0,origins=[None]); self.addAsyncCleanup(self.srv.wait_closed); self.addCleanup(self.srv.close)
        # cleanup is LIFO across async and sync registered actions
        self.port=self.srv.sockets[0].getsockname()[1]
        self.ws=await connect(f'ws://127.0.0.1:{self.port}'); self.addAsyncCleanup(self.ws.close)
    async def request(self,kind,data=None,ws=None):
        ws=ws or self.ws; await ws.send(json.dumps(vts.envelope(kind,data or {},'test-id')))
        response=json.loads(await ws.recv()); self.assertEqual(response['requestID'],'test-id'); return response
    async def auth(self,ws=None):
        data={'pluginName':'VBridger','pluginDeveloper':'Test Dev'}
        r=await self.request('AuthenticationTokenRequest',data,ws); token=r['data']['authenticationToken']
        r=await self.request('AuthenticationRequest',dict(data,authenticationToken=token),ws); self.assertTrue(r['data']['authenticated']); return token
    async def test_auth_and_model(self):
        self.assertEqual((await self.request('Live2DParameterListRequest'))['data']['errorID'],8)
        self.assertEqual((await self.request('AuthenticationTokenRequest',{'pluginName':'Unknown','pluginDeveloper':'Test Dev'}))['data']['errorID'],50)
        await self.auth()
        r=await self.request('Live2DParameterListRequest'); self.assertEqual(r['data']['parameters'][0]['value'],12)
        self.assertEqual((await self.request('CurrentModelRequest'))['data']['numberOfLive2DParameters'],2)
    async def test_injection_and_custom(self):
        await self.auth()
        r=await self.request('ParameterCreationRequest',dict(parameterName='VBCustom',min=0,max=1,defaultValue=0)); self.assertEqual(r['messageType'],'ParameterCreationResponse')
        r=await self.request('InjectParameterDataRequest',dict(faceFound=True,mode='set',parameterValues=[dict(id='FaceAngleX',value=15),dict(id='VBCustom',value=.8)]))
        self.assertEqual(r['messageType'],'InjectParameterDataResponse')
        packet=await asyncio.wait_for(asyncio.get_running_loop().sock_recv(self.rx,16384),1)
        self.assertEqual(packet,b'set plugin 1000 ParamAngleX 15 VBCustom 0.8')
        self.assertTrue((await self.request('FaceFoundRequest'))['data']['found'])
        self.assertEqual((await self.request('ParameterValueRequest',{'name':'VBCustom'}))['data']['value'],.8)
        self.assertEqual((await self.request('ParameterDeletionRequest',{'parameterName':'VBCustom'}))['messageType'],'ParameterDeletionResponse')
    async def test_bad_packet_atomic(self):
        await self.auth()
        r=await self.request('InjectParameterDataRequest',dict(parameterValues=[dict(id='FaceAngleX',value=15),dict(id='Missing',value=0)]))
        self.assertEqual(r['messageType'],'APIError')
        with self.assertRaises(BlockingIOError): self.rx.recv(16384)
        r=await self.request('InjectParameterDataRequest',dict(parameterValues=[dict(id='FaceAngleX',value=15,weight=.5)]))
        self.assertEqual(r['messageType'],'APIError')
    async def test_alias_collision(self):
        await self.auth()
        r=await self.request('InjectParameterDataRequest',dict(parameterValues=[dict(id='FaceAngleX',value=15),dict(id='ParamAngleX',value=0)]))
        self.assertEqual(r['messageType'],'APIError')
    async def test_custom_unmapped_and_timeout(self):
        await self.auth(); await self.request('ParameterCreationRequest',dict(parameterName='Unmapped',min=0,max=1,defaultValue=0))
        r=await self.request('InjectParameterDataRequest',dict(parameterValues=[dict(id='Unmapped',value=.9)]))
        self.assertEqual(r['messageType'],'InjectParameterDataResponse')
        with self.assertRaises(BlockingIOError): self.rx.recv(16384)
        self.assertEqual((await self.request('ParameterValueRequest',{'name':'Unmapped'}))['data']['value'],.9)
        self.api.values['Unmapped']=(.9,0,'test')
        self.assertEqual((await self.request('ParameterValueRequest',{'name':'Unmapped'}))['data']['value'],0)

    async def test_imported_ranges_and_routing(self):
        await self.auth()
        self.api.snapshot.path.write_text('P\tParamAngleX2\t-30\t30\t0\t0\nP\tParamBodyAngleX\t-10\t10\t0\t0\nP\tParamMouthPucker\t0\t2\t0\t0\nM\tFaceAngleX\tParamAngleX2\t-40\t40\t-30\t30\nM\tFaceAngleX\tParamBodyAngleX\t-50\t50\t10\t-10\nM\tMouthPucker\tParamMouthPucker\t0\t-0.15\t0\t2\n')
        reply=await self.request('InputParameterListRequest')
        ps={p['name']:p for p in reply['data']['defaultParameters']}
        self.assertEqual((ps['MouthPucker']['min'],ps['MouthPucker']['max']),(-.15,0))
        self.assertEqual((ps['FaceAngleX']['min'],ps['FaceAngleX']['max']),(-50,50))
        reply=await self.request('ParameterCreationRequest',dict(parameterName='MouthPucker',min=-1,max=1,defaultValue=0))
        self.assertEqual(reply['messageType'],'ParameterCreationResponse')
        reply=await self.request('InjectParameterDataRequest',dict(parameterValues=[dict(id='FaceAngleX',value=15),dict(id='MouthPucker',value=-.1)]))
        self.assertEqual(reply['messageType'],'InjectParameterDataResponse')
        self.assertEqual(await asyncio.wait_for(asyncio.get_running_loop().sock_recv(self.rx,16384),1),b'set plugin 1000 FaceAngleX 15 MouthPucker -0.1')

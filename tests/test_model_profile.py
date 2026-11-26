import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from studio_model_state import ModelState
spec=importlib.util.spec_from_file_location('prepare_model',Path(__file__).resolve().parents[1]/'scripts/prepare-model.py')
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)

class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.src=self.root/'original';self.src.mkdir()
        self.model=self.src/'Test.model3.json';self.config=self.src/'Test.vtube.json';self.output=self.root/'profile'
        self.physics={'Version':3,'Meta':{'Fps':30},'PhysicsSettings':[{'Id':'PhysicsSetting3','Output':[{'Scale':2,'Weight':100,'Type':'Angle'}]},{'Id':'PhysicsSetting16','Output':[{'Scale':-3,'Weight':50,'Type':'Angle'}]}]}
        self.data={'Version':1,'FileReferences':{'Model':self.model.name,'IdleAnimation':'Idle.motion3.json'},'PhysicsSettings':{'Use':True,'PhysicsStrength':50,'Live2DPhysicsFPS':1},'PhysicsCustomizationSettings':{'PhysicsMultipliersPerPhysicsGroup':[{'ID':'PhysicsSetting3','Value':1.2},{'ID':'PhysicsSetting16','Value':1.8}]}}
        self.model.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'body.moc3','Textures':['tex/image.png'],'Physics':'body.physics3.json','Motions':{'TapBody':[{'File':'tap.motion3.json'}]}}}))
        (self.src/'tex').mkdir();(self.src/'tex/image.png').touch();(self.src/'body.moc3').touch()
        for name in ('Idle.motion3.json','tap.motion3.json'): (self.src/name).write_text('{"Version":3}')
        (self.src/'body.physics3.json').write_text(json.dumps(self.physics));self.config.write_text(json.dumps(self.data))
    def test_prepare(self):
        before={p:p.read_bytes() for p in self.src.rglob('*') if p.is_file()}
        path,report=profile.prepare(self.model,self.output)
        self.assertEqual(json.loads(path.read_text())['FileReferences']['Motions']['Idle'],[{'File':'Idle.motion3.json'}])
        self.assertTrue((self.output/'tex/image.png').is_symlink())
        physics=json.loads((self.output/'body.physics3.json').read_text());self.assertEqual(physics['Meta']['Fps'],30)
        self.assertAlmostEqual(physics['PhysicsSettings'][0]['Output'][0]['Scale'],2.4)
        self.assertAlmostEqual(physics['PhysicsSettings'][1]['Output'][0]['Scale'],-5.4)
        self.assertEqual(physics['PhysicsSettings'][1]['Output'][0]['Weight'],50)
        self.assertEqual(report['physicsOutputMultipliers']['PhysicsSetting16'],1.8)
        self.assertEqual(before,{p:p.read_bytes() for p in self.src.rglob('*') if p.is_file()})
        with self.assertRaises(FileExistsError):profile.prepare(self.model,self.output)
    def test_linear_outputs_unchanged(self):
        self.physics["PhysicsSettings"][0]["Output"].append({"Scale":7,"Type":"X"})
        (self.src/"body.physics3.json").write_text(json.dumps(self.physics))
        _,report=profile.prepare(self.model,self.output)
        self.assertEqual(json.loads((self.output/"body.physics3.json").read_text())["PhysicsSettings"][0]["Output"][1]["Scale"],7)
        self.assertIn("Non-angle",report["warnings"][0])
    def test_disable(self):
        self.data['PhysicsSettings']['Use']=False;self.config.write_text(json.dumps(self.data))
        path,report=profile.prepare(self.model,self.output)
        self.assertNotIn('Physics',json.loads(path.read_text())['FileReferences']);self.assertFalse(report['physicsEnabled'])
    def test_fps_and_missing_group(self):
        self.data['PhysicsCustomizationSettings']['PhysicsMultipliersPerPhysicsGroup'].append({'ID':'Unknown','Value':1})
        self.config.write_text(json.dumps(self.data));_,report=profile.prepare(self.model,self.output,fps=60)
        self.assertEqual(json.loads((self.output/'body.physics3.json').read_text())['Meta']['Fps'],60);self.assertIn('Unknown',report['warnings'][0])
    def test_missing_idle(self):
        (self.src/'Idle.motion3.json').unlink()
        with self.assertRaises(OSError):profile.prepare(self.model,self.output)
        self.assertFalse(self.output.exists())
    def test_protect_source(self):
        with self.assertRaises(ValueError):profile.prepare(self.model,self.src/'bad')
        self.data['FileReferences']['IdleAnimation']='../outside.motion3.json';self.config.write_text(json.dumps(self.data))
        with self.assertRaises(ValueError):profile.prepare(self.model,self.output)
    def test_state(self):
        p=self.root/'state.json';state=ModelState(p,'abc',self.model);self.assertIsNone(state.saved)
        state.save(['one','two']);self.assertEqual(ModelState(p,'abc',self.model).saved,['one','two'])
        state.save([]);self.assertEqual(ModelState(p,'abc',self.model).saved,[])
        self.assertEqual(p.stat().st_mode&0o777,0o600)
        with self.assertRaises(ValueError):ModelState(p,'other',self.model)
        with self.assertRaises(ValueError):ModelState(self.config,'abc',self.model)

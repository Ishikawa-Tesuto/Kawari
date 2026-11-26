import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('import_vtube',Path(__file__).resolve().parents[1]/'scripts/import-vtube.py')
imp=importlib.util.module_from_spec(spec);spec.loader.exec_module(imp)

def row(source='MouthPucker',target='ParamPucker',**kwargs):
    return dict(Input=source,OutputLive2D=target,InputRangeLower=0.,InputRangeUpper=-.15,OutputRangeLower=0.,OutputRangeUpper=2.,Smoothing=20,ClampInput=False,ClampOutput=False)|kwargs

def config(*rows): return dict(Version=1,ParameterSettings=list(rows))

class ImportTest(unittest.TestCase):
    def test_reversed_and_extended(self):
        txt,n,inputs,skip=imp.convert(config(row(),row('MouthPucker','ParamOther',OutputRangeUpper=3,ClampOutput=True)))
        self.assertIn('MouthPucker ParamPucker 0 -0.15 0 2 100 vts 0 0',txt)
        self.assertIn('MouthPucker ParamOther 0 -0.15 0 3 100 vts 0 1',txt)
        self.assertEqual((n,inputs,skip),(2,1,[]))
    def test_procedural(self):
        _,n,_,skip=imp.convert(config(row(),row('', 'ParamBreath',UseBreathing=True)))
        self.assertEqual(n,1);self.assertIn('ParamBreath',skip[0])
    def test_invalid(self):
        for bad in [row(InputRangeUpper=0),row(Smoothing=float('nan')),row(Input='bad name'),row(ClampInput=1)]:
            with self.subTest(bad=bad),self.assertRaises(ValueError): imp.convert(config(bad))
        with self.assertRaises(ValueError): imp.convert(config(row(),row()))
    def test_smoothing(self):
        txt,*_=imp.convert(config(row()),2)
        self.assertIn('0 2 40 vts',txt)

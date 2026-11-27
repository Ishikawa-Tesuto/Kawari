"""Timer samples are model-scoped, refreshed by API, and never fabricated by UI."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class Management(unittest.TestCase):
    def test_timer_sample(self):
        code=r'''
#include "studio_ui_layout.hpp"
#include <cassert>
using namespace studio;
int main(){
    std::istringstream first("M\tmodelA\nQ\tАкулий хвост.exp3.json\t60000\t100000\nQ\tInvalid\t999999999\t100000\nE\tkey\t1\tA\n");
    auto timers=ui_expression_timers(first,"modelA");
    assert(timers.size()==1);
    auto t=timers.at("Акулий хвост.exp3.json");
    assert(ui_expression_remaining(t,100000)==60000);
    assert(ui_expression_remaining(t,100500)==59500);
    assert(ui_expression_remaining(t,101501)==0); // stale API
    assert(ui_expression_remaining(t,99999)==0); // incompatible clock
    std::istringstream refreshed("M\tmodelA\nQ\tАкулий хвост.exp3.json\t60000\t101000\n");
    t=ui_expression_timers(refreshed,"modelA").begin()->second;
    assert(ui_expression_remaining(t,101100)==59900); // new redemption
    std::istringstream wrong("M\tmodelB\nQ\tАкулий хвост.exp3.json\t60000\t101000\n");
    assert(ui_expression_timers(wrong,"modelA").empty());
    std::istringstream off("M\tmodelA\nE\tkey\t0\tАкулий хвост.exp3.json\n");
    assert(ui_expression_timers(off,"modelA").empty());
    std::istringstream ending("M\tmodelA\nQ\tA\t200\t102000\n");
    t=ui_expression_timers(ending,"modelA").at("A");
    assert(ui_expression_remaining(t,102199)==1);
    assert(ui_expression_remaining(t,102200)==0);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_wiring(self):
        api=(ROOT/'api-rust/src/main.rs').read_text()
        mgmt=(ROOT/'api-rust/src/expression_management.rs').read_text()
        ui=(ROOT/'native/studio_ui.cpp').read_text()
        for action in ('configure-expression','new-hotkey','delete-hotkey','delete-expression','deactivate-all'):
            self.assertIn('"'+action+'"',api)
            self.assertIn(action,ui)
        self.assertIn('self.activate_raw(&other,false,None,0.,false)?',api)
        self.assertIn('!b.expressions.management.protected(n)',mgmt)
        self.assertIn('CLOCK_MONOTONIC',mgmt)
        self.assertIn('ui_expression_timers(timers,t.model)',ui)
        self.assertIn('r.mode!="timed"',ui)
        self.assertIn('Missing expression - remove or change binding',ui)

if __name__=='__main__':unittest.main()

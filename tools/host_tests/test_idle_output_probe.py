"""Exercise production bounded pixel bookkeeping; not LVGL/USB/panel proof."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'main/boards/ep-chat-p4-ml307/idle_output_probe.h'
HARNESS = r'''
#include "idle_output_probe.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    using P = IdleOutputProbe;
    P::Area band{0,384,479,431};
    std::vector<uint8_t> pixels(46080, 23);
    const auto unchanged = pixels;
    uint32_t expected = P::Hash(pixels.data(), pixels.size());
    P::Result r;
    P p;
    CHECK(p.Start(1, band, expected, 100) == 1);
    CHECK(!p.Observe({0,0,479,49}, nullptr, 0, 0, false, 110, r));
    CHECK(p.Observe(band, pixels.data(), pixels.size(), 960, true, 120, r));
    CHECK(r.full && r.match && r.reason == 0 && r.seq == 1 && r.frame == 1);
    pixels[200] ^= 1;
    CHECK(p.Observe(band, pixels.data(), pixels.size(), 960, true, 130, r));
    CHECK(r.full && !r.match && r.reason == 0); // Must use actual pixels, not source hash.
    pixels = unchanged;
    CHECK(p.Observe({0,384,479,400}, pixels.data(), pixels.size(), 960, true, 140, r));
    CHECK(!r.full && !r.match && r.reason == 0);
    CHECK(p.Observe(band, reinterpret_cast<const uint8_t*>(1), 46080, 960, false, 150, r));
    CHECK(r.reason == 1 && !r.full && !r.match); // Unsupported memory never dereferenced.
    CHECK(p.Observe(band, pixels.data(), pixels.size()-1, 960, true, 160, r));
    CHECK(r.reason == 2 && !r.match);
    CHECK(p.Observe(band, pixels.data(), pixels.size(), 958, true, 170, r));
    CHECK(r.reason == 2 && !r.match);
    CHECK(p.Observe(band, nullptr, pixels.size(), 960, true, 180, r));
    CHECK(r.reason == 2 && !r.match);
    CHECK(p.Observe(band, pixels.data(), 48001, 960, true, 190, r));
    CHECK(r.reason == 2 && !r.match);
    CHECK(!p.Observe(band, pixels.data(), pixels.size(), 960, true, 1101, r));
    CHECK(!p.Active());
    P enlarged_flush;
    std::vector<uint8_t> padded(48000, 91);
    for (size_t i = 0; i < pixels.size(); ++i) padded[1920+i] = pixels[i];
    enlarged_flush.Start(1, band, expected, 0);
    CHECK(enlarged_flush.Observe({0,382,479,431}, padded.data(), padded.size(), 960, true, 1, r));
    CHECK(r.full && r.match && r.reason == 0); // Hash only the band, not other dirty rows.
    p.Start(2, band, expected, 2000); p.Cancel();
    CHECK(!p.Observe(band, pixels.data(), pixels.size(), 960, true, 2010, r));
    P wrap;
    wrap.Start(0, band, expected, UINT32_MAX - 10U);
    CHECK(wrap.Observe(band, pixels.data(), pixels.size(), 960, true, 5, r));
    CHECK(r.match);
    P capped;
    for (unsigned i = 0; i < 8; ++i) CHECK(capped.Start(i%4, band, expected, 0) == i+1);
    CHECK(!capped.CanStart());
    CHECK(capped.Start(0, band, expected, 0) == 0 && !capped.Active());
    P flush_cap;
    flush_cap.Start(0, band, expected, 0);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(flush_cap.Observe(band, pixels.data(), pixels.size(), 960, true, 1, r));
    CHECK(!flush_cap.Observe(band, pixels.data(), pixels.size(), 960, true, 1, r));
    P invalid;
    invalid.Start(0, {-5000,0,0,1}, expected, 0);
    CHECK(!invalid.Active());
    invalid.Start(0, {0,0,480,47}, expected, 0);
    CHECK(!invalid.Active());
    CHECK(pixels == unchanged);
    std::puts("PASS production probe: match/mismatch/partial/range/expiry/cancel/caps; renderer not exercised");
}
'''


class IdleOutputProbeTest(unittest.TestCase):
    def test_production_header_and_false_match_mutant(self):
        compiler = shutil.which(os.environ.get('CXX', 'g++'))
        self.assertIsNotNone(compiler, 'Host compiler required; no SKIP')
        source = HEADER.read_text(encoding='utf-8')
        mutation = 'out.match = out.full && hash == source_hash_;'
        self.assertEqual(source.count(mutation), 1)
        with tempfile.TemporaryDirectory(prefix='idle_output_probe_') as td:
            work = Path(td)
            (work / 'test.cc').write_text(HARNESS, encoding='utf-8')
            for mutant in (False, True):
                (work / HEADER.name).write_text(source.replace(mutation, 'out.match = out.full;') if mutant else source,
                                                encoding='utf-8')
                exe = work / 'test.exe'
                built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(work/'test.cc'), '-o', str(exe)],
                                       capture_output=True, text=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, 1 if mutant else 0, run.stdout + run.stderr)
                if mutant:
                    self.assertIn('r.full && !r.match', run.stderr)

    def test_application_hook_is_pre_driver_and_observer_is_bounded(self):
        board = HEADER.parent
        adapter = (board/'eezui_display_adapter.cc').read_text(encoding='utf-8')
        self.assertEqual(adapter.count('if (code == LV_EVENT_FLUSH_START) IdleFlashBandOverlay::TraceFlush(disp, area);'), 1)
        overlay = (board/'idle_flash_band_overlay.cc').read_text(encoding='utf-8')
        self.assertIn('esp_ptr_internal(buffer->data)', overlay)
        self.assertIn('buffer->header.cf == LV_COLOR_FORMAT_RGB565', overlay)
        self.assertIn('#if S1GY_IDLE_OUTPUT_PROBE', overlay)
        refr = (ROOT/'managed_components/lvgl__lvgl/src/core/lv_refr.c').read_text(encoding='utf-8')
        start = refr.index('static void call_flush_cb(', refr.index('static void call_flush_cb(')+1)
        body = refr[start:refr.index('static void wait_for_flushing', start)]
        self.assertLess(body.index('LV_EVENT_FLUSH_START'), body.index('disp->flush_cb('))
        self.assertIn('layer->draw_buf = disp_refr->buf_act;', refr)


if __name__ == '__main__':
    unittest.main()

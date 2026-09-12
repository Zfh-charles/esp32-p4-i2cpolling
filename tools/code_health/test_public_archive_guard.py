import unittest
from public_archive_guard import forbidden


class PublicArchiveGuardTest(unittest.TestCase):
    def test_rejects_local_harness_and_game_anywhere(self):
        for name in ('game/app/index.ts', 'tools/game/demo.py', 'AGENTS.md',
                     'docs/Agent.md', '.cursor/rules/example.mdc', 'build/x.o',
                     'fw_archive/x.bin', 'tmp/x.elf', 'tools/app/surface.local.json',
                     '.env', 'tools/app/.env.production', 'x.code-workspace',
                     'state.sqlite', 'state.sqlite3', 'state.sqlite3-wal'):
            with self.subTest(name=name):
                self.assertTrue(forbidden(name))

    def test_rejects_unused_nested_ui(self):
        self.assertTrue(forbidden('components/eezui/eezui/src/ui/screens.c'))

    def test_keeps_product_code_and_required_assets(self):
        for name in ('main/application.cc', 'components/eezui/src/ui/screens.c',
                     'components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c',
                     'sdkconfig.defaults.esp32p4', 'tools/app/.env.example',
                     'tools/mjpeg_ai_dialogue_v5p3_mouth_focus/happy/frames.mjpeg',
                     'tools/product_contracts/validate_contracts.py'):
            with self.subTest(name=name):
                self.assertFalse(forbidden(name))


if __name__ == '__main__':
    unittest.main()

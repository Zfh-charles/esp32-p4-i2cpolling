#!/usr/bin/env python3

import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

import run_gate


class ValidationGateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def check(check_id, source, *, timeout=15, when_path=None):
        check = {
            "id": check_id,
            "label": check_id,
            "tier": "T0",
            "scopes": ["fast"],
            "timeout_seconds": timeout,
            "required": True,
            "command": ["{python}", "-c", source],
        }
        if when_path is not None:
            check["when_path"] = when_path
            check["optional_when_missing"] = True
        return check

    @staticmethod
    def manifest(*checks):
        return {
            "schema_version": 1,
            "manifest_version": "test-v1",
            "checks": list(checks),
        }

    def test_pass_and_optional_missing_path_skip(self):
        report = run_gate.execute_manifest(
            self.manifest(
                self.check("pass", "print('ok')"),
                self.check("future", "raise SystemExit(99)", when_path="future/contracts"),
            ),
            "fast",
            self.repo,
        )
        self.assertEqual(report["overall_status"], "PASS")
        self.assertEqual([item["status"] for item in report["results"]], ["PASS", "SKIP"])

    def test_failure_does_not_stop_following_checks(self):
        report = run_gate.execute_manifest(
            self.manifest(
                self.check("fail", "raise SystemExit(7)"),
                self.check("still_runs", "print('after failure')"),
            ),
            "fast",
            self.repo,
        )
        self.assertEqual(report["overall_status"], "FAIL")
        self.assertEqual([item["status"] for item in report["results"]], ["FAIL", "PASS"])
        self.assertEqual(report["failed_required"], ["fail"])

    def test_timeout_is_a_required_failure(self):
        report = run_gate.execute_manifest(
            self.manifest(self.check("slow", "import time; time.sleep(2)", timeout=0.05)),
            "fast",
            self.repo,
        )
        self.assertEqual(report["results"][0]["status"], "TIMEOUT")
        self.assertEqual(report["overall_status"], "FAIL")

    def test_report_is_atomic_valid_json(self):
        report = run_gate.execute_manifest(
            self.manifest(self.check("pass", "print('ok')")), "fast", self.repo
        )
        output = self.repo / "report.json"
        run_gate.write_report(output, report)
        loaded = run_gate.read_json(output)
        self.assertEqual(loaded["schema_version"], 1)
        self.assertEqual(loaded["overall_status"], "PASS")
        self.assertFalse(output.with_name("report.json.tmp").exists())

    def test_production_face_checks_are_wired_and_fail_closed(self):
        manifest = run_gate.read_json(Path(__file__).with_name('gate_manifest.json'))
        entries = {check['id']: check for check in manifest['checks']}
        visual = entries['visual_port_production_host']
        route = entries['face_route_admission_characterization']
        self.assertIn('fast', visual['scopes'])
        self.assertNotIn('fast', route['scopes'])
        self.assertIn('KNOWN_GAP', route['label'])
        for check in (visual, route):
            self.assertTrue(check['required'])
            self.assertFalse(check.get('optional_when_missing', False))
            self.assertTrue((Path(__file__).resolve().parents[3] / check['command'][-1]).is_file())

        def result(check, repo):
            return {'id': check['id'], 'required': check.get('required', True),
                    'status': 'FAIL' if check['id'] == route['id'] else 'PASS'}

        with patch.object(run_gate, 'run_check', side_effect=result):
            report = run_gate.execute_manifest(manifest, 'production-face', self.repo)
        self.assertEqual({row['id'] for row in report['results']},
                         {'production_face_trace_observer', visual['id'], route['id']})
        self.assertEqual(report['overall_status'], 'FAIL')
        self.assertEqual(report['failed_required'], [route['id']])

    def test_production_control_input_guard_is_required(self):
        manifest = run_gate.read_json(Path(__file__).with_name('gate_manifest.json'))
        checks = [item for item in manifest['checks'] if 'production-control' in item['scopes']]
        expected = {'tts_state_input_guard', 'tts_callback_effects'}
        self.assertEqual({item['id'] for item in checks}, expected)
        for check in checks:
            self.assertIn('fast', check['scopes'])
            self.assertTrue(check['required'])
            self.assertFalse(check.get('optional_when_missing', False))
            self.assertTrue((Path(__file__).resolve().parents[3] / check['command'][-1]).is_file())
            def result(item, repo):
                return {'id': item['id'], 'required': True,
                        'status': 'FAIL' if item['id'] == check['id'] else 'PASS'}
            with patch.object(run_gate, 'run_check', side_effect=result):
                report = run_gate.execute_manifest(manifest, 'production-control', self.repo)
            self.assertEqual({row['id'] for row in report['results']}, expected)
            self.assertEqual(report['overall_status'], 'FAIL')
            self.assertEqual(report['failed_required'], [check['id']])

    def test_build_graph_check_is_required(self):
        manifest = run_gate.read_json(Path(__file__).with_name('gate_manifest.json'))
        check = next(item for item in manifest['checks'] if item['id'] == 'build_graph_only_host')
        self.assertIn('fast', check['scopes'])
        self.assertTrue(check['required'])
        self.assertFalse(check.get('optional_when_missing', False))
        self.assertTrue((Path(__file__).resolve().parents[3] / check['command'][-1]).is_file())
        with patch.object(run_gate, 'run_check', return_value={
            'id': check['id'], 'required': True, 'status': 'FAIL'
        }):
            report = run_gate.execute_manifest(manifest, 'build-graph', self.repo)
        self.assertEqual([row['id'] for row in report['results']], [check['id']])
        self.assertEqual(report['overall_status'], 'FAIL')


if __name__ == "__main__":
    unittest.main()

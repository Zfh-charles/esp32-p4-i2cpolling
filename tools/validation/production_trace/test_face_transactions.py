import json
from pathlib import Path
import unittest

from face_transactions import observe

FIXTURE = Path(__file__).parent / 'fixtures/s1gk_generation6.json'


def records():
    return [entry['text'] for entry in json.loads(FIXTURE.read_text())['records']]


class ProductionObservationTests(unittest.TestCase):
    def analyze(self, lines):
        return observe('\n'.join(lines), max_presenter_ms=250)

    def test_exact_historical_observations_not_completed_frame_inference(self):
        result = self.analyze(records())
        self.assertEqual(result['status'], 'OBSERVED_VIOLATION')
        window = result['windows'][0]
        self.assertEqual((window['generation'], window['begins'],
                          window['begins_after_audio'], window['full_seed_records'],
                          window['max_presenter_ms']), (6, 3, 2, 2, 1357))
        self.assertEqual(set(result['violations'][0]['reasons']),
                         {'multiple_transaction_begins', 'transaction_begin_after_audio_first',
                          'presenter_budget_exceeded'})
        self.assertTrue(window['reset_observed'])
        self.assertFalse(result['completion_or_cause_proven'])
        self.assertFalse(result['runtime_fix_proven'])

    def test_missing_audio_record_does_not_invent_after_audio_count(self):
        result = self.analyze([line for line in records() if 'tts_audio_first' not in line])
        self.assertEqual(result['windows'][0]['begins_after_audio'], 0)
        self.assertNotIn('transaction_begin_after_audio_first', result['violations'][0]['reasons'])

    def test_distinguishes_seed_record_from_transaction_begin(self):
        result = self.analyze([line for line in records() if 'lvgl_face seed' not in line])
        self.assertEqual(result['windows'][0]['begins'], 3)
        self.assertEqual(result['windows'][0]['full_seed_records'], 0)

    def test_each_new_generation_gets_independent_window(self):
        lines = records()[:2] + ['W (4000000) ScreenPresenter: s1es speech_generation=7',
                                 records()[1]]
        result = self.analyze(lines)
        self.assertEqual([w['begins'] for w in result['windows']], [1, 1])
        self.assertEqual(result['status'], 'INSUFFICIENT_FOR_PASS')

    def test_reset_closes_window_even_if_next_boot_reuses_generation(self):
        lines = records() + [records()[1], records()[0], records()[1]]
        result = self.analyze(lines)
        self.assertEqual([w['begins'] for w in result['windows']], [3, 1])

    def test_silence_unknown_dialect_and_no_generation_never_pass(self):
        for lines in ([], ['I idle'], records()[1:], ['W new_dialect full_screen=1']):
            with self.subTest(lines=lines):
                self.assertEqual(self.analyze(lines)['status'], 'INSUFFICIENT_FOR_PASS')

    def test_budget_is_explicit_and_independent(self):
        result = observe('\n'.join(records()), max_presenter_ms=1400)
        self.assertNotIn('presenter_budget_exceeded', result['violations'][0]['reasons'])
        with self.assertRaises(ValueError):
            observe('', max_presenter_ms=0)

    def test_monitor_crlf_variants_preserve_observations(self):
        expected = self.analyze(records())
        actual = observe('\r\r\n'.join(records()), max_presenter_ms=250)
        self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()

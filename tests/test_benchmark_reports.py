import unittest

from benchmarks.analysis.profile_report import reports


class BenchmarkReportTests(unittest.TestCase):
    def setUp(self):
        self.rows = [dict(kernel='kernel', phase=f'evaluation-{i:03d}', runtime_module='0',
                          gpu_timestamp_count='1', nvapi_launch_count='1', gpu_total_us=str(i * 10))
                     for i in (1, 2)]
        self.trace = {'traceEvents': [dict(ph='X', name='kernel', cat='module0', ts=i * 100,
                                          dur=i * 10, args={'phase': f'evaluation-{i:03d}'}) for i in (1, 2)]}

    def test_warmup_exclusion_and_timeline(self):
        summary, speedscope = reports(self.rows, self.trace)
        self.assertEqual(summary['steadyStateGpuTotalUs'], 20)
        self.assertEqual(summary['timestampLaunches'], 2)
        self.assertEqual(summary['hotEntries'][0]['calls'], 1)
        self.assertEqual(speedscope['activeProfileIndex'], 1)
        self.assertEqual(speedscope['profiles'][1]['endValue'], 20)

    def test_missing_trace_rejected(self):
        self.trace['traceEvents'].pop()
        with self.assertRaisesRegex(ValueError, 'launch mismatch'):
            reports(self.rows, self.trace)

    def test_nvapi_mismatch_rejected(self):
        self.rows[1]['nvapi_launch_count'] = '2'
        with self.assertRaisesRegex(ValueError, 'launch mismatch'):
            reports(self.rows, self.trace)

    def test_overlap_rejected(self):
        self.rows[1].update(gpu_timestamp_count='2', nvapi_launch_count='2')
        self.trace['traceEvents'].append(dict(ph='X', name='other', ts=201, dur=4,
                                             args={'phase': 'evaluation-002'}))
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            reports(self.rows, self.trace)
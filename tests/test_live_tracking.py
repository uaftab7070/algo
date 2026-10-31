import threading
import unittest
from unittest.mock import Mock

from tools.live_tracking import LatestFrame, capture_frames, process_frames


class LiveCameraChecks(unittest.TestCase):
    def test_mailbox_retains_only_latest_frame(self):
        mailbox = LatestFrame()
        self.assertIsNone(mailbox.get())
        for number in range(100):
            mailbox.put(number, number / 30)
        self.assertEqual(mailbox.get(), (100, 99, 99 / 30))

    def test_capture_failure_releases_camera_and_signals_loss(self):
        camera, runtime, stop = Mock(), Mock(), threading.Event()
        camera.read.side_effect = [(True, 'first'), (True, 'latest'), (False, None)]
        mailbox = LatestFrame()
        capture_frames(camera, mailbox, runtime, stop)
        self.assertEqual(mailbox.get()[1], 'latest')
        self.assertTrue(stop.is_set())
        runtime.mark_camera_failure.assert_called_once_with()
        camera.release.assert_called_once_with()

    def test_capture_exception_releases_camera_and_signals_loss(self):
        camera, runtime, stop = Mock(), Mock(), threading.Event()
        camera.read.side_effect = RuntimeError('camera disconnected')
        with self.assertLogs(level='ERROR'):
            capture_frames(camera, LatestFrame(), runtime, stop)
        self.assertTrue(stop.is_set())
        runtime.mark_camera_failure.assert_called_once_with()
        camera.release.assert_called_once_with()

    def test_worker_selects_latest_frame_and_stops_on_fault(self):
        runtime, stop, mailbox = Mock(), threading.Event(), LatestFrame()
        mailbox.put('old', 1)
        mailbox.put('new', 2)
        runtime.initialize.return_value = {'ok': False, 'runtime_fault': 'expired'}
        process_frames(mailbox, runtime, {'x': 1}, stop)
        runtime.initialize.assert_called_once_with('new', {'x': 1}, 2)
        runtime.update.assert_not_called()
        self.assertTrue(stop.is_set())


if __name__ == '__main__':
    unittest.main()

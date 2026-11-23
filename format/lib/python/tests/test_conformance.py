"""The conformance suite of format/tests, one test per case."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import conformance  # noqa: E402


class ConformanceTest(unittest.TestCase):
    def test_suite(self):
        count = 0
        for name, case, run, folder in conformance.cases():
            count += 1
            with self.subTest(case=name):
                problems = conformance.check(name, case, run(case, folder), folder)
                self.assertEqual(problems, [], '\n'.join(problems))
        self.assertGreater(count, 0)


if __name__ == '__main__':
    unittest.main()

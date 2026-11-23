"""Unit tests of the pass engine's API: the parts the pass cases of the conformance suite do not reach."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import mncf  # noqa: E402

FILE = b'''format = "mncf/1"
name = "Example"

[game]
platform = "switch"
id = "0100000000ABC000"

[[cheat]]
id = "money"
name = "Money"
default = true
toggle = "L+R"
  [cheat.values.count]
  type = "u32"
  default = 5
  [[cheat.write]]
  at = { region = "heap", offset = 0x10 }
  type = "u32"
  value = "$count"
'''

HEAP = bytes(0x100)


def engine():
    def memory(address, size):
        off = address - 0x10000000
        return HEAP[off:off + size] if 0 <= off and off + size <= len(HEAP) else None

    rt = mncf.Runtime('switch', regions={'heap': (0x10000000, len(HEAP))}, memory=memory)
    return mncf.Engine(mncf.load(FILE), rt)


class EngineTest(unittest.TestCase):
    def test_load_is_the_evaluation(self):
        e = engine()
        self.assertEqual(e.evaluation.cheat('money').state, 'on')
        self.assertEqual(e.state('money').state, 'on')
        self.assertIsNone(e.build)

    def test_tick_with_the_runtimes_memory(self):
        e = engine()
        p = e.tick()
        self.assertEqual(p.number, 1)
        self.assertEqual([c.as_dict() for c in p.changes],
                         [{'kind': 'write', 'address': 0x10000010, 'bytes': '05 00 00 00', 'cheat': 'money'}])
        self.assertEqual(p.cheat('money').state, 'on')
        self.assertIsNone(p.cheat('nothing'))

    def test_press_as_a_list(self):
        e = engine()
        e.press(['R', 'L'])
        p = e.tick()
        self.assertEqual(p.cheat('money').state, 'off')
        self.assertEqual(p.changes, [])

    def test_a_value_that_does_not_fit(self):
        e = engine()
        e.set_value('money.count', -1)
        self.assertEqual(e.state('money').reason, 'value-out-of-range')
        e.set_value('money.count', 'many')
        self.assertEqual(e.state('money').reason, 'value-out-of-range')

    def test_mistakes_of_the_caller(self):
        e = engine()
        with self.assertRaises(KeyError):
            e.switch('nothing')
        with self.assertRaises(KeyError):
            e.set_value('money.other', 1)
        with self.assertRaises(ValueError):
            mncf.Engine(mncf.load(FILE), mncf.Runtime('switch'), rate=0)

    def test_stop_changes_nothing_without_code(self):
        e = engine()
        e.tick()
        self.assertEqual(e.stop(), [])
        self.assertEqual(e.state('money').state, 'off')
        self.assertEqual(e.tick().changes, [])


if __name__ == '__main__':
    unittest.main()

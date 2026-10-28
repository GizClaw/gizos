import struct
import unittest
import zlib
from decode_bk import decode


def fixture():
    data = bytearray(352)
    data[4:12] = b'H2BKDMP1'
    for offset, value in [(0, 352), (12, 1), (16, 1), (20, 3), (24, 32), (28, 256), (84, 0x28010000), (92, 0x2345678), (252, 0x43444F4E)]:
        struct.pack_into('<I', data, offset, value)
    struct.pack_into('<I', data, 248, zlib.crc32(data[:248]))
    data[256:264] = b'H2STACK1'
    data[288:320] = bytes(range(32))
    for offset, value in [(264, 0x28010000), (268, 32), (272, zlib.crc32(data[288:320])), (284, 0x53544F4E)]:
        struct.pack_into('<I', data, offset, value)
    struct.pack_into('<I', data, 280, zlib.crc32(data[256:280]))
    data[320:328] = b'H2END001'
    return data


class DecoderTest(unittest.TestCase):
    def test_complete(self):
        result = decode(fixture())
        self.assertEqual(result['registers']['pc'], 0x2345678)
        self.assertEqual(result['declared_bytes'], len(fixture()))
        self.assertTrue(result['stack_valid'])
        self.assertEqual(result['stack_bytes'], 32)

    def test_cp_processor(self):
        data = fixture()
        struct.pack_into('<I', data, 12, 2)
        struct.pack_into('<I', data, 16, 0)
        struct.pack_into('<I', data, 144, 2)
        struct.pack_into('<I', data, 248, zlib.crc32(data[:248]))
        result = decode(data)
        self.assertEqual(result['processor'], 'cp')
        self.assertEqual(result['core'], 0)
        self.assertTrue(result['stack_valid'])

    def test_torn_stack_preserves_registers(self):
        result = decode(fixture()[:304])
        self.assertTrue(result['registers_valid'])
        self.assertFalse(result['stack_valid'])

    def test_corrupt_header_rejected(self):
        data = fixture(); data[32] ^= 1
        with self.assertRaises(ValueError): decode(data)

    def test_corrupt_stack_rejected(self):
        data = fixture(); data[288] ^= 1
        self.assertFalse(decode(data)['stack_valid'])

    def test_invalid_size_rejected(self):
        data = fixture(); struct.pack_into('<I', data, 268, 0xFFFFFFE0)
        struct.pack_into('<I', data, 280, zlib.crc32(data[256:280]))
        self.assertFalse(decode(data)['stack_valid'])


if __name__ == '__main__':
    unittest.main()

import tempfile

from .fs_test import FSTestCase, mounted

import overrides_hack
import utils
from utils import TestTags, tag_test

from gi.repository import BlockDev, GLib


class UdfTestCase(FSTestCase):

    def setUp(self):
        if not self.udf_avail:
            self.skipTest("skipping UDF: not available")

        super(UdfTestCase, self).setUp()

        self.mount_dir = tempfile.mkdtemp(prefix="libblockdev.", suffix="udf_test")


class UdfTestMkfs(UdfTestCase):
    def test_udf_mkfs(self):
        """Verify that it is possible to create a new udf file system"""

        with self.assertRaises(GLib.GError):
            BlockDev.fs_udf_mkfs("/non/existing/device", None)

        succ = BlockDev.fs_udf_mkfs(self.loop_dev)
        self.assertTrue(succ)

        # just try if we can mount the file system
        with mounted(self.loop_dev, self.mount_dir):
            pass

        # check the fstype
        fstype = BlockDev.fs_get_fstype(self.loop_dev)
        self.assertEqual(fstype, "udf")

        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.revision, "2.01")
        self.assertEqual(fi.block_size, 512)

        BlockDev.fs_wipe(self.loop_dev, True)

        # now try with custom revision and media type
        succ = BlockDev.fs_udf_mkfs(self.loop_dev, "bdr", "2.50", 4096)
        self.assertTrue(succ)

        # check the fstype
        fstype = BlockDev.fs_get_fstype(self.loop_dev)
        self.assertEqual(fstype, "udf")

        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.revision, "2.50")
        self.assertEqual(fi.block_size, 4096)


class UdfTestWipe(UdfTestCase):
    def test_udf_wipe(self):
        """Verify that it is possible to wipe an udf file system"""

        succ = BlockDev.fs_udf_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        succ = BlockDev.fs_udf_wipe(self.loop_dev)
        self.assertTrue(succ)

        # already wiped, should fail this time
        with self.assertRaises(GLib.GError):
            BlockDev.fs_udf_wipe(self.loop_dev)

        utils.run("pvcreate -ff -y %s >/dev/null" % self.loop_dev)

        # LVM PV signature, not an udf file system
        with self.assertRaises(GLib.GError):
            BlockDev.fs_udf_wipe(self.loop_dev)

        BlockDev.fs_wipe(self.loop_dev, True)

        utils.run("mkfs.ext2 -F %s >/dev/null 2>&1" % self.loop_dev)

        # ext2, not an udf file system
        with self.assertRaises(GLib.GError):
            BlockDev.fs_udf_wipe(self.loop_dev)

        BlockDev.fs_wipe(self.loop_dev, True)


class UdfGetInfo(UdfTestCase):
    def test_udf_get_info(self):
        """Verify that it is possible to get info about an udf file system"""

        succ = BlockDev.fs_udf_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "LinuxUDF")
        self.assertEqual(fi.vid, "LinuxUDF")
        self.assertEqual(fi.lvid, "LinuxUDF")
        # should be an non-empty string
        self.assertTrue(fi.uuid)
        self.assertEqual(fi.revision, "2.01")
        self.assertEqual(fi.block_size, 512)
        self.assertGreater(fi.block_count, 0)
        self.assertGreater(fi.free_blocks, 0)


class UdfSetLabel(UdfTestCase):
    def test_udf_set_label(self):
        """Verify that it is possible to set label of an udf file system"""

        succ = BlockDev.fs_udf_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "LinuxUDF")

        succ = BlockDev.fs_udf_set_label(self.loop_dev, "test_label")
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "test_label")
        self.assertEqual(fi.vid, "test_label")
        self.assertEqual(fi.lvid, "test_label")

        # longer label -- vid should be truncated to 30
        succ = BlockDev.fs_udf_set_label(self.loop_dev, "a" * 126)
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "a" * 126)
        self.assertEqual(fi.vid, "a" * 30)
        self.assertEqual(fi.lvid, "a" * 126)

        succ = BlockDev.fs_udf_set_label(self.loop_dev, "??" * 126)
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "??" * 126)
        self.assertEqual(fi.vid, "??" * 30)
        self.assertEqual(fi.lvid, "??" * 126)

        # with unicode -- vid should be truncated to 15 or 30 based on position
        succ = BlockDev.fs_udf_set_label(self.loop_dev, "??" * 63)
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "??" * 63)
        self.assertEqual(fi.vid, "??" * 15)
        self.assertEqual(fi.lvid, "??" * 63)

        succ = BlockDev.fs_udf_set_label(self.loop_dev, "??" + "a" * 62)
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "??" + "a" * 62)
        self.assertEqual(fi.vid, "??" + "a" * 14)
        self.assertEqual(fi.lvid, "??" + "a" * 62)

        succ = BlockDev.fs_udf_set_label(self.loop_dev, "a" * 62 + "??")
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "a" * 62 + "??")
        self.assertEqual(fi.vid, "a" * 30)
        self.assertEqual(fi.lvid, "a" * 62 + "??")

        # check label
        succ = BlockDev.fs_udf_check_label("test_label")
        self.assertTrue(succ)

        with self.assertRaises(GLib.GError):
            BlockDev.fs_udf_check_label("a" * 127)

        # non-ascii but latin1
        succ = BlockDev.fs_udf_check_label("??" * 126)
        self.assertTrue(succ)

        succ = BlockDev.fs_udf_check_label("p????li?? ??lu??ou??k??")
        self.assertTrue(succ)

        with self.assertRaises(GLib.GError):
            BlockDev.fs_udf_check_label("??" * 64)


class UdfSetUUID(UdfTestCase):

    test_uuid = "5fae9ade7938dfc8"

    def test_udf_set_uuid(self):
        """Verify that it is possible to set UUID of an UDF file system"""

        if not self.udf_avail:
            self.skipTest("skipping UDF: not available")

        succ = BlockDev.fs_udf_mkfs(self.loop_dev)
        self.assertTrue(succ)

        succ = BlockDev.fs_udf_set_uuid(self.loop_dev, self.test_uuid)
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.uuid, self.test_uuid)

        # no uuid -> random
        succ = BlockDev.fs_udf_set_uuid(self.loop_dev, None)
        self.assertTrue(succ)
        fi = BlockDev.fs_udf_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertNotEqual(fi.uuid, "")
        self.assertNotEqual(fi.uuid, self.test_uuid)

        succ = BlockDev.fs_udf_check_uuid(self.test_uuid)
        self.assertTrue(succ)

        with self.assertRaisesRegex(GLib.GError, "16 characters long"):
            BlockDev.fs_udf_check_uuid(9 * "a")

        with self.assertRaisesRegex(GLib.GError, "must be a lowercase hexadecimal number"):
            BlockDev.fs_udf_check_uuid(16 * "z")

        with self.assertRaisesRegex(GLib.GError, "must be a lowercase hexadecimal number"):
            BlockDev.fs_udf_check_uuid(self.test_uuid.upper())

#!/usr/bin/env python3
"""The corpus sweep's reading of the tools and its report; fixtures contain no
game content: the tools' output is written here in their format.

    test_sweep.py"""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("sweep", Path(__file__).resolve().parents[1] / "sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)

LISTING = """\
       3708616  lba=55120    /PSP_GAME/SYSDIR/BOOT.BIN
       3708960  lba=544      /PSP_GAME/SYSDIR/EBOOT.BIN
      23372384  lba=6480     /PSP_GAME/SYSDIR/UPDATE/DATA.BIN
       5545568  lba=2384     /PSP_GAME/SYSDIR/UPDATE/EBOOT.BIN
         40696  lba=57952    /PSP_GAME/USRDIR/PRX/libfont.prx
          9000  lba=57990    /PSP_GAME/SYSDIR/UPDATE/fw.prx
"""

COVER = """\
image:    a.elf
extent:   0x08804040 + 400 bytes (100 words)
decoded:  98 (98.00%)
vfpu:     1 (1.00%)  — recognised as VFPU, not yet named
unknown:  2 (2.00%)  — decoder gaps, or data misread as code
"""

def funcs(unimplemented=(), exports=(), ctors=".cplinit 3, 2 of them discovered", undiscovered=("0x08900010",)):
    lines = ["module:     a.elf", "functions:  120  (7 merged away)",
             "reached:    400 bytes (99.5% of .text, 402 bytes)", "imports:    5 distinct firmware calls",
             "", f"unimplemented: {len(unimplemented)} of 5 firmware calls"]
    lines += [f"  unimplemented {lib:<24} {nid}" for lib, nid in unimplemented]
    lines += [f"  undiscovered .cplinit constructor {a}" for a in undiscovered]
    lines += [f"ctors:      {ctors}", "ctors:      .ctors 0, 0 of them discovered",
              f"exports:    {len(exports)} functions to other modules"]
    lines += [f"  export {nid}" for nid in exports]
    lines += ["", "over discovered code:", "  instructions 1000", "  vfpu         12 (1.20%)", "  invalid      3 (0.30%)"]
    return "\n".join(lines) + "\n"


class Reading(unittest.TestCase):
    def test_a_disc_is_its_executable_and_its_modules_outside_the_update(self):
        self.assertEqual(sweep.disc_modules(LISTING),
                         ["/PSP_GAME/SYSDIR/EBOOT.BIN", "/PSP_GAME/USRDIR/PRX/libfont.prx"])

    def test_cover(self):
        self.assertEqual(sweep.parse_cover(COVER), dict(decoded_pct=98.0, vfpu_unnamed=1, unknown=2))

    def test_funcs(self):
        f = sweep.parse_funcs(funcs(unimplemented=[("sceUtility", "0x0000AAAA")], exports=["0x0000BBBB"]))
        self.assertEqual(f["functions"], 120)
        self.assertEqual(f["reached_pct"], 99.5)
        self.assertEqual(f["imports"], 5)
        self.assertEqual(f["unimplemented"], [dict(lib="sceUtility", nid="0x0000AAAA")])
        self.assertEqual(f["exports"], ["0x0000BBBB"])
        self.assertEqual(f["ctors"], {"cplinit": dict(listed=3, discovered=2), "ctors": dict(listed=0, discovered=0)})
        self.assertEqual(f["undiscovered_ctors"], ["0x08900010"])
        self.assertEqual((f["vfpu_pct"], f["invalid"]), (1.2, 3))
        self.assertFalse(f["no_sections"])
        self.assertTrue(sweep.parse_funcs("ctors:      no section headers: the tables are not named\n")["no_sections"])


class Names(unittest.TestCase):
    def test_a_name_in_a_header_names_the_nid_it_hashes_to(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "a.h").write_text("int sceKernelExitGame(void);\nint notAFirmwareCall(int);\n")
            # The NID the firmware gives sceKernelExitGame, and that hle.c registers.
            self.assertEqual(sweep.nid_names(tmp), {"0x05572A5F": "sceKernelExitGame"})
        self.assertEqual(sweep.nid_names(None), {})


class Disc(unittest.TestCase):
    def test_a_call_another_module_on_the_disc_exports_is_answered(self):
        """The executable imports two calls nothing in the runtime answers;
        the disc's own module exports one of them."""
        outputs = {"/PSP_GAME/SYSDIR/EBOOT.BIN": funcs(unimplemented=[("sceFont", "0x0000F001"),
                                                                    ("sceUtility", "0x0000AAAA")]),
                   "/PSP_GAME/USRDIR/PRX/libfont.prx": funcs(exports=["0x0000F001"], undiscovered=())}
        order = []

        def run(args, capture=False):
            args = list(map(str, args))
            if args[1] == "ls":
                return LISTING
            if args[1] == "extract":
                path, out = args[3], Path(args[4])
                order.append(path)
                (out / path.lstrip("/").replace("/", "_")).write_bytes(b"\x7fELF" + path.encode())
                return None
            if args[1] == "cover":
                return COVER
            return outputs[order[int(Path(args[2]).name[:2])]]

        class Importer:
            @staticmethod
            def unwrap_module(run, data, out, pspdecrypt):
                return data

        with tempfile.TemporaryDirectory() as tmp, patch.object(sweep, "run", run):
            disc = sweep.sweep_disc(Path(tmp) / "Some Game.iso", dict(allegrexrecomp="ar", pspdecrypt="pd"),
                                    Path(tmp) / "out", Importer)
            self.assertTrue((Path(tmp) / "out/some-game.json").exists())
        eboot, prx = disc["modules"]
        self.assertEqual(len(eboot["funcs"]["unimplemented"]), 2)
        self.assertEqual(eboot["funcs"]["unanswered"], [dict(lib="sceUtility", nid="0x0000AAAA")])
        self.assertEqual(prx["funcs"]["unanswered"], [])

        report = sweep.report([disc])
        self.assertIn("| Some Game.iso | SYSDIR/EBOOT.BIN | 98.0% | 2 | 1.2% | 120 | 99.5% | 5 | 1 | cplinit 2/3 |", report)
        self.assertIn("| sceUtility | 0x0000AAAA |  | 1: Some Game.iso |", report)
        self.assertIn("| sceUtility | 0x0000AAAA | sceSomeCall | 1: Some Game.iso |",
                      sweep.report([disc], {"0x0000AAAA": "sceSomeCall"}))
        self.assertNotIn("0x0000F001 |", report)
        self.assertIn("- Some Game.iso, SYSDIR/EBOOT.BIN: 0x08900010", report)

    def test_a_module_it_cannot_read_is_reported_not_swept(self):
        def run(args, capture=False):
            args = list(map(str, args))
            if args[1] == "ls":
                return "      3708960  lba=544      /PSP_GAME/SYSDIR/EBOOT.BIN\n"
            out = Path(args[4])
            (out / "PSP_GAME_SYSDIR_EBOOT.BIN").write_bytes(b"~PSP")

        class Importer:
            @staticmethod
            def unwrap_module(run, data, out, pspdecrypt):
                return None

        with tempfile.TemporaryDirectory() as tmp, patch.object(sweep, "run", run):
            disc = sweep.sweep_disc(Path(tmp) / "x.iso", dict(allegrexrecomp="ar", pspdecrypt="pd"),
                                    Path(tmp) / "out", Importer)
        self.assertIn("not a module this toolkit reads", disc["modules"][0]["error"])
        self.assertIn("| x.iso | SYSDIR/EBOOT.BIN | not a module", sweep.report([disc]))


if __name__ == "__main__":
    unittest.main()

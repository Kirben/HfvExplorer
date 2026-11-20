"""Master independent optical regression fixtures with PyCdlib (pip install pycdlib)."""

import argparse
from io import BytesIO
from pathlib import Path
from unittest.mock import patch

import pycdlib


def master(path, revision, joliet=False):
    image = pycdlib.PyCdlib()
    image.new(interchange_level=1, vol_ident="ROCK_RIDGE",
              rock_ridge=revision, joliet=3 if joliet else None)
    try:
        image.add_directory("/DOCS", rr_name="Unix docs",
                            joliet_path="/Joliet docs" if joliet else None)
        payload = b"independently mastered Rock Ridge data\n"
        image.add_fp(BytesIO(payload), len(payload), "/DOCS/FILE.TXT;1",
                     rr_name="Caf\u00e9 notes.txt", file_mode=0o100640,
                     creation_time=1700000000,
                     joliet_path="/Joliet docs/Joliet file.txt" if joliet else None)
        if not joliet:
            long_name = "a" * 225 + ".txt"
            image.add_fp(BytesIO(payload), len(payload), "/DOCS/LONG.TXT;1",
                         rr_name=long_name)
            image.add_symlink("/LINK.;1", rr_symlink_name="Unix link",
                              rr_path="/Unix docs/Caf\u00e9 notes.txt")
            parent = ""
            for depth in range(1, 11):
                parent += f"/D{depth}"
                image.add_directory(parent, rr_name=f"Long level {depth}")
            image.add_fp(BytesIO(payload), len(payload), parent + "/DEEP.TXT;1",
                         rr_name="Deep file.txt")
        image.write(str(path))
    finally:
        image.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    with patch("time.time", return_value=1700000120):
        master(args.directory / "rrip-109.iso", "1.09")
        master(args.directory / "rrip-112.iso", "1.12")
        master(args.directory / "rock-ridge-joliet.iso", "1.12", joliet=True)

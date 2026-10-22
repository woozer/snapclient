#!/usr/bin/env python3
"""Prepare local keys and the documented logger shim; never print credentials."""
from pathlib import Path
import os,secrets,subprocess
ROOT=Path(__file__).resolve().parents[1]
p=ROOT/'components/udp_logger';target=p/'CMakeLists.txt';shim=ROOT/'cr2/patches/udp_logger.CMakeLists.txt'
if not target.exists():raise SystemExit('Run git submodule update --init --recursive first.')
original=subprocess.check_output(['git','-C',str(p),'show','HEAD:CMakeLists.txt'])
if target.read_bytes() not in (original,shim.read_bytes()):raise SystemExit('Logger has an unrelated local edit; leaving it unchanged.')
target.write_bytes(shim.read_bytes())
for name,macro in [('ota','CR2_OTA_TOKEN'),('music','CR2_MUSIC_KEY')]:
 path=ROOT/f'components/custom_board/cr2_bench/cr2_{name}_secret.h'
 if path.exists():continue
 fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
 with os.fdopen(fd,'w') as f:f.write('#pragma once\n#define '+macro+' "'+secrets.token_hex(32)+'"\n')
print('Local keys ready (not printed); disabled-logger build shim applied.')

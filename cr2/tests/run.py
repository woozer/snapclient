#!/usr/bin/env python3
"""Host tests of the actual startup/retry blocks and DMA probe. No device access."""
from pathlib import Path
import subprocess,tempfile,shutil
here=Path(__file__).resolve().parent;root=here.parents[1]
source=root/'components/custom_board/cr2_bench'
player=(root/'components/lightsnapcast/player.c').read_text()
# Reject stale test overlays: these are the exact blocks embedded in player.c.
for name in ['startup_block.c.inc','wait_block.c.inc']:
 block=(here/name).read_text().strip()
 assert block in player, 'Test overlay differs from actual player: '+name
with tempfile.TemporaryDirectory(prefix='cr2-host-') as work:
 # Copy actual files byte-for-byte so quoted cr2_trace.h resolves to the host
 # stub instead of the adjacent production trace header. No logic is patched.
 host_source=Path(work)/'source';host_source.mkdir()
 for f in source.iterdir():
  if f.is_file() and f.name.startswith(('cr2_dma','cr2_wait','cr2_inventory','cr2_startup','cr2_sync')):
   shutil.copy2(f,host_source/f.name)
 source=host_source
 for frames in [240,256,480,1023]:
  for name in ['startup','dma_probe']:
   binary=Path(work)/(name+str(frames))
   subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined',
    '-DCR2_DMA_FRAMES='+str(frames),'-I'+str(here/'stubs'),'-I'+str(source),
    str(here/('test_'+name+'.c')),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
 binary=Path(work)/'wait'
 subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined',
  '-I'+str(source),str(here/'test_wait.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('PASS source-block parity and ASan/UBSan host tests; no physical timing claim')

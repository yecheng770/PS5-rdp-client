from pathlib import Path
import sys
from PIL import Image
import pymupdf
src, out = map(Path,sys.argv[1:])
doc=pymupdf.open(src); p=doc[0].get_pixmap(alpha=True)
a=Image.frombytes('RGBA',(p.width,p.height),p.samples).getchannel('A').resize((448,448),Image.Resampling.LANCZOS)
with out.open('w',encoding='ascii',newline='\n') as f:
    f.write('/* DualSense SVG by Alex Martynov, CC Attribution, SVG Repo.\n * Rasterized alpha mask. Source and attribution are in assets/dual-sense/. */\n#define DUALSENSE_SIZE 448\nstatic const unsigned char dualsense_alpha[DUALSENSE_SIZE * DUALSENSE_SIZE] = {\n')
    data=a.tobytes()
    for i in range(0,len(data),32): f.write('    '+','.join(map(str,data[i:i+32]))+',\n')
    f.write('};\n')

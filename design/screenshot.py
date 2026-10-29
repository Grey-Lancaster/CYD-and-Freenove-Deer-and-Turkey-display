"""Grab what a board is showing: python design/screenshot.py [host] [out.png]   (default deerhunt28.local)"""
import sys, urllib.request
import numpy as np
from PIL import Image

host = sys.argv[1] if len(sys.argv) > 1 else "deerhunt28.local"
out = sys.argv[2] if len(sys.argv) > 2 else "design/screenshot.png"
r = urllib.request.urlopen("http://%s/api/frame" % host, timeout=15)
w, h = int(r.headers["X-W"]), int(r.headers["X-H"])
v = np.frombuffer(r.read(), ">u2").reshape(h, w).astype(np.uint32)
rgb = np.dstack([(v >> 11) & 31, (v >> 5) & 63, v & 31])
rgb = (rgb * [255 / 31, 255 / 63, 255 / 31]).astype(np.uint8)
Image.fromarray(rgb).resize((w * 2, h * 2), Image.NEAREST).save(out)
print(out, w, h)

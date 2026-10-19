# Pre-build step (platformio.ini, extra_scripts): web/index.html -> web/index.html.gz, which is embedded into the firmware
# (board_build.embed_files) and sent to the browser as it is, marked as gzip (~4x smaller than the page). mtime 0 and no
# file name in the gzip header: the same page always gives the same bytes, so the same firmware image. The file is
# rewritten only when its content changes, so an unchanged page does not rebuild anything.
import gzip
import os

Import("env")  # noqa: F821 (SCons)

src = os.path.join(env["PROJECT_DIR"], "web", "index.html")  # noqa: F821
dst = src + ".gz"
with open(src, "rb") as f:
    packed = gzip.compress(f.read(), compresslevel=9, mtime=0)
old = None
if os.path.exists(dst):
    with open(dst, "rb") as f:
        old = f.read()
if old != packed:
    with open(dst, "wb") as f:
        f.write(packed)

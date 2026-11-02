import os, struct, sys

root = r"D:\Android\charging-investigation\x300\cmsdt\soc\vfcs_cms"
rows = []
for n in sorted(os.listdir(root)):
    p = os.path.join(root, n)
    if not os.path.isfile(p):
        continue
    b = open(p, "rb").read()
    if len(b) % 4:
        continue
    cells = struct.unpack(">%dI" % (len(b) // 4), b)
    # signed interpretation too
    rows.append((n, cells))

def fmt(cells):
    return " ".join(str(c) for c in cells)

print("%d files" % len(rows))
for n, c in rows:
    if "tc-data" in n or "cur" in n or "temp" in n:
        print("%-52s n=%-3d %s" % (n, len(c), fmt(c)))

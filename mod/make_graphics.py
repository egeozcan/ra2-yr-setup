#!/usr/bin/env python3
"""Build the Liberator's art: voxel models (attnk.vxl/.hva, attnktur.vxl/.hva) and cameo (attkicon.shp).

usage: make_graphics.py [OUT_DIR]      (default: mod/assets; needs numpy + Pillow)
The model is built from scratch by liberator_model.py. Palettes, the stock .hva files (one
identity section each, reused as-is) and the stock cameo frame are read from the game's MIX archives.
"""
import os, shutil, sys
import numpy as np
import vxl

FA2 = "/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II/FinalAlert2"
NORMALS = vxl.load_normal_table(os.path.join(FA2, "voxel_normal_tables.bin"), 4)


def extract_sources(dst):
    import mixextract
    game = os.path.dirname(FA2)
    for archive, path in (("ra2.mix", "local.mix/ttnk.vxl"), ("ra2.mix", "local.mix/ttnk.hva"),
                          ("ra2.mix", "local.mix/ttnktur.vxl"), ("ra2.mix", "local.mix/ttnktur.hva"),
                          ("ra2.mix", "cache.mix/unittem.pal"), ("ra2.mix", "cache.mix/cameo.pal"),
                          ("language.mix", "cameo.mix/ttnkicon.shp")):
        with open(os.path.join(game, archive), "rb") as f:
            data = mixextract.extract(f.read(), path)
        open(os.path.join(dst, os.path.basename(path)), "wb").write(data)


def main():
    import tempfile
    import liberator_model
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as src:
        extract_sources(src)
        hull, turret = liberator_model.build(NORMALS)
        for name, sec in (("attnk", hull), ("attnktur", turret)):
            v = vxl.Vxl.load(os.path.join(src, "ttnk.vxl"))  # keeps the stock header palette
            v.sections = [sec]
            v.save(os.path.join(out, name + ".vxl"))
        shutil.copy(os.path.join(src, "ttnk.hva"), os.path.join(out, "attnk.hva"))
        shutil.copy(os.path.join(src, "ttnktur.hva"), os.path.join(out, "attnktur.hva"))
        make_cameo(hull, turret, src, os.path.join(out, "attkicon.shp"), "Liberator")
    print("wrote attnk.vxl/.hva, attnktur.vxl/.hva, attkicon.shp to", out)


# ---------------- cameo ----------------
# 3x5 capitals in the style of the stock cameo labels
FONT = {
    "A": ["###", "#.#", "###", "#.#", "#.#"], "B": ["##.", "#.#", "##.", "#.#", "##."],
    "C": ["###", "#..", "#..", "#..", "###"], "D": ["##.", "#.#", "#.#", "#.#", "##."],
    "E": ["###", "#..", "##.", "#..", "###"], "F": ["###", "#..", "##.", "#..", "#.."],
    "G": ["###", "#..", "#.#", "#.#", "###"], "H": ["#.#", "#.#", "###", "#.#", "#.#"],
    "I": ["###", ".#.", ".#.", ".#.", "###"], "K": ["#.#", "#.#", "##.", "#.#", "#.#"],
    "L": ["#..", "#..", "#..", "#..", "###"], "M": ["#.#", "###", "###", "#.#", "#.#"],
    "N": ["##.", "#.#", "#.#", "#.#", "#.#"], "O": ["###", "#.#", "#.#", "#.#", "###"],
    "P": ["###", "#.#", "###", "#..", "#.."], "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "S": ["###", "#..", "###", "..#", "###"], "T": ["###", ".#.", ".#.", ".#.", ".#."],
    "U": ["#.#", "#.#", "#.#", "#.#", "###"], "V": ["#.#", "#.#", "#.#", "#.#", ".#."],
    "W": ["#.#", "#.#", "###", "###", "#.#"], "Y": ["#.#", "#.#", ".#.", ".#.", ".#."],
    " ": ["..", "..", "..", "..", ".."],
}


def make_cameo(hull, turret, src, out, label):
    """Render the new model into a 60x48 cameo SHP using cameo.pal."""
    from PIL import Image, ImageFilter
    import render as R
    upal = R.remap_colors(R.load_pal(os.path.join(src, "unittem.pal")), (70, 110, 235))
    cpal = R.load_pal(os.path.join(src, "cameo.pal"))
    W, H, K = 60, 48, 4

    # painted-style backdrop: stormy blue sky, bright horizon, snowy ground
    yy = np.linspace(0, 1, H * K)[:, None, None]
    sky = np.array([40, 55, 95]) * (1 - yy) + np.array([150, 170, 200]) * yy
    ground = np.array([200, 205, 215]) * (1 - (yy - .55) / .45) + np.array([120, 125, 140]) * ((yy - .55) / .45)
    bg = np.where(yy < .55, sky, ground) * np.ones((1, W * K, 1))
    rng = np.random.default_rng(7)
    bg += rng.normal(0, 6, bg.shape)
    back = Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8), "RGB").filter(ImageFilter.GaussianBlur(3)).convert("RGBA")

    tank = R.render([(hull, None, (0, 0, 0), 0), (turret, None, (0, 0, 0), 0)], upal, NORMALS,
                    yaw_deg=215, elev_deg=26, px=4.5, size=(W * K, H * K), light=(-0.6, -1, 1.3), center=(-4, 0, 13))
    from PIL import ImageEnhance
    tank = ImageEnhance.Brightness(tank).enhance(1.05)
    # soft cyan glow around the orb and coil tips
    a = np.asarray(tank).astype(np.float32)
    glowmask = ((a[..., 1] > 150) & (a[..., 2] > 150) & (a[..., 0] < 120)).astype(np.uint8) * 255
    halo = Image.fromarray(glowmask, "L").filter(ImageFilter.GaussianBlur(9))
    glow = Image.new("RGBA", back.size, (120, 240, 255, 0))
    glow.putalpha(halo.point(lambda v: min(255, v * 2)))
    shadow = Image.new("RGBA", back.size, (20, 25, 35, 0))
    shadow.putalpha(tank.getchannel("A").filter(ImageFilter.GaussianBlur(6)).point(lambda v: v * 0.6))
    img = back.copy()
    lift = -1 * K  # keep the tank clear of the label strip
    img.alpha_composite(shadow, (8, 10 + lift))
    img.alpha_composite(glow, (0, lift))
    img.alpha_composite(tank, (0, lift))
    img = img.convert("RGB").resize((W, H), Image.LANCZOS)
    rgb = np.asarray(img).astype(np.float32)

    # label strip
    rgb[40:48] *= 0.35
    rows = [r for r in zip(*[FONT[ch] for ch in label.upper()])]
    text_w = sum(len(FONT[ch][0]) + 1 for ch in label.upper()) - 1
    x = (W - text_w) // 2
    for ch in label.upper():
        g = FONT[ch]
        for gy in range(5):
            for gx, px in enumerate(g[gy]):
                if px == "#":
                    rgb[42 + gy, x + gx] = [255, 255, 255] if gy < 3 else [185, 185, 190]
        x += len(g[0]) + 1

    # nearest colour in cameo.pal, skipping index 0
    p = cpal[1:].astype(np.float32)
    flat = rgb.reshape(-1, 3)
    idx = np.argmin(((flat[:, None, :] - p[None]) ** 2).sum(2), axis=1) + 1
    idx = idx.reshape(H, W)
    # reuse the stock cameo's 2-pixel frame (dark rim, blue corner marks)
    _, _, frames = R.read_shp(os.path.join(src, "ttnkicon.shp"))
    stock = frames[0][4]
    ring = np.ones((H, W), bool)
    ring[2:-2, 2:-2] = False
    ring[40:47, 2:-2] = False  # but keep our own label
    idx[ring] = stock[ring]
    R.write_shp(out, idx, os.path.join(src, "ttnkicon.shp"))


if __name__ == "__main__":
    main()

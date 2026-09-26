#!/usr/bin/env python3
"""Generate painted cameo art for the Liberator with fal.ai, from a render of the voxel model.

usage: fal_cameo.py KEYFILE [N]      (KEYFILE holds the fal.ai key; or set FAL_KEY and pass -)

Renders the model (the reference, cameo-art/reference.png), asks the image model to repaint it as a
Red Alert 2 style cameo, and saves N candidates as cameo-art/candidate-K.png (git ignores them).
Look at them at cameo size, copy the one you like to cameo-art/liberator.png, set CAMEO_ART_CROP in
make_graphics.py, then run make_graphics.py. The key is only sent to fal.ai and is never written anywhere.
Needs numpy and Pillow (the mod venv).
"""
import base64, json, os, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = "fal-ai/nano-banana/edit"
# The prompt that produced cameo-art/liberator.png (2026-09-26). Most results of a batch stayed voxel-like;
# generate several and pick a painted one.
PROMPT = (
    "Repaint this blocky voxel tank as a detailed, realistic unit portrait in the style of the Command & Conquer: "
    "Red Alert 2 sidebar cameo art (late-1990s pre-rendered CGI with a painted finish). Keep the vehicle's design "
    "and its pose and framing: a low, heavy tank with a stepped hull, blue-grey steel armour, royal-blue painted "
    "side skirts and turret cheeks, a squat faceted turret, a front Tesla coil gun made of stacked grey metal discs "
    "ending in a glowing blue-white electrode, two small coil stacks on the turret roof and a thin antenna. "
    "The whole tank, including the full length of the coil gun and the electrode, stays inside the picture and "
    "fills most of it; the electrode sits clear of the left edge with small blue sparks around it. "
    "Bright clear blue sky with a few white clouds, warm sandy ground, strong afternoon sunlight, high contrast, "
    "rich saturated colours, crisp detail. No text, no logos, no border, no frame."
)


def reference(path):
    """A 5:4 render of the model framed like a cameo, blue house colour, on a plain background."""
    import tempfile
    import make_graphics as MG, liberator_model, render as R
    hull, turret = liberator_model.build(MG.NORMALS)
    with tempfile.TemporaryDirectory() as src:
        MG.extract_sources(src)
        pal = R.remap_colors(R.load_pal(os.path.join(src, "unittem.pal")), (40, 90, 230))
    img = R.render([(hull, None, (0, 0, 0), 0), (turret, None, (0, 0, 0), -10)], pal, MG.NORMALS,
                   yaw_deg=222, elev_deg=20, px=21, size=(1250, 1000), light=(-0.6, -1, 1.3), center=(-1, 0, 14),
                   bg=(235, 235, 235, 255))
    img.convert("RGB").save(path)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    key = os.environ["FAL_KEY"] if sys.argv[1] == "-" else open(sys.argv[1]).read().strip()
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    out = os.path.join(HERE, "cameo-art")
    ref = os.path.join(out, "reference.png")
    reference(ref)
    payload = {"prompt": PROMPT, "num_images": n, "output_format": "png",
               "image_urls": ["data:image/png;base64," + base64.b64encode(open(ref, "rb").read()).decode()]}
    req = urllib.request.Request("https://fal.run/" + MODEL, data=json.dumps(payload).encode(),
                                 headers={"Authorization": "Key " + key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        images = json.load(r).get("images", [])
    for i, image in enumerate(images):
        path = os.path.join(out, f"candidate-{i}.png")
        urllib.request.urlretrieve(image["url"], path)
        print("saved", path)


if __name__ == "__main__":
    main()

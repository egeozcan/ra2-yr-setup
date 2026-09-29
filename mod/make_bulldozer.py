#!/usr/bin/env python3
"""Generate the Soviet bulldozer voxel, identity HVA, sidebar cameo and previews.
Run with mod/.venv/bin/python mod/make_bulldozer.py [asset output directory].
Reuses geometry/format helpers, but contains entirely new vehicle geometry.
"""
import os
import shutil
import sys
import tempfile
import numpy as np
from PIL import Image
import vxl
import render as R
from liberator_model import Grid, STEEL, PLATE, DARK, TRACK, REMAP, LENS
from make_graphics import NORMALS, extract_sources, write_cameo, painted_cameo, FONT
from cheatdef_art import VPL_LIGHT


def build():
    g = Grid(-25, 33, -21, 21, 0, 28)
    X, Y, Z = g.X, g.Y, g.Z
    ay = np.abs(Y)
    # Broad exposed crawler tracks, wheels and raised fenders.
    tracks = (ay >= 10) & (ay <= 16) & ((X-np.clip(X,-18,15))**2+(Z-4)**2 <= 16)
    g.add(tracks, TRACK)
    g.paint(tracks & (np.floor(X) % 3 == 0), PLATE)
    for y in [-16, 15]:
        for x in [-17,-10,-3,4,12]:
            g.add(g.cyl_y(y,y+1,x,4,2.7), DARK)
            g.add(g.cyl_y(y,y+1,x,4,1), STEEL)
    g.add(g.box(-21,19,-11,11,3,10), PLATE)
    g.add(g.box(-21,19,-17,17,8,10), REMAP)
    # Long sloping engine hood and rear armored operator cab.
    hood = g.box(-5,19,-9,9,10,17) & (Z <= 17-np.maximum(X-7,0)*.35)
    g.add(hood, STEEL)
    g.paint(hood & (ay < 6) & (X>3) & (X<13) & (np.floor(X)%3==0) & (Z>15), DARK)
    cab = g.box(-18,-4,-9,9,10,25) & (ay <= 9-np.maximum(Z-19,0)*.35)
    g.add(cab, REMAP)
    g.paint(cab & (Z>=18) & (Z<22) & ((X>-6)|(ay>7)), DARK)
    g.paint(cab & (Z>=21) & (Z<22) & (X>-6), STEEL)
    g.add(g.box(-19,-3,-9,9,24,26), STEEL)
    g.add(g.box(-14,-8,-4,4,26,27), PLATE)
    g.add(g.cyl_z(12,27,-2,-8,1.2), DARK)
    g.add(g.box(-4,0,-10,-6,26,28), DARK)
    # Push arms and hydraulic rams attach the blade to the chassis.
    for y in [-13,13]:
        g.add(g.box(5,28,y-1.5,y+1.5,4,7), DARK)
        g.add(g.cyl_x(13,25,y,9,1.4), PLATE)
        g.add(g.cyl_x(23,29,y,9,0.8), LENS)
        g.add(g.box(15,18,y-1,y+1,10,12), LENS)
    # Curved moldboard: protruding lower edge, tall shoulders and side endplates.
    bx = 28 + .055*(Z-8)**2 - .006*Y**2
    blade = (ay<=20) & (Z>=1) & (Z<=14) & (X>=bx-1.5) & (X<=bx+1.5)
    g.add(blade, STEEL)
    g.paint(blade & (Z<3), LENS)
    g.paint(blade & (Z>12), PLATE)
    for y in [-19,19]:
        g.add(g.box(25,31,y-1,y+1,2,14), PLATE)
    # Reinforcement ribs on the rear of the blade, visible from behind.
    for y in [-12,0,12]:
        g.add((np.abs(Y-y)<1) & (Z>3) & (Z<13) & (X>bx-3) & (X<bx-1), DARK)
    return g.to_section(NORMALS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv)>1 else os.path.join(here,'assets')
    prev = os.path.join(here,'previews')
    os.makedirs(out,exist_ok=True)
    os.makedirs(prev,exist_ok=True)
    hull = build()
    with tempfile.TemporaryDirectory() as src:
        extract_sources(src)
        model = vxl.Vxl.load(os.path.join(src,'ttnk.vxl'))
        model.sections = [hull]
        model.save(os.path.join(out,'sbdozr.vxl'))
        loaded = vxl.Vxl.load(os.path.join(out,'sbdozr.vxl')).sections[0]
        for field in ['solid','color','normal']:
            assert np.array_equal(getattr(hull,field),getattr(loaded,field)), field
        shutil.copy(os.path.join(src,'ttnk.hva'),os.path.join(out,'sbdozr.hva'))
        pal = R.remap_colors(R.load_pal(os.path.join(src,'unittem.pal')),(210,40,30))
        vpl = R.load_vpl(os.path.join(src,'voxels.vpl'))
        def draw(facing,size,scale):
            ix=R.draw_vpl([(hull,0)],facing,vpl,NORMALS,VPL_LIGHT,size,scale)
            rgb=np.full((*ix.shape,3),(74,92,52),dtype=np.uint8)
            rgb[ix>0]=pal[ix[ix>0]]
            return rgb
        views=[draw(f,(300,240),4) for f in [30,120,210,300]]
        Image.fromarray(np.concatenate(views,axis=1)).save(os.path.join(prev,'bulldozer-model.png'))
        strip=Image.fromarray(np.concatenate([draw(f,(96,72),1) for f in range(0,360,45)],axis=1))
        strip.resize((strip.width*3,strip.height*3),Image.Resampling.NEAREST).save(os.path.join(prev,'bulldozer-ingame-scale.png'))
        # Painted portrait, reduced to cameo.pal and framed/labeled like stock icons.
        # Keep the procedural model render as a fallback if the portrait is absent.
        portrait = os.path.join(here, 'cameo-art', 'bulldozer.png')
        if os.path.exists(portrait):
            rgb = painted_cameo(portrait, None)
        else:
            scene=R.render([(hull,None,(0,0,0),0)],pal,NORMALS,yaw_deg=35,elev_deg=24,
                           px=3.1,size=(240,192),light=(-.6,-1,1.3),center=(3,0,13))
            bg=Image.new('RGBA',scene.size,(87,102,121,255))
            bg.alpha_composite(scene)
            rgb=np.asarray(bg.convert('RGB').resize((60,48),Image.Resampling.LANCZOS)).astype(float)
        FONT['Z']=['###','..#','.#.','#..','###']
        cameo=os.path.join(out,'sbdzicon.shp')
        write_cameo(rgb,src,cameo,'Bulldozer')
        _,_,frames=R.read_shp(cameo)
        pixels=frames[0][4]
        assert pixels.shape==(48,60)
        cp=R.load_pal(os.path.join(src,'cameo.pal'))
        Image.fromarray(cp[pixels]).resize((240,192),Image.Resampling.NEAREST).save(os.path.join(prev,'bulldozer-cameo.png'))
    print('Built and round-trip checked bulldozer VXL and cameo; wrote HVA and previews.')


if __name__=='__main__':
    main()

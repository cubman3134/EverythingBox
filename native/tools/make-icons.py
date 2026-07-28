"""Generate every EverythingBox app icon from one definition.

Run:  python native/tools/make-icons.py
It rewrites the committed icon assets in place, so the icon is reproducible rather than a
pile of exported PNGs nobody can regenerate.

THE DESIGN — an opening box with EB on the front and light pouring out of it. Two things
about how it is drawn are deliberate and were both learned by getting them wrong first.

DRAW ORDER IS THE TRICK. Painting the light first and the box on top makes a halo BEHIND
the box: the shafts appear to start at the back edge and it reads as a backdrop, no matter
how bright you make it. The order here is back flap, then the glowing mouth, then the shafts
(which therefore pass IN FRONT of the back flap), then the near geometry which cuts off the
base of the shafts, and finally a bloom spilling over the lit lip. Light emerging from behind
a near edge is what actually reads as emission.

THE LIGHT IS GENERATED, NOT DRAWN. Blurred polygons keep their outline and read as
translucent card. The shafts are a fan mask multiplied by a radial falloff centred on the
mouth, so brightness dies with distance and the shafts have no top edge at all. The colour is
gold rather than white because a white glow is invisible on a white background — which is
exactly where a taskbar puts it.

TWO CROPS, ON PURPOSE. Windows sits icons in a tight grid and a padded icon looks small next
to its neighbours, so Windows gets the full-bleed crop. macOS, iOS and Android all inset or
MASK the icon (Android to a circle or squircle, iOS to a squircle), which would clip the flap
tips — those get the padded crop. Same artwork, different framing.

At 16px the EB and every facet are gone, so the box-plus-wedge silhouette has to carry it.
"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)              # native/
K = 4                                     # supersample factor

# ---- palette -------------------------------------------------------------
BOX_TOP   = (40, 47, 72)
BOX_BOT   = (22, 27, 43)
FLAP_OUT  = (46, 53, 81)
FLAP_EDGE = (168, 146, 110)   # flap thickness catching light from inside
MOUTH     = (58, 46, 30)      # interior, in shadow away from the beam
RIM       = (255, 216, 146)
LIGHT     = (255, 203, 104)   # gold: must read on a light background too
EB_INK    = (255, 247, 231)
IOS_PLATE = (18, 21, 33)      # opaque backing for iOS, which forbids alpha in app icons


FONTS = (r"C:\Windows\Fonts\segoeuib.ttf", r"C:\Windows\Fonts\arialbd.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "/Library/Fonts/Arial Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf")


def font(size):
    """A real bold face, or nothing. Falling back to PIL's built-in bitmap font would still
    'work' — it would just silently emit a tiny jagged EB into a shipped app icon, on whichever
    machine happened to lack the font. A loud failure is the only safe behaviour here."""
    for p in FONTS:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    raise SystemExit("make-icons: no bold TrueType font found. Tried:\n  " + "\n  ".join(FONTS) +
                     "\nInstall one (e.g. fonts-dejavu-core) or add its path to FONTS.")


def render(S=1024, scale=1.0):
    """The icon at S x S. `scale` < 1 shrinks the artwork about the centre (the padded crop)."""
    W = S * K
    C = 512.0

    def px(*pts):
        return [(((x - C) * scale + C) * S / 1024.0 * K,
                 ((y - C) * scale + C) * S / 1024.0 * K) for (x, y) in pts]

    def u(v):                       # scalar in 1024-space -> device px
        return v * scale * S / 1024.0 * K

    def poly_mask(pts):
        m = Image.new("L", (W, W), 0)
        ImageDraw.Draw(m).polygon(pts, fill=255)
        return m

    APEX = (512, 566)
    FRONT  = px((150, 604), (874, 604), (826, 962), (198, 962))
    MOUTHP = px((150, 604), (874, 604), (778, 550), (246, 550))
    BACKF  = px((246, 554), (778, 554), (720, 408), (304, 408))
    LFLAP  = px((246, 552), (150, 606), (26, 470), (140, 402))
    RFLAP  = px((778, 552), (874, 606), (998, 470), (884, 402))

    base = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)

    # 1. back flap — it tips AWAY, so we see its outside: dark, not lit
    d.polygon(BACKF, fill=FLAP_OUT)
    d.polygon(px((246, 554), (778, 554), (772, 538), (252, 538)), fill=FLAP_EDGE)

    # 2. the mouth, with a hot core confined to the opening
    d.polygon(MOUTHP, fill=MOUTH)
    hot = Image.new("L", (W, W), 0)
    ImageDraw.Draw(hot).ellipse(px((250, 528), (774, 620)), fill=255)
    hot = ImageChops.multiply(hot.filter(ImageFilter.GaussianBlur(u(11))), poly_mask(MOUTHP))
    g = Image.new("RGBA", (W, W), (255, 246, 222, 0)); g.putalpha(hot)
    base = Image.alpha_composite(base, g)

    # 3. shafts, ON TOP of the back flap
    fan = Image.new("L", (W, W), 0)
    fd = ImageDraw.Draw(fan)
    for spread, top_y, v in ((470, -60, 58), (300, -80, 92), (166, -100, 132)):
        fd.polygon(px(APEX, (512 - spread, top_y), (512 + spread, top_y)), fill=v)
    fd.polygon(px(APEX, (10, 200), (190, 40)), fill=74)
    fd.polygon(px(APEX, (1014, 200), (834, 40)), fill=74)
    fan = fan.filter(ImageFilter.GaussianBlur(u(16)))

    R = int(u(820))
    rad = ImageOps.invert(Image.radial_gradient("L")).resize((R * 2, R * 2), Image.BICUBIC)
    rad = rad.point(lambda v: int(255 * ((v / 255.0) ** 2.4)))
    falloff = Image.new("L", (W, W), 0)
    ax, ay = px(APEX)[0]
    falloff.paste(rad, (int(ax) - R, int(ay) - R))

    sh = Image.new("RGBA", (W, W), LIGHT + (0,))
    sh.putalpha(ImageChops.multiply(fan, falloff))
    base = Image.alpha_composite(base, sh)

    # 4. near geometry, which cuts the shafts off at their base
    d = ImageDraw.Draw(base)
    d.polygon(LFLAP, fill=FLAP_OUT)
    d.polygon(RFLAP, fill=FLAP_OUT)
    d.polygon(px((246, 552), (140, 402), (158, 394), (260, 544)), fill=FLAP_EDGE)
    d.polygon(px((778, 552), (884, 402), (866, 394), (764, 544)), fill=FLAP_EDGE)

    grad = Image.linear_gradient("L").rotate(180).resize((W, W), Image.BICUBIC)
    face = Image.composite(Image.new("RGBA", (W, W), BOX_TOP + (255,)),
                           Image.new("RGBA", (W, W), BOX_BOT + (255,)), grad)
    base.paste(face, (0, 0), poly_mask(FRONT))
    d = ImageDraw.Draw(base)
    d.polygon(px((150, 604), (874, 604), (872, 624), (152, 624)), fill=RIM)

    # 5. EB
    f = font(int(u(228)))
    l, t, r_, b = d.textbbox((0, 0), "EB", font=f)
    cx, cy = px((512, 796))[0]
    d.text((cx - (r_ - l) / 2 - l, cy - (b - t) / 2 - t), "EB", font=f, fill=EB_INK + (255,))

    # 6. spill over the lit lip, on top of everything
    sp = Image.new("L", (W, W), 0)
    ImageDraw.Draw(sp).ellipse(px((214, 566), (810, 634)), fill=104)
    sp = sp.filter(ImageFilter.GaussianBlur(u(17)))
    bl = Image.new("RGBA", (W, W), (255, 238, 198, 0)); bl.putalpha(sp)
    base = Image.alpha_composite(base, bl)

    return base.resize((S, S), Image.LANCZOS)


def banner(w=320, h=180):
    """Android TV leanback banner. The launcher shows NO app name over it, so the name is
    part of the artwork — a banner with only a glyph is unidentifiable in a TV row."""
    up = 4
    W, H = w * up, h * up
    im = Image.new("RGBA", (W, H), (18, 21, 33, 255))

    side = int(H * 0.88)
    icon = render(S=side, scale=0.94)
    im.alpha_composite(icon, (int(H * 0.03), (H - side) // 2))

    d = ImageDraw.Draw(im)
    tx = int(H * 0.93)
    avail = W - tx - int(H * 0.07)

    # Size the wordmark to FIT rather than assuming: at a guessed size "Everything" ran off the
    # right edge, and a banner is the one asset where the name is the only thing identifying
    # the app — the TV launcher draws no label over it.
    size = int(H * 0.21)
    while size > 8:
        f = font(size)
        if max(d.textlength(s, font=f) for s in ("Everything", "Box")) <= avail:
            break
        size -= 2

    lh = size * 1.02
    top = (H - lh * 2) / 2
    d.text((tx, top), "Everything", font=f, fill=(240, 243, 252, 255))
    d.text((tx, top + lh), "Box", font=f, fill=LIGHT + (255,))
    return im.resize((w, h), Image.LANCZOS)


def main():
    full   = render(1024, scale=1.00)   # Windows: full-bleed, sits in a tight grid
    padded = render(1024, scale=0.84)   # macOS / iOS / Android: they inset or mask

    res, andr = os.path.join(ROOT, "resources"), os.path.join(ROOT, "android", "res", "drawable")
    # The appiconset MUST live inside an .xcassets — iOS only reads a COMPILED asset catalog, so an
    # .appiconset sitting loose beside it is never built and the app gets a blank springboard tile.
    ios = os.path.join(ROOT, "ios", "Assets.xcassets", "AppIcon.appiconset")
    os.makedirs(ios, exist_ok=True)
    wrote = []

    def save(img, path, **kw):
        img.save(path, **kw); wrote.append(os.path.relpath(path, os.path.dirname(ROOT)))

    # Windows + the in-app window icon (qrc) + the AppImage icon
    save(full.resize((256, 256), Image.LANCZOS), os.path.join(res, "appicon.png"))
    save(full, os.path.join(res, "appicon.ico"),
         sizes=[(256, 256), (64, 64), (48, 48), (32, 32), (16, 16)])
    # macOS bundle
    save(padded, os.path.join(res, "appicon.icns"))
    # Android launcher + Android TV banner
    save(padded.resize((256, 256), Image.LANCZOS), os.path.join(andr, "icon.png"))
    save(banner(), os.path.join(andr, "banner.png"))

    # iOS asset catalog
    specs = [(20, 2), (20, 3), (29, 2), (29, 3), (40, 2), (40, 3),
             (60, 2), (60, 3), (76, 1), (76, 2), (83.5, 2), (1024, 1)]
    images = []
    for pt, s in specs:
        n = int(round(pt * s))
        fn = f"icon-{pt}x{pt}@{s}x.png".replace(".0", "")
        # EVERY iOS icon is flattened, not just the 1024 store art. iOS app icons may not carry
        # an alpha channel at all — the asset compiler rejects them and the home screen has no
        # notion of a transparent icon. Flattening only the store size (the first version of this
        # script) shipped eleven icons with alpha, which looked correct in any preview that
        # composited them itself and would have failed on a real device.
        art = padded.resize((n, n), Image.LANCZOS)
        img = Image.new("RGB", (n, n), IOS_PLATE)
        img.paste(art, (0, 0), art)
        if pt == 1024:
            fn = "icon-1024.png"
        save(img, os.path.join(ios, fn))
        images.append({"size": f"{pt:g}x{pt:g}", "idiom": "universal" if pt == 1024 else
                       ("ipad" if pt in (76, 83.5) else "iphone"),
                       "filename": fn, "scale": f"{s}x"})
    INFO = {"version": 1, "author": "xcode"}
    with open(os.path.join(ios, "Contents.json"), "w", encoding="utf-8") as fh:
        json.dump({"images": images, "info": INFO}, fh, indent=2)
    wrote.append("native/ios/Assets.xcassets/AppIcon.appiconset/Contents.json")
    # The catalog root needs its own Contents.json too — actool rejects a directory that only LOOKS
    # like an .xcassets, so regenerating into a clean tree has to produce a complete catalog.
    with open(os.path.join(os.path.dirname(ios), "Contents.json"), "w", encoding="utf-8") as fh:
        json.dump({"info": INFO}, fh, indent=2)
    wrote.append("native/ios/Assets.xcassets/Contents.json")

    for p in wrote:
        print("wrote", p.replace("\\", "/"))


if __name__ == "__main__":
    main()

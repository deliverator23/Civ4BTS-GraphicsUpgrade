# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow>=10.1"]
# ///
"""Make an alternating before/after GIF from two screenshots of the same view.

  uv run tools/ab_gif.py before.png after.png -o water.gif
  uv run tools/ab_gif.py before.png after.png -o water.gif --width 1280 --hold 2 --fade 6 --labels "BtS" "GraphicsUpgrade"

Both frames share one palette, so parts that don't change stay perfectly still. --fade adds crossfade frames between
the two (bigger file); --labels "" "" leaves the labels off.
"""
import argparse

from PIL import Image, ImageDraw, ImageFont


def label(img, text):
    if not text:
        return img
    img = img.copy()
    draw = ImageDraw.Draw(img)
    size = max(14, img.width // 40)
    try:
        font = ImageFont.truetype('arialbd.ttf', size)
    except OSError:
        font = ImageFont.load_default(size)
    pad = size // 2
    box = draw.textbbox((0, 0), text, font=font)
    w, h = box[2] - box[0], box[3] - box[1]
    draw.rectangle((pad, pad, pad + w + 2 * pad, pad + h + 2 * pad), fill=(0, 0, 0))
    draw.text((2 * pad - box[0], 2 * pad - box[1]), text, font=font, fill=(255, 255, 255))
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('before')
    ap.add_argument('after')
    ap.add_argument('-o', '--out', default='before_after.gif')
    ap.add_argument('--width', type=int, default=960, help='output width in pixels (0 = as is; default 960)')
    ap.add_argument('--hold', type=float, default=1.5, help='seconds each picture is shown (default 1.5)')
    ap.add_argument('--fade', type=int, default=0, help='crossfade frames between them (default 0 = hard cut)')
    ap.add_argument('--labels', nargs=2, default=['Before', 'After'], metavar=('BEFORE', 'AFTER'))
    args = ap.parse_args()

    a, b = Image.open(args.before).convert('RGB'), Image.open(args.after).convert('RGB')
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
    if args.width and a.width > args.width:
        size = (args.width, round(a.height * args.width / a.width))
        a, b = a.resize(size, Image.LANCZOS), b.resize(size, Image.LANCZOS)

    hold = round(args.hold * 1000)
    fade = [i / (args.fade + 1) for i in range(1, args.fade + 1)]
    frames = [(label(a, args.labels[0]), hold)]
    frames += [(Image.blend(a, b, t), 60) for t in fade]
    frames += [(label(b, args.labels[1]), hold)]
    frames += [(Image.blend(b, a, t), 60) for t in fade]

    # One palette for every frame, from both pictures side by side.
    both = Image.new('RGB', (a.width * 2, a.height))
    both.paste(frames[0][0], (0, 0))
    both.paste(frames[1 + len(fade)][0], (a.width, 0))
    palette = both.quantize(256, method=Image.Quantize.MEDIANCUT)
    images = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f, _ in frames]
    images[0].save(args.out, save_all=True, append_images=images[1:], duration=[d for _, d in frames], loop=0,
                   optimize=True, disposal=1)
    print(f'{args.out}: {len(images)} frames, {a.width}x{a.height}')


if __name__ == '__main__':
    main()

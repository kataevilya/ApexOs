from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
CHAR_W, CHAR_H = 8, 16
FIRST, LAST = 32, 126  # печатный ASCII

font = ImageFont.truetype(FONT_PATH, 13)  # подобрано, чтобы влезало в 8x16 с небольшим отступом

glyphs = {}
preview_cols = 16
preview_rows = (LAST - FIRST) // preview_cols + 1
preview = Image.new("1", (preview_cols*(CHAR_W+1), preview_rows*(CHAR_H+1)), 0)

for idx, code in enumerate(range(FIRST, LAST+1)):
    ch = chr(code)
    img = Image.new("1", (CHAR_W, CHAR_H), 0)
    draw = ImageDraw.Draw(img)
    bbox = draw.textbbox((0,0), ch, font=font)
    w = bbox[2]-bbox[0]
    h = bbox[3]-bbox[1]
    x = (CHAR_W - w)//2 - bbox[0]
    y = 0  # baseline handling: just top-align with slight offset
    draw.text((max(0,x), -1), ch, font=font, fill=1)
    rows = []
    for ry in range(CHAR_H):
        byte = 0
        for rx in range(CHAR_W):
            px = img.getpixel((rx, ry))
            byte = (byte << 1) | (1 if px else 0)
        rows.append(byte)
    glyphs[code] = rows
    px_off = (idx % preview_cols) * (CHAR_W+1)
    py_off = (idx // preview_cols) * (CHAR_H+1)
    preview.paste(img, (px_off, py_off))

preview = preview.resize((preview.width*4, preview.height*4), Image.NEAREST)
preview.convert("RGB").save("/home/claude/fontgen/preview.png")

with open("/home/claude/fontgen/font8x16.c", "w") as f:
    f.write("/* Auto-generated 8x16 monospace bitmap font (ASCII 32-126), rasterized from\n")
    f.write("   DejaVu Sans Mono via PIL — not hand-transcribed, to avoid transcription\n")
    f.write("   errors. Each glyph is 16 bytes, one byte per row, MSB = leftmost pixel. */\n\n")
    f.write("#include \"font8x16.h\"\n\n")
    f.write(f"const uint8_t font8x16_data[FONT8X16_NUM_GLYPHS][FONT8X16_HEIGHT] = {{\n")
    for code in range(FIRST, LAST+1):
        rows = glyphs[code]
        rowstr = ", ".join(f"0x{b:02X}" for b in rows)
        ch = chr(code)
        comment = ch if ch not in ('\\', "'") else ('\\\\' if ch=='\\' else "\\'")
        f.write(f"    {{ {rowstr} }}, /* {code} '{comment}' */\n")
    f.write("};\n")

print("done, glyphs:", len(glyphs))

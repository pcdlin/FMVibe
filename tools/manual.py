#!/usr/bin/env python3
"""Turns the user manual (docs/manual.md) into one self-contained HTML file.

    python tools/manual.py docs/manual.md FMVibe-Manual.html

The page holds everything it needs: its styles inline, the screenshots as data URIs and system fonts only, so it
opens offline from a zip. It also checks the manual and fails (exit code 1) on a link to a heading that isn't there,
an image that isn't there, or Markdown it couldn't convert, so a broken manual stops the build (the `manual` test)
rather than the release. It reads only what the manual uses: headings, paragraphs, lists (nested by two spaces),
tables, fenced blocks, images, links, **bold**, *italics* and `code`. The version comes from project() in
CMakeLists.txt. Needs nothing beyond Python 3's standard library.
"""

import base64
import html
import mimetypes
import re
import sys
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def project_version():
    try:
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    except OSError:
        return None
    m = re.search(r"^project\(FMVibe VERSION ([0-9.]+)", text, re.M)
    return m.group(1) if m else None


class Converter:
    def __init__(self, source_dir):
        self.source_dir = source_dir
        self.errors = []
        self.ids = {}
        self.toc = []  # (id, text, [(id, text)])
        self.links = []

    # GitHub's heading ids, so the manual's links work on GitHub and on this page alike.
    def heading_id(self, text):
        base = re.sub(r"[^\w\- ]", "", text.lower()).replace(" ", "-")
        n = self.ids.get(base, 0)
        self.ids[base] = n + 1
        return base if n == 0 else f"{base}-{n}"

    def image(self, alt, src):
        # The text around it is escaped already, but for its quotes, which would end the attribute.
        alt = alt.replace('"', "&quot;")
        path = self.source_dir / src
        if not path.is_file():
            self.errors.append(f"image not found: {src}")
            return f'<img alt="{alt}">'
        mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        data = base64.b64encode(path.read_bytes()).decode("ascii")
        return f'<img src="data:{mime};base64,{data}" alt="{alt}">'

    def inline(self, text):
        codes = []

        def keep(m):
            codes.append("<code>" + html.escape(m.group(1)) + "</code>")
            return f"\x00{len(codes) - 1}\x00"

        t = re.sub(r"`([^`]+)`", keep, text)
        t = html.escape(t, quote=False)
        t = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", lambda m: self.image(m.group(1), m.group(2)), t)

        def link(m):
            label, href = m.group(1), m.group(2)
            if href.startswith("#"):
                self.links.append(href[1:])
                return f'<a href="{href}">{label}</a>'
            if re.match(r"https?://", href):
                return f'<a href="{href}" target="_blank" rel="noopener">{label}</a>'
            self.errors.append(f"link to a file, which the page can't carry: {href}")
            return label

        t = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, t)
        t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
        t = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?!\w)", r"<em>\1</em>", t)
        t = re.sub("\x00(\\d+)\x00", lambda m: codes[int(m.group(1))], t)
        for left in ("**", "](", "``"):
            if left in re.sub(r"<code>.*?</code>", "", t):
                self.errors.append(f"unconverted Markdown ({left}) in: {text[:60]}")
        return t

    def list_block(self, block):
        items = []
        for line in block:
            if not line.strip():
                continue
            m = re.match(r"^(\s*)(- |\d+\. )(.*)$", line)
            if m:
                items.append([len(m.group(1)), m.group(2) != "- ", m.group(3)])
            else:
                items[-1][2] += " " + line.strip()

        def build(i, indent):
            tag = "ol" if items[i][1] else "ul"
            out = f"<{tag}>"
            while i < len(items) and items[i][0] >= indent:
                level, _, text = items[i]
                out += "<li>" + self.inline(text)
                i += 1
                if i < len(items) and items[i][0] > level:
                    sub, i = build(i, items[i][0])
                    out += sub
                out += "</li>"
            return out + f"</{tag}>", i

        return build(0, items[0][0])[0]

    def convert(self, source):
        lines = source.split("\n")
        out, i = [], 0
        title, lede, skip_contents = None, None, False
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                i += 1
                continue
            if line.startswith("```"):
                j = i + 1
                while j < len(lines) and not lines[j].startswith("```"):
                    j += 1
                if j == len(lines):
                    self.errors.append("a fenced block is never closed")
                out.append('<pre class="diagram"><code>' + html.escape("\n".join(lines[i + 1:j])) + "</code></pre>")
                i = j + 1
                continue
            m = re.match(r"^(#{1,6}) (.+)$", line)
            if m:
                level, text = len(m.group(1)), m.group(2)
                i += 1
                if level == 1 and title is None:
                    title = text
                    continue
                hid = self.heading_id(text)
                # The page's sidebar takes the place of the manual's own contents, up to its rule.
                skip_contents = level == 2 and hid == "contents"
                if skip_contents:
                    continue
                if level == 2:
                    self.toc.append((hid, text, []))
                elif level == 3 and self.toc:
                    self.toc[-1][2].append((hid, text))
                out.append(f'<h{level} id="{hid}">{self.inline(text)}</h{level}>')
                continue
            if line.strip() == "---":
                skip_contents = False
                i += 1
                continue
            if line.startswith("|"):
                rows = []
                while i < len(lines) and lines[i].startswith("|"):
                    rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                    i += 1
                if skip_contents:
                    continue
                head = "".join(f"<th>{self.inline(c)}</th>" for c in rows[0])
                body = "".join(
                    "<tr>" + "".join(f"<td>{self.inline(c)}</td>" for c in row) + "</tr>" for row in rows[2:]
                )
                out.append(
                    f'<div class="table"><table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table></div>'
                )
                continue
            if re.match(r"^\s*(- |\d+\. )", line):
                block = []
                while i < len(lines) and (
                    re.match(r"^\s*(- |\d+\. )|^\s+\S", lines[i])
                    or (not lines[i].strip() and i + 1 < len(lines) and re.match(r"^\s+\S", lines[i + 1]))
                ):
                    block.append(lines[i])
                    i += 1
                if skip_contents:
                    # The manual's contents link to its sections; count them as links all the same.
                    for entry in block:
                        self.inline(entry)
                    continue
                out.append(self.list_block(block))
                continue
            para = []
            while i < len(lines) and lines[i].strip() and not re.match(r"^(#|\||```|\s*- |\d+\. |---)", lines[i]):
                para.append(lines[i].strip())
                i += 1
            text = " ".join(para)
            if title is not None and lede is None and not out:
                lede = self.inline(text)
            elif text.startswith("!["):
                out.append("<figure>" + self.inline(text) + "</figure>")
            else:
                out.append("<p>" + self.inline(text) + "</p>")
        for target in self.links:
            if target not in self.ids:
                self.errors.append(f"link to a heading that isn't there: #{target}")
        return title or "FMVibe User Manual", lede or "", "\n".join(out)

    def nav(self):
        out = ""
        for hid, text, subs in self.toc:
            out += f'<li><a href="#{hid}">{self.inline(text)}</a>'
            if subs:
                out += "<ul>" + "".join(f'<li><a href="#{s}">{self.inline(t)}</a></li>' for s, t in subs) + "</ul>"
            out += "</li>"
        return out


class TagCheck(HTMLParser):
    VOID = {"img", "meta", "hr", "br", "input"}

    def __init__(self):
        super().__init__()
        self.stack, self.errors = [], []

    def handle_starttag(self, tag, attrs):
        if tag not in self.VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in self.VOID:
            return
        if self.stack and self.stack[-1] == tag:
            self.stack.pop()
        else:
            self.errors.append(f"</{tag}> doesn't close <{self.stack[-1] if self.stack else '-'}>")


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>{title}</title>
<style>
/* A sticky contents rail beside one reading column, under a slim title band. System fonts, so it reads offline. */
:root {{
  --bg: #f5f5fa; --surface: #ffffff; --fg: #1c1a2e; --muted: #5d5a75; --line: #dddbe9;
  --accent: #5442d6; --accent-soft: #ebe8fd; --code-bg: #eeedf7; --row: #f9f9fd;
  --font-display: "Segoe UI Variable Display", "Segoe UI", system-ui, -apple-system, sans-serif;
  --font-body: "Segoe UI Variable Text", "Segoe UI", system-ui, -apple-system, sans-serif;
  --font-mono: "Cascadia Mono", Consolas, "SF Mono", Menlo, monospace;
  color-scheme: light;
}}
@media (prefers-color-scheme: dark) {{
  :root:not([data-theme="light"]) {{
    --bg: #12102a; --surface: #1b1938; --fg: #e7e5f6; --muted: #a29fc0; --line: #2f2c55;
    --accent: #a294ff; --accent-soft: #29245a; --code-bg: #242150; --row: #201d42;
    color-scheme: dark;
  }}
}}
:root[data-theme="dark"] {{
  --bg: #12102a; --surface: #1b1938; --fg: #e7e5f6; --muted: #a29fc0; --line: #2f2c55;
  --accent: #a294ff; --accent-soft: #29245a; --code-bg: #242150; --row: #201d42;
  color-scheme: dark;
}}
* {{ box-sizing: border-box; }}
html {{ scroll-behavior: smooth; scroll-padding-top: 24px; }}
@media (prefers-reduced-motion: reduce) {{ html {{ scroll-behavior: auto; }} }}
body {{
  margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.65 var(--font-body);
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px));
}}
a {{ color: var(--accent); text-underline-offset: 2px; }}
a:focus-visible {{ outline: 2px solid var(--accent); outline-offset: 2px; border-radius: 2px; }}
.masthead {{
  max-width: 1180px; margin: 0 auto; padding-block: 40px 28px; border-bottom: 1px solid var(--line);
  display: grid; gap: 10px;
}}
.eyebrow {{
  font: 500 12px/1 var(--font-mono); letter-spacing: .12em; text-transform: uppercase; color: var(--muted);
  display: flex; gap: 14px; flex-wrap: wrap;
}}
.eyebrow span + span::before {{ content: "\\00b7"; margin-right: 14px; }}
.masthead h1 {{
  font: 700 clamp(30px, 5vw, 46px)/1.05 var(--font-display); letter-spacing: -.02em; margin: 0; text-wrap: balance;
}}
.masthead p {{ margin: 0; max-width: 68ch; color: var(--muted); font-size: 16px; }}
.ops {{ display: flex; gap: 6px; margin-top: 8px; }}
.ops i {{
  width: 30px; height: 30px; border-radius: 50%; border: 2px solid var(--accent); display: grid; place-items: center;
  font: 600 12px var(--font-display); font-style: normal; color: var(--accent);
}}
.ops i:nth-child(n+2) {{ opacity: .55; border-style: dashed; }}
.layout {{
  max-width: 1180px; margin: 0 auto; padding-block: 28px 80px;
  display: grid; grid-template-columns: 250px minmax(0, 1fr); gap: 56px; align-items: start;
}}
nav.toc {{
  position: sticky; top: calc(16px + env(safe-area-inset-top, 0px)); max-height: calc(100vh - 32px);
  overflow-y: auto; padding-right: 6px; font-size: 13.5px;
}}
nav.toc .label {{
  font: 500 11px var(--font-mono); letter-spacing: .12em; text-transform: uppercase; color: var(--muted);
  margin-bottom: 10px;
}}
nav.toc ul {{ list-style: none; margin: 0; padding: 0; }}
nav.toc > ul > li > a {{
  display: block; padding: 5px 10px; border-radius: 6px; color: var(--fg); text-decoration: none; font-weight: 500;
}}
nav.toc ul ul {{ display: none; margin: 2px 0 8px 10px; border-left: 1px solid var(--line); }}
nav.toc li.active > ul {{ display: block; }}
nav.toc ul ul a {{ display: block; padding: 3px 12px; color: var(--muted); text-decoration: none; font-size: 13px; }}
nav.toc a:hover {{ color: var(--accent); }}
nav.toc li.active > a {{ background: var(--accent-soft); color: var(--accent); }}
nav.toc ul ul a.current {{ color: var(--accent); }}
details.mobile-toc {{ display: none; }}
main {{ min-width: 0; max-width: 760px; }}
main h2 {{
  font: 700 28px/1.2 var(--font-display); letter-spacing: -.01em; margin: 56px 0 14px; padding-top: 18px;
  border-top: 1px solid var(--line); text-wrap: balance;
}}
main > h2:first-of-type {{ margin-top: 0; border-top: 0; padding-top: 0; }}
main h3 {{ font: 600 20px/1.3 var(--font-display); margin: 34px 0 10px; text-wrap: balance; }}
main h4 {{ font: 600 16px/1.3 var(--font-display); margin: 26px 0 8px; color: var(--accent); }}
main p, main li {{ max-width: 70ch; }}
main ul, main ol {{ padding-left: 22px; }}
main li {{ margin: 4px 0; }}
main li > ul {{ margin: 4px 0; }}
main li::marker {{ color: var(--muted); }}
strong {{ font-weight: 600; }}
code {{
  font: 500 .86em var(--font-mono); background: var(--code-bg); padding: 1px 5px; border-radius: 4px;
  overflow-wrap: anywhere;
}}
pre.diagram {{
  background: var(--surface); border: 1px solid var(--line); border-radius: 8px; padding: 16px 18px;
  overflow-x: auto; font-size: 13px; line-height: 1.45;
}}
pre.diagram code {{ background: none; padding: 0; overflow-wrap: normal; }}
.table {{
  overflow-x: auto; margin: 16px 0; border: 1px solid var(--line); border-radius: 8px; background: var(--surface);
}}
table {{ border-collapse: collapse; width: 100%; font-size: 14px; }}
th, td {{ text-align: left; vertical-align: top; padding: 8px 12px; border-bottom: 1px solid var(--line); }}
th {{
  font: 600 12px var(--font-body); letter-spacing: .04em; text-transform: uppercase; color: var(--muted);
  background: var(--row);
}}
tr:last-child td {{ border-bottom: 0; }}
td:first-child {{ white-space: nowrap; font-weight: 500; }}
figure {{ margin: 20px 0; }}
figure img {{
  display: block; width: 100%; height: auto; border-radius: 8px; border: 1px solid var(--line);
  box-shadow: 0 6px 24px rgb(20 16 60 / .14);
}}
footer {{
  max-width: 1180px; margin: 0 auto; padding-block: 20px 40px; border-top: 1px solid var(--line);
  color: var(--muted); font-size: 13px;
}}
@media (max-width: 900px) {{
  .layout {{ grid-template-columns: minmax(0, 1fr); gap: 20px; }}
  nav.toc {{ display: none; }}
  details.mobile-toc {{
    display: block; border: 1px solid var(--line); border-radius: 8px; background: var(--surface); padding: 10px 14px;
    margin-bottom: 24px;
  }}
  details.mobile-toc summary {{ cursor: pointer; font-weight: 600; }}
  details.mobile-toc ul {{
    list-style: none; padding: 0; margin: 8px 0 0; columns: 2; column-gap: 20px; font-size: 14px;
  }}
  details.mobile-toc ul ul {{ display: none; }}
  details.mobile-toc li {{ break-inside: avoid; padding: 3px 0; }}
  td:first-child {{ white-space: normal; }}
}}
@media (max-width: 480px) {{ details.mobile-toc ul {{ columns: 1; }} }}
</style>
</head>
<body>
<header class="masthead">
  <div class="eyebrow">{eyebrow}</div>
  <h1>{title}</h1>
  <p>{lede}</p>
  <div class="ops" aria-hidden="true"><i>A</i><i>B</i><i>C</i><i>D</i><i>E</i><i>F</i></div>
</header>
<div class="layout">
  <nav class="toc" aria-label="Contents">
    <div class="label">Contents</div>
    <ul>{nav}</ul>
  </nav>
  <main>
    <details class="mobile-toc"><summary>Contents</summary><ul>{nav}</ul></details>
{body}
  </main>
</div>
<footer>FMVibe{version_text} is released under the MIT license.</footer>
<script>
(() => {{
  const links = new Map();
  document.querySelectorAll('nav.toc a').forEach(a => links.set(a.getAttribute('href').slice(1), a));
  const heads = [...document.querySelectorAll('main h2[id], main h3[id]')].filter(h => links.has(h.id));
  let last = null;
  function update() {{
    let cur = heads[0];
    for (const h of heads) {{ if (h.getBoundingClientRect().top < 120) cur = h; else break; }}
    if (!cur || cur === last) return;
    last = cur;
    document.querySelectorAll('nav.toc li.active').forEach(li => li.classList.remove('active'));
    document.querySelectorAll('nav.toc a.current').forEach(a => a.classList.remove('current'));
    const a = links.get(cur.id);
    a.classList.add('current');
    const top = a.closest('nav.toc > ul > li');
    if (top) top.classList.add('active');
  }}
  addEventListener('scroll', update, {{ passive: true }});
  update();
}})();
</script>
</body>
</html>
"""


def main(argv):
    if len(argv) != 3:
        print("usage: manual.py <manual.md> <out.html>", file=sys.stderr)
        return 2
    source, target = Path(argv[1]), Path(argv[2])
    conv = Converter(source.parent)
    title, lede, body = conv.convert(source.read_text(encoding="utf-8"))
    version = project_version()
    eyebrow = ["CLAP · VST3", "Windows x64"] + ([f"Version {version}"] if version else [])
    page = PAGE.format(
        title=html.escape(title),
        lede=lede,
        eyebrow="".join(f"<span>{e}</span>" for e in eyebrow),
        nav=conv.nav(),
        body=body,
        version_text=f" {version}" if version else "",
    )
    check = TagCheck()
    check.feed(page)
    conv.errors += check.errors
    if check.stack:
        conv.errors.append("unclosed tags: " + ", ".join(check.stack))
    if conv.errors:
        for error in conv.errors:
            print(f"{source}: {error}", file=sys.stderr)
        return 1
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(page, encoding="utf-8")
    print(f"{target}: {len(page) // 1024} KB, {len(conv.toc)} sections")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

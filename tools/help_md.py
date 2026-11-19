"""Builds the Markdown of the device's built-in help page (/help, /help.md).

The repository documentation is split into README.md, docs/*.md and
CHANGELOG.md; the device serves it as ONE page (rendered offline by
include/help_page.h). This module concatenates the files in a fixed order
and rewrites the links for a single page:

- links between the included files (`docs/x.md#anchor`, `x.md#anchor`,
  `#anchor`) become in-page anchors `#anchor`; a link to a whole file
  points to that file's first heading,
- links to other repository files become absolute GitHub URLs,
- links leaving the repository are reduced to their text, images are dropped,
- blocks between "<!-- help:skip -->" and "<!-- /help:skip -->" (picture
  galleries, badges — raw HTML and images the renderer can't show, whose
  files aren't embedded) are left out.

help_page.h derives heading ids with its own slugify() and does not
de-duplicate them, so a heading whose id already occurs earlier in the
combined page gets the title of its file appended (and links to it are
remapped accordingly).

Used by tools/version.py; also runnable on its own:
    python3 tools/help_md.py [project_dir] > help.md
"""

import posixpath
import re
import sys
from pathlib import Path

HELP_FILES = [
    "README.md",
    "docs/installation.md",
    "docs/wireless-sensors.md",
    "docs/minimal-build.md",
    "docs/web-dashboard.md",
    "docs/mqtt-home-assistant.md",
    "docs/weather-networks.md",
    "docs/loxone.md",
    "docs/values.md",
    "docs/settings.md",
    "docs/lorawan.md",
    "docs/pcb.md",
    "docs/troubleshooting.md",
    "docs/development.md",
    "CHANGELOG.md",
]

REPO_URL = "https://github.com/franzenhof/universal-weather-node"

HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
LINK_RE = re.compile(r"(!?)\[([^\]]*)\]\(([^)\s]+)\)")


def gh_slug(text):
    """GitHub's heading anchor (without the -1/-2 duplicate suffix)."""
    t = text.strip().lower()
    t = re.sub(r"[^\w\- ]", "", t)
    return t.replace(" ", "-")


def help_slug(text):
    """Same as slugify() in include/help_page.h."""
    return re.sub(r"[^a-z0-9 -]", "", text.lower()).strip().replace(" ", "-")


def strip_skip_blocks(name, text):
    out, skipping = [], False
    for line in text.splitlines():
        marker = line.strip()
        if marker == "<!-- help:skip -->":
            skipping = True
            continue
        if marker == "<!-- /help:skip -->":
            skipping = False
            continue
        if not skipping:
            out.append(line)
    if skipping:
        raise RuntimeError(f"{name}: unterminated <!-- help:skip --> block")
    return out


def iter_text_lines(lines):
    """Yields (index, line) for lines outside fenced code blocks."""
    in_code = False
    for i, line in enumerate(lines):
        if line.startswith("```"):
            in_code = not in_code
            continue
        if not in_code:
            yield i, line


def build(project_dir, files=HELP_FILES, warn=None):
    project_dir = Path(project_dir)
    warn = warn or (lambda msg: print("help_md: " + msg))
    docs = {}
    for name in files:
        p = project_dir / name
        if p.exists():
            docs[name] = strip_skip_blocks(name, p.read_text(encoding="utf-8"))
    if not docs:
        return None

    # Pass 1: heading ids. anchor_map[(file, github anchor)] = help-page id.
    anchor_map, title_id, used = {}, {}, set()
    for name, lines in docs.items():
        seen, file_title = {}, None
        for i, line in iter_text_lines(lines):
            m = HEADING_RE.match(line)
            if not m:
                continue
            text = m.group(2)
            if file_title is None:
                file_title = text
            gs = gh_slug(text)
            n = seen.get(gs)
            seen[gs] = 0 if n is None else n + 1
            gh_anchor = gs if n is None else f"{gs}-{n + 1}"
            hid = help_slug(text)
            if hid in used:
                text = f"{text} ({file_title})"
                hid = help_slug(text)
                k = 2
                while hid in used:
                    text = f"{m.group(2)} ({file_title} {k})"
                    hid = help_slug(text)
                    k += 1
                lines[i] = f"{m.group(1)} {text}"
            used.add(hid)
            anchor_map[(name, gh_anchor)] = hid
            title_id.setdefault(name, hid)

    # Pass 2: links.
    def resolve(name, label, target):
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("//"):
            return f"[{label}]({target})"
        path, _, frag = target.partition("#")
        if path:
            ref = posixpath.normpath(posixpath.join(posixpath.dirname(name), path))
        else:
            ref = name
        if ref.startswith(".."):
            return label  # outside the repository: keep the text only
        if ref in docs:
            if not frag:
                return f"[{label}](#{title_id[ref]})"
            hid = anchor_map.get((ref, frag))
            if hid is None:
                warn(f"{name}: no heading for link {target}")
                hid = frag
            return f"[{label}](#{hid})"
        kind = "tree" if (project_dir / ref).is_dir() else "blob"
        if not (project_dir / ref).exists():
            warn(f"{name}: link to missing file {target}")
        url = f"{REPO_URL}/{kind}/main/{ref}"
        return f"[{label}]({url}{'#' + frag if frag else ''})"

    parts = []
    for name, lines in docs.items():
        for i, line in iter_text_lines(lines):
            if "](" in line:
                lines[i] = LINK_RE.sub(
                    lambda m: "" if m.group(1) else resolve(name, m.group(2), m.group(3)), line)
        parts.append("\n".join(lines).strip("\n"))
    return "\n\n---\n\n".join(parts) + "\n"


if __name__ == "__main__":
    sys.stdout.write(build(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent))

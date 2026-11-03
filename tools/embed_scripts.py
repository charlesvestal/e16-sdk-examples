#!/usr/bin/env python3
"""Embed the scripts listed in scripts/manifest.txt into the firmware.

  embed_scripts.py <manifest> <script dir> <output .c> [--dump-json <out.json>]

Each manifest line (blank lines and '#' comments are skipped):

  <file>  <title, may contain spaces>  <page names, comma-separated>  <pad pages>  <settings page>

<file> is a name in <script dir> (external/oxi-e16-lua), or, if it has a
'/' in it, a path relative to the manifest's own directory (the
launcher's demos: demos/canvas.lua is scripts/demos/canvas.lua).

Writes a C file defining (see src/scripts.h):

  const rt_scene_t scenes[]          title, pages, wiring (from --@assign lines)
  const char *const sources[]        the script source, comments kept
  const unsigned source_lens[]
  const int scene_count

Wiring follows tools/make_scene.py in external/oxi-e16-lua: only ids a
`--@assign id=N ...` line declares are wired, and each one's abbr= becomes
that control's default label.

A script may also carry a header line:

  --@scene title="..." pages="Page A,Page B" pad_pages=N settings_page=M

This is for scripts dropped onto the disk volume (src/disk/scene_parse.c
reads it there, with no manifest involved). For a script listed in the
manifest, as here, the precedence is manifest > --@scene > defaults: the
manifest's title/pages/pad_pages/settings_page (required on every line,
see parse_manifest()) always win for the scenes[] this script generates.
--@scene and the file-name/ids-used defaults only matter for the
--@scene-and-defaults values --dump-json reports below, which exist to
let test/disk/scene_parse_test.c check scene_parse.c against this file's
parsing of the same scripts with no manifest in the picture.

--dump-json <out.json> writes, for each manifest entry, the *header-only*
scene info (ignoring the manifest's title/pages/pad_pages/settings_page
entirely): name, title, pages, pad_pages, settings_page, assigned (the
sorted --@assign ids), and abbr (id -> its --@assign abbr, truncated to 4
bytes as scenes[] above does). It changes nothing else main() does.
"""
import json
import os
import re
import sys

RT_MAX_PAGES = 12
RT_SCENE_NAME_MAX = 31
# src/persist.h: each script gets a fixed pair of flash sectors by its
# menu index, and the layout has room for this many.
PERSIST_MAX_SCRIPTS = 32


def assigns(src, path):
    """Parse --@assign directives into {id: {key: value}} (as make_scene.py)."""
    res = {}
    for m in re.finditer(r"^--@assign\s+(.*)$", src, re.M):
        line = src.count("\n", 0, m.start()) + 1
        kv = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', m.group(1)))
        kv = {k: v.strip('"') for k, v in kv.items()}
        if "id" not in kv:
            sys.exit(f"{path}:{line}: --@assign with no id=")
        try:
            id_ = int(kv["id"])
        except ValueError:
            sys.exit(f"{path}:{line}: --@assign id={kv['id']!r} is not a number")
        res[id_] = kv
    return res


def scene_header(src, path):
    """Parses a script's own --@scene line (if any) and --@assign lines,
    the way src/disk/scene_parse.c does for a disk script with no
    manifest entry: {name, title, pages, pad_pages, settings_page,
    assigned}. assigned is assigns()'s {id: {key: value}}; the rest
    mirror scene_parse.c's defaulting, key by key:

      - no title=  -> the file's name (not capitalized -- same string
        scene_parse.c's caller passes it as the scene's own name)
      - no pages=  -> ["P1"], or ["P1", "Set"] if any --@assign id is
        33-64 (the settings-page convention, make_scene.py)
      - no pad_pages=    -> 1
      - no settings_page -> 2 if pages was also defaulted and the file
        uses a 33-64 id, else 0
    """
    a = assigns(src, path)
    name = os.path.splitext(os.path.basename(path))[0]
    if len(name) > RT_SCENE_NAME_MAX:
        sys.exit(f"{path}: scene name over {RT_SCENE_NAME_MAX} characters")

    scene_lines = list(re.finditer(r"^--@scene\b\s*(.*)$", src, re.M))
    if len(scene_lines) > 1:
        line = src.count("\n", 0, scene_lines[1].start()) + 1
        sys.exit(f"{path}:{line}: duplicate --@scene line")

    title = pages = pad = settings = None
    if scene_lines:
        m = scene_lines[0]
        line = src.count("\n", 0, m.start()) + 1
        kv = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', m.group(1)))
        kv = {k: v.strip('"') for k, v in kv.items()}
        if "title" in kv:
            title = kv["title"]
        if "pages" in kv:
            pages = [p for p in kv["pages"].split(",") if p]
            if not pages:
                sys.exit(f"{path}:{line}: --@scene pages: need at least one page name")
            if len(pages) > RT_MAX_PAGES:
                sys.exit(f"{path}:{line}: --@scene pages: too many (max {RT_MAX_PAGES})")
        if "pad_pages" in kv:
            try:
                pad = int(kv["pad_pages"])
            except ValueError:
                sys.exit(f"{path}:{line}: --@scene pad_pages= is not a number")
        if "settings_page" in kv:
            try:
                settings = int(kv["settings_page"])
            except ValueError:
                sys.exit(f"{path}:{line}: --@scene settings_page= is not a number")

    had_pages = pages is not None
    needs_settings = any(id_ >= 33 for id_ in a)
    if title is None:
        title = name
    if pages is None:
        pages = ["P1", "Set"] if needs_settings else ["P1"]
    if pad is None:
        pad = 1
    if settings is None:
        settings = 2 if (not had_pages and needs_settings) else 0
    if not (0 <= pad <= len(pages) and 0 <= settings <= len(pages)):
        sys.exit(f"{path}: pad_pages/settings_page out of range")
    return {"name": name, "title": title, "pages": pages, "pad_pages": pad,
            "settings_page": settings, "assigned": a}


def script_path(file, script_dir, manifest_dir):
    """Where a manifest entry's file is: see the module docstring."""
    if "/" in file:
        return os.path.join(manifest_dir, file)
    return os.path.join(script_dir, file)


def dump_json(entries, script_dir, out_path, manifest_dir):
    """Writes the header-only scene info (--@scene/--@assign only, no
    manifest fields -- see scene_header()) for each manifest entry's
    script, as src/disk/scene_parse.c would compute it from that same
    file with nothing else to go on. test/disk/scene_parse_test.c
    compares its C parser's output against this."""
    scenes = []
    for file, _title, _pages, _pad, _settings in entries:
        path = script_path(file, script_dir, manifest_dir)
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        hdr = scene_header(src, file)
        abbr = {str(id_): kv["abbr"][:4] for id_, kv in hdr["assigned"].items() if kv.get("abbr")}
        scenes.append({
            "name": hdr["name"],
            "title": hdr["title"],
            "pages": hdr["pages"],
            "pad_pages": hdr["pad_pages"],
            "settings_page": hdr["settings_page"],
            "assigned": sorted(hdr["assigned"].keys()),
            "abbr": abbr,
        })
    with open(out_path, "w") as f:
        json.dump(scenes, f, separators=(",", ":"))


def c_string(s):
    """A C string literal for s (bytes as written; non-ASCII as octal escapes)."""
    out = []
    for b in s.encode("utf-8") if isinstance(s, str) else s:
        c = chr(b)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif 32 <= b < 127:
            out.append(c)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def c_source(data):
    """The script as one string literal split across lines (one per source line)."""
    lines = data.split(b"\n")
    parts = []
    for i, line in enumerate(lines):
        if i < len(lines) - 1:
            line += b"\n"
        if line:
            parts.append("    " + c_string(line))
    return "\n".join(parts) if parts else '    ""'


def parse_manifest(path):
    entries = []
    with open(path) as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            tok = line.split()
            if len(tok) < 5:
                sys.exit(f"{path}:{n}: expected: file title pages pad_pages settings_page")
            file, pages = tok[0], tok[-3]
            try:
                pad, settings = int(tok[-2]), int(tok[-1])
            except ValueError:
                sys.exit(f"{path}:{n}: pad_pages and settings_page must be numbers")
            title = " ".join(tok[1:-3])
            names = [p for p in pages.split(",") if p]
            if not 1 <= len(names) <= RT_MAX_PAGES:
                sys.exit(f"{path}:{n}: 1-{RT_MAX_PAGES} pages")
            if len(title.encode("utf-8")) > 15:
                sys.exit(f"{path}:{n}: title over 15 bytes (UTF-8)")
            if not (0 <= pad <= len(names) and 0 <= settings <= len(names)):
                sys.exit(f"{path}:{n}: pad/settings page out of range")
            entries.append((file, title, names, pad, settings))
    return entries


def main():
    argv = sys.argv[1:]
    dump_json_path = None
    if "--dump-json" in argv:
        i = argv.index("--dump-json")
        if i + 1 >= len(argv):
            sys.exit("--dump-json needs a path")
        dump_json_path = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) != 3:
        sys.exit(__doc__)
    manifest, script_dir, out_path = argv
    entries = parse_manifest(manifest)
    if len(entries) > PERSIST_MAX_SCRIPTS:
        sys.exit(f"{manifest}: {len(entries)} scripts, but the flash layout holds at most "
                 f"{PERSIST_MAX_SCRIPTS} (PERSIST_MAX_SCRIPTS in src/persist.h)")
    manifest_dir = os.path.dirname(os.path.abspath(manifest))
    if dump_json_path:
        dump_json(entries, script_dir, dump_json_path, manifest_dir)
    out = [
        "/* Generated by tools/embed_scripts.py from scripts/manifest.txt. Do not edit. */",
        '#include "scripts.h"',
        "",
    ]
    for i, (file, title, names, pad, settings) in enumerate(entries):
        with open(script_path(file, script_dir, manifest_dir), "rb") as f:
            data = f.read()
        out.append(f"/* {file} */")
        out.append(f"static const char src{i}[] =")
        out.append(c_source(data) + ";")
        out.append("")
    out.append("const rt_scene_t scenes[] = {")
    seen = set()
    for i, (file, title, names, pad, settings) in enumerate(entries):
        with open(script_path(file, script_dir, manifest_dir), encoding="utf-8", errors="replace") as f:
            a = assigns(f.read(), file)
        name = os.path.splitext(os.path.basename(file))[0]
        if len(name) > RT_SCENE_NAME_MAX:
            sys.exit(f"{file}: scene name over {RT_SCENE_NAME_MAX} characters")
        if name in seen:
            # Saved vars/store are matched to a script by this name.
            sys.exit(f"{file}: scene name {name!r} listed twice")
        seen.add(name)
        bits = [0] * 8
        abbr = {}
        for id_, kv in a.items():
            if not 1 <= id_ <= 64:
                sys.exit(f"{file}: --@assign id={id_} out of range 1-64")
            bits[(id_ - 1) // 8] |= 1 << ((id_ - 1) % 8)
            if kv.get("abbr"):
                abbr[id_] = kv["abbr"][:4]
        out.append("    {")
        out.append(f"        .name = {c_string(name)},")
        out.append(f"        .title = {c_string(title)},")
        out.append("        .page_names = {" + ", ".join(c_string(p) for p in names) + "},")
        out.append(f"        .pages = {len(names)},")
        out.append(f"        .pad_pages = {pad},")
        out.append(f"        .settings_page = {settings},")
        out.append("        .assigned = {" + ", ".join(f"0x{b:02X}" for b in bits) + "},")
        out.append("        .abbr = {")
        for id_ in sorted(abbr):
            out.append(f"            [{id_ - 1}] = {c_string(abbr[id_])},")
        out.append("        },")
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("const char *const sources[] = {" + ", ".join(f"src{i}" for i in range(len(entries))) + "};")
    out.append("const unsigned source_lens[] = {" +
               ", ".join(f"sizeof src{i} - 1" for i in range(len(entries))) + "};")
    out.append(f"const int scene_count = {len(entries)};")
    out.append("")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()

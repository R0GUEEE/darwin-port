#!/usr/bin/env python3
"""pxcodebuild - build an Xcode project on Linux, without Xcode.

Reads `*.xcodeproj/project.pbxproj` (OpenStep-style plist), compiles every
source with the extracted macOS SDK and links a real Mach-O with ld64.lld.
Produces the same bundle layout Xcode produces: `Foo.app/Contents/MacOS/Foo`.

  pxcodebuild.py build  <proj.xcodeproj|project.pbxproj> [--target NAME]
                        [--sdk PATH] [--out DIR] [-v]
  pxcodebuild.py list   <proj>                     targets + sources + settings
  pxcodebuild.py dump   <proj> [--target NAME]     resolved build plan as JSON

Exit codes: 0 built, 1 error, 2 usage.
"""
import json, os, re, shutil, subprocess, sys, time

# ---------------------------------------------------------------- pbxproj ---
_TOKEN = re.compile(r'''
    (?P<comment>/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\])*")
  | (?P<line>//[^\n]*)
  | (?P<data><[0-9a-fA-F\s]*>)
  | (?P<punct>[{}()=;,])
  | (?P<word>[^\s{}()=;,"]+)
''', re.S | re.X)


class P:
    def __init__(self, text):
        # comments carry the human-readable name; keep them as annotations
        self.toks = []
        for m in _TOKEN.finditer(text):
            kind = m.lastgroup
            if kind == 'comment':
                self.toks.append(('comment', m.group()[2:-2].strip()))
            elif kind == 'line':
                self.toks.append(('comment', m.group()[2:].strip()))
            elif kind == 'str':
                s = m.group()[1:-1]
                self.toks.append(('str', s.encode().decode('unicode_escape')))
            elif kind == 'data':
                self.toks.append(('str', m.group()))
            elif kind == 'punct':
                self.toks.append(('punct', m.group()))
            elif kind == 'word':
                self.toks.append(('str', m.group()))
        self.i = 0

    def _skip(self):
        while self.i < len(self.toks) and self.toks[self.i][0] == 'comment':
            self.i += 1

    def peek(self):
        self._skip()
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def take(self):
        self._skip()
        t = self.toks[self.i]; self.i += 1
        return t

    def parse(self):
        t, v = self.peek()
        if v == '{':
            return self._dict()
        if v == '(':
            return self._array()
        self.take()
        return v

    def _dict(self):
        self.take()                       # {
        d = {}
        while True:
            t, v = self.peek()
            if v == '}':
                self.take(); return d
            if t is None:
                return d
            k = self.take()[1]
            if self.peek()[1] == '=':
                self.take()
            d[k] = self.parse()
            if self.peek()[1] == ';':
                self.take()

    def _array(self):
        self.take()                       # (
        a = []
        while True:
            t, v = self.peek()
            if v == ')':
                self.take(); return a
            if t is None:
                return a
            a.append(self.parse())
            if self.peek()[1] == ',':
                self.take()


def load_project(path):
    if os.path.isdir(path):
        path = os.path.join(path, 'project.pbxproj')
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        txt = f.read()
    root = P(txt).parse()
    # paths in a project are relative to the directory that CONTAINS the
    # .xcodeproj bundle, not to the bundle itself
    d = os.path.dirname(os.path.abspath(path))
    if d.endswith('.xcodeproj'):
        d = os.path.dirname(d)
    return root, d


# ------------------------------------------------------------- resolution ---
def settings_for(objects, target):
    """Flatten buildSettings from the target's configuration list."""
    out = {}
    cl = objects.get(target.get('buildConfigurationList'), {})
    for cid in cl.get('buildConfigurations', []):
        c = objects.get(cid, {})
        out.update({k: v for k, v in c.get('buildSettings', {}).items()})
    # project-level settings first, target overrides them
    return out


def project_settings(objects, proj):
    out = {}
    cl = objects.get(proj.get('buildConfigurationList'), {})
    for cid in cl.get('buildConfigurations', []):
        out.update(objects.get(cid, {}).get('buildSettings', {}))
    return out


def resolve_path(objects, ref_id, groups, proj_dir, depth=0):
    """Walk a file reference up its group chain to a path on disk.

    Xcode records the file's path relative to its group; some projects record
    a project-relative path instead, so both candidates are returned and the
    caller picks whichever exists.
    """
    ref = objects.get(ref_id, {})
    p = ref.get('path')
    if ref.get('sourceTree') == '<absolute>':
        return p
    if not p or depth > 12:
        return None
    plain = os.path.normpath(os.path.join(proj_dir, p))
    parent = groups.get(ref_id)
    if not parent:
        return plain
    base = resolve_path(objects, parent, groups, proj_dir, depth + 1)
    if not base:
        return plain
    joined = os.path.normpath(os.path.join(base, p))
    return joined if os.path.exists(joined) or not os.path.exists(plain) else plain


def parent_map(objects):
    """child id -> parent id, from every PBXGroup/PBXVariantGroup children list."""
    m = {}
    for oid, o in objects.items():
        if o.get('isa') in ('PBXGroup', 'PBXVariantGroup', 'XCVersionGroup'):
            for c in o.get('children', []):
                m[c] = oid
    return m


def plan(root, proj_dir, target_name=None):
    objs = root.get('objects', {})
    proj = objs.get(root.get('rootObject'), {})
    groups = parent_map(objs)
    base = project_settings(objs, proj)

    targets = proj.get('targets', [])
    chosen = []
    for tid in targets:
        t = objs.get(tid, {})
        if target_name and t.get('name') != target_name:
            continue
        cfg = dict(base); cfg.update(settings_for(objs, t))
        sources, frameworks, resources = [], [], []
        for ph_id in t.get('buildPhases', []):
            ph = objs.get(ph_id, {})
            isa = ph.get('isa')
            for f in ph.get('files', []):
                bf = objs.get(f, {})
                ref = bf.get('fileRef')
                path = resolve_path(objs, ref, groups, proj_dir) if ref else None
                if isa == 'PBXSourcesBuildPhase' and path:
                    sources.append(path)
                elif isa == 'PBXFrameworksBuildPhase' and path:
                    frameworks.append(path)
                elif isa == 'PBXResourcesBuildPhase' and path:
                    resources.append(path)
        product = objs.get(t.get('productReference'), {})
        chosen.append({
            'name': t.get('name'),
            'productType': t.get('productType', ''),
            'product': product.get('path') or (cfg.get('PRODUCT_NAME', t.get('name')) + '.app'),
            'settings': cfg,
            'sources': sources,
            'frameworks': frameworks,
            'resources': resources,
        })
    return chosen


def subst(v, cfg):
    if not isinstance(v, str):
        return v
    def rep(m):
        k = m.group(1)
        return str(cfg.get(k, m.group(0)))
    return re.sub(r'\$[({]([A-Za-z_][A-Za-z0-9_]*)[)}]', rep, v)


# ----------------------------------------------------------------- build ----
def run(cmd, verbose, sdk=None):
    if verbose:
        print('  ' + ' '.join(cmd))
    env = dict(os.environ)
    if sdk:
        env['DARWIN_SDK'] = sdk
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(1)
    return r


def build(proj_path, target_name=None, sdk=None, outdir=None, verbose=False):
    root, proj_dir = load_project(proj_path)
    plans = plan(root, proj_dir, target_name)
    if not plans:
        raise SystemExit('no target matched')

    sdk = sdk or os.environ.get('DARWIN_SDK') or '/opt/xcode/MacOSX.sdk'
    if not os.path.isdir(sdk):
        raise SystemExit('no SDK at %s (set DARWIN_SDK)' % sdk)
    tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bin')
    xclang = os.path.join(tools, 'xclang')
    if not os.path.exists(xclang):
        raise SystemExit('xclang not found next to pxcodebuild')

    for pl in plans:
        cfg = pl['settings']
        name = cfg.get('PRODUCT_NAME', pl['name'])
        name = subst(name, cfg)
        plat = cfg.get('PLATFORM_NAME', 'macosx')
        if not outdir:
            base_out = os.path.join(proj_dir, 'build')
        else:
            base_out = outdir
        bundle = os.path.join(base_out, pl['product'] if pl['product'].endswith('.app')
                              else name + '.app')
        macos = os.path.join(bundle, 'Contents', 'MacOS')
        res = os.path.join(bundle, 'Contents', 'Resources')
        os.makedirs(macos, exist_ok=True)
        os.makedirs(res, exist_ok=True)

        print('[%s] %s → %s' % (plat, pl['name'], bundle))
        objdir = os.path.join(base_out, 'obj', name)
        os.makedirs(objdir, exist_ok=True)

        # compile
        objs = []
        cflags = [xclang, '-isysroot', sdk, '-O2', '-fno-stack-protector',
                  '-I', proj_dir]
        cflags += ['-D' + subst(d, cfg) for d in cfg.get('GCC_PREPROCESSOR_DEFINITIONS', [])]
        cflags += ['-I' + subst(i, cfg) for i in cfg.get('HEADER_SEARCH_PATHS', [])]
        cflags += ['-F' + subst(f, cfg) for f in cfg.get('FRAMEWORK_SEARCH_PATHS', [])]
        for src in pl['sources']:
            if not os.path.exists(src):
                sys.stderr.write('missing source: %s\n' % src)
                continue
            ext = os.path.splitext(src)[1].lower()
            o = os.path.join(objdir, os.path.basename(src) + '.o')
            extra = []
            if ext in ('.m', '.mm'):
                extra += ['-fobjc-arc']
            if ext == '.mm':
                extra += ['-std=c++17', '-lc++']
            run(cflags + extra + ['-c', src, '-o', o], verbose, sdk)
            objs.append(o)

        # link
        executable = os.path.join(macos, name)
        link = [xclang, '-isysroot', sdk, '-o', executable] + objs
        for fw in pl['frameworks']:
            fwname = os.path.basename(fw).replace('.framework', '')
            if fw.endswith('.framework'):
                link += ['-framework', fwname]
            elif fw.endswith(('.dylib', '.a', '.o')):
                link.append(fw)
        link += ['-Wl,-fixup_chains']
        run(link, verbose, sdk)

        # resources + Info.plist
        for r in pl['resources']:
            if os.path.exists(r):
                dst = os.path.join(res, os.path.basename(r))
                if os.path.isdir(r):
                    shutil.copytree(r, dst, dirs_exist_ok=True)
                else:
                    shutil.copy2(r, dst)
        plist = cfg.get('INFOPLIST_FILE')
        dest_plist = os.path.join(bundle, 'Contents', 'Info.plist')
        if plist and os.path.exists(os.path.join(proj_dir, subst(plist, cfg))):
            shutil.copy2(os.path.join(proj_dir, subst(plist, cfg)), dest_plist)
        else:
            write_plist(dest_plist, name, subst(cfg.get('PRODUCT_BUNDLE_IDENTIFIER',
                                                        'com.example.' + name), cfg))
        print('  %d objects, %d bytes binary' % (len(objs), os.path.getsize(executable)))
    return 0


def write_plist(path, name, bundle_id):
    with open(path, 'w') as f:
        f.write('''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>%s</string>
  <key>CFBundleIdentifier</key><string>%s</string>
  <key>CFBundleName</key><string>%s</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>15.0</string>
</dict></plist>
''' % (name, bundle_id, name))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, proj = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    target = sdk = outdir = None
    verbose = False
    i = 0
    while i < len(args):
        if args[i] == '--target': target = args[i + 1]; i += 2
        elif args[i] == '--sdk': sdk = args[i + 1]; i += 2
        elif args[i] == '--out': outdir = args[i + 1]; i += 2
        elif args[i] == '-v': verbose = True; i += 1
        else: i += 1

    if cmd in ('list', 'dump'):
        root, proj_dir = load_project(proj)
        plans = plan(root, proj_dir, target)
        if cmd == 'dump':
            print(json.dumps(plans, indent=1))
        else:
            for pl in plans:
                print('%s  (%s)' % (pl['name'], pl['productType'].replace('com.apple.product-type.', '')))
                for s in pl['sources']: print('   src  ', os.path.relpath(s, proj_dir))
                for f in pl['frameworks']: print('   link ', os.path.relpath(f, proj_dir))
                for r in pl['resources']: print('   res  ', os.path.relpath(r, proj_dir))
        return 0
    if cmd != 'build':
        print(__doc__)
        return 2
    return build(proj, target, sdk, outdir, verbose)


if __name__ == '__main__':
    sys.exit(main())

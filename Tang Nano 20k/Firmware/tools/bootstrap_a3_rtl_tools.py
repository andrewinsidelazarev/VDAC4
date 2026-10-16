"""Fetch portable MSYS2 RTL tools to an explicit directory; no PATH/registry edits.

Downloads are verified against SHA256 published by packages.msys2.org. Archives
are inspected before extraction. This is an optional development tool bootstrap,
not FPGA programming. Use --package yosys --core-only for the Verilog-only flow
without unused VHDL/Python backends. It also installs the directly linked libffi.
"""
import argparse
import hashlib
import html
import json
import re
import sys
import tarfile
import urllib.request
from pathlib import Path, PurePosixPath


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent":"A3-local-tool-bootstrap/1.0"})
    with urllib.request.urlopen(request, timeout=45) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--package", default="iverilog")
    parser.add_argument("--core-only",action="store_true",help="Yosys Verilog CLI only; omit VHDL/Python helpers")
    parser.add_argument("--env", choices=("mingw64","ucrt64"), default="mingw64",
                        help="MSYS2 environment; iverilog 13 is published only for ucrt64/clang64")
    args = parser.parse_args()
    env = args.env
    prefix = {"mingw64":"mingw-w64-x86_64-","ucrt64":"mingw-w64-ucrt-x86_64-"}[env]
    root = args.directory.resolve()
    if root == Path(root.anchor) or root == Path.home().resolve():
        raise SystemExit("Specify a dedicated tool directory")
    root.mkdir(parents=True, exist_ok=True)
    archive_dir = root / "packages"
    archive_dir.mkdir(exist_ok=True)
    sys.path.insert(0,str(root / "python"))
    import zstandard  # Optional dependency installed only in this tool directory.
    manifest = root / "download-manifest.json"
    visited = set()
    records = json.loads(manifest.read_text(encoding="utf-8")) if manifest.exists() else []

    def install(name):
        if name == prefix + "cc-libs":
            name = prefix + "gcc-libs"
        if name == prefix + "ghdl":
            name = prefix + "ghdl-llvm"  # Sole provider on the MSYS2 virtual-package page.
        if name in visited:
            return
        visited.add(name)
        if not re.fullmatch(re.escape(prefix) + r"[a-zA-Z0-9+_.-]+", name):
            raise ValueError(name)
        page_url = f"https://packages.msys2.org/packages/{name}"
        page = fetch(page_url).decode("utf-8")
        links = re.findall(r'href="(https://mirror\.msys2\.org/mingw/' + env + r'/[^"<>]+\.pkg\.tar\.zst)"',page)
        digests = re.findall(r'\b[0-9a-f]{64}\b',page)
        if not links or not digests:
            raise ValueError(f"Missing archive/checksum on {page_url}")
        url, digest = html.unescape(links[0]), digests[0]
        already_installed = any(r['package']==name and r['sha256']==digest for r in records)
        archive = archive_dir / url.rsplit("/",1)[1]
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            data = fetch(url)
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f"Checksum mismatch: {url}")
            archive.write_bytes(data)
        if not already_installed:
            with archive.open("rb") as compressed:
                with zstandard.ZstdDecompressor().stream_reader(compressed) as decompressed:
                    with tarfile.open(fileobj=decompressed,mode="r|") as bundle:
                        for member in bundle:
                            path = PurePosixPath(member.name)
                            if path.is_absolute() or ".." in path.parts or ":" in member.name or "\\" in member.name:
                                raise ValueError(f"Unsafe archive member: {member.name}")
                            if path.parts and path.parts[0] == env:
                                bundle.extract(member,root,filter="data")
        record = dict(package=name,url=url,sha256=digest,metadata_url=page_url)
        records[:] = [r for r in records if r['package'] != name]
        records.append(record)
        manifest.write_text(json.dumps(records,indent=2)+"\n",encoding="utf-8")
        print(f"Installed {name}: {digest}",flush=True)
        # Dependency section only, not optional/build/reverse dependencies.
        section = re.search(r'>Dependencies:</[^>]+>(.*?)(?:>Optional Dependencies:|>Build Dependencies:)',page,re.S)
        if section:
            dependencies = re.findall(r'href="(?:https://packages\.msys2\.org)?/packages/(' + re.escape(prefix) + r'[^"#?]+)"',section[1])
            for dependency in dict.fromkeys(dependencies):
                if args.core_only and name==prefix+'yosys' and dependency in (
                        prefix+'ghdl',prefix+'python'):
                    print('Not installed (Verilog core-only): '+dependency,flush=True)
                    continue
                install(dependency)

    install(prefix + args.package)
    # MinGW runtime dependencies are not consistently listed on executable pages.
    install(prefix + "gcc-libs")
    # Newer MSYS2 splits the C/C++ runtimes out of gcc-libs; without them vvp crashes at start.
    install(prefix + "libgcc")
    install(prefix + "libstdc++")
    install(prefix + "libwinpthread")
    if args.package=='yosys': install(prefix + 'libffi')
    print(root / env / "bin",flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Parse clang-scan-deps p1689 JSON output and generate Makefile dependency rules."""
import sys
import json

def main():
    moduledir = sys.argv[1] if len(sys.argv) > 1 else ""
    objectdir = sys.argv[2] if len(sys.argv) > 2 else ""
    
    data = json.load(sys.stdin)
    
    for r in data.get("rules", []):
        prov = r.get("provides", [])
        req = r.get("requires", [])
        if not prov:
            continue
        
        p = prov[0]
        src = p["source-path"]
        logical = p.get("logical-name", "")
        
        # Build PCM path
        pcm_name = logical.replace(":", "-") + ".pcm" if logical else ""
        pcm = f"{moduledir}/{pcm_name}" if pcm_name else ""
        
        # Collect module dependencies
        deps_parts = []
        for m in req:
            ln = m.get("logical-name", "")
            if ln and ln != "std":
                ln_clean = ln.replace(":", "-")
                deps_parts.append(f"{moduledir}/{ln_clean}.pcm")
        deps = " ".join(deps_parts)
        
        # Determine output file
        base = src.split("/")[-1]
        base_no_ext = base.rsplit(".", 1)[0]
        o = f"{objectdir}/{base_no_ext}.o"
        
        if src.endswith((".c++m", ".cppm")):
            # Module interface: produces both .pcm and .o
            print(f"{pcm} {o}: {src}")
            if deps:
                print(f"{pcm}: {deps}")
        elif src.endswith(".c++"):
            # Regular TU or implementation: produces .o, may depend on .pcm
            pcm_dep = f" {pcm}" if pcm and logical else ""
            print(f"{o}: {src}{pcm_dep}")
            if deps:
                print(f"{o}: {deps}")

if __name__ == "__main__":
    main()


#!/usr/bin/env python3
"""Parse clang-scan-deps p1689 JSON output and generate Makefile dependency rules."""
import sys
import json
import os

def main():
    moduledir = sys.argv[1] if len(sys.argv) > 1 else ""
    objectdir = sys.argv[2] if len(sys.argv) > 2 else ""
    source_file = sys.argv[3] if len(sys.argv) > 3 else ""
    
    data = json.load(sys.stdin)
    
    for r in data.get("rules", []):
        prov = r.get("provides", [])
        req = r.get("requires", [])
        
        # Get logical name from provides
        logical = ""
        if prov:
            p = prov[0]
            logical = p.get("logical-name", "")
        
        # Always use source_file argument if provided (it's the actual path we're scanning)
        # The source-path from JSON may be relative to where clang-scan-deps was run
        if source_file:
            src = source_file
        elif prov:
            p = prov[0]
            src = p.get("source-path", "")
        else:
            # Skip if we can't determine the source
            continue
        
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
        
        # Determine output file based on source path and extension
        # Handle different file types: .c++m, .c++, .impl.c++, .test.c++
        base = os.path.basename(src)
        
        # Extract directory relative to project root to handle fixer/ and tests/ correctly
        # For modules in tests/, the output goes to obj/tests/
        if "tests/" in src and src.endswith((".c++m", ".cppm")):
            # Test directory modules go to obj/tests/
            base_no_ext = base.rsplit(".", 1)[0]
            o = f"{objectdir}/tests/{base_no_ext}.o"
        else:
            # For other files, preserve the full extension pattern
            # .impl.c++ -> .impl.o, .test.c++ -> .test.o, .c++ -> .o
            if src.endswith(".impl.c++"):
                # Remove .impl.c++ and add .impl.o
                base_no_ext = base.rsplit(".impl.c++", 1)[0]
                o = f"{objectdir}/{base_no_ext}.impl.o"
            elif src.endswith(".test.c++"):
                # Remove .test.c++ and add .test.o
                base_no_ext = base.rsplit(".test.c++", 1)[0]
                o = f"{objectdir}/{base_no_ext}.test.o"
            elif src.endswith((".c++m", ".cppm")):
                base_no_ext = base.rsplit(".", 1)[0]
                o = f"{objectdir}/{base_no_ext}.o"
            elif src.endswith(".c++"):
                base_no_ext = base.rsplit(".", 1)[0]
                o = f"{objectdir}/{base_no_ext}.o"
            else:
                continue
        
        if src.endswith((".c++m", ".cppm")):
            # Module interface: produces both .pcm and .o
            print(f"{pcm} {o}: {src}")
            if deps:
                print(f"{pcm}: {deps}")
        elif src.endswith((".c++", ".impl.c++", ".test.c++")):
            # Regular TU, implementation, or test: produces .o, may depend on .pcm
            pcm_dep = f" {pcm}" if pcm and logical else ""
            print(f"{o}: {src}{pcm_dep}")
            if deps:
                print(f"{o}: {deps}")

if __name__ == "__main__":
    main()


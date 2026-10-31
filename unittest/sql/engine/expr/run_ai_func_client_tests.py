#!/usr/bin/env python3
"""Build real HTTP client regression tests using a configured seekdb build.

Requires the project-local dependency bundle and liboblib. All outputs stay in
.cwk-build/ai-client-unit. --source permits checking the original implementation
against the same regression tests without modifying the working tree.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[4]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build_release")
    parser.add_argument("--source", type=Path, help="alternate client source for a regression baseline")
    options = parser.parse_args()
    build = options.build_dir.resolve()
    out = root / ".cwk-build" / "ai-client-unit"
    out.mkdir(parents=True, exist_ok=True)
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(e for e in entries if "ob_sql_engine_expr" in e["file"])
    args = shlex.split(entry["command"])
    for flag in ("-o", "-c"):
        index = args.index(flag)
        del args[index:index + 2]
    args = [a for a in args if not a.startswith("-O")]
    args.append("-I" + str(root / "src/sql/engine/expr/ob_expr_ai"))
    sources = [str(options.source.resolve()) if options.source else
               "src/sql/engine/expr/ob_expr_ai/ob_ai_func_client.cpp",
               "unittest/sql/engine/expr/test_ob_ai_func_client.cpp"]
    objects = []
    for source in sources:
        obj = out / (Path(source).stem + ".o")
        objects.append(str(obj))
        subprocess.run(["nice", "-n", "10", *args, "-O0", "-c", str(root / source),
                        "-o", str(obj)], cwd=entry["directory"], check=True)
    deps = root / "deps/3rd/usr/local/oceanbase/deps/devel"
    dev = root / "deps/3rd/usr/local/oceanbase/devtools"
    executable = out / "test_ai_func_client"
    link = [args[0], f"--gcc-toolchain={dev}", f"-fuse-ld={dev}/bin/ld.lld",
            "-Wl,--gc-sections", *objects, "-o", str(executable), f"-L{deps}/lib",
            "-Wl,--start-group", str(deps / "lib/libgtest_main.a"),
            str(deps / "lib/libgtest.a"), str(build / "src/oblib/liboblib.a"),
            str(build / "src/oblib/lib/libob_malloc.a"),
            str(build / "third-party/jemalloc/lib/libjemalloc_pic.a"),
            f"-L{deps}/lib/vsag_lib", "-lvsag_static", "-ldiskann", "-lcpuinfo",
            "-lsimd", "-lopenblas", "-lgfortran_static", "-lquadmath_static",
            "-lgomp_static", "-lroaring", "-lantlr4-runtime", "-lantlr4-autogen",
            "-lfmt", "-lunwind", "-Wl,--end-group", "-lpthread", "-ldl", "-lrt",
            "-lz", "-lssl", "-lcrypto", "-lcurl", "-laio",
            f"-Wl,-rpath={dev}/lib64", f"-Wl,-rpath={deps}/lib"]
    subprocess.run(link, cwd=out, check=True)
    subprocess.run([str(executable)], cwd=out, check=True)


if __name__ == "__main__":
    main()

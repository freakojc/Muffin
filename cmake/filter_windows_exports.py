"""Export project APIs and resources, excluding incidental Qt/STL templates."""

import argparse
from pathlib import Path
import subprocess


def project_symbol(symbol):
    # Resource initializers are intentionally global Qt-generated functions.
    if symbol.startswith(("?qInitResources_", "?qCleanupResources_")):
        return True
    # RTTI for project types is needed by dynamic_cast across DLL boundaries.
    if symbol.startswith("??_R"):
        return "@muffin@@" in symbol and "?$" not in symbol
    # The first @@ terminates a non-template function's qualified name, before
    # its parameter/return types. A Qt/STL function accepting a Muffin type is
    # not part of our DLL API. Header-defined template instantiations belong to
    # their consumers; explicit Q_DECL_EXPORT directives remain authoritative.
    name = symbol.split("@@", 1)[0]
    return name.endswith("@muffin") and "?$" not in name


def filtered_definition(text):
    lines = []
    for line in text.splitlines():
        tokens = line.split()
        if tokens and project_symbol(tokens[0]):
            lines.append(line)
    if not lines:
        raise ValueError("No Muffin symbols found in generated DLL exports")
    return "EXPORTS\n" + "\n".join(lines) + "\n", len(lines)


def write_definition(output, definition):
    if output.exists() and output.read_text(encoding="utf-8") == definition:
        return
    output.write_text(definition, encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--objects", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    raw = output.with_suffix(".all.def")
    subprocess.run([args.cmake, "-E", "__create_def", str(raw), args.objects], check=True)
    original = raw.read_text(encoding="utf-8")
    definition, count = filtered_definition(original)
    # PRE_LINK also runs during incremental builds. Preserve the definition's
    # timestamp when the API is unchanged so consumers do not relink needlessly.
    write_definition(output, definition)
    print(f"Muffin DLL exports: {len(original.splitlines()) - 1} scanned, {count} project symbols")


if __name__ == "__main__":
    main()

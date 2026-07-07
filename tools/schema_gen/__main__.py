"""`python -m tools.schema_gen` entry point -> the generate CLI."""

from .generate import main

if __name__ == "__main__":
    raise SystemExit(main())

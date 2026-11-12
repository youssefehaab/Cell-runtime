"""Entry point: ``python -m cellctl`` and the ``cellctl`` console script."""

from cellctl.cli import app


def main() -> None:
    app()


if __name__ == "__main__":
    main()

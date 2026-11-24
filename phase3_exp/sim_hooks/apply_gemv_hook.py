"""Point executeGemv's data rows at phase3GemvRowBias.  Mode-register rows stay put."""
import pathlib
import sys


def main() -> None:
    path = pathlib.Path(sys.argv[1])
    text = path.read_text()
    if "phase3GemvRowBias" in text:
        return
    needle = '#include "tests/PIMKernel.h"\n'
    if needle not in text:
        raise SystemExit("PIMKernel include anchor moved")
    text = text.replace(needle, needle + '#include "ActivationObserver.h"\n', 1)
    old_row = "    unsigned row = 0;\n    unsigned col = (num_grfA_ * num_grfB_)"
    new_row = (
        "    unsigned row = phase3GemvRowBias > 0 ? static_cast<unsigned>(phase3GemvRowBias) : 0;\n"
        "    unsigned col = (num_grfA_ * num_grfB_)"
    )
    if old_row not in text:
        raise SystemExit("GEMV row anchor moved")
    text = text.replace(old_row, new_row, 1)
    old_store = 'addTransactionAll(true, 0, 1, 0, col, "GRFB_TO_BANK_"'
    new_store = 'addTransactionAll(true, 0, 1, phase3GemvRowBias > 0 ? phase3GemvRowBias : 0, col, "GRFB_TO_BANK_"'
    if old_store not in text:
        raise SystemExit("GRFB anchor moved")
    text = text.replace(old_store, new_store)
    path.write_text(text)


if __name__ == "__main__":
    main()

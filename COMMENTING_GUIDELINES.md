# Commenting Guidelines

This project uses comments to explain intent and system behavior, not obvious syntax.

## Rule of thumb
- Add comments where a future reader might ask "why is this done this way?"
- Skip comments that simply restate the code.

## Minimum expectation for new source files
- File header: what the file owns and what it intentionally does not own.
- Public APIs: brief contract comments (inputs, outputs, side effects).
- Non-trivial logic blocks: one concise comment before the block.
- Hardware/config constants: grouped with short purpose comments.

## Style
- Keep comments concise and actionable.
- Prefer plain language over abbreviations.
- Update comments when code behavior changes.

## ESP-IDF specifics
- Document Kconfig assumptions and fallback defaults.
- Note backend choices (GPIO vs LED strip, RMT vs SPI) where selected.
- For task loops, comment timing behavior and failure handling.

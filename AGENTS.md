# AGENTS.md

Coding agent instructions for the hipi project.

## Project Overview

`hipi` is a log highlighting utility that reads stdin and colorizes matching keywords while filtering out excluded patterns. It uses the Aho-Corasick string matching algorithm for O(n) multi-pattern matching. Single-file C++17, no external dependencies.

## Build & Install

```bash
make                    # Build
sudo make install       # Install to /usr/local/bin
sudo make uninstall     # Remove
make clean             # Clean artifacts
```

## Manual Testing

```bash
# Basic usage
./hipi --help
./hipi --version

# Built-in keywords (error, warn, info, debug, etc.)
echo "error: something went wrong" | ./hipi
echo "debug: trace message" | ./hipi

# Custom keywords with colors
cat app.log | ./hipi "timeout=yellow,failed=red"
dmesg | ./hipi "kernel=blue,usb=green"

# Config file exclusion (~/.config/hipi.conf)
echo -e "debug\ntrace" > ~/.config/hipi.conf
tail -f app.log | ./hipi
```

## Architecture

- **ACTree**: Aho-Corasick automaton with goto optimization (128-entry array per node)
- **Two tries**: `highlight_trie` (colorize matches) + `exclude_trie` (filter lines)
- **Single-pass**: Pre-computed lowercase buffer shared between exclude check and highlight

### Key Data Structures

```cpp
struct ACTree {
    struct Node {
        int next[128] = {};  // Direct-indexed transitions
        int fail = 0;         // Failure link
        int out = -1;         // Pattern index or -1
    };
    vector<Node> nodes;
    vector<Pattern> patterns;
};
```

## Code Style

### Naming
- Functions: `snake_case` - `add_pattern()`, `build()`
- Structs: `PascalCase` - `Node`, `Pattern`, `ACTree`
- Constants: `snake_case` - `RESET`, `RED`
- Global tries: `highlight_trie`, `exclude_trie`

### Formatting
- Indent: 4 spaces
- Line limit: 100 characters
- Opening braces: same line

### Headers
```cpp
#include <cctype>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

using namespace std;
```

### C++17 Features Used
- Structured bindings: `auto& [c, nxt]`
- Range-based for: `for (char c : pat)`
- Default initializers: `int fail = 0`

## Making Changes

1. Edit `hipi.cpp` for all changes
2. Build and test manually
3. Update `--help` output if CLI changes
4. Update READMEs if behavior changes

## Performance Notes

- **mark buffer**: Reused vector, resized per line (avoid allocation)
- **lower_buf**: Single lowercase conversion shared between exclude + highlight
- **goto optimization**: `next[128]` fully filled during build, O(1) lookup
- **reserve**: Output string reserves `size + size/2` to minimize reallocations

/*
 * hipi - Log Highlighter
 * 
 * Uses Aho-Corasick algorithm for O(n) multi-pattern matching.
 * Case-insensitive, supports exclusion rules via config file.
 */

#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <string_view>
#include <cctype>
#include <fstream>

using namespace std;

/* Version information */
#define HIPI_VERSION "1.0.0"

/* ==========================================================================
 * ANSI Color Definitions
 * 
 * Terminal escape sequences for text styling. Format: ESC[<code>m
 * - 0: reset all attributes
 * - 31-36: foreground colors (red to cyan)
 * - 1: bold/bright
 * ========================================================================== */
namespace Color {
    const string RESET   = "\033[0m";
    const string RED     = "\033[31m";
    const string GREEN   = "\033[32m";
    const string YELLOW  = "\033[33m";
    const string BLUE    = "\033[34m";
    const string MAGENTA = "\033[35m";
    const string CYAN    = "\033[36m";
    const string BOLD    = "\033[1m";
}

/* Color name to ANSI mapping for user-specified colors */
static const char* COLOR_NAMES[] = {"red", "green", "yellow", "blue", "magenta", "cyan"};
static const string* COLOR_VALS[] = {&Color::RED, &Color::GREEN, &Color::YELLOW, &Color::BLUE, &Color::MAGENTA, &Color::CYAN};
static constexpr int NUM_COLORS = 6;

/* Pattern with its highlight color */
struct Pattern {
    string text;   /* The keyword to match */
    string color;  /* ANSI escape sequence for highlighting */
};

/* ==========================================================================
 * Aho-Corasick Automaton
 * 
 * Aho-Corasick is a string matching algorithm that builds a finite automaton
 * from a set of patterns, enabling O(n + m) matching where n is text length
 * and m is total pattern length.
 * 
 * Key concepts:
 * 1. Trie structure stores all patterns
 * 2. Failure links enable efficient backtracking on mismatch
 * 3. Output links indicate pattern matches at each node
 * 
 * Time complexity:
 * - Build: O(m) where m is total pattern length
 * - Match: O(n) where n is input text length
 * ========================================================================== */
struct ACTree {
    struct Node {
        /*
         * next[128]: Transition table for ASCII characters.
         * Using fixed array instead of hash map for O(1) lookup.
         * Value 0 means no transition (except for root node).
         */
        int next[128] = {};
        
        /*
         * fail: Failure link - points to the longest proper suffix
         * that is also a prefix of some pattern. Used for backtracking
         * when current character has no direct transition.
         * 
         * Example: patterns {"he", "she"}
         *   Node for "she" has fail link to node for "he"
         *   When matching "she", we can also detect "he" via fail link
         */
        int fail = 0;
        
        /*
         * out: Index of pattern that ends at this node, or -1 if none.
         * Only stores one pattern index (the last added), since we only
         * need to know if ANY pattern matches here for highlighting.
         */
        int out = -1;
    };
    
    vector<Node> nodes;
    vector<Pattern> patterns;
    
    ACTree() { nodes.emplace_back(); }  /* Create root node */
    
    /*
     * add - Insert a pattern into the trie
     * 
     * Traverses the trie character by character, creating new nodes
     * as needed. Non-ASCII characters (>=128) are skipped to keep
     * the transition table simple.
     * 
     * After insertion, the final node's 'out' is set to the pattern index,
     * marking where this pattern ends.
     */
    void add(const string& pat, const string& color) {
        int node = 0;
        for (unsigned char c : pat) {
            if (c >= 128) continue;  /* Skip non-ASCII */
            if (!nodes[node].next[c]) {
                nodes[node].next[c] = nodes.size();
                nodes.emplace_back();
            }
            node = nodes[node].next[c];
        }
        nodes[node].out = patterns.size();
        patterns.push_back({pat, color});
    }
    
    /*
     * build - Construct failure links and fill goto table using BFS
     * 
     * Standard Aho-Corasick build with goto (jump) optimization:
     * 1. Root's direct children have fail = 0 (root)
     * 2. For each node v and each character c:
     *    - If v has a real child on c: compute fail link normally, enqueue child
     *    - If v has no child on c: fill next[c] = nodes[fail(v)].next[c]
     *      (the "goto" shortcut — already computed since BFS processes parent first)
     * 
     * Result: every next[c] is filled for every node, so matching is
     * a single array lookup per character — no while loop needed.
     * 
     * We also propagate output: if fail node has a pattern output,
     * current node should also report that pattern (since it's a suffix).
     */
    void build() {
        queue<int> q;
        
        /* Initialize root: missing transitions loop back to root (0) */
        for (int c = 0; c < 128; ++c) {
            if (nodes[0].next[c]) {
                q.push(nodes[0].next[c]);
            }
            /* nodes[0].next[c] already 0 for missing — correct goto for root */
        }
        
        /* BFS to compute fail links and fill complete goto table */
        while (!q.empty()) {
            int v = q.front(); q.pop();
            
            /* Propagate output from fail node */
            if (nodes[nodes[v].fail].out >= 0 && nodes[v].out < 0) {
                nodes[v].out = nodes[nodes[v].fail].out;
            }
            
            for (int c = 0; c < 128; ++c) {
                int nxt = nodes[v].next[c];
                if (nxt) {
                    /* Real child: compute its fail link using parent's goto table
                     * (already filled since parent was processed before child in BFS) */
                    nodes[nxt].fail = nodes[nodes[v].fail].next[c];
                    q.push(nxt);
                } else {
                    /* No real child: fill goto shortcut — jump to where fail would go */
                    nodes[v].next[c] = nodes[nodes[v].fail].next[c];
                }
            }
        }
    }
    
    /*
     * match_any - Check if any pattern matches in the text
     * 
     * Used for exclusion checking. Returns true on first match.
     * Characters are lowercased for case-insensitive matching.
     * 
     * The matching loop:
     * 1. Convert char to lowercase
     * 2. While no transition and not at root, follow fail link
     * 3. Take transition (or stay at root if none)
     * 4. Check if current node has output (pattern match)
     */
    /* s must already be lowercased by the caller */
    bool match_any(const char* s, size_t len) const {
        int state = 0;
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 128) { state = 0; continue; }
            
            /* Goto table is fully filled by build(): single O(1) lookup */
            state = nodes[state].next[c];
            
            /* Check for pattern match at current position */
            if (nodes[state].out >= 0) return true;
        }
        return false;
    }
};

/* Two automata: one for highlighting, one for exclusion */
ACTree highlight_trie;
ACTree exclude_trie;

/* Convert string to lowercase in-place */
void to_lower_inplace(string& s) {
    for (char& c : s) {
        c = tolower(static_cast<unsigned char>(c));
    }
}

/* Get config file path: ~/.config/hipi.conf */
string get_config_path() {
    const char* home = getenv("HOME");
    return home ? string(home) + "/.config/hipi.conf" : "";
}

/*
 * load_config - Load exclusion keywords from config file
 * 
 * Format: one keyword per line, lines starting with # are comments.
 * Keywords are added to exclude_trie for O(n) filtering.
 */
void load_config() {
    string path = get_config_path();
    if (path.empty()) return;
    
    ifstream file(path);
    if (!file) return;
    
    string line;
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        to_lower_inplace(line);
        exclude_trie.add(line, "");
    }
}

/* Look up color by name, default to cyan */
const string& get_color(const string& name) {
    for (int i = 0; i < NUM_COLORS; ++i) {
        if (name == COLOR_NAMES[i]) return *COLOR_VALS[i];
    }
    return Color::CYAN;
}

/*
 * parse_keyword - Parse "keyword=color" token
 * 
 * Extracts keyword and optional color from user input.
 * If no color specified, uses cyan.
 */
void parse_keyword(const string& token) {
    size_t eq = token.find('=');
    string key = token.substr(0, eq);
    to_lower_inplace(key);
    
    string color = Color::CYAN + Color::BOLD;
    if (eq != string::npos) {
        color = get_color(token.substr(eq + 1)) + Color::BOLD;
    }
    highlight_trie.add(key, color);
}

/*
 * highlight_line - Apply syntax highlighting to a line
 * 
 * Two-pass algorithm:
 * 1. Mark all matched positions using AC automaton on pre-lowercased buffer
 * 2. Reconstruct string with ANSI color codes, copying non-highlighted spans
 *    in bulk (append range) rather than character-by-character
 * 
 * Buffers (mark, out) are passed in from the caller and reused across lines
 * to avoid per-line heap allocation.
 *
 * lower_buf: caller-provided lowercase copy of line (shared with exclude check)
 * mark:      caller-provided scratch buffer; resized+filled here each call
 */
void highlight_line(const string& line, const string& lower_buf,
                    vector<int>& mark, string& out) {
    const auto& nodes = highlight_trie.nodes;
    const auto& patterns = highlight_trie.patterns;
    const size_t n = line.size();
    
    /* Reuse mark buffer: resize if needed, then reset to -1 */
    mark.assign(n, -1);
    
    /* Pass 1: Find all matches on the pre-lowercased buffer */
    int state = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(lower_buf[i]);
        if (c >= 128) { state = 0; continue; }
        
        /* Goto table is fully filled by build(): single O(1) lookup */
        state = nodes[state].next[c];
        
        /* Mark all positions of matched pattern */
        if (nodes[state].out >= 0) {
            int idx = nodes[state].out;
            int plen = patterns[idx].text.size();
            for (int k = 0; k < plen && i + 1 >= static_cast<size_t>(plen - k); ++k) {
                size_t pos = i - plen + 1 + k;
                if (pos < n) mark[pos] = idx;
            }
        }
    }
    
    /* Pass 2: Reconstruct with colors, bulk-copying non-highlighted spans */
    out.clear();
    out.reserve(n + n / 2);  /* Conservative reserve; realloc rare after warmup */
    
    for (size_t i = 0; i < n;) {
        if (mark[i] >= 0) {
            /* Highlight matched region */
            int idx = mark[i];
            size_t plen = patterns[idx].text.size();
            out += patterns[idx].color;
            out.append(line, i, plen);
            out += Color::RESET;
            i += plen;
        } else {
            /* Find end of consecutive non-highlighted span and copy in bulk */
            size_t j = i + 1;
            while (j < n && mark[j] < 0) ++j;
            out.append(line, i, j - i);
            i = j;
        }
    }
}

/* Print help message */
void print_usage(const char* prog) {
    cerr << "Usage: " << prog << " [OPTIONS] [KEYWORDS]\n"
         << "Highlight keywords in log output with colors.\n\n"
         << "Options:\n"
         << "  -h, --help       Show this help message\n"
         << "  -v, --version    Show version information\n\n"
         << "Keywords format: word1=color,word2=color,...\n"
         << "  Colors: red, green, yellow, blue, magenta, cyan\n"
         << "  Default color: cyan\n\n"
<< "Built-in keywords:\n"
          << "  error, excep, cause, critical, panic, crash, abort,\n"
          << "  denied, refused, rejected, invalid, illegal -> red+bold\n"
          << "  warn, warning, deprecated, timeout, retry, missing,\n"
          << "  unknown, unexpected, not -> yellow+bold\n"
          << "  info, success, complete, started, stopped, ready, ok -> green\n"
          << "  debug, trace, verbose -> blue\n"
          << "  fatal, fail, emergency, alert -> magenta+bold\n\n"
         << "Config file: ~/.config/hipi.conf\n"
         << "  One keyword per line to exclude lines containing it.\n"
         << "  Lines starting with # are comments.\n\n"
         << "Examples:\n"
         << "  cat log.txt | " << prog << "\n"
         << "  dmesg | " << prog << " kernel=blue,usb=green\n"
         << "  tail -f /var/log/syslog | " << prog << "\n";
}

int main(int argc, char* argv[]) {
    /* Disable sync for faster I/O */
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    
    if (argc > 1 && (string(argv[1]) == "-h" || string(argv[1]) == "--help")) {
        print_usage(argv[0]);
        return 0;
    }
    
    if (argc > 1 && (string(argv[1]) == "-v" || string(argv[1]) == "--version")) {
        cout << "hipi " << HIPI_VERSION << '\n';
        return 0;
    }
    
    /* Add built-in highlight patterns */
    highlight_trie.add("excep", Color::RED + Color::BOLD);
    highlight_trie.add("cause", Color::RED + Color::BOLD);
    highlight_trie.add("error", Color::RED + Color::BOLD);
    highlight_trie.add("critical", Color::RED + Color::BOLD);
    highlight_trie.add("panic", Color::RED + Color::BOLD);
    highlight_trie.add("crash", Color::RED + Color::BOLD);
    highlight_trie.add("abort", Color::RED + Color::BOLD);
    highlight_trie.add("denied", Color::RED + Color::BOLD);
    highlight_trie.add("deny", Color::RED + Color::BOLD);
    highlight_trie.add("refused", Color::RED + Color::BOLD);
    highlight_trie.add("refuse", Color::RED + Color::BOLD);
    highlight_trie.add("rejected", Color::RED + Color::BOLD);
    highlight_trie.add("reject", Color::RED + Color::BOLD);
    highlight_trie.add("invalid", Color::RED + Color::BOLD);
    highlight_trie.add("illegal", Color::RED + Color::BOLD);
    highlight_trie.add("not", Color::YELLOW + Color::BOLD);
    highlight_trie.add("warn", Color::YELLOW + Color::BOLD);
    highlight_trie.add("warning", Color::YELLOW + Color::BOLD);
    highlight_trie.add("deprecated", Color::YELLOW + Color::BOLD);
    highlight_trie.add("deprecate", Color::YELLOW + Color::BOLD);
    highlight_trie.add("timeout", Color::YELLOW + Color::BOLD);
    highlight_trie.add("timed out", Color::YELLOW + Color::BOLD);
    highlight_trie.add("retry", Color::YELLOW + Color::BOLD);
    highlight_trie.add("retries", Color::YELLOW + Color::BOLD);
    highlight_trie.add("missing", Color::YELLOW + Color::BOLD);
    highlight_trie.add("unknown", Color::YELLOW + Color::BOLD);
    highlight_trie.add("unexpected", Color::YELLOW + Color::BOLD);
    highlight_trie.add("info", Color::GREEN);
    highlight_trie.add("success", Color::GREEN);
    highlight_trie.add("successful", Color::GREEN);
    highlight_trie.add("complete", Color::GREEN);
    highlight_trie.add("completed", Color::GREEN);
    highlight_trie.add("started", Color::GREEN);
    highlight_trie.add("starting", Color::GREEN);
    highlight_trie.add("stopped", Color::GREEN);
    highlight_trie.add("stopping", Color::GREEN);
    highlight_trie.add("ready", Color::GREEN);
    highlight_trie.add("ok", Color::GREEN);
    highlight_trie.add("debug", Color::BLUE);
    highlight_trie.add("trace", Color::BLUE);
    highlight_trie.add("verbose", Color::BLUE);
    highlight_trie.add("fatal", Color::MAGENTA + Color::BOLD);
    highlight_trie.add("fail", Color::MAGENTA + Color::BOLD);
    highlight_trie.add("emergency", Color::MAGENTA + Color::BOLD);
    highlight_trie.add("alert", Color::MAGENTA + Color::BOLD);
    
    /* Parse user-specified keywords */
    if (argc > 1) {
        string arg = argv[1];
        size_t start = 0, pos;
        while ((pos = arg.find(',', start)) != string::npos) {
            parse_keyword(arg.substr(start, pos - start));
            start = pos + 1;
        }
        if (start < arg.size()) {
            parse_keyword(arg.substr(start));
        }
    }
    
    /* Load exclusion rules from config */
    load_config();
    
    /* Build failure links for both automata */
    highlight_trie.build();
    exclude_trie.build();
    
    /* Main loop: read, filter, highlight, output
     * 
     * lower_buf: lowercase copy of each line, computed once and shared
     *   between exclude check and highlight pass (avoids double tolower).
     * mark: scratch buffer for highlight positions, reused across lines
     *   (avoids per-line heap allocation inside highlight_line).
     * out: output buffer, also reused across lines.
     */
    string line, lower_buf, out;
    vector<int> mark;
    while (getline(cin, line)) {
        /* Compute lowercase copy once; shared by both exclude and highlight */
        lower_buf = line;
        for (char& ch : lower_buf) {
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        }
        
        if (!exclude_trie.match_any(lower_buf.c_str(), lower_buf.size())) {
            highlight_line(line, lower_buf, mark, out);
            cout << out << '\n';
        }
    }
    
    return 0;
}
#!/bin/sh
# check-publish.sh [<since-ref>]: the gate before anything is pushed. Exit 0 = clean, 1 = fix first.
#
# Rule 1, no AI attribution: no commit in <since-ref>..HEAD may have an AI author or committer, a trailer naming an AI tool or its vendor
#   (Co-Authored-By, Signed-off-by, Assisted-by), or a "Generated with" line. The person who sends a change is its author.
# Rule 2, no private data: no changed file may contain a LAN address, a local home or user path, an adb-over-network address or a
#   personal e-mail address.
# Rule 3, source only: no changed file may be binary, and none may be a game file, a key or a signing key.
# Rule 4, text: changed text files follow the text rules (scripts/check_text.py).
#
# <since-ref> defaults to the merge base with origin/main.
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
since="${1:-$(git merge-base HEAD origin/main 2>/dev/null)}"
[ -n "$since" ] || { echo "check-publish: no <since-ref>, and no origin/main to take the merge base from"; exit 1; }
fail=0
tmp="${TMPDIR:-/tmp}/check-publish.$$"
trap 'rm -f "$tmp"' EXIT
AI='claude|anthropic|codex|openai|chatgpt|gpt-[0-9]|copilot|gemini|cursor|devin|aider|windsurf|noreply@anthropic'

# Rule 1: authors, committers, trailers
bad=$(git log "$since"..HEAD --format='%h %an <%ae> | %cn <%ce>' | grep -iE "$AI")
[ -n "$bad" ] && { echo "AI author or committer:"; echo "$bad"; fail=1; }
bad=$(git log "$since"..HEAD --format='@@%h%n%B' | awk '/^@@/{h=substr($0,3); next} {print h": "$0}' \
  | grep -iE "^[0-9a-f]+: *(co-authored-by|signed-off-by|generated[- ]with|assisted[- ]by|with[- ]the[- ]help[- ]of).*($AI)|🤖")
[ -n "$bad" ] && { echo "AI trailer in a commit message:"; echo "$bad"; fail=1; }

# The changed files (their current contents), without the checks themselves
git diff --name-only --diff-filter=d "$since" HEAD -- . ':!scripts/check-publish.sh' ':!scripts/check_text.py' > "$tmp"

# Rule 2: private data
PRIV='192\.168\.[0-9]+\.[0-9]+|(^|[^0-9.])10\.[0-9]+\.[0-9]+\.[0-9]+|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]+\.[0-9]+|/home/[a-z_][a-z0-9_-]*/|/Users/[A-Za-z]|[A-Za-z]:\\Users\\|:5555\b|\.ts\.net|@gmail\.com|@googlemail\.|@yahoo\.|@outlook\.|@hotmail\.|@icloud\.|@gmx\.|@web\.de'
hits=$(tr '\n' '\0' < "$tmp" | xargs -0 grep -nIE "$PRIV" 2>/dev/null | grep -vE '127\.0\.0\.1|0\.0\.0\.0')
[ -n "$hits" ] && { echo "private data in changed files (use a placeholder such as <work dir> or <device ip>):"; echo "$hits" | cut -c1-200; fail=1; }

# Rule 3: binaries, game files, keys
bin=$(git diff --numstat --diff-filter=d "$since" HEAD | awk -F'\t' '$1 == "-" && $2 == "-" {print $3}')
[ -n "$bin" ] && { echo "binary files (source only, see CONTRIBUTING.md):"; echo "$bin"; fail=1; }
game=$(grep -iE '\.(nsp|nsz|xci|xcz|nca|ncz|nso|nro|nup|keys|key|pem|p12|minisign)$|(^|/)(prod|title|dev)\.keys' "$tmp")
[ -n "$game" ] && { echo "game files or keys:"; echo "$game"; fail=1; }

# Rule 4: text rules
if command -v python3 >/dev/null 2>&1; then py=python3; elif command -v python >/dev/null 2>&1; then py=python; else py=; fi
if [ -n "$py" ]; then
  tr '\n' '\0' < "$tmp" | xargs -0 "$py" scripts/check_text.py || fail=1
else
  echo "check-publish: no python to run scripts/check_text.py"; fail=1
fi

[ $fail = 0 ] && echo "check-publish: clean ($since..HEAD: no AI attribution, no private data, no binaries, text rules kept)"
exit $fail

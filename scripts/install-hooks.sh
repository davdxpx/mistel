#!/bin/sh
# install-hooks.sh: runs scripts/check-publish.sh before every push from this clone.
set -eu
cd "$(git rev-parse --show-toplevel)"
hooks="$(git rev-parse --git-common-dir)/hooks"
mkdir -p "$hooks"
cat > "$hooks/pre-push" <<'EOF'
#!/bin/sh
exec sh "$(git rev-parse --show-toplevel)/scripts/check-publish.sh"
EOF
chmod +x "$hooks/pre-push"
echo "install-hooks: pre-push runs scripts/check-publish.sh"

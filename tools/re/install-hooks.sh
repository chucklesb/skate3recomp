#!/bin/sh
# Installs the repo's git hooks (docs/workflow.md, step 8):
#   pre-commit: tools/re/check_docs.py --staged (code and docs agree).
# Run once per clone, from anywhere in the repo.
set -e
root=$(git rev-parse --show-toplevel)
hooks=$(git rev-parse --git-path hooks)
mkdir -p "$hooks"
cat > "$hooks/pre-commit" <<'HOOK'
#!/bin/sh
# Installed by tools/re/install-hooks.sh: code and docs must agree.
root=$(git rev-parse --show-toplevel)
exec python3 -I "$root/tools/re/check_docs.py" --staged
HOOK
chmod +x "$hooks/pre-commit"
echo "installed $hooks/pre-commit (tools/re/check_docs.py --staged)"

#!/usr/bin/env bash
# #290 — built-in metot bilgisi tek kayıttan okunur (src/data/ +
# semantic/thread_intrinsics.hpp). Hata sınıfı: tip denetleyici ya da LSP'nin
# kayıttan ayrı elle tutulan bir liste/kural kullanması (bayat ipucu listesi,
# iki yerde kopyalanmış alıcı kuralı).
set -eu

binary=$1

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

# expect_check_error <dosya> <mesaj parçası> <ipucu parçası>
expect_check_error() {
    set +e
    "$binary" check "$1" >"$dir/out" 2>&1
    local actual=$?
    set -e
    if [ "$actual" -ne 65 ]; then
        echo "FAIL: '$1' beklenen exit 65, gercek $actual" >&2
        cat "$dir/out" >&2
        exit 1
    fi
    grep -qF -- "$2" "$dir/out" || { echo "FAIL: mesaj '$2' yok: $(cat "$dir/out")" >&2; exit 1; }
    grep -qF -- "$3" "$dir/out" || { echo "FAIL: ipucu '$3' yok: $(cat "$dir/out")" >&2; exit 1; }
}

# N1: alıcı kısıtı kayıtta — toString yalnız byte[] (params[0] = byte[]).
cat >"$dir/tostring.sqt" <<'EOF'
int main() {
    int[] a = [1, 2];
    print(a.toString());
    return 0;
}
EOF
expect_check_error "$dir/tostring.sqt" "'toString' requires a byte[] receiver, got 'int[]'" \
    "methods of 'int[]': length, push"

# N2: ipucu listesi kayıttan türer — string metotlarının SONUNCUSU (toBuffer)
# elle yazılmış eski listede yoktu.
cat >"$dir/strpush.sqt" <<'EOF'
int main() {
    string s = "a";
    s.push(1);
    return 0;
}
EOF
expect_check_error "$dir/strpush.sqt" "'push' is not a built-in method for type 'string'" "toBuffer"

# N3: Pool/List/Thread metotları tek tablodan.
cat >"$dir/pool.sqt" <<'EOF'
shared Pool q = Pool(int);

int main() {
    q.shift();
    return 0;
}
EOF
expect_check_error "$dir/pool.sqt" "'shift' is not a method of Pool" "Pool methods: push, pop, setMax, length"

# P1: byte[] alıcıda toString çalışır.
cat >"$dir/ok.sqt" <<'EOF'
int main() {
    byte[] b = "hey".toBuffer();
    print(b.toString());
    return 0;
}
EOF
out=$("$binary" run "$dir/ok.sqt")
[ "$out" = "hey" ] || { echo "FAIL: byte[].toString beklenen 'hey', gercek '$out'" >&2; exit 1; }

echo "builtin_method_registry: TUM TESTLER GECTI"

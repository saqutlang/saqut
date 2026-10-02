#!/bin/bash
# Benchmark runner: her testi optimizasyonlu (varsayilan) ve
# --dont-optimize ile calistirir
# Kullanim: bash tests/bench/perf_runner.sh

BUILD="build-rel"
SAQUT="./$BUILD/saqut"
RESULTS="tests/bench/results.txt"

echo "saQut Performance Benchmarks" > "$RESULTS"
echo "===============================" >> "$RESULTS"
echo "Date: $(date)" >> "$RESULTS"
echo "Binary: $(file $SAQUT | grep -o 'x86-64\|aarch64')" >> "$RESULTS"
echo "" >> "$RESULTS"

run_test() {
    local name="$1"
    local file="$2"
    local label="$3"
    
    echo "  $name..."
    
    # --no-opt (no optimization)
    echo "--- $label (no-opt) ---" >> "$RESULTS"
    TIMEFORMAT='%3R real  %3U user  %3S sys'
    { time $SAQUT run "$file" --dont-optimize 2>/dev/null; } 2>&1 >> "$RESULTS"
    
    # optimizasyonlu (varsayilan)
    echo "--- $label (optimized) ---" >> "$RESULTS"
    TIMEFORMAT='%3R real  %3U user  %3S sys'
    { time $SAQUT run "$file" 2>/dev/null; } 2>&1 >> "$RESULTS"
    
    echo "" >> "$RESULTS"
}

# Test 1: DCE benchmark - code with dead code
echo "Test 1: DCE"
run_test "DCE" "tests/bench/01_dce_bench.sqt" "DCE"

# Test 2: Constant folding benchmark
echo "Test 2: ConstFold"
run_test "ConstFold" "tests/bench/02_constfold_bench.sqt" "ConstFold"

# Test 3: String concat vs push benchmark
echo "Test 3: String"
run_test "String" "tests/bench/03_string_bench.sqt" "String"

# Test 4: Loop invariant benchmark
echo "Test 4: LoopInvariant"
run_test "LoopInvariant" "tests/bench/04_loop_invariant.sqt" "LoopInvariant"

# Test 5: Well vs poorly written combined
echo "Test 5: Combined"
run_test "Combined" "tests/bench/05_combined_bench.sqt" "Combined"

# Test 6: Matrix multiply (computation heavy)
echo "Test 6: Matrix"
run_test "Matrix" "tests/bench/06_matrix_bench.sqt" "Matrix"

# Test 7: Crypto (GC + array pressure)
echo "Test 7: Crypto"
echo "--- Crypto (no-opt) ---" >> "$RESULTS"
TIMEFORMAT='%3R real  %3U user  %3S sys'
{ time $SAQUT run "tests/general/crypto/crypto_stress_64kb.sqt" 2>/dev/null; } 2>&1 >> "$RESULTS"
echo "--- Crypto (optimized) ---" >> "$RESULTS"
TIMEFORMAT='%3R real  %3U user  %3S sys'
{ time $SAQUT run "tests/general/crypto/crypto_stress_64kb.sqt" 2>/dev/null; } 2>&1 >> "$RESULTS"

echo "" >> "$RESULTS"
echo "Done: $(date)" >> "$RESULTS"

cat "$RESULTS"

#!/usr/bin/env bash
# Test procedure for LED lighting API endpoints
#
# Usage: ./tools/test_lights_api.sh [options] [base_url]
#
# Options:
#   --fast           No pauses (CI/automated mode)
#   --timed [SECS]   Pause SECS seconds between visual tests (default: 2)
#   --interactive    Wait for Enter between visual tests (default)
#   -h, --help       Show this help
#
# Examples:
#   ./tools/test_lights_api.sh --fast http://192.168.50.148
#   ./tools/test_lights_api.sh --timed 3 http://192.168.50.148
#   ./tools/test_lights_api.sh --interactive http://192.168.50.148

set -uo pipefail

# Defaults
MODE="interactive"
TIMED_SECS=2
BASE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fast)        MODE="fast"; shift ;;
        --timed)
            MODE="timed"
            shift
            if [[ $# -gt 0 && "$1" =~ ^[0-9]+\.?[0-9]*$ ]]; then
                TIMED_SECS="$1"; shift
            fi
            ;;
        --interactive) MODE="interactive"; shift ;;
        -h|--help)
            sed -n '2,/^$/{ s/^# \?//; p }' "$0"
            exit 0
            ;;
        *)             BASE="$1"; shift ;;
    esac
done

BASE="${BASE:-http://vanfan.local}"
API="$BASE/api/v1"
PASS=0
FAIL=0

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
DIM='\033[0;90m'
NC='\033[0m'

check() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        printf "${GREEN}PASS${NC} %s\n" "$desc"
        ((PASS++))
    else
        printf "${RED}FAIL${NC} %s\n  expected: %s\n  got:      %s\n" "$desc" "$expected" "$actual"
        ((FAIL++))
    fi
}

check_status() {
    local desc="$1" expected_code="$2" actual_code="$3" body="$4"
    if [ "$actual_code" = "$expected_code" ]; then
        printf "${GREEN}PASS${NC} %s (HTTP %s)\n" "$desc" "$actual_code"
        ((PASS++))
    else
        printf "${RED}FAIL${NC} %s -- expected HTTP %s, got %s\n  body: %s\n" "$desc" "$expected_code" "$actual_code" "$body"
        ((FAIL++))
    fi
}

post() {
    local url="$1" data="$2"
    curl -s -X POST -H 'Content-Type: application/json' -d "$data" "$url"
}

post_with_code() {
    local url="$1" data="$2"
    curl -s -o /tmp/test_resp.json -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d "$data" "$url"
}

# Visual pause -- shows expected hardware state, pauses based on mode
visual() {
    local desc="$1"
    case "$MODE" in
        fast)
            ;;
        timed)
            printf "${YELLOW}  >> HW: %s${NC} ${DIM}(${TIMED_SECS}s)${NC}\n" "$desc"
            sleep "$TIMED_SECS"
            ;;
        interactive)
            printf "${YELLOW}  >> HW: %s${NC}\n" "$desc"
            printf "${YELLOW}     Press Enter to continue...${NC}"
            read -r
            ;;
    esac
}

echo ""
printf "${CYAN}===========================================${NC}\n"
printf "${CYAN}  LED Lighting API Test Procedure${NC}\n"
printf "${CYAN}  Target: %s${NC}\n" "$BASE"
printf "${CYAN}  Mode:   %s${NC}\n" "$MODE"
printf "${CYAN}===========================================${NC}\n"
echo ""

# --- Test 0: Ensure device is reachable ---
printf "${CYAN}--- Connectivity ---${NC}\n"
RESP=$(curl -s --connect-timeout 5 "$API/info")
check "Device reachable" '"version"' "$RESP"
echo ""

# --- Test 1: GET /lights -- initial state (all off) ---
printf "${CYAN}--- 1. GET /api/v1/lights (initial state) ---${NC}\n"
post "$API/lights/off" "" >/dev/null 2>&1
sleep 0.3
RESP=$(curl -s "$API/lights")
check "Returns zones array" '"zones"' "$RESP"
check "Zone 1 off" '"on":false' "$RESP"
visual "All 3 LEDs should be OFF"
echo ""

# --- Test 2: POST /lights/zone -- set single zone ---
printf "${CYAN}--- 2. POST /api/v1/lights/zone (zone 1 on at 75%%) ---${NC}\n"
RESP=$(post "$API/lights/zone" '{"zone":1,"on":true,"brightness":75}')
check "Zone 1 on" '"on":true' "$RESP"
check "Zone 1 brightness 75" '"brightness":75' "$RESP"
check "Zone 2 still off" '"on":false' "$RESP"
visual "Zone 1 ON at 75% -- zones 2 and 3 OFF"
echo ""

# --- Test 3: POST /lights/zone -- brightness only (on implied) ---
printf "${CYAN}--- 3. POST /lights/zone (zone 2 at 30%%, on implied) ---${NC}\n"
RESP=$(post "$API/lights/zone" '{"zone":2,"brightness":30}')
check "Zone 2 turned on" '"on":true,"brightness":30' "$RESP"
visual "Zone 1 at 75% (bright), zone 2 at 30% (dim), zone 3 OFF"
echo ""

# --- Test 4: POST /lights/zone -- turn off single zone ---
printf "${CYAN}--- 4. POST /lights/zone (turn off zone 1) ---${NC}\n"
RESP=$(post "$API/lights/zone" '{"zone":1,"on":false,"brightness":0}')
check "Zone 1 off" '"on":false' "$RESP"
visual "Zone 1 OFF, zone 2 at 30% (dim), zone 3 OFF"
echo ""

# --- Test 5: POST /lights/zones -- batch update ---
printf "${CYAN}--- 5. POST /api/v1/lights/zones (batch: z1=100%%, z3=40%%) ---${NC}\n"
RESP=$(post "$API/lights/zones" '{"zones":[{"zone":1,"on":true,"brightness":100},{"zone":3,"on":true,"brightness":40}]}')
check "Zone 1 at 100" '"on":true,"brightness":100' "$RESP"
check "Zone 3 at 40" '"on":true,"brightness":40' "$RESP"
visual "Zone 1 at 100% (full), zone 2 at 30%, zone 3 at 40%"
echo ""

# --- Test 6: POST /lights/all -- set all uniformly ---
printf "${CYAN}--- 6. POST /api/v1/lights/all (all at 60%%) ---${NC}\n"
RESP=$(post "$API/lights/all" '{"on":true,"brightness":60}')
check "All zones on at 60" '"on":true,"brightness":60' "$RESP"
COUNT=$(echo "$RESP" | grep -o '"brightness":60' | wc -l | tr -d ' ')
check "All 3 zones at brightness 60" "3" "$COUNT"
visual "All 3 LEDs ON at equal 60% brightness"
echo ""

# --- Test 7: POST /lights/off -- all off ---
printf "${CYAN}--- 7. POST /api/v1/lights/off ---${NC}\n"
RESP=$(post "$API/lights/off" '')
COUNT=$(echo "$RESP" | grep -o '"on":false' | wc -l | tr -d ' ')
check "All 3 zones off" "3" "$COUNT"
visual "All 3 LEDs should be OFF"
echo ""

# --- Test 8: GET /lights -- verify state persisted ---
printf "${CYAN}--- 8. GET /api/v1/lights (verify after off) ---${NC}\n"
RESP=$(curl -s "$API/lights")
COUNT=$(echo "$RESP" | grep -o '"on":false' | wc -l | tr -d ' ')
check "GET confirms all off" "3" "$COUNT"
echo ""

# --- Test 9: Validation -- zone out of range ---
printf "${CYAN}--- 9. Validation: zone out of range ---${NC}\n"
CODE=$(post_with_code "$API/lights/zone" '{"zone":0,"brightness":50}')
BODY=$(cat /tmp/test_resp.json)
check_status "Zone 0 -> 422" "422" "$CODE" "$BODY"

CODE=$(post_with_code "$API/lights/zone" '{"zone":4,"brightness":50}')
BODY=$(cat /tmp/test_resp.json)
check_status "Zone 4 -> 422" "422" "$CODE" "$BODY"
echo ""

# --- Test 10: Validation -- brightness out of range ---
printf "${CYAN}--- 10. Validation: brightness out of range ---${NC}\n"
CODE=$(post_with_code "$API/lights/zone" '{"zone":1,"brightness":101}')
BODY=$(cat /tmp/test_resp.json)
check_status "Brightness 101 -> 422" "422" "$CODE" "$BODY"

CODE=$(post_with_code "$API/lights/zone" '{"zone":1,"brightness":-1}')
BODY=$(cat /tmp/test_resp.json)
check_status "Brightness -1 -> 422" "422" "$CODE" "$BODY"
echo ""

# --- Test 11: Validation -- missing zone field ---
printf "${CYAN}--- 11. Validation: missing zone ---${NC}\n"
CODE=$(post_with_code "$API/lights/zone" '{"brightness":50}')
BODY=$(cat /tmp/test_resp.json)
check_status "Missing zone -> 400" "400" "$CODE" "$BODY"
echo ""

# --- Test 12: Validation -- bad JSON ---
printf "${CYAN}--- 12. Validation: bad JSON ---${NC}\n"
CODE=$(post_with_code "$API/lights/zone" 'not json')
BODY=$(cat /tmp/test_resp.json)
check_status "Bad JSON -> 400" "400" "$CODE" "$BODY"
echo ""

# --- Test 13: Validation -- /zones with bad entry rejects whole request ---
printf "${CYAN}--- 13. Validation: /zones rejects on any bad entry ---${NC}\n"
post "$API/lights/zone" '{"zone":1,"on":true,"brightness":80}' >/dev/null
CODE=$(post_with_code "$API/lights/zones" '{"zones":[{"zone":1,"brightness":50},{"zone":5,"brightness":20}]}')
BODY=$(cat /tmp/test_resp.json)
check_status "Bad entry in batch -> 422" "422" "$CODE" "$BODY"
RESP=$(curl -s "$API/lights")
check "Zone 1 unchanged after rejected batch" '"brightness":80' "$RESP"
echo ""

# --- Test 14: SSE -- light events stream ---
printf "${CYAN}--- 14. SSE: light events ---${NC}\n"
curl -s -N --max-time 4 "$API/events" > /tmp/sse_output.txt 2>&1 &
SSE_PID=$!
sleep 0.5
post "$API/lights/zone" '{"zone":1,"on":true,"brightness":55}' >/dev/null
sleep 1
post "$API/lights/off" '' >/dev/null
sleep 2.5
wait $SSE_PID 2>/dev/null || true
SSE=$(cat /tmp/sse_output.txt)
check "SSE contains 'event: lights'" "event: lights" "$SSE"
check "SSE contains brightness data" '"brightness":55' "$SSE"
check "SSE contains source" '"source":"api"' "$SSE"
echo ""

# --- Cleanup: all lights off ---
post "$API/lights/off" '' >/dev/null

# --- Summary ---
echo ""
printf "${CYAN}===========================================${NC}\n"
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    printf "${GREEN}  ALL %d TESTS PASSED${NC}\n" "$TOTAL"
else
    printf "${RED}  %d/%d FAILED${NC}\n" "$FAIL" "$TOTAL"
fi
printf "${CYAN}===========================================${NC}\n"
echo ""

rm -f /tmp/test_resp.json /tmp/sse_output.txt
exit "$FAIL"

#!/bin/bash
# 테스트 묶음을 한 번에 돌리고 PASS/FAIL만 모아서 보여줘요 (๑•̀ᴗ•́)و
#
# 쓰는 법 (컨테이너 안, 어느 폴더에서든):
#   bash /IdeaProjects/SW-AI-W07/scripts/check.sh            ← alarm 6개
#   bash /IdeaProjects/SW-AI-W07/scripts/check.sh priority   ← priority 묶음
#   bash /IdeaProjects/SW-AI-W07/scripts/check.sh all        ← threads 18개 전부
#
# 하는 일:
#   1. pintos/threads 에서 make (실패하면 에러 끝부분만 보여주고 멈춰요)
#   2. 이전 결과(.output .result)를 지우고 테스트를 하나씩 다시 돌려요
#   3. PASS/FAIL 표, FAIL 난 테스트의 이유, alarm-multiple 의 idle ticks 줄을 보여줘요
# 테스트 하나당 60초가 넘으면 make 쪽 TIMEOUT 이 끊어 줘요.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GROUP="${1:-alarm}"
LOG="/tmp/check-make.log"

source "$ROOT/activate"
cd "$ROOT/threads" || exit 1

echo "== make =="
if ! make >"$LOG" 2>&1; then
  echo "빌드 실패예요. 마지막 30줄:"
  tail -n 30 "$LOG"
  exit 1
fi
warn=$(grep -c "warning:" "$LOG")
if [ "$warn" -gt 0 ]; then
  echo "빌드 성공, 경고 ${warn}개 (이번에 다시 컴파일된 파일만 보여요. 전부 보려면 make clean 후 실행):"
  grep "warning:" "$LOG" | sort -u
else
  echo "빌드 성공, 경고 없음"
fi

cd build || exit 1
all=$(sed -n '/threads_TESTS/,/)/p' ../../tests/threads/Make.tests | grep -oE '[a-z]+(-[a-z0-9]+)+')
if [ "$GROUP" = "all" ]; then
  tests="$all"
else
  tests=$(echo "$all" | grep -E "^${GROUP}-")
fi
if [ -z "$tests" ]; then
  echo "'${GROUP}' 로 시작하는 테스트가 없어요."
  exit 1
fi

echo
echo "== 테스트 =="
pass=0; fail=0; failed=""
for t in $tests; do
  r="tests/threads/$t"
  rm -f "$r.output" "$r.result" "$r.errors"
  make "$r.result" >/dev/null 2>&1
  if [ -f "$r.result" ] && [ "$(head -n 1 "$r.result")" = "PASS" ]; then
    printf "  PASS  %s\n" "$t"; pass=$((pass + 1))
  else
    printf "  FAIL  %s\n" "$t"; fail=$((fail + 1)); failed="$failed $t"
  fi
done
echo
echo "PASS ${pass} / FAIL ${fail}"

for t in $failed; do
  r="tests/threads/$t"
  echo
  echo "---- $t 실패 이유 ($r.result) ----"
  if [ ! -f "$r.result" ]; then
    echo "(.result 파일이 없어요. $r.output 을 보세요)"
  elif grep -q "^Differences" "$r.result"; then
    # 기대 출력 목록은 길어서 건너뛰고, 차이(- 기대 / + 실제)만 보여줘요
    head -n 2 "$r.result"
    echo "(- 는 기대한 줄, + 는 실제로 나온 줄)"
    d=$(sed -n '/^Differences/,$p' "$r.result")
    n=$(printf '%s\n' "$d" | wc -l)
    printf '%s\n' "$d" | head -n 80
    [ "$n" -gt 80 ] && echo "(… $((n - 80))줄 더 있어요. 전체: $r.result)"
  else
    head -n 20 "$r.result"
  fi
done

if [ -f tests/threads/alarm-multiple.output ]; then
  echo
  echo "== 판정기 (alarm-multiple) =="
  grep "idle ticks" tests/threads/alarm-multiple.output || echo "idle ticks 줄이 없어요 (끝까지 못 갔어요)"
fi